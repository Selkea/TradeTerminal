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

    THIS SCRIPT IS THE ONLY PLACE A LOGIN IS EVER ATTEMPTED, and that is why
    the two safety mechanisms live here rather than in any one caller. There
    are three callers and they cannot see each other: the app on startup, the
    app's 08:45 pre-open check (v0.14.x), and Watch-IbGateway.ps1's port-down
    self-heal - which the app spawns itself, and which re-arms after every
    attempt for the whole life of the app. ANY relaunch takes the API port
    down, which is precisely what arms the keepalive, so an app-side "one
    attempt per day" stamp bounds nothing at all: the keepalive picks the retry
    up ~2 minutes later and repeats it for as long as the login keeps failing.
    IBKR LOCKS ACCOUNTS AFTER REPEATED FAILED LOGINS, and a locked account
    cannot be recovered from this machine.

      1. a cross-process launch MUTEX, so two callers can never both reach IBC.
         "Two gateways in a login fight over the same username" is a hazard
         this script already warned about but did not prevent: -Restart killed
         first and only then probed for an existing gateway, so two concurrent
         -Restart runs both sailed past the probe and both launched.
      2. a login-attempt GOVERNOR: a persisted rolling record of when logins
         were actually attempted, with a minimum gap and hourly/daily caps.
         A refusal leaves any running gateway UNTOUCHED - the kill happens
         after the decision, not before it. -Force overrides it for a human at
         the console, who can see what the gateway is actually doing.

    Exit codes: 0 launched or already up, 1 misconfigured, 3 refused (governor,
    or another launch already in flight).
#>
[CmdletBinding()]
param(
    [string]$Account,   # store id; default = the active account
    [switch]$Stop,
    [switch]$Restart,   # stop, then fall through into a fresh launch + login
    [switch]$Force      # bypass the login-attempt governor (human at the console)
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Security

function Stop-Gateway {
    Get-CimInstance Win32_Process -Filter "Name = 'java.exe'" |
        Where-Object { $_.CommandLine -match 'ibcalpha\.ibc|ibgateway' } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -Confirm:$false }
}

function Unprotect-Str([string]$hex) {
    $bytes = [byte[]]::new($hex.Length / 2)
    for ($i = 0; $i -lt $bytes.Length; $i++) {
        $bytes[$i] = [Convert]::ToByte($hex.Substring($i * 2, 2), 16)
    }
    $dec = [Security.Cryptography.ProtectedData]::Unprotect($bytes, $null, 'CurrentUser')
    [Text.Encoding]::UTF8.GetString($dec)
}

# --- login-attempt governor ----------------------------------------------------
# A rolling record of when a login was actually ATTEMPTED - not of when this
# script ran, since the idempotent "already up" exit spends nothing. Persisted
# to disk because the callers are separate processes, and one of them is
# restarted whenever the app is.
#
# MinGapSec must exceed BOTH this script's own 120s wait for the API port and
# the keepalive's port-down grace, or the two ping-pong: the keepalive sees the
# port still down during a legitimate cold start and kills the gateway mid-login
# to start another one. Keep it in step with Watch-IbGateway.ps1's DownGraceSec.
$AttemptFile = Join-Path $env:LOCALAPPDATA 'TradeTerminal\gateway-login-attempts.txt'
$MinGapSec = 300
$MaxPerHour = 4
$MaxPerDay = 12

function Get-LoginAttempts {
    # Unix seconds, one per line, pruned to the last 24h. Anything unreadable or
    # dated in the future is discarded: a governor that can WEDGE THE GATEWAY
    # SHUT is worse than no governor, so every failure in here fails OPEN.
    $nowSec = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
    if (-not (Test-Path $AttemptFile)) { return @() }
    try {
        return @(Get-Content $AttemptFile -ErrorAction Stop |
                 Where-Object { $_ -match '^\s*\d+\s*$' } |
                 ForEach-Object { [int64]$_.Trim() } |
                 Where-Object { $_ -le $nowSec -and ($nowSec - $_) -lt 86400 })
    } catch { return @() }
}

