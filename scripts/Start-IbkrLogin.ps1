<#
    Start-IbkrLogin.ps1 - headless IBKR paper login via IBeam.

    Reads the DPAPI-encrypted credentials saved by Save-IbkrCred.ps1, decrypts
    them in-memory, and hands them to IBeam through the child process's
    environment (never written to disk in plaintext). Starts the Client Portal
    Gateway if it isn't already running, then has IBeam drive a headless Chrome
    login against it - one step for "launch gateway + sign in".

    Prereq: run Save-IbkrCred.ps1 once before first use.

    Params:
      (none)      single login attempt, then exit (used by the app button)
      -Daemon     stay logged in: log in now, then keep the session alive via
                  IBeam's maintenance loop; relaunch IBeam if it ever exits.
      -Maintain   IBeam's bare maintenance loop (-m), no immediate first login
#>
[CmdletBinding()]
param(
    [switch]$Daemon,
    [switch]$Maintain,
    [string]$Account    # log in with this saved account label (else the active one)
)
Add-Type -AssemblyName System.Security

$dir   = Join-Path $env:LOCALAPPDATA 'TradeTerminal'
$store = Join-Path $dir 'ibkr-accounts.json'

function Unprotect-Str([string]$hex) {
    $bytes = [byte[]]::new($hex.Length / 2)
    for ($i = 0; $i -lt $bytes.Length; $i++) {
        $bytes[$i] = [Convert]::ToByte($hex.Substring($i * 2, 2), 16)
    }
    $dec = [Security.Cryptography.ProtectedData]::Unprotect($bytes, $null, 'CurrentUser')
    [Text.Encoding]::UTF8.GetString($dec)
}

# One-time migration of the old single-account file into the multi-account store.
$legacy = Join-Path $dir 'ibkr.cred'
if (-not (Test-Path $store) -and (Test-Path $legacy)) {
    $l  = Get-Content $legacy -Raw | ConvertFrom-Json
    $nm = Unprotect-Str $l.user
    $mig = [pscustomobject]@{ active = $nm; accounts = @([pscustomobject]@{ name = $nm; user = $l.user; pass = $l.pass; paper = $true }) }
    ($mig | ConvertTo-Json -Depth 6) | Set-Content -Path $store -Encoding ASCII
}

if (-not (Test-Path $store)) {
    Write-Error "No saved IBKR accounts. Run scripts\Save-IbkrCred.ps1 first."
    exit 1
}

$store_obj = Get-Content $store -Raw | ConvertFrom-Json
if ($Account) {
    $store_obj.active = $Account
    ($store_obj | ConvertTo-Json -Depth 6) | Set-Content -Path $store -Encoding ASCII
}
$active = $store_obj.active
$o = @($store_obj.accounts) | Where-Object { $_.name -eq $active } | Select-Object -First 1
if (-not $o) {
    Write-Error "Active IBKR account '$active' not found in $store."
    exit 1
}

# Resolve a concrete python.exe. The bare Microsoft Store 'python' alias does not
# resolve under Task Scheduler's session, so cache the real interpreter path
# (sys._base_executable) for the unattended daemon to reuse.
$pyCache = Join-Path $dir 'python-path.txt'
$Py = $null
if (Test-Path $pyCache) { $Py = (Get-Content $pyCache -Raw).Trim() }
if (-not $Py -or -not (Test-Path $Py)) {
    try { $Py = (& python -c "import sys;print(sys._base_executable)" 2>$null).Trim() } catch { }
    if ($Py -and (Test-Path $Py)) { Set-Content -Path $pyCache -Value $Py -Encoding ASCII }
}
if (-not $Py -or -not (Test-Path $Py)) { $Py = 'python' }

# Resolve IBeam + a Chrome-matched chromedriver dynamically (survives Chrome updates).
$ibeamDir = & $Py -c "import ibeam,os;print(os.path.dirname(ibeam.__file__))"
$starter  = Join-Path $ibeamDir 'ibeam_starter.py'
$selDir   = & $Py -c "import selenium,os;print(os.path.dirname(selenium.__file__))"
$mgr      = Join-Path $selDir 'webdriver\common\windows\selenium-manager.exe'
$driver   = (& $mgr --browser chrome --output json | ConvertFrom-Json).result.driver_path

# Default to paper when the field is absent (older stored accounts / migration).
$isPaper = $true
if ($o.PSObject.Properties.Name -contains 'paper') { $isPaper = [bool]$o.paper }

