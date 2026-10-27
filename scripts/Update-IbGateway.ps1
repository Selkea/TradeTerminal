<#
    Update-IbGateway.ps1 - upgrade IB Gateway in place, side by side.

    WHY THIS EXISTS AND Install-IbGateway.ps1 DOES NOT DO IT. That script is an
    INSTALLER and idempotent in the wrong direction for an upgrade:

        if ($numeric.Count -gt 0) { "already installed"; }   # 1045 -> no-op
        else { ...download and install... }

    Point it at a box that already has 10.45 and it prints "already installed:
    C:\Jts\ibgateway\1045" and changes nothing. Running it to meet the December
    deadline would look like success and leave the box locked out.

    THE DEADLINE. IBKR desupports IB Gateway 10.39 on 2026-12-15 and the minimum
    supported build becomes 10.50.1. This is a VERSION FLOOR, not the product
    retirement the notice reads like - the TWS socket API is unchanged and
    nothing about the terminal's architecture moves. The VPS is on 10.45, which
    is clear of 10.39 but BELOW the floor, so it does need this.

    SIDE BY SIDE, NEVER IN PLACE. The new build goes into its own numeric
    directory and the old one is left alone. Start-IbGateway.ps1 selects the
    HIGHEST numeric version under C:\Jts\ibgateway, so the new build is picked up
    with no path edits anywhere - and rollback is renaming the new directory to
    something non-numeric, which takes seconds and needs no download.

    WHAT IT WILL NOT DO. It refuses while a session is live or the book is not
    flat: the upgrade takes the gateway down for minutes, and a gateway that
    disappears from under an open position is how this project lost $846 once
    already. -Force overrides that and is for a weekend when /diag is
    unreachable, with a human watching.

    IBC IS NOT TOUCHED. IbcAlpha/IBC was retired 2026-09-01 and 3.24.1 is what
    drives the login here. Whether it copes with 10.50's dialogs is the one thing
    this upgrade cannot predict, which is exactly why the verification step below
    matters and why docs\IBC-MIGRATION.md says to do this upgrade FIRST and
    alone - it is the experiment that tells you whether IBC needs replacing.

    USAGE (run from an RDP session on the VPS, as Administrator)

      # look, change nothing - safe any time, including mid-session
      powershell -ExecutionPolicy Bypass -File C:\dev\TradeTerminal\scripts\Update-IbGateway.ps1 -Check

      # the whole thing: download, install, restart via IBC, verify
      powershell -ExecutionPolicy Bypass -File C:\dev\TradeTerminal\scripts\Update-IbGateway.ps1

      # download and install only, restart later yourself
      powershell -ExecutionPolicy Bypass -File C:\dev\TradeTerminal\scripts\Update-IbGateway.ps1 -StageOnly
#>
param(
    # Report the preconditions and the installed builds, then exit. Changes
    # nothing and is safe to run while trading.
    [switch]$Check,
    # Download and install, but do not restart the gateway. Use when you want the
    # new build staged now and the login attempt spent later.
    [switch]$StageOnly,
    # Skip the live-session / flat-book gate, and pass -Force to the restart so
    # the login-attempt governor is bypassed. A HUMAN AT THE CONSOLE ONLY:
    # IBKR locks accounts on repeated failed logins.
    [switch]$Force,
    # The floor that applies from 2026-12-15. Exposed so a future bump needs no
    # edit, not because it is expected to change often.
    [int]$MinimumVersion = 1050
)

$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$jts = 'C:\Jts\ibgateway'
$stage = Join-Path $jts '_upgrade'
$installer = 'https://download2.interactivebrokers.com/installers/ibgateway/stable-standalone/ibgateway-stable-standalone-windows-x64.exe'

