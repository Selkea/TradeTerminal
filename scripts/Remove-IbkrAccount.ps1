<#
    Remove-IbkrAccount.ps1 -Account <label>

    Delete a saved IBKR account from ibkr-accounts.json. If it is the account
    that is currently active/signed in, the session is signed out first (the
    daemon is stopped and the gateway logged out) and no account is left active.
#>
[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$Account)

$dir   = Join-Path $env:LOCALAPPDATA 'TradeTerminal'
$store = Join-Path $dir 'ibkr-accounts.json'
if (-not (Test-Path $store)) { Write-Error "No IBKR accounts saved."; exit 1 }

$o = Get-Content $store -Raw | ConvertFrom-Json
if (-not (@($o.accounts) | Where-Object { $_.name -eq $Account })) {
    Write-Error "IBKR account '$Account' not found."
    exit 1
}

# Signing account removed => sign the session out first.
if ($o.active -eq $Account) {
    & (Join-Path $PSScriptRoot 'Stop-IbkrLogin.ps1')
    $o.active = ''
}

$o.accounts = @($o.accounts | Where-Object { $_.name -ne $Account })
($o | ConvertTo-Json -Depth 6) | Set-Content -Path $store -Encoding ASCII
Write-Host "Removed IBKR account '$Account'."