$env:IBEAM_ACCOUNT            = Unprotect-Str $o.user
$env:IBEAM_PASSWORD           = Unprotect-Str $o.pass
$env:IBEAM_USE_PAPER_ACCOUNT  = if ($isPaper) { 'True' } else { 'False' }   # Paper toggle on the login page
$env:IBEAM_CHROME_DRIVER_PATH = $driver
$env:IBEAM_GATEWAY_BASE_URL   = 'https://localhost:5000'
$env:IBEAM_GATEWAY_DIR        = (Join-Path (Split-Path $PSScriptRoot -Parent) 'tools\clientportal.gw')
$env:IBEAM_INPUTS_DIR         = (Join-Path $dir 'ibeam\inputs')
$env:IBEAM_OUTPUTS_DIR        = (Join-Path $dir 'ibeam\outputs')
New-Item -ItemType Directory -Force -Path $env:IBEAM_INPUTS_DIR, $env:IBEAM_OUTPUTS_DIR | Out-Null

# Authenticator (TOTP) 2FA: if this account stored a secret, let IBeam generate
# the code itself (pyotp) so a live login completes headlessly.
if (($o.PSObject.Properties.Name -contains 'totp') -and $o.totp) {
    $env:IBEAM_TWO_FA_HANDLER = 'PYOTP'
    $env:IBEAM_PYOTP_SECRET   = Unprotect-Str $o.totp
    # IBKR's current 2FA field is id 'xyz-field-silver-response' (they renamed it
    # from IBeam's default 'bronze'); use the ID so IBeam both detects the 2FA
    # step and types the generated TOTP code into it.
    $env:IBEAM_TWO_FA_EL_ID       = 'ID@@xyz-field-silver-response'
    $env:IBEAM_TWO_FA_INPUT_EL_ID = 'ID@@xyz-field-silver-response'
} else {
    $env:IBEAM_TWO_FA_HANDLER     = $null
    $env:IBEAM_PYOTP_SECRET       = $null
    $env:IBEAM_TWO_FA_EL_ID       = $null
    $env:IBEAM_TWO_FA_INPUT_EL_ID = $null
}

$daemonLog = Join-Path $env:IBEAM_OUTPUTS_DIR 'daemon.log'
function Log([string]$m) { "[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $m | Add-Content -Path $daemonLog }
Log "launcher start: Daemon=$Daemon Maintain=$Maintain Py=$Py starter=$starter driver=$driver"

function Test-Gateway {
    try {
        $c = [Net.Sockets.TcpClient]::new()
        $c.Connect('127.0.0.1', 5000); $c.Close(); return $true
    } catch { return $false }
}

function Start-GatewayIfNeeded {
    if (Test-Gateway) { return }
    $gwDir = $env:IBEAM_GATEWAY_DIR
    Write-Host "Client Portal Gateway not running - starting it (hidden)..."
    Start-Process -FilePath 'cmd.exe' -ArgumentList '/c', 'bin\run.bat', 'root\conf.yaml' -WorkingDirectory $gwDir -WindowStyle Hidden
    $deadline = (Get-Date).AddSeconds(40)
    while ((Get-Date) -lt $deadline -and -not (Test-Gateway)) { Start-Sleep -Milliseconds 500 }
    if (-not (Test-Gateway)) { Write-Warning "Gateway did not come up on :5000 within 40s." }
}

if ($Daemon) {
    # Stay logged in: log in now + maintain, and resurrect IBeam if it ever exits
    # (e.g. it self-shuts-down after a burst of failures during an IBKR outage).
    while ($true) {
        Start-GatewayIfNeeded
        Log "launching IBeam (default mode)"
        & $Py $starter                  # default mode: start+authenticate, then maintenance loop (IBeam logs to its own file)
        Log "IBeam exited (code $LASTEXITCODE); relaunching in 60s"
        Start-Sleep -Seconds 60
    }
} else {
    Start-GatewayIfNeeded
    $ibeamArgs = if ($Maintain) { @($starter, '-m') } else { @($starter, '-a') }
    try {
        & $Py @ibeamArgs
    } finally {
        # Scrub secrets from this process's environment.
        $env:IBEAM_ACCOUNT      = $null
        $env:IBEAM_PASSWORD     = $null
        $env:IBEAM_PYOTP_SECRET = $null
    }
}