function Step($m) { Write-Host "`n=== $m ===" -ForegroundColor Cyan }
function Ok($m)   { Write-Host "  [ OK ] $m" -ForegroundColor Green }
function Info($m) { Write-Host "  [info] $m" }
function Warn($m) { Write-Host "  [warn] $m" -ForegroundColor Yellow }
function Die($m)  { Write-Host "  [FAIL] $m" -ForegroundColor Red; exit 1 }

# Numeric version directories, newest first - deliberately the SAME selection
# Start-IbGateway.ps1 makes, so this can never report a build the launcher would
# not actually use.
function Get-GwDirs {
    return @(Get-ChildItem $jts -Directory -ErrorAction SilentlyContinue |
             Where-Object { $_.Name -match '^\d+$' } |
             Sort-Object { [int]$_.Name } -Descending)
}

# The build number comes from the launch jar (jts4launch-1045.jar), which is how
# Install-IbGateway.ps1 identifies a build and therefore the only name that is
# guaranteed to agree with it.
function Get-GwVersion($dir) {
    $jar = Get-ChildItem (Join-Path $dir 'jars') -Filter '*launch-*.jar' -ErrorAction SilentlyContinue |
           Select-Object -First 1
    if ($jar -and $jar.Name -match '(\d{3,5})') { return [int]$Matches[1] }
    return 0
}

Step "IB Gateway update"
Info "deadline: 10.39 desupported 2026-12-15, minimum build becomes $MinimumVersion"

# ---- what is installed now -------------------------------------------------
$dirs = Get-GwDirs
if ($dirs.Count -eq 0) {
    Die "no numeric build under $jts - this is an UPGRADE script. Run Install-IbGateway.ps1 first."
}
$current = [int]$dirs[0].Name
Info ("installed builds: " + (($dirs | ForEach-Object { $_.Name }) -join ', '))
if ($current -ge $MinimumVersion) {
    Ok "launcher would use $current, already at or above the $MinimumVersion floor"
    if (-not $Check) { Info "nothing to do; exiting without changes" }
    exit 0
}
Warn "launcher would use $current - BELOW the $MinimumVersion floor"

# ---- is it safe to take the gateway down? ----------------------------------
# Asked of the APP, which is the only thing that knows whether it is mid-trade.
# Regex rather than ConvertFrom-Json: PowerShell 5.1's parser chokes on this
# config.json, same as Watch-IbGateway.ps1 documents.
Step "Safety gate"
$diagPort = 8787
$diagToken = ''
try {
    $cfgRaw = Get-Content (Join-Path $env:LOCALAPPDATA 'TradeTerminal\config.json') -Raw -ErrorAction Stop
    if ($cfgRaw -match '"diag_port"\s*:\s*(\d+)') { $diagPort = [int]$Matches[1] }
    if ($cfgRaw -match '"diag_token"\s*:\s*"([^"]+)"') { $diagToken = $Matches[1] }
} catch { }