function Add-LoginAttempt([int64[]]$prior) {
    $nowSec = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
    try {
        New-Item -ItemType Directory -Force -Path (Split-Path $AttemptFile) | Out-Null
        (@($prior) + $nowSec | Sort-Object) -join "`r`n" |
            Set-Content -Path $AttemptFile -Encoding ASCII
    } catch { "warning: could not record the login attempt ($($_.Exception.Message))" }
}

# Stopping is always allowed, needs no account, and spends no login attempt.
# Only the LAUNCH paths go through the guards below.
if ($Stop -and -not $Restart) {
    Stop-Gateway
    "IB Gateway stopped."
    exit 0
}

# --- one launcher at a time ----------------------------------------------------
# Named mutex rather than a lock file: the OS hands the next waiter ownership
# via AbandonedMutexException if a holder dies mid-launch, instead of leaving a
# stale file that blocks every future start. Local\ (not Global\) - every caller
# runs as the same user in the same session.
#
# The release MUST be in a finally. Watch-IbGateway.ps1 calls this script
# IN-PROCESS (& $script), where `exit` returns from the script rather than
# ending the process - so without one, the keepalive would hold this mutex for
# its entire lifetime and no gateway could ever be launched again.
$launchMutex = New-Object System.Threading.Mutex($false, 'Local\TradeTerminal.IbGateway.Launch')
$haveLock = $false
try {
    $haveLock = $launchMutex.WaitOne(0)
} catch [System.Threading.AbandonedMutexException] {
    $haveLock = $true   # previous holder died mid-launch; ownership is ours
}
if (-not $haveLock) {
    "Another IB Gateway launch is already in progress - not starting a second."
    exit 3
}

