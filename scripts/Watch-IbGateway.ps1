<#
    Watch-IbGateway.ps1 - keep the classic IB Gateway (TWS route) logged in for
    as long as the TradeTerminal app is running, and STOP as soon as the app
    exits.

    The app spawns this on start (TWS route) with -AppPid <its own pid>. Unlike a
    one-shot launch, it self-heals a login that fails at a bad moment - e.g. a
    forced re-login that lands in IBKR's overnight maintenance window and gets a
    transient "Unrecognized Username or Password". The one-shot IBC launch has no
    retry, so a single bad attempt could leave the gateway down for hours; this
    loop just re-logs-in a couple minutes later and comes up on its own.

    It delegates every launch/restart to Start-IbGateway.ps1 (idempotent; -Restart
    kills a stuck gateway + its leftover error dialog, then re-logs-in). It never
    kills the gateway on its own exit - the gateway is left running so the login
    survives an app restart (and so its autorestart soft-restart chain, which
    avoids re-auth for ~a week, stays intact).

    Params:
      -AppPid <int>       REQUIRED. Exit when this process is gone (i.e. the app
                          closed) - so the daemon never outlives the app.
      -PollSec <int>      health-check cadence (default 20s).
      -DownGraceSec <int> how long the API port may be down before forcing a
                          re-login (default 90s), so a brief reconnect blip does
                          not trigger a needless gateway restart.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][int]$AppPid,
    [int]$PollSec = 20,
    [int]$DownGraceSec = 90
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

# API port up? Probe both paper (4002) and live (4001) so we don't need the
# account's mode here; either listening means the gateway is authenticated.
function Test-GatewayUp {
    foreach ($p in 4002, 4001) {
        try {
            $c = [Net.Sockets.TcpClient]::new()
            $c.Connect('127.0.0.1', $p); $c.Close(); return $true
        } catch { }
    }
    return $false
}

# The app closed if that pid is gone, OR was reused by a different process
# (guard against PID reuse by checking the name).
function Test-AppAlive {
    $p = Get-Process -Id $AppPid -ErrorAction SilentlyContinue
    return ($p -and $p.ProcessName -eq 'tt_terminal')
}

Log "keepalive start: AppPid=$AppPid poll=${PollSec}s grace=${DownGraceSec}s"

# Initial launch (idempotent - Start-IbGateway exits fast if the port is up).
& $script *>&1 | ForEach-Object { Log "launch> $_" }

$downSince = $null
while ($true) {
    if (-not (Test-AppAlive)) {
        Log "app (pid $AppPid) gone - keepalive exiting; gateway left running"
        break
    }

    try {
        if (Test-GatewayUp) {
            if ($downSince) { Log 'gateway back up'; $downSince = $null }
        } else {
            if (-not $downSince) {
                $downSince = Get-Date
                Log 'gateway API port down - watching'
            } elseif (((Get-Date) - $downSince).TotalSeconds -ge $DownGraceSec) {
                Log "gateway down > ${DownGraceSec}s - forcing re-login (Start-IbGateway -Restart)"
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
