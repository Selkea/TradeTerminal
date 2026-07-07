<#
    Stop-IbkrLogin.ps1 - sign out of IBKR.

    Stops the auto-login daemon (so it stops re-authenticating) and logs the
    gateway brokerage session out. The scheduled task stays registered, so
    auto-login resumes at your next logon; to disable it for good, run
    Install-IbkrAutoLogin.ps1 -Uninstall.

    The gateway process itself is left running (just logged out), matching a
    normal browser sign-out.
#>

$TaskName = 'TradeTerminal IBKR AutoLogin'

# 1. Stop the maintenance daemon so nothing re-logs-in behind us.
Stop-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" |
    Where-Object { $_.CommandLine -match '-File.*Start-IbkrLogin' } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
Get-CimInstance Win32_Process |
    Where-Object { $_.Name -match 'python' -and $_.CommandLine -match 'ibeam' } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }

# 2. Log the brokerage session out. Use HttpWebRequest (built into PS 5.1, no
#    Add-Type, no console child window when run hidden). The gateway uses a
#    self-signed cert and rejects requests that send no User-Agent.
try {
    [Net.ServicePointManager]::ServerCertificateValidationCallback = { $true }
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    $req = [Net.HttpWebRequest]::Create('https://localhost:5000/v1/api/logout')
    $req.Method = 'POST'
    $req.UserAgent = 'Mozilla/5.0'
    $req.ContentLength = 0
    $req.Timeout = 10000
    $req.GetResponse().Close()
} catch { }

Write-Host "Signed out of IBKR. Auto-login resumes at next logon (Install-IbkrAutoLogin.ps1 -Uninstall to disable)."
