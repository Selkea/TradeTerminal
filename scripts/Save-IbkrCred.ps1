<#
    Save-IbkrCred.ps1 - add (or update) an IBKR account for auto-login.

    Prompts for an account label, username, and password. The password is
    entered here, locally, and encrypted with Windows DPAPI (CurrentUser scope)
    - bound to this Windows user + machine, like TradeTerminal's other secrets.
    Accounts are stored in %LOCALAPPDATA%\TradeTerminal\ibkr-accounts.json
    (usernames/passwords DPAPI-encrypted; only the label is plaintext, so the
    app can list accounts). Re-run any time to add another or update one.
#>
Add-Type -AssemblyName System.Security

$dir = Join-Path $env:LOCALAPPDATA 'TradeTerminal'
New-Item -ItemType Directory -Force -Path $dir | Out-Null
$store = Join-Path $dir 'ibkr-accounts.json'

$user = Read-Host 'IBKR username'
if ([string]::IsNullOrWhiteSpace($user)) { Write-Error 'Username is required.'; exit 1 }
$name = Read-Host "Account label [$user]"
if ([string]::IsNullOrWhiteSpace($name)) { $name = $user }

$paperAns = Read-Host 'Paper account? (Y/n)'
$paper = -not ($paperAns -match '^[Nn]')   # default: paper

function Read-Plain([Security.SecureString]$s) {
    $b = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($s)
    try { [Runtime.InteropServices.Marshal]::PtrToStringBSTR($b) }
    finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($b) }
}

# For a live account: offer read-only (view/test with no real-money orders) and
# an authenticator (TOTP) secret so the daemon can complete 2FA headlessly.
$readonly = $false
$totpPlain = ''
if (-not $paper) {
    $roAns = Read-Host 'Read-only (disable trading)? (Y/n)'
    $readonly = -not ($roAns -match '^[Nn]')   # default: read-only for live
    Write-Host 'TOTP secret enables unattended login (from IBKR "Mobile Authenticator" setup; base32).'
    $totpSec = Read-Host 'Authenticator TOTP secret (blank to skip)' -AsSecureString
    $totpPlain = (Read-Plain $totpSec) -replace '\s', ''   # strip spaces/grouping
}

$plain = Read-Plain (Read-Host 'IBKR password' -AsSecureString)

function Protect-Str([string]$s) {
    $bytes = [Text.Encoding]::UTF8.GetBytes($s)
    $enc   = [Security.Cryptography.ProtectedData]::Protect($bytes, $null, 'CurrentUser')
    ($enc | ForEach-Object { $_.ToString('x2') }) -join ''
}

if (Test-Path $store) {
    $o = Get-Content $store -Raw | ConvertFrom-Json
} else {
    $o = [pscustomobject]@{ active = ''; accounts = @() }
}

# Preserve an existing display label so a re-save keeps the shown name (lets a
# paper + live account share one label, e.g. "claudiagosselin"). Default to key.
$existing = @($o.accounts) | Where-Object { $_.name -eq $name } | Select-Object -First 1
$label = if ($existing -and $existing.label) { [string]$existing.label } else { $name }

$totp = if ([string]::IsNullOrWhiteSpace($totpPlain)) { '' } else { Protect-Str $totpPlain }
$entry = [pscustomobject]@{ name = $name; label = $label; user = (Protect-Str $user); pass = (Protect-Str $plain); paper = $paper; readonly = $readonly; totp = $totp }
$plain = $null
$totpPlain = $null

# Upsert by label; keep the rest.
$list = @($o.accounts | Where-Object { $_.name -ne $name })
$list += $entry
$o.accounts = $list
if ([string]::IsNullOrWhiteSpace($o.active)) { $o.active = $name }

($o | ConvertTo-Json -Depth 6) | Set-Content -Path $store -Encoding ASCII
$kind = if ($paper) { 'paper' } elseif ($readonly) { 'LIVE, read-only' } else { 'LIVE, TRADING ENABLED' }
$twofa = if ($totp) { ', TOTP 2FA' } else { '' }
Write-Host "Saved IBKR account '$name' (user '$user', $kind$twofa) to $store" -ForegroundColor Green