$liveRunning = $null      # $null = could not tell
$openPositions = $null
if ($diagToken) {
    try {
        $body = (Invoke-WebRequest -Uri "http://127.0.0.1:$diagPort/diag?token=$diagToken" `
                    -TimeoutSec 8 -UseBasicParsing -ErrorAction Stop).Content
        $liveRunning = ($body -match '"live_running"\s*:\s*true')
        # Compared NUMERICALLY, not by a leading-digit regex. The obvious
        # '"position"\s*:\s*-?[1-9]' looks right and silently misses a
        # fractional holding like 0.5, whose failure direction is the wrong one:
        # it would ALLOW the upgrade over an open position.
        $openPositions = $false
        foreach ($m in [regex]::Matches($body, '"position"\s*:\s*(-?[0-9.]+)')) {
            if ([double]$m.Groups[1].Value -ne 0) { $openPositions = $true; break }
        }
    } catch { }
}

# Gate findings are RECORDED, not acted on, so -Check can report every one of
# them. A check that cannot change anything has nothing to be unsafe about, and
# an earlier cut of this died at the first problem - telling the operator "I will
# change nothing" and then hiding the rest of what it knew.
$blocked = $null
if ($liveRunning -eq $null) {
    Warn "could not reach /diag - cannot confirm the app is idle"
    if (-not $Force) { $blocked = "cannot confirm the app is idle (/diag unreachable). Stop the app, or use -Force if you know it is." }
    else { Warn "-Force given: would proceed without proof" }
} elseif ($liveRunning) {
    $blocked = "a LIVE SESSION is running. Let the 16:15 guard stop it, or stop it in the Trade panel."
    Warn $blocked
} elseif ($openPositions) {
    $blocked = "the book is NOT FLAT. Taking the gateway down over an open position is how 2026-08-06 cost `$846."
    Warn $blocked
} else {
    # NOT "book flat". With no live session /diag carries no position rows at
    # all - the symbols array is session-scoped - so their absence is not
    # evidence of a flat book. And this box ends every day on the 16:15 guard,
    # which KEEPS positions for the next session to re-adopt, so overnight is
    # precisely when something can be held that the app cannot see.
    #
    # Not treated as blocking: nothing is managing those positions while the app
    # is idle anyway, the broker-side stops stay live at IBKR across a gateway
    # restart, and the gateway is back in minutes. But it is said plainly
    # instead of printing a reassurance that was never checked.
    Ok "no live session running"
    Info "the app is idle, so /diag cannot see the broker's book - if the 16:15"
    Info "guard kept positions overnight they are held at IBKR and invisible here"
}

$free = (Get-PSDrive C).Free / 1GB
if ($free -lt 2) {
    $blocked = "only {0:N1} GB free on C: - the installer needs room" -f $free
    Warn $blocked
} else {
    Ok ("{0:N1} GB free on C:" -f $free)
}

$admin = ([Security.Principal.WindowsPrincipal] `
          [Security.Principal.WindowsIdentity]::GetCurrent()
         ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if ($admin) { Ok "running elevated" } else { Warn "NOT elevated - the install4j installer may fail silently" }

if ($Check) {
    Step "Check only - nothing was changed"
    if ($blocked) {
        Warn "an upgrade right now would REFUSE: $blocked"
        Info "fix that, then re-run without -Check"
    } else {
        Ok "an upgrade would proceed: $current -> whatever the stable channel currently is"
        Info "to do it:  powershell -ExecutionPolicy Bypass -File $PSCommandPath"
    }
    exit 0
}
if ($blocked) { Die $blocked }

# ---- download --------------------------------------------------------------
Step "Download"
$exe = Join-Path $env:TEMP 'ibgateway-stable-standalone.exe'
if (Test-Path $exe) { Remove-Item $exe -Force }
curl.exe -sL --fail -o $exe $installer
if ($LASTEXITCODE -ne 0) { Die "download failed (curl exit $LASTEXITCODE)" }
$mb = (Get-Item $exe).Length / 1MB
# A few hundred KB means a redirect page or an error body, not an installer.
# Running that would do nothing and report success.
if ($mb -lt 50) { Die ("downloaded only {0:N1} MB - that is not the installer" -f $mb) }
Ok ("{0:N0} MB downloaded" -f $mb)

# ---- install, side by side -------------------------------------------------
Step "Install"
if (Test-Path $stage) {
    Info "clearing a leftover staging directory from a previous run"
    Remove-Item $stage -Recurse -Force
}
# install4j needs an explicit -dir; a bare -q no-ops. Staged under its own name
# so a half-finished install can never be mistaken for a numeric build.
Start-Process $exe -ArgumentList '-q', '-dir', $stage -Wait
if (-not (Test-Path $stage)) { Die "the installer produced nothing at $stage" }

$newVer = Get-GwVersion $stage
if ($newVer -eq 0) { Remove-Item $stage -Recurse -Force; Die "could not read a build number from the staged install" }
Info "staged build: $newVer"

# REFUSE A SIDEWAYS OR BACKWARD MOVE. The stable channel is a moving target, and
# silently installing the same build - or an older one - would leave the box
# below the floor while the script reported success.
if ($newVer -le $current) {
    Remove-Item $stage -Recurse -Force
    Die "staged build $newVer is not newer than the installed $current. The stable channel may not have moved yet; nothing changed."
}
if ($newVer -lt $MinimumVersion) {
    Remove-Item $stage -Recurse -Force
    Die "staged build $newVer is still below the $MinimumVersion floor. Nothing changed - the stable channel has not reached it."
}

$target = Join-Path $jts "$newVer"
if (Test-Path $target) { Die "$target already exists but the launcher chose $current - resolve that by hand before re-running" }
Rename-Item $stage "$newVer"
Ok "installed as $target (build $current left in place for rollback)"

if ($StageOnly) {
    Step "Staged"
    Info "the new build is installed but the gateway is still running $current."
    Info "when ready:  C:\dev\TradeTerminal\scripts\Start-IbGateway.ps1 -Restart"
    Info "then verify: C:\dev\TradeTerminal\scripts\Verify-IbGateway.ps1"
    Info "rollback:    Rename-Item '$target' '$newVer.disabled'"
    exit 0
}

# ---- restart onto the new build --------------------------------------------
Step "Restart"
Info "the gateway will be down for a minute or two"
$start = Join-Path $PSScriptRoot 'Start-IbGateway.ps1'
if (-not (Test-Path $start)) { Die "Start-IbGateway.ps1 not found next to this script" }
# NOT $args - that is a PowerShell automatic variable, and assigning to it
# inside a script is at best confusing and at worst silently ignored.
$startArgs = @('-Restart')
if ($Force) { $startArgs += '-Force' }
& powershell -ExecutionPolicy Bypass -NoProfile -File $start @startArgs
$restartCode = $LASTEXITCODE
if ($restartCode -ne 0) {
    Warn "Start-IbGateway.ps1 exited $restartCode"
    Warn "if it refused on the login-attempt governor that is DELIBERATE - IBKR locks"
    Warn "accounts on repeated failed logins. Check the console for a 2FA or"
    Warn "disclaimer dialog before re-running with -Force."
}

# ---- verify ----------------------------------------------------------------
Step "Verify"
# The farms take a moment to come up after a cold login; checking instantly
# would report a healthy upgrade as a failure.
Info "waiting 45s for the login and the data farms to settle"
Start-Sleep -Seconds 45
$verify = Join-Path $PSScriptRoot 'Verify-IbGateway.ps1'
if (Test-Path $verify) {
    & powershell -ExecutionPolicy Bypass -NoProfile -File $verify
    $vc = $LASTEXITCODE
} else {
    Warn "Verify-IbGateway.ps1 not found - verify by hand"
    $vc = 1
}

Step "Result"
if ($vc -eq 0) {
    Ok "build $newVer is live and authenticated"
    Write-Host ""
    Write-Host "  STILL TO DO, and it is the half that matters:" -ForegroundColor Yellow
    Write-Host "  Watch ONE overnight cold restart (ColdRestartTime=23:55), then run"
    Write-Host "  Verify-IbGateway.ps1 again before the open. That is the only check"
    Write-Host "  that exercises the SCHEDULED login path, and whether IBC 3.24.1"
    Write-Host "  still drives this build's dialogs is the one thing this upgrade"
    Write-Host "  could not predict. See docs\IBC-MIGRATION.md."
} else {
    Warn "verification did NOT pass - do not leave it like this"
    Write-Host ""
    Write-Host "  ROLLBACK (seconds, no download):" -ForegroundColor Yellow
    Write-Host "    Rename-Item '$target' '$newVer.disabled'"
    Write-Host "    C:\dev\TradeTerminal\scripts\Start-IbGateway.ps1 -Restart"
    Write-Host "  That puts build $current back, because the launcher picks the"
    Write-Host "  highest NUMERIC directory and the renamed one stops counting."
}
exit $vc
