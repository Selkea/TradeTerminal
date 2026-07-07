<#
    Stop-IbkrSession.ps1 - full stop for app shutdown.

    Kills the auto-login daemon, any IBeam processes, and the Client Portal
    Gateway itself. Used when the gateway/daemon are tied to the terminal app's
    lifetime, so nothing is left running after the app closes.
#>

# Stop a leftover scheduled-task daemon, if one was ever installed.
Stop-ScheduledTask -TaskName 'TradeTerminal IBKR AutoLogin' -ErrorAction SilentlyContinue

# Daemon supervisor (the -Daemon PowerShell loop).
Get-CimInstance Win32_Process |
    Where-Object { $_.CommandLine -match '-File.*Start-IbkrLogin' } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }

# IBeam (headless login / maintenance).
Get-CimInstance Win32_Process |
    Where-Object { $_.Name -match 'python' -and $_.CommandLine -match 'ibeam' } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }

# The gateway itself (its JVM holds the SSO session).
Get-CimInstance Win32_Process |
    Where-Object { $_.CommandLine -match 'clientportal\.gw\.GatewayStart' } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
