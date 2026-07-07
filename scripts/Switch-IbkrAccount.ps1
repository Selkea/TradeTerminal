<#
    Switch-IbkrAccount.ps1 -Account <label>

    Switch the signed-in IBKR account (also used for the first sign-in). It signs
    the current session out first, makes <label> the active account, then starts
    the auto-login for it: via the scheduled-task daemon if installed (so it
    stays logged in), otherwise a one-off headless login.
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

# 1. Sign the current session out (stops the daemon + logs the gateway out).
& (Join-Path $PSScriptRoot 'Stop-IbkrLogin.ps1')

# 1b. Restart the gateway. /logout only drops the brokerage session; the gateway
#     keeps the previous account's SSO cookies in memory and would just
#     re-authenticate THAT account. Killing it clears the cookie jar so the new
#     daemon starts a fresh gateway and does a real browser login as $Account.
Get-CimInstance Win32_Process |
    Where-Object { $_.CommandLine -match 'clientportal\.gw\.GatewayStart' } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
Start-Sleep -Seconds 2

# 2. Make the chosen account active.
$o.active = $Account
($o | ConvertTo-Json -Depth 6) | Set-Content -Path $store -Encoding ASCII

# 3. Start auto-login for the new active account.
$TaskName = 'TradeTerminal IBKR AutoLogin'
if (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue) {
    Start-ScheduledTask -TaskName $TaskName
} else {
    $starter = Join-Path $PSScriptRoot 'Start-IbkrLogin.ps1'
    Start-Process 'powershell.exe' -WindowStyle Hidden -ArgumentList @(
        '-NoProfile', '-WindowStyle', 'Hidden', '-ExecutionPolicy', 'Bypass',
        '-File', "`"$starter`"", '-Daemon')
}
Write-Host "Switched IBKR account to '$Account'."
