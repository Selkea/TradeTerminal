<#
    Start-IbGateway.ps1 - launch IB Gateway via IBC, logged in as the active
    saved account (or -Account <id>). -Stop tears it down.

    Credentials are decrypted from the DPAPI store and handed to IBC through
    the launcher's environment (TWSUSERID/TWSPASSWORD) - never written to
    disk. The IBC config.ini holds only non-secret behavior settings.

    2FA: IBC cannot generate TOTP codes. On a 2FA-enrolled login the gateway
    shows the security-code dialog - type the authenticator code via RDP.
    With auto-restart enabled the session survives daily restarts for ~a week
    before the next code entry.
#>
[CmdletBinding()]
param(
    [string]$Account,   # store id; default = the active account
    [switch]$Stop
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Security

if ($Stop) {
    Get-CimInstance Win32_Process -Filter "Name = 'java.exe'" |
        Where-Object { $_.CommandLine -match 'ibcalpha\.ibc|ibgateway' } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -Confirm:$false }
    "IB Gateway stopped."
    exit 0
}

function Unprotect-Str([string]$hex) {
    $bytes = [byte[]]::new($hex.Length / 2)
    for ($i = 0; $i -lt $bytes.Length; $i++) {
        $bytes[$i] = [Convert]::ToByte($hex.Substring($i * 2, 2), 16)
    }
    $dec = [Security.Cryptography.ProtectedData]::Unprotect($bytes, $null, 'CurrentUser')
    [Text.Encoding]::UTF8.GetString($dec)
}

# --- resolve account -----------------------------------------------------------
$store = Join-Path $env:LOCALAPPDATA 'TradeTerminal\ibkr-accounts.json'
if (-not (Test-Path $store)) { Write-Error "No saved accounts. Run Save-IbkrCred.ps1 first."; exit 1 }
$o = Get-Content $store -Raw | ConvertFrom-Json
$name = if ($Account) { $Account } else { $o.active }
$acc = @($o.accounts) | Where-Object { $_.name -eq $name } | Select-Object -First 1
if (-not $acc) { Write-Error "Account '$name' not found."; exit 1 }
$isPaper = $true
if ($acc.PSObject.Properties.Name -contains 'paper') { $isPaper = [bool]$acc.paper }
$mode = if ($isPaper) { 'paper' } else { 'live' }
$port = if ($isPaper) { 4002 } else { 4001 }

# --- locate IB Gateway + IBC ---------------------------------------------------
$gwVer = Get-ChildItem "C:\Jts\ibgateway" -Directory -ErrorAction SilentlyContinue |
         Where-Object { $_.Name -match '^\d+$' } |
         Sort-Object { [int]$_.Name } -Descending | Select-Object -First 1
if (-not $gwVer) { Write-Error "IB Gateway not installed (no numeric version dir). Run Install-IbGateway.ps1."; exit 1 }
if (-not (Test-Path "C:\IBC\IBC.jar")) { Write-Error "IBC not installed. Run Install-IbGateway.ps1."; exit 1 }

# --- IBC config (no secrets) ---------------------------------------------------
$ibcCfgDir = Join-Path $env:LOCALAPPDATA 'TradeTerminal\ibc'
New-Item -ItemType Directory -Force -Path $ibcCfgDir | Out-Null
$ini = Join-Path $ibcCfgDir 'config.ini'
@"
FIX=no
TradingMode=$mode
AcceptIncomingConnectionAction=accept
AcceptNonBrokerageAccountWarning=yes
ReadOnlyApi=no
OverrideTwsApiPort=$port
ExistingSessionDetectedAction=primary
ReloginAfterSecondFactorAuthenticationTimeout=yes
SecondFactorAuthenticationExitInterval=60
AutoRestartTime=11:55 PM
MinimizeMainWindow=yes
"@ | Set-Content -Path $ini -Encoding ASCII

# --- launch via IBC's documented wrapper interface -----------------------------
$logDir = Join-Path $ibcCfgDir 'logs'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$env:TWS_MAJOR_VRSN = $gwVer.Name
$env:IBC_INI = $ini
$env:TRADING_MODE = $mode
$env:IBC_PATH = 'C:\IBC'
$env:TWS_PATH = 'C:\Jts'
$env:TWS_SETTINGS_PATH = ''
$env:LOG_PATH = $logDir
$env:TWSUSERID = Unprotect-Str $acc.user
$env:TWSPASSWORD = Unprotect-Str $acc.pass
$env:FIXUSERID = ''
$env:FIXPASSWORD = ''
$env:JAVA_PATH = ''
$env:APP = 'GATEWAY'

Start-Process -FilePath (Join-Path $env:IBC_PATH 'scripts\DisplayBannerAndLaunch.bat') `
    -WindowStyle Minimized
Start-Sleep -Seconds 2
$env:TWSUSERID = $null
$env:TWSPASSWORD = $null

"IB Gateway launching as '$name' ($mode, api port $port)."
"If the security-code dialog appears, type the authenticator code (RDP)."
$deadline = (Get-Date).AddSeconds(120)
while ((Get-Date) -lt $deadline) {
    try {
        $c = [Net.Sockets.TcpClient]::new()
        $c.Connect('127.0.0.1', $port); $c.Close()
        "API port $port is up."
        exit 0
    } catch { Start-Sleep -Seconds 3 }
}
"API port $port not up after 120s - check the gateway window (2FA prompt?) and $logDir."
