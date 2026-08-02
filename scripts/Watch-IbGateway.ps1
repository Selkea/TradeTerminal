<#
    Watch-IbGateway.ps1 - keep the classic IB Gateway (TWS route) logged in and
    ACTUALLY USABLE for as long as the TradeTerminal app is running, and STOP as
    soon as the app exits.

    The app spawns this on start (TWS route) with -AppPid <its own pid>. It
    self-heals two failure modes by delegating to Start-IbGateway.ps1 -Restart
    (which kills a stuck gateway + its leftover error dialog, then re-logs-in):

      1. API port DOWN  - the gateway crashed / a login failed at a bad moment
         (e.g. a forced re-login in IBKR's maintenance window). One-shot IBC has
         no retry; this loop re-logs-in a couple minutes later.

      2. Port UP but UNUSABLE - after a stale soft-restart (or an unrecovered
         IBKR disconnect) the gateway keeps the API port open yet can't reach
         IBKR, so orders can't get through. A plain port probe can't see this;
         we ask the app's /diag (v0.2.13+ reports broker_connected = socket AND
         upstream) and force a COLD relaunch when a live session genuinely can't
         reach the broker. See [[tt-gateway-overnight-reauth]].

    The port check is PASSIVE (Get-NetTCPConnection) rather than a TCP connect:
    connecting without sending the API version made the gateway log "Client
    disconnected before version was sent" on every poll - pure noise.

    It never kills the gateway on its own exit - the gateway is left running so
    the login survives an app restart.

    Params:
      -AppPid <int>         REQUIRED. Exit when this process is gone (app closed).
      -PollSec <int>        health-check cadence (default 20s).
      -DownGraceSec <int>   how long the API port may be down before forcing a
                            re-login (default 90s) - rides out a brief blip.
      -BrokerGraceSec <int> how long a live session may be unable to reach the
                            broker (port up) before a COLD relaunch (default
                            240s) - long enough that a transient 1100->1102 blip
                            self-heals without a needless restart.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][int]$AppPid,
    [int]$PollSec = 20,
    [int]$DownGraceSec = 90,
    [int]$BrokerGraceSec = 240
)
$ErrorActionPreference = 'Continue'

$script = Join-Path $PSScriptRoot 'Start-IbGateway.ps1'

$logDir = Join-Path $env:LOCALAPPDATA 'TradeTerminal\logs'
if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Force -Path $logDir | Out-Null }
$log = Join-Path $logDir 'gateway-keepalive.log'
function Log([string]$m) {
    "$([DateTime]::Now.ToString('yyyy-MM-dd HH:mm:ss')) $m" |
        Out-File -FilePath $log -Append -Encoding utf8
}

# Read the read-only /diag port + token so we can ask the app whether the gateway
# is usable. Regex, not ConvertFrom-Json - Windows PowerShell 5.1's JSON parser
# chokes on the app's config.json. Best-effort: no token => skip the usability
# check (degrade to the port check).
$diagPort = 8787
$diagToken = ''
try {
    $cfgRaw = Get-Content (Join-Path $env:LOCALAPPDATA 'TradeTerminal\config.json') -Raw -ErrorAction Stop
    if ($cfgRaw -match '"diag_port"\s*:\s*(\d+)') { $diagPort = [int]$Matches[1] }
    if ($cfgRaw -match '"diag_token"\s*:\s*"([^"]+)"') { $diagToken = $Matches[1] }
} catch { }

# API port up? Passive listening-socket check (no client connection to the
# gateway, so no "client disconnected before version" noise in its log). Probe
# both paper (4002) and live (4001) so we don't need the account's mode here.
function Test-GatewayUp {
    foreach ($p in 4002, 4001) {
        if (Get-NetTCPConnection -LocalPort $p -State Listen -ErrorAction SilentlyContinue) {
            return $true
        }
    }
    return $false
}

# The gateway is up on the port but can the app actually trade through it? Ask
# /diag. Stuck ONLY when a live session is running yet broker_connected is false
# (socket up but no IBKR upstream) - so this never fires on a healthy gateway or
# when idle. Unreachable/unknown => not stuck (fall back to the port check).
function Test-BrokerStuck {
    if (-not $diagToken) { return $false }
    try {
        $body = (Invoke-WebRequest -Uri "http://127.0.0.1:$diagPort/diag?token=$diagToken" `
                    -TimeoutSec 5 -UseBasicParsing -ErrorAction Stop).Content
    } catch { return $false }
    $live = $body -match '"live_running"\s*:\s*true'
    $brokerUp = $body -match '"broker_connected"\s*:\s*true'
    return ($live -and -not $brokerUp)
}

# The app closed if that pid is gone, OR was reused by a different process
# (guard against PID reuse by checking the name).
function Test-AppAlive {
    $p = Get-Process -Id $AppPid -ErrorAction SilentlyContinue
    return ($p -and $p.ProcessName -eq 'tt_terminal')
}

Log "keepalive start: AppPid=$AppPid poll=${PollSec}s portGrace=${DownGraceSec}s brokerGrace=${BrokerGraceSec}s diag=127.0.0.1:$diagPort"

# Initial launch (idempotent - Start-IbGateway exits fast if the port is up).
& $script *>&1 | ForEach-Object { Log "launch> $_" }

$downSince = $null         # API port down
$brokerDownSince = $null   # port up but the app can't reach the broker
while ($true) {
    if (-not (Test-AppAlive)) {
        Log "app (pid $AppPid) gone - keepalive exiting; gateway left running"
        break
    }

    try {
        if (Test-GatewayUp) {
            if ($downSince) { Log 'gateway API port back up'; $downSince = $null }
            # Port is up - but is the gateway actually usable (reaching IBKR)?
            if (Test-BrokerStuck) {
                if (-not $brokerDownSince) {
                    $brokerDownSince = Get-Date
                    Log 'gateway port up but the app cannot reach the broker - watching'
                } elseif (((Get-Date) - $brokerDownSince).TotalSeconds -ge $BrokerGraceSec) {
                    Log "broker unreachable > ${BrokerGraceSec}s despite the port being up - forcing a COLD relaunch"
                    & $script -Restart *>&1 | ForEach-Object { Log "relogin> $_" }
                    $brokerDownSince = $null
                }
            } elseif ($brokerDownSince) {
                Log 'broker reachable again'
                $brokerDownSince = $null
            }
        } else {
            $brokerDownSince = $null   # the port-down path owns recovery now
            if (-not $downSince) {
                $downSince = Get-Date
                Log 'gateway API port down - watching'
            } elseif (((Get-Date) - $downSince).TotalSeconds -ge $DownGraceSec) {
                Log "gateway port down > ${DownGraceSec}s - forcing re-login (Start-IbGateway -Restart)"
                # -Restart blocks up to ~120s waiting for the port; that IS the
                # backoff. If the login still fails (e.g. mid-maintenance), the
                # next pass re-arms $downSince and retries.
                & $script -Restart *>&1 | ForEach-Object { Log "relogin> $_" }
                $downSince = $null
            }
        }
    } catch {
        Log "watch error: $($_.Exception.Message)"
    }

    Start-Sleep -Seconds $PollSec
}