try {
    # --- resolve account -------------------------------------------------------
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

    # Already running? Launching IBC twice would put two gateways in a login
    # fight over the same username. The app calls this script on every start
    # (TWS route), so this makes it idempotent. Costs no login attempt, hence it
    # is checked before the governor - and skipped for -Restart, whose entire
    # purpose is to replace a gateway that IS up.
    if (-not $Restart) {
        try {
            $probe = [Net.Sockets.TcpClient]::new()
            $probe.Connect('127.0.0.1', $port)
            $probe.Close()
            "IB Gateway already up (api port $port)."
            exit 0
        } catch {}
    }

    # --- may we spend a login attempt? -----------------------------------------
    # BEFORE the kill, deliberately. The old order killed the gateway first and
    # decided afterwards, so a refusal - or any failure further down - left the
    # machine with no gateway at all, strictly worse than the state being
    # recovered from.
    $attempts = Get-LoginAttempts
    $nowSec = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
    $lastAt = 0
    if ($attempts.Count) { $lastAt = ($attempts | Measure-Object -Maximum).Maximum }
    $inHour = @($attempts | Where-Object { ($nowSec - $_) -lt 3600 }).Count
    $refuse = $null
    if ($lastAt -gt 0 -and ($nowSec - $lastAt) -lt $MinGapSec) {
        $refuse = "the last login attempt was $($nowSec - $lastAt)s ago (minimum ${MinGapSec}s apart)"
    } elseif ($inHour -ge $MaxPerHour) {
        $refuse = "$inHour login attempts in the last hour (cap $MaxPerHour)"
    } elseif ($attempts.Count -ge $MaxPerDay) {
        $refuse = "$($attempts.Count) login attempts in the last 24h (cap $MaxPerDay)"
    }
    if ($refuse -and -not $Force) {
        "REFUSING to log in: $refuse."
        "IBKR locks accounts after repeated failed logins, and a locked account"
        "cannot be recovered from this machine. The gateway is untouched."
        "Log in via RDP, or re-run with -Force once you can see what it is doing."
        exit 3
    }
    if ($refuse) { "governor: $refuse - overridden by -Force." }

    if ($Restart) {
        Stop-Gateway
        "IB Gateway stopped."
        Start-Sleep -Seconds 2   # let the API port + settings files release
    }
    Add-LoginAttempt $attempts

    # --- locate IB Gateway + IBC -----------------------------------------------
    $gwVer = Get-ChildItem "C:\Jts\ibgateway" -Directory -ErrorAction SilentlyContinue |
             Where-Object { $_.Name -match '^\d+$' } |
             Sort-Object { [int]$_.Name } -Descending | Select-Object -First 1
    if (-not $gwVer) { Write-Error "IB Gateway not installed (no numeric version dir). Run Install-IbGateway.ps1."; exit 1 }
    if (-not (Test-Path "C:\IBC\IBC.jar")) { Write-Error "IBC not installed. Run Install-IbGateway.ps1."; exit 1 }

    # --- IBC config (no secrets) -----------------------------------------------
    # Written to IBC's DEFAULT location: IBC 3.24's launcher ignores a custom
    # IBC_INI env var and resolves %USERPROFILE%\Documents\IBC\config.ini itself.
    $ibcCfgDir = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'IBC'
    New-Item -ItemType Directory -Force -Path $ibcCfgDir | Out-Null
    $ini = Join-Path $ibcCfgDir 'config.ini'
    # Nightly restart mode. AutoRestartTime = the gateway's OWN soft restart: it
    # reuses the session (no re-auth) and survives ~a week - but that reused
    # session can come back STALE, leaving the gateway up (API port open) yet
    # unable to reconnect to IBKR, rejecting API clients ("Client disconnected
    # before version was sent") until a full COLD relaunch. ColdRestartTime =
    # IBC kills + relaunches with a fresh login, which never lands in that stale
    # state. Paper never prompts 2FA, so cold is safe + strictly better here. A
    # LIVE account MAY require a TOTP on the cold re-login (IBC can't type it)
    # and would wedge on the 2FA dialog nightly, so keep the soft restart there.
    # See [[tt-gateway-overnight-reauth]]. That asymmetry is also why the app's
    # 08:45 pre-open check only ever issues -Restart for a PAPER account: a cold
    # re-login it cannot complete would leave no login at all, 40 minutes before
    # the open, fixable only by RDP.
    # 24-HOUR FORMAT, NOT '11:55 PM'. IBC 3.24.1 silently drops the AM/PM suffix:
    # with 'ColdRestartTime=11:55 PM' its own log said "Gateway will be cold
    # restarted at 2026/08/02 11:55" and it duly restarted at 11:55 *AM* on
    # 2026-08-02 -- midday, i.e. mid-session on any weekday. (It was a Sunday, and
    # the collateral damage was the data session wedging on the resulting socket
    # close: see [[tt-ereader-disconnect-deadlock]].)
    $restartLine = if ($isPaper) { 'ColdRestartTime=23:55' } else { 'AutoRestartTime=23:55' }
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
$restartLine
MinimizeMainWindow=no
"@ | Set-Content -Path $ini -Encoding ASCII
    # MinimizeMainWindow stays 'no': dialogs (paper-trading disclaimer, 2FA) are
    # modal children of the main window and get lost when it is minimized.

    # --- launch via IBC's documented wrapper interface --------------------------
    $logDir = Join-Path $env:LOCALAPPDATA 'TradeTerminal\ibc\logs'
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

    # Hidden: this is only IBC's console (banner + log mirror; the real logs land
    # in $logDir). Hiding it also makes it impossible to kill the gateway by
    # closing a stray console window. The gateway GUI itself still shows.
    Start-Process -FilePath (Join-Path $env:IBC_PATH 'scripts\DisplayBannerAndLaunch.bat') `
        -WindowStyle Hidden
    Start-Sleep -Seconds 2
    $env:TWSUSERID = $null
    $env:TWSPASSWORD = $null

    "IB Gateway launching as '$name' ($mode, api port $port)."
    "If the security-code dialog appears, type the authenticator code (RDP)."
    # The mutex is held across this wait ON PURPOSE: until the port is up, a
    # second caller launching IBC would be the login fight described at the top.
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
} finally {
    if ($haveLock) {
        try { $launchMutex.ReleaseMutex() } catch {}
    }
    $launchMutex.Dispose()
}
