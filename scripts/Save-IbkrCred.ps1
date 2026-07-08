<#
    Save-IbkrCred.ps1 - add (or update) an IBKR account for auto-login.

    Prompts for the username, mode, a display name, and the password. The
    password is entered here, locally, and encrypted with Windows DPAPI
    (CurrentUser scope) - bound to this Windows user + machine, like
    TradeTerminal's other secrets. Accounts are stored in
    %LOCALAPPDATA%\TradeTerminal\ibkr-accounts.json. Re-run any time to add
    another account or update one.

    The store's unique id is derived automatically: the username, with a
    -paper/-live suffix only when the same username is saved in the other mode
    (IBKR lets one login run either mode). -Id overrides it for custom keys.
#>
param(
    [string]$Id   # optional: custom unique id (default: derived from username)
)
Add-Type -AssemblyName System.Security

$dir = Join-Path $env:LOCALAPPDATA 'TradeTerminal'
New-Item -ItemType Directory -Force -Path $dir | Out-Null
$store = Join-Path $dir 'ibkr-accounts.json'

$user = Read-Host 'IBKR username'
if ([string]::IsNullOrWhiteSpace($user)) { Write-Error 'Username is required.'; exit 1 }

$paperAns = Read-Host 'Paper account? (Y/n)'
$paper = -not ($paperAns -match '^[Nn]')   # default: paper

if (Test-Path $store) {
    $o = Get-Content $store -Raw | ConvertFrom-Json
} else {
    $o = [pscustomobject]@{ active = ''; accounts = @() }
}

# Unique id: the username, suffixed only if the same username already exists
# in the other mode (so paper + live entries of one login can coexist).
$name = $Id
if ([string]::IsNullOrWhiteSpace($name)) {
    $name = $user
    $clash = @($o.accounts) | Where-Object { $_.name -eq $name } | Select-Object -First 1
    if ($clash) {
        $clashPaper = $true
        if ($clash.PSObject.Properties.Name -contains 'paper') { $clashPaper = [bool]$clash.paper }
        if ($clashPaper -ne $paper) {
            $name = $user + $(if ($paper) { '-paper' } else { '-live' })
        }
    }
}
$existing = @($o.accounts) | Where-Object { $_.name -eq $name } | Select-Object -First 1
if ($existing) {
    $ow = Read-Host "Account '$name' already exists. Overwrite it? (y/N)"
    if ($ow -notmatch '^[Yy]') { Write-Host 'Aborted - nothing changed.'; exit 1 }
}

$labelDefault = $name
if ($existing -and ($existing.PSObject.Properties.Name -contains 'label') -and $existing.label) {
    $labelDefault = [string]$existing.label
}
$display = Read-Host "Display name [$labelDefault]"
if ([string]::IsNullOrWhiteSpace($display)) { $display = $labelDefault }

function Read-Plain([Security.SecureString]$s) {
    $b = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($s)
    try { [Runtime.InteropServices.Marshal]::PtrToStringBSTR($b) }
    finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($b) }
}

# Read-only only makes sense for live (paper money is not real). The TOTP
# prompt applies to BOTH modes: IBKR's Secure Login 2FA is per-username, so a
# 2FA-enrolled login gets the challenge even with the Paper toggle.
$readonly = $false
if (-not $paper) {
    $roAns = Read-Host 'Read-only (disable trading)? (Y/n)'
    $readonly = -not ($roAns -match '^[Nn]')   # default: read-only for live
}
Write-Host 'TOTP secret enables unattended login when the username has 2FA (base32; blank to skip).'
$totpSec = Read-Host 'Authenticator TOTP secret' -AsSecureString
$totpPlain = (Read-Plain $totpSec) -replace '\s', ''   # strip spaces/grouping

$plain = Read-Plain (Read-Host 'IBKR password' -AsSecureString)

function Protect-Str([string]$s) {
    $bytes = [Text.Encoding]::UTF8.GetBytes($s)
    $enc   = [Security.Cryptography.ProtectedData]::Protect($bytes, $null, 'CurrentUser')
    ($enc | ForEach-Object { $_.ToString('x2') }) -join ''
}

$totp = if ([string]::IsNullOrWhiteSpace($totpPlain)) { '' } else { Protect-Str $totpPlain }
$entry = [pscustomobject]@{ name = $name; label = $display; user = (Protect-Str $user); pass = (Protect-Str $plain); paper = $paper; readonly = $readonly; totp = $totp }
$plain = $null
$totpPlain = $null

# Upsert by unique id; keep the rest.
$list = @($o.accounts | Where-Object { $_.name -ne $name })
$list += $entry
$o.accounts = $list
if ([string]::IsNullOrWhiteSpace($o.active)) { $o.active = $name }

($o | ConvertTo-Json -Depth 6) | Set-Content -Path $store -Encoding ASCII
$kind = if ($paper) { 'paper' } elseif ($readonly) { 'LIVE, read-only' } else { 'LIVE, TRADING ENABLED' }
$twofa = if ($totp) { ', TOTP 2FA' } else { '' }
Write-Host "Saved IBKR account '$name' (user '$user', $kind$twofa) to $store" -ForegroundColor Green
