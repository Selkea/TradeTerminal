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
# Note: Windows Server images often lack winget; when it is missing the script
# falls back to direct silent installers (Git, Python, MSYS2, Temurin, Chrome)
# automatically.

param(
    [string]$RepoUrl = "https://github.com/Selkea/TradeTerminal.git",
    [string]$RepoDir = "C:\dev\TradeTerminal",
    [int]$PinEngineCore = 4,   # engine thread's core (leave 0-1 for Windows)
    [int]$PinFeedCore = 5      # feed I/O thread's core (must differ)
)

$ErrorActionPreference = "Stop"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

function Step($msg) { Write-Host "`n=== $msg ===" -ForegroundColor Cyan }
function Update-PathEnv {
    $env:Path = [Environment]::GetEnvironmentVariable("Path", "Machine") + ";" +
                [Environment]::GetEnvironmentVariable("Path", "User")
}
$haveWinget = [bool](Get-Command winget -ErrorAction SilentlyContinue)
function Install-ViaWinget($id) {
    winget install --id $id -e --silent --accept-package-agreements --accept-source-agreements
    if ($LASTEXITCODE -ne 0) { Write-Host "  ($id may already be installed - continuing)" }
    Update-PathEnv
}
function Get-File($url, $out) {
    curl.exe -sL -o $out $url
    if ($LASTEXITCODE -ne 0) { throw "download failed: $url" }
}

# --- 1. toolchain ------------------------------------------------------------
Step "Toolchain: Git, Python, MSYS2"
if (-not $haveWinget) {
    Write-Host "  winget not available - using direct installers" -ForegroundColor Yellow
}

# Git
if (Get-Command git -ErrorAction SilentlyContinue) {
    Write-Host "  git present"
} elseif ($haveWinget) {
    Install-ViaWinget "Git.Git"
} else {
    $rel = Invoke-RestMethod "https://api.github.com/repos/git-for-windows/git/releases/latest"
    $asset = $rel.assets | Where-Object { $_.name -match "^Git-.*-64-bit\.exe$" } |
             Select-Object -First 1
    if (-not $asset) { throw "could not resolve the Git for Windows installer" }
    $exe = Join-Path $env:TEMP $asset.name
    Get-File $asset.browser_download_url $exe
    Start-Process $exe -ArgumentList "/VERYSILENT", "/NORESTART" -Wait
    Update-PathEnv
}

# Python: a real interpreter on PATH (not the Store alias), which the IBKR
# auto-login daemon needs to run headless.
if (Get-Command python -ErrorAction SilentlyContinue) {
    Write-Host "  python present"
} elseif ($haveWinget) {
    Install-ViaWinget "Python.Python.3.12"
} else {
    $exe = Join-Path $env:TEMP "python-3.12.10-amd64.exe"
    Get-File "https://www.python.org/ftp/python/3.12.10/python-3.12.10-amd64.exe" $exe
    Start-Process $exe -ArgumentList "/quiet", "InstallAllUsers=1", "PrependPath=1",
                                     "Include_test=0" -Wait
    Update-PathEnv
}

# MSYS2
if (Test-Path "C:\msys64\usr\bin\bash.exe") {
    Write-Host "  msys2 present"
} elseif ($haveWinget) {
    Install-ViaWinget "MSYS2.MSYS2"
} else {
    $sfx = Join-Path $env:TEMP "msys2-base.sfx.exe"
    Get-File "https://github.com/msys2/msys2-installer/releases/latest/download/msys2-base-x86_64-latest.sfx.exe" $sfx
    & $sfx -y "-oC:\" | Out-Null   # self-extracts to C:\msys64
    if (-not (Test-Path "C:\msys64\usr\bin\bash.exe")) { throw "MSYS2 extraction failed" }
}

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
# Commits from this box (if any) use the GitHub username + noreply address.
git -C $RepoDir config --local user.name "Selkea"
git -C $RepoDir config --local user.email "4535629+Selkea@users.noreply.github.com"

Step "Building (ucrt64-release)"
Set-Location $RepoDir
& C:\msys64\ucrt64\bin\cmake.exe --preset ucrt64-release
if ($LASTEXITCODE -ne 0) { throw "configure failed" }
& C:\msys64\ucrt64\bin\cmake.exe --build --preset ucrt64-release
if ($LASTEXITCODE -ne 0) { throw "build failed" }

# --- 2b. IBKR Client Portal Gateway (all market data + orders flow here) ------
Step "Java runtime (gateway requirement)"
if (Get-Command java -ErrorAction SilentlyContinue) {
    Write-Host "  java present: $((Get-Command java).Source)"
} else {
    if ($haveWinget) {
        Install-ViaWinget "EclipseAdoptium.Temurin.21.JRE"
    } else {
        # Adoptium's API redirects to the latest Temurin 21 JRE MSI.
        $msi = Join-Path $env:TEMP "temurin21-jre.msi"
        Get-File "https://api.adoptium.net/v3/installer/latest/21/ga/windows/x64/jre/hotspot/normal/eclipse" $msi
        Start-Process msiexec.exe -ArgumentList "/i", $msi, "/quiet", "/norestart" -Wait
        Update-PathEnv
    }
    if (-not (Get-Command java -ErrorAction SilentlyContinue)) {
        Write-Host "  WARNING: java still not on PATH - open a fresh terminal or install Temurin manually" -ForegroundColor Yellow
    }
}

