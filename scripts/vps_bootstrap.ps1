# TradeTerminal VPS bootstrap.
# Run in an ELEVATED PowerShell on a fresh Windows box (QuantVPS or similar):
#
#   Set-ExecutionPolicy -Scope Process Bypass -Force
#   .\vps_bootstrap.ps1
#
# Installs the toolchain (Git, Python, MSYS2 + UCRT64 packages), clones and
# builds TradeTerminal (release), and applies trading-box tuning (power plan,
# clock sync, core-pinning env vars). Re-runnable: every step is idempotent.
#
# Note: Windows Server images sometimes lack winget. If the winget steps fail,
# install "App Installer" from the Microsoft Store, or install Git/Python/MSYS2
# manually and re-run — the rest of the script picks up from there.

param(
    [string]$RepoUrl = "https://github.com/Selkea/TradeTerminal.git",
    [string]$RepoDir = "C:\dev\TradeTerminal",
    [int]$PinEngineCore = 4,   # engine thread's core (leave 0-1 for Windows)
    [int]$PinFeedCore = 5      # feed I/O thread's core (must differ)
)

$ErrorActionPreference = "Stop"

function Step($msg) { Write-Host "`n=== $msg ===" -ForegroundColor Cyan }

# --- 1. toolchain ------------------------------------------------------------
Step "Installing Git, MSYS2 (winget)"
$pkgs = @("Git.Git", "MSYS2.MSYS2")
foreach ($p in $pkgs) {
    winget install --id $p -e --silent --accept-package-agreements --accept-source-agreements
    if ($LASTEXITCODE -ne 0) { Write-Host "  ($p may already be installed - continuing)" }
}
$env:Path = [Environment]::GetEnvironmentVariable("Path", "Machine") + ";" +
            [Environment]::GetEnvironmentVariable("Path", "User")

Step "Installing MSYS2 UCRT64 packages"
# First -Syu on a fresh MSYS2 can ask to restart the shell; running the
# package install as a separate invocation handles that.
& C:\msys64\usr\bin\bash.exe -lc "pacman -Syu --noconfirm" | Out-Null
& C:\msys64\usr\bin\bash.exe -lc "pacman -S --noconfirm --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-glfw mingw-w64-ucrt-x86_64-curl mingw-w64-ucrt-x86_64-sqlite3"
if ($LASTEXITCODE -ne 0) { throw "pacman package install failed" }

# --- 2. clone + build ---------------------------------------------------------
Step "Cloning $RepoUrl"
if (-not (Test-Path "$RepoDir\.git")) {
    New-Item -ItemType Directory -Force (Split-Path $RepoDir) | Out-Null
    git clone $RepoUrl $RepoDir     # private repo: Git Credential Manager will prompt
} else {
    git -C $RepoDir pull --ff-only
}

Step "Building (ucrt64-release)"
Set-Location $RepoDir
& C:\msys64\ucrt64\bin\cmake.exe --preset ucrt64-release
if ($LASTEXITCODE -ne 0) { throw "configure failed" }
& C:\msys64\ucrt64\bin\cmake.exe --build --preset ucrt64-release
if ($LASTEXITCODE -ne 0) { throw "build failed" }

# Market data + orders flow through the IBKR Client Portal Gateway (Java).
# Download it from IBKR, then set "ibkr_gateway_cmd" in
# %LOCALAPPDATA%\TradeTerminal\config.json for the in-app Launch button.

# --- 3. trading-box tuning ----------------------------------------------------
Step "Power plan: High performance (no core parking / frequency scaling)"
powercfg /setactive 8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c

Step "Clock sync"
w32tm /resync 2>$null

Step "Core pinning env vars (machine): engine=$PinEngineCore feed=$PinFeedCore"
[Environment]::SetEnvironmentVariable("TT_PIN_ENGINE", "$PinEngineCore", "Machine")
[Environment]::SetEnvironmentVariable("TT_PIN_FEED", "$PinFeedCore", "Machine")

# --- 4. wire check ------------------------------------------------------------
Step "Network placement check (connect times, lower is better)"
foreach ($ep in @("https://api.ibkr.com/v1/api/tickle",
                  "https://data.alpaca.markets/v2/stocks/AAPL/bars")) {
    $times = 1..5 | ForEach-Object {
        [double](curl.exe -o NUL -s -w "%{time_connect}" $ep)
    }
    $avg = ($times | Measure-Object -Average).Average
    Write-Host ("  {0}  avg connect {1:N1} ms" -f $ep, ($avg * 1000))
}

Step "Done"
Write-Host @"
Next steps (manual):
  1. Launch: C:\dev\build\TradeTerminal\ucrt64-release\terminal\tt_terminal.exe
     (allow the Windows Firewall prompts on private networks only)
  2. Set Windows Update active hours to cover the trading day.
  3. When leaving RDP, CLOSE the window (disconnect) - do not sign out.
  4. For remote monitoring, install Tailscale; do NOT open dashboard/RDP
     ports to the public internet.
"@
