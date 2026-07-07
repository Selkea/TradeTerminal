<#
    Install-IbkrAutoLogin.ps1 - keep the IBKR paper session logged in by default.

    Registers a Windows Scheduled Task that runs at your logon and launches
    scripts\Start-IbkrLogin.ps1 -Daemon: it starts the Client Portal Gateway if
    needed, signs in headlessly (IBeam), and keeps the session alive across
    IBKR's periodic session drops. The task runs as YOU with an interactive
    logon so DPAPI can decrypt the stored password and Chrome can run.

    Prereq: run Save-IbkrCred.ps1 once first.

    Usage:
      Install-IbkrAutoLogin.ps1              install, and start it now
      Install-IbkrAutoLogin.ps1 -Uninstall   remove the task
#>
[CmdletBinding()]
param([switch]$Uninstall)

$TaskName = 'TradeTerminal IBKR AutoLogin'

if ($Uninstall) {
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue
    Write-Host "Removed scheduled task: $TaskName"
    return
}

$script = Join-Path $PSScriptRoot 'Start-IbkrLogin.ps1'
if (-not (Test-Path $script)) { Write-Error "Missing $script"; exit 1 }

$cred = Join-Path $env:LOCALAPPDATA 'TradeTerminal\ibkr.cred'
if (-not (Test-Path $cred)) {
    Write-Warning "No saved credentials yet. Run Save-IbkrCred.ps1 first, or the task cannot log in."
}

# The bare Store 'python' alias won't resolve under Task Scheduler; cache the
# concrete interpreter path here (interactively) for the daemon to reuse.
$pyCache = Join-Path $env:LOCALAPPDATA 'TradeTerminal\python-path.txt'
$py = ''
try { $py = (& python -c "import sys;print(sys._base_executable)").Trim() } catch { }
if ($py -and (Test-Path $py)) {
    Set-Content -Path $pyCache -Value $py -Encoding ASCII
    Write-Host "Resolved python: $py"
} else {
    Write-Warning "Could not resolve a concrete python path; the task may not find python."
}

$user = "$env:USERDOMAIN\$env:USERNAME"
$arg  = '-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File "' + $script + '" -Daemon'

$action    = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument $arg
$trigger   = New-ScheduledTaskTrigger -AtLogOn -User $user
$settings  = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable -MultipleInstances IgnoreNew -ExecutionTimeLimit ([TimeSpan]::Zero) -RestartCount 99 -RestartInterval (New-TimeSpan -Minutes 2)
$principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited

Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $trigger -Settings $settings -Principal $principal -Force | Out-Null
Write-Host "Registered '$TaskName' - runs automatically at every logon."

Start-ScheduledTask -TaskName $TaskName
Write-Host "Started now. The IBKR session will stay logged in from here on."
Write-Host "To remove: Install-IbkrAutoLogin.ps1 -Uninstall"
