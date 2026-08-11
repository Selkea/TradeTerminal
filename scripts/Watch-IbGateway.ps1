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

         "Genuinely" excludes one case, added in 0.20.1: /diag's
         client_id_conflict says the gateway is healthy and is refusing the app
         its API client id (IB error 326). Nothing this script does can hand the
         id back, so a relaunch there is a gateway restart every BrokerGraceSec
         for the rest of the day. It logs the real cause and leaves the gateway
         alone; the app retries on its own.

    The port check is PASSIVE (Get-NetTCPConnection) rather than a TCP connect:
    connecting without sending the API version made the gateway log "Client
    disconnected before version was sent" on every poll - pure noise.

    It never kills the gateway on its own exit - the gateway is left running so
    the login survives an app restart.

    THIS LOOP IS NOT THE THING THAT BOUNDS LOGIN ATTEMPTS. It has no day stamp
    and re-arms after every attempt, by design - it has to, since it is the only
    thing that can recover a gateway that died at 03:00. The cap lives in
    Start-IbGateway.ps1's governor, which every caller goes through and which
    refuses an attempt that comes too soon. See the header there: the app's
    08:45 pre-open check ALSO relaunches the gateway, and a relaunch is exactly
    what takes the API port down and arms the loop below, so the two would
    otherwise take turns re-logging-in an account IBKR locks for it.

    Params:
      -AppPid <int>         REQUIRED. Exit when this process is gone (app closed).
      -PollSec <int>        health-check cadence (default 20s).
      -DownGraceSec <int>   how long the API port may be down before forcing a
                            re-login (default 180s). MUST exceed the 120s
                            Start-IbGateway.ps1 itself allows for the API port
                            to come up: at the old 90s this loop routinely
                            declared a cold start failed while it was still
                            logging in, killed it, and spent another attempt.
      -BrokerGraceSec <int> how long a live session may be unable to reach the
                            broker (port up) before a COLD relaunch (default
                            240s) - long enough that a transient 1100->1102 blip
                            self-heals without a needless restart.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][int]$AppPid,
    [int]$PollSec = 20,
    [int]$DownGraceSec = 180,
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
#
# EXCEPT when the app says it is being REFUSED its API client id (IB error 326,
# reported in /diag as client_id_conflict). That is the one broker_connected=false
# whose cause is not the gateway: the gateway is healthy and has given the id to
# something else. A cold relaunch there kills a working gateway, disconnects
# whatever legitimately holds the id, and spends one of the day's few login
# attempts on a fault the app's own retry loop is already working on - every
# BrokerGraceSec, for the rest of the trading day. Returns "conflict" so the
# caller can log the real cause instead of "the gateway is stuck".
function Get-BrokerState {
    if (-not $diagToken) { return 'unknown' }
    try {
        $body = (Invoke-WebRequest -Uri "http://127.0.0.1:$diagPort/diag?token=$diagToken" `
                    -TimeoutSec 5 -UseBasicParsing -ErrorAction Stop).Content
    } catch { return 'unknown' }
    $live = $body -match '"live_running"\s*:\s*true'
    $brokerUp = $body -match '"broker_connected"\s*:\s*true'
    if (-not $live -or $brokerUp) { return 'ok' }
    # "client_id_conflict":["orders","feed"] - a non-empty ARRAY. The empty
    # array is the healthy case and must not match, so require a quote inside.
    if ($body -match '"client_id_conflict"\s*:\s*\[\s*"') { return 'conflict' }
    return 'stuck'
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
$conflictLogged = $false   # say "refused our client id" once per episode, not every poll
while ($true) {
    if (-not (Test-AppAlive)) {
        Log "app (pid $AppPid) gone - keepalive exiting; gateway left running"
        break
    }

    try {
        if (Test-GatewayUp) {
            if ($downSince) { Log 'gateway API port back up'; $downSince = $null }
            # Port is up - but is the gateway actually usable (reaching IBKR)?
            $state = Get-BrokerState
            if ($state -eq 'conflict') {
                # NOT a gateway fault: it is up, logged in, and has handed our
                # API client id to something else (IB error 326). Restarting it
                # would be an unbreakable loop - the app cannot get the id back
                # any faster for it, so broker_connected stays false and the next
                # window restarts it again. The app retries on its own and logs
                # "API CLIENT ID IN USE" / "API CLIENT ID FREE AGAIN"; all this
                # loop should do is not make it worse, and say so once.
                if (-not $conflictLogged) {
                    $conflictLogged = $true
                    Log 'app reports IB error 326 (another program holds its API client id) - NOT restarting the gateway; it is healthy. See the app log for which client and what to close.'
                }
                $brokerDownSince = $null
            } elseif ($state -eq 'stuck') {
                $conflictLogged = $false
                if (-not $brokerDownSince) {
                    $brokerDownSince = Get-Date
                    Log 'gateway port up but the app cannot reach the broker - watching'
                } elseif (((Get-Date) - $brokerDownSince).TotalSeconds -ge $BrokerGraceSec) {
                    Log "broker unreachable > ${BrokerGraceSec}s despite the port being up - forcing a COLD relaunch"
                    & $script -Restart *>&1 | ForEach-Object { Log "relogin> $_" }
                    $brokerDownSince = $null
                }
            } else {
                if ($conflictLogged) {
                    Log 'app is no longer reporting a client-id conflict'
                    $conflictLogged = $false
                }
                if ($brokerDownSince) {
                    Log 'broker reachable again'
                    $brokerDownSince = $null
                }
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
                # next pass re-arms $downSince and retries - and Start-IbGateway's
                # governor refuses the ones that come too fast, so "retries
                # forever" cannot become "locks the account". A refusal is cheap
                # and leaves the gateway alone; it just gets logged below.
                & $script -Restart *>&1 | ForEach-Object { Log "relogin> $_" }
                $downSince = $null
            }
        }
    } catch {
        Log "watch error: $($_.Exception.Message)"
    }

    Start-Sleep -Seconds $PollSec
}