# Into the repo's tools\ dir. Caveat: the gateway's web server fails to bind
# port 5000 from OneDrive paths or paths containing '+' - the default
# C:\dev\TradeTerminal is clean; keep -RepoDir clean too.
Step "Client Portal Gateway (tools\clientportal.gw)"
$gwDir = Join-Path $RepoDir "tools\clientportal.gw"
if (Test-Path (Join-Path $gwDir "bin\run.bat")) {
    Write-Host "  already present"
} else {
    $zip = Join-Path $env:TEMP "clientportal.gw.zip"
    curl.exe -sL -o $zip https://download2.interactivebrokers.com/portal/clientportal.gw.zip
    Expand-Archive -Path $zip -DestinationPath $gwDir -Force
    Remove-Item $zip
    # Some zips wrap everything in a single top-level folder - flatten it.
    if (-not (Test-Path (Join-Path $gwDir "bin\run.bat"))) {
        $inner = Get-ChildItem $gwDir -Directory | Where-Object {
            Test-Path (Join-Path $_.FullName "bin\run.bat")
        } | Select-Object -First 1
        if ($inner) {
            Get-ChildItem $inner.FullName | Move-Item -Destination $gwDir
            Remove-Item $inner.FullName -Recurse -Force
        }
    }
    if (Test-Path (Join-Path $gwDir "bin\run.bat")) {
        Write-Host "  installed - TradeTerminal's Sign In dialog will auto-detect it"
    } else {
        Write-Host "  WARNING: extraction layout unexpected - see tools\README.md" -ForegroundColor Yellow
    }
}

# --- 2c. IBKR auto-login (headless login via IBeam) ---------------------------
Step "Auto-login deps: Chrome + IBeam (pip)"
# IBeam drives a headless Chrome to log the gateway in automatically. Chrome is
# required by Selenium; the matching chromedriver is fetched at runtime by
# Selenium Manager. Credentials are stored/consumed by scripts\Save-IbkrCred.ps1
# and scripts\Start-IbkrLogin.ps1 (DPAPI-encrypted, never in the repo).
$chromeExe = "C:\Program Files\Google\Chrome\Application\chrome.exe"
if (Test-Path $chromeExe) {
    Write-Host "  chrome present"
} elseif ($haveWinget) {
    Install-ViaWinget "Google.Chrome"
} else {
    $msi = Join-Path $env:TEMP "chrome64.msi"
    Get-File "https://dl.google.com/dl/chrome/install/googlechromestandaloneenterprise64.msi" $msi
    Start-Process msiexec.exe -ArgumentList "/i", $msi, "/quiet", "/norestart" -Wait
}
Update-PathEnv
if (Get-Command python -ErrorAction SilentlyContinue) {
    python -m pip install --upgrade pip | Out-Null
    python -m pip install --upgrade ibeam pyotp   # pyotp: authenticator (TOTP) 2FA
    if ($LASTEXITCODE -ne 0) { throw "pip install ibeam failed" }
    Write-Host "  IBeam installed. To enable auto-login on this box:"
    Write-Host "    1) scripts\Save-IbkrCred.ps1  (store IBKR credentials, once per account)"
    Write-Host "    2) launch TradeTerminal - the gateway + auto-login daemon start and"
    Write-Host "       stop with the app (no scheduled task needed)"
} else {
    Write-Host "  WARNING: python not on PATH - open a fresh terminal, then: python -m pip install ibeam pyotp" -ForegroundColor Yellow
}

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
                  "https://finnhub.io/api/v1/quote?symbol=AAPL",
                  "https://api.polygon.io/v3/reference/tickers?limit=1")) {
    $times = 1..5 | ForEach-Object {
        [double](curl.exe -o NUL -s -w "%{time_connect}" $ep)
    }
    $avg = ($times | Measure-Object -Average).Average
    Write-Host ("  {0}  avg connect {1:N1} ms" -f $ep, ($avg * 1000))
}

Step "Done"
Write-Host @"
Next steps (manual):
  1. scripts\Save-IbkrCred.ps1 - store IBKR credentials (DPAPI-encrypted; add
     a TOTP secret for a live account so 2FA completes headlessly).
  2. Launch: C:\dev\build\TradeTerminal\ucrt64-release\terminal\tt_terminal.exe
     (allow the Windows Firewall prompts on private networks only).
     The gateway + auto-login start with the app; the Account menu shows the
     session, and switching accounts lives in Account > Sign In.
  3. Data feed keys (optional): Data menu > Add Feed for Finnhub/Polygon.
  4. Set Windows Update active hours to cover the trading day.
  5. When leaving RDP, CLOSE the window (disconnect) - do not sign out.
  6. For remote monitoring, install Tailscale; do NOT open dashboard/RDP
     ports to the public internet.
"@
