<#
    Verify-IbGateway.ps1 - is the gateway actually usable, right now?

    READ-ONLY. It never starts, stops, restarts or reconfigures anything, and it
    never opens a client socket to the gateway (that is what puts "Client
    disconnected before version was sent" in the gateway's own log). Safe to run
    at any time, including mid-session.

    WHY THIS EXISTS. Until now, checking an upgrade meant SSHing in, curling
    /diag and eyeballing fields, plus grepping the app log for the three farm
    codes. That is fine when the person checking wrote the app and remembers
    which fields lie. It is not fine as the acceptance test for a change made
    under a deadline, by whoever is at the RDP console - and this project's
    recurring failure is a signal that LOOKS healthy:

      - the API port listens while the gateway cannot reach IBKR at all
        (the nightly stale-session state - see [[tt-gateway-overnight-reauth]])
      - data.connected reads true against a login modal
      - broker_connected=false can mean a HEALTHY gateway that has given the
        app's client id to something else (IB 326), where a relaunch is the
        wrong answer and spends one of the day's few login attempts

    So this checks the whole chain and names the link that failed.

    THE TWO VERSIONS IT PRINTS ARE THE POINT during a migration. After the
    December 2026 upgrade (IB Gateway 10.39 desupported 2026-12-15, minimum
    10.50.1) the most likely silent failure is not a crash: it is the launcher
    still selecting the old build, or a new one installed beside it that nothing
    picked up. Start-IbGateway.ps1 chooses the HIGHEST numeric version directory,
    so this reports the same choice it would make. The IBC version is printed for
    the same reason - IbcAlpha/IBC was retired on 2026-09-01, so which jar is
    live is a fact worth seeing on every check from here on.

    EXIT CODE 0 only when every check passed. Non-zero means do not trust the
    session; the summary names what failed.

    USAGE
      powershell -ExecutionPolicy Bypass -File scripts\Verify-IbGateway.ps1
      ...\Verify-IbGateway.ps1 -Quiet      # exit code only, for a scheduled job
#>
param(
    [switch]$Quiet
)

$ErrorActionPreference = 'Continue'
$failures = New-Object System.Collections.ArrayList
$warnings = New-Object System.Collections.ArrayList

function Say($msg) { if (-not $Quiet) { Write-Host $msg } }
function Pass($what, $detail) {
    Say ("  [ OK ] {0,-28} {1}" -f $what, $detail)
}
function Fail($what, $detail) {
    [void]$failures.Add($what)
    Say ("  [FAIL] {0,-28} {1}" -f $what, $detail)
}
function Warn($what, $detail) {
    [void]$warnings.Add($what)
    Say ("  [warn] {0,-28} {1}" -f $what, $detail)
}

Say ""
Say "IB Gateway verification - $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
Say "-------------------------------------------------------------------"

# ---- 1. which build would the launcher actually use? -----------------------
# Deliberately the SAME selection Start-IbGateway.ps1 makes (highest numeric
# version directory), so this cannot report a build the launcher would not pick.
$gwDirs = Get-ChildItem "C:\Jts\ibgateway" -Directory -ErrorAction SilentlyContinue |
          Where-Object { $_.Name -match '^\d+$' } |
          Sort-Object { [int]$_.Name } -Descending
if (-not $gwDirs) {
    Fail "gateway installed" "no numeric version dir under C:\Jts\ibgateway"
} else {
    $chosen = $gwDirs[0].Name
    $all = ($gwDirs | ForEach-Object { $_.Name }) -join ', '
    # 1050 is the minimum from 2026-12-15, compared NUMERICALLY.
    #
    # Checked rather than assumed, because the obvious justification for the cast
    # is wrong: '1045' -lt '1050' is True as a string too, so today's versions
    # happen to compare correctly either way. Where they diverge is a differing
    # digit count - '999' -lt '1050' is FALSE as text and True as a number - and
    # that is also why Start-IbGateway.ps1 sorts with [int] when choosing which
    # build to launch. The cast is right; the usual story told about it is not.
    if ([int]$chosen -ge 1050) {
        Pass "gateway build" "$chosen (installed: $all)"
    } else {
        Fail "gateway build" "$chosen is below the 1050 minimum that applies from 2026-12-15 (installed: $all)"
    }
    if ($gwDirs.Count -gt 1) {
        Warn "multiple builds present" "launcher takes $chosen; the others are inert but still on disk"
    }
}

# ---- 2. which IBC is live? -------------------------------------------------
# IbcAlpha/IBC was retired 2026-09-01. This does not fail the run - 3.24.1 works
# - but the version is recorded on every check so a swap is visible immediately.
$ibcJar = "C:\IBC\IBC.jar"
if (Test-Path $ibcJar) {
    $ibcVer = 'unknown'
    try {
        Add-Type -AssemblyName System.IO.Compression.FileSystem -ErrorAction Stop
        $zip = [IO.Compression.ZipFile]::OpenRead($ibcJar)
        $entry = $zip.Entries | Where-Object { $_.FullName -eq 'ibcalpha/ibc/IbcVersionInfo.class' }
        if ($entry) {
            $rdr = New-Object IO.StreamReader($entry.Open())
            $blob = $rdr.ReadToEnd()
            $rdr.Close()
            if ($blob -match '(\d+\.\d+\.\d+)') { $ibcVer = $Matches[1] }
        }
        $zip.Dispose()
    } catch { }
    $stamp = (Get-Item $ibcJar).LastWriteTime.ToString('yyyy-MM-dd')
    Pass "IBC" "$ibcVer (jar dated $stamp)"
} else {
    Fail "IBC" "C:\IBC\IBC.jar not found - Start-IbGateway.ps1 will refuse to launch"
}

# ---- 3. is the API port listening? ----------------------------------------
# PASSIVE. A listening-socket check, never a client connection: connecting just
# to test puts "Client disconnected before version was sent" in the gateway's
# log and muddies the one signal that matters when diagnosing a stale session.
$portUp = $null
foreach ($p in 4002, 4001) {
    if (Get-NetTCPConnection -LocalPort $p -State Listen -ErrorAction SilentlyContinue) {
        $portUp = $p
        break
    }
}
if ($portUp) {
    $mode = 'live'
    if ($portUp -eq 4002) { $mode = 'paper' }
    Pass "API port" "$portUp listening ($mode)"
} else {
    Fail "API port" "neither 4002 nor 4001 is listening - the gateway is not up"
}

# ---- 4. ask the app, which is the only thing that knows if it can TRADE ----
# Regex, not ConvertFrom-Json: PowerShell 5.1's parser chokes on the app's
# config.json (and on /diag). Same approach as Watch-IbGateway.ps1.
$diagPort = 8787
$diagToken = ''
try {
    $cfgRaw = Get-Content (Join-Path $env:LOCALAPPDATA 'TradeTerminal\config.json') -Raw -ErrorAction Stop
    if ($cfgRaw -match '"diag_port"\s*:\s*(\d+)') { $diagPort = [int]$Matches[1] }
    if ($cfgRaw -match '"diag_token"\s*:\s*"([^"]+)"') { $diagToken = $Matches[1] }
} catch { }

if (-not $diagToken) {
    Warn "app /diag" "no diag_token in config.json - skipping every check below"
} else {
    $body = ''
    try {
        $body = (Invoke-WebRequest -Uri "http://127.0.0.1:$diagPort/diag?token=$diagToken" `
                    -TimeoutSec 8 -UseBasicParsing -ErrorAction Stop).Content
    } catch {
        Fail "app /diag" "unreachable on 127.0.0.1:$diagPort - is tt_terminal running?"
    }
    if ($body) {
        $ver = 'unknown'
        if ($body -match '"version"\s*:\s*"([^"]+)"') { $ver = $Matches[1] }
        Pass "app /diag" "reachable (terminal $ver)"

        # THE AUTHORITATIVE AUTH SIGNAL. Not "the process exists" and not "the
        # port listens" - both of those are true of a gateway that cannot reach
        # IBKR at all, which is the exact nightly failure this box has had.
        # farms_ok counts the three data farms the app has seen come up.
        $farms = -1
        if ($body -match '"farms_ok"\s*:\s*(\d+)') { $farms = [int]$Matches[1] }
        if ($farms -ge 3) {
            Pass "data farms" "$farms/3 up"
        } elseif ($farms -ge 0) {
            Fail "data farms" "$farms/3 - the gateway is up but has not reached IBKR's farms"
        } else {
            Warn "data farms" "farms_ok not present in /diag"
        }

        if ($body -match '"gateway_authed"\s*:\s*true') {
            Pass "gateway authed" "yes"
        } else {
            Fail "gateway authed" "no - logged out, or sitting on a login/2FA dialog"
        }

        # IB 326: a HEALTHY gateway refusing the app's client id because
        # something else holds it. Checked BEFORE broker_connected, because this
        # is the one broker_connected=false whose cause is not the gateway - and
        # restarting it here would be exactly the wrong move.
        if ($body -match '"client_id_conflict"\s*:\s*\[\s*"') {
            Fail "client id" "CONFLICT - the gateway gave the app's client id to something else (IB 326). Do NOT relaunch the gateway; find the other client."
        } else {
            Pass "client id" "no conflict"
        }

        # Only meaningful while a session is running: idle, broker_connected is
        # legitimately false and says nothing about the gateway.
        if ($body -match '"live_running"\s*:\s*true') {
            $brokerOk = ($body -match '"broker_connected"\s*:\s*true')
            $upstreamOk = ($body -match '"broker_upstream"\s*:\s*true')
            if ($brokerOk -and $upstreamOk) {
                Pass "order path" "connected, upstream up"
            } elseif ($brokerOk) {
                Fail "order path" "socket up but IBKR upstream is DOWN - orders cannot reach the market"
            } else {
                Fail "order path" "broker not connected while a session is live"
            }
        } else {
            Say  "  [ -- ] order path                  no live session (idle: not a fault)"
        }
    }
}

# ---- verdict ---------------------------------------------------------------
Say "-------------------------------------------------------------------"
if ($failures.Count -eq 0) {
    if ($warnings.Count -gt 0) {
        Say "PASS with $($warnings.Count) warning(s): $($warnings -join ', ')"
    } else {
        Say "PASS - the gateway is up, authenticated and reachable."
    }
    exit 0
}
Say "FAIL ($($failures.Count)): $($failures -join ', ')"
Say ""
Say "Do not treat the session as healthy. If the gateway needs a cold relaunch:"
Say "  scripts\Start-IbGateway.ps1 -Restart"
Say "...but NOT if the failure is 'client id' (relaunching kills a working"
Say "gateway) and NOT if credentials may be wrong - IBKR locks accounts on"
Say "repeated failed logins, which is what the login-attempt governor exists"
Say "to protect. -Force bypasses it; use that only with a human watching."
exit 1
