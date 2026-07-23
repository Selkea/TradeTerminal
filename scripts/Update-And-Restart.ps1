# Self-update helper for TradeTerminal. Launched detached by the app's
# "Restart & Update" panel; it waits for the app to exit, pulls the latest
# main, rebuilds the release preset, and relaunches the same exe. Best-effort
# and fully logged (%LOCALAPPDATA%\TradeTerminal\logs\update.log) so a headless
# VPS run can be diagnosed after the fact.
param(
    [int]$WaitPid = 0,
    [string]$Exe = "",
    [string]$Preset = "ucrt64-release"
)

$ErrorActionPreference = "Continue"

# Repo root = this script's parent (scripts\..). Configure-time source tree.
$src = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

$logDir = Join-Path $env:LOCALAPPDATA "TradeTerminal\logs"
if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Force -Path $logDir | Out-Null }
$log = Join-Path $logDir "update.log"
function Log($m) {
    "$([DateTime]::Now.ToString('yyyy-MM-dd HH:mm:ss')) $m" |
        Out-File -FilePath $log -Append -Encoding utf8
}

Log "=== updater start: pid=$WaitPid preset=$Preset src=$src exe=$Exe ==="

# The build toolchain (cmake/ninja/gcc) lives in the MSYS2 UCRT64 tree, which a
# plain PowerShell doesn't have on PATH. Prepend it so the build resolves; a
# system git after it still resolves for the pull.
$ucrt = "C:\msys64\ucrt64\bin"
if (Test-Path $ucrt) { $env:PATH = "$ucrt;$env:PATH" }

# 1) Wait for the old instance to exit so it releases the single-instance mutex
#    and its file locks before we rebuild + relaunch.
if ($WaitPid -gt 0) {
    try {
        Wait-Process -Id $WaitPid -Timeout 60 -ErrorAction Stop
        Log "old instance ($WaitPid) exited"
    } catch {
        Log "wait-process: $($_.Exception.Message)"
    }
}

# 2) Pull latest main (fast-forward only; a dirty/diverged tree is left alone
#    and the rebuild below just re-links the current source).
Log "git fetch/pull..."
& git -C $src fetch origin main 2>&1 | ForEach-Object { Log "git> $_" }
& git -C $src pull --ff-only 2>&1 | ForEach-Object { Log "git> $_" }
& git -C $src rev-parse --short=12 HEAD 2>&1 | ForEach-Object { Log "git> now at $_" }

# 3) Rebuild the release preset. Ninja auto-reconfigures if CMakeLists moved
#    (e.g. a new source file landed), so a plain build is enough.
Log "cmake --build --preset $Preset ..."
Push-Location $src
& cmake --build --preset $Preset 2>&1 | ForEach-Object { Log "build> $_" }
$buildExit = $LASTEXITCODE
Pop-Location
Log "build exit=$buildExit"

# 4) Relaunch. On a failed build ninja leaves the previously-linked exe in
#    place, so we relaunch either way rather than leave the box dark; the log
#    (and /diag git_commit after restart) shows whether the new build took.
if ($Exe -and (Test-Path $Exe)) {
    Log "relaunching $Exe"
    Start-Process -FilePath $Exe
} else {
    Log "relaunch SKIPPED: exe not found ($Exe)"
}
Log "=== updater done ==="
