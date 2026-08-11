<#
    Invoke-LineupDryRun.ps1 - run ONE full daily-lineup build headlessly and
    report what it produced, without a human at the GUI and without trading.

    This drives the terminal with TT_AUTORUN_LINEUP=1, which builds the day's
    lineup through the SAME code path the 09:35 schedule fires - IBKR scan,
    volatility rank, one strategy tournament per pick - and then exits. The run
    is PROPOSE-ONLY and the app forces that itself: no live session is started
    and no order is ever submitted, whatever config.json says about
    lineup_propose_only or the session schedule. See App::start_live_session.

    WHAT IT IS FOR. The lineup is the one production path that cannot be
    exercised without a data session, a market scanner and several minutes of
    optimizer time, so until now the only evidence about it was a morning's
    worth of prose in optimizer.log. The run writes machine-readable lines:

        dryrun: kind=phase phase=scan ms=1180 pool=30
        dryrun: kind=tournament symbol=TQQQ idx=2/6 ms=48210 candidates=4/5 outcome=fitted
        dryrun: kind=summary result=PASS total_ms=345678 ... exit=0

    Field 0 is always the tag and every other field is key=value, so
    `Select-String '^dryrun:'` (or grep) is the whole extraction contract.

    EXIT CODES: 0 the build fitted at least one symbol; 1 it fitted NONE (the
    2026-08-10 failure, which looks like a successful build from the outside -
    it logs picks, runs every tournament and reaches admission); 2 the run was
    refused before it started or did not finish inside -TimeoutMinutes; 3
    another tt_terminal is already running.

    IT MUTATES CONFIG THE WAY A PROPOSE-ONLY BUILD DOES. The picks land in the
    Trade tabs and are saved to config.json on exit - exactly what a
    propose-only 09:35 build leaves behind. Point -DataDir somewhere disposable
    if the run must not touch the production config.
#>
[CmdletBinding()]
param(
    # Release exe. Default = the ucrt64-release preset's output for this repo.
    [string]$Exe = "C:\dev\build\TradeTerminal\ucrt64-release\terminal\tt_terminal.exe",
    # Where the run's transcript goes. The file is overwritten, not appended:
    # this is the record of ONE run, and a scheduled job that appends turns the
    # summary line into a needle in its own haystack.
    [string]$LogFile = (Join-Path $env:LOCALAPPDATA 'TradeTerminal\logs\lineup-dryrun.log'),
    # Hard wall. A full build is minutes (the 2026-08-10 one was 1543 s), and the
    # app has its own 5-minute give-up when no data session ever comes up, so 20
    # covers a slow-but-healthy build with room to spare.
    [int]$TimeoutMinutes = 20,
    # Overrides %LOCALAPPDATA% for the child only, so a run can be pointed at a
    # throwaway config/journal instead of the production one.
    [string]$DataDir = ""
)
$ErrorActionPreference = "Stop"

if (-not (Test-Path $Exe)) {
    Write-Error "tt_terminal not found at $Exe - build the release preset first."
    exit 2
}

# --- refuse to run beside a live terminal ---------------------------------------
# Not politeness: the app holds a single-instance mutex and the two would fight
# over the TWS API client ids, so the second one exits immediately with a message
# box nobody sees. Worse, the FIRST one may be the production instance holding a
# live session - a dry run must never be the reason it is disturbed.
$existing = @(Get-Process -Name 'tt_terminal' -ErrorAction SilentlyContinue)
if ($existing.Count -gt 0) {
    "REFUSING to start: tt_terminal is already running (pid $($existing.Id -join ', '))."
    "Stop it first - this run needs the single-instance mutex and the API client ids."
    exit 3
}

New-Item -ItemType Directory -Force -Path (Split-Path $LogFile) | Out-Null

# --- launch ---------------------------------------------------------------------
# The exe is built WIN32_EXECUTABLE (no console), so its stdout only goes
# anywhere when the parent redirects it - which is exactly what
# -RedirectStandardOutput does. TT_LOG_STDOUT=1 mirrors the log console into it.
#
# Environment is set on THIS process and inherited: Start-Process has no
# per-child environment parameter, and they are restored in the finally below so
# an in-process caller (`& $script`) is not left with them set.
$prevLineup = $env:TT_AUTORUN_LINEUP
$prevStdout = $env:TT_LOG_STDOUT
$prevAppData = $env:LOCALAPPDATA
$errFile = "$LogFile.err"
$proc = $null
try {
    $env:TT_AUTORUN_LINEUP = "1"
    $env:TT_LOG_STDOUT = "1"
    if ($DataDir) {
        New-Item -ItemType Directory -Force -Path $DataDir | Out-Null
        $env:LOCALAPPDATA = $DataDir
    }

    "$([DateTime]::Now.ToString('yyyy-MM-dd HH:mm:ss')) starting lineup dry run: $Exe"
    "  log:     $LogFile"
    "  timeout: $TimeoutMinutes min"
    $proc = Start-Process -FilePath $Exe -PassThru `
                          -RedirectStandardOutput $LogFile `
                          -RedirectStandardError $errFile
    # Touch .Handle BEFORE the child can exit. Without this, PowerShell releases
    # the process handle and $proc.ExitCode reads back $null once the child is
    # gone - measured 2026-08-11, where a run whose terminal exited 1 was
    # reported as "exit=" and this script returned 0. A dry run that cannot
    # report its own failure is worse than no dry run, because a scheduled job
    # then goes green on a lineup that produced nothing.
    $null = $proc.Handle

    # --- wait, with a hard wall -------------------------------------------------
    # WaitForExit on the .NET object, NOT Wait-Process: Wait-Process throws
    # "cannot find a process with the process identifier" when the child has
    # already exited, which is indistinguishable from its timeout exception - so
    # a build that finished fast would be reported, and killed, as a hang. This
    # returns $false only for a real timeout, and returns the instant the child
    # exits so a fast build is not rounded up to a poll interval.
    $timedOut = -not $proc.WaitForExit($TimeoutMinutes * 60 * 1000)

    if ($timedOut) {
        "TIMED OUT after $TimeoutMinutes minutes - killing pid $($proc.Id)."
        # The app's own shutdown watchdog force-exits after 8s, but that only
        # helps once it has decided to quit. A wedged build never does, and a
        # leftover window-less tt_terminal.exe holds the single-instance mutex
        # and blocks every future launch - the documented auto-restart failure.
        # So: kill, then PROVE it is gone.
        Stop-Process -Id $proc.Id -Force -Confirm:$false -ErrorAction SilentlyContinue
    }
} finally {
    # Leave no stray process behind, on ANY exit path from the block above -
    # including Ctrl-C and an exception between launch and wait.
    if ($proc) {
        $still = Get-Process -Id $proc.Id -ErrorAction SilentlyContinue
        if ($still) {
            Stop-Process -Id $proc.Id -Force -Confirm:$false -ErrorAction SilentlyContinue
            Start-Sleep -Seconds 2
        }
    }
    $env:TT_AUTORUN_LINEUP = $prevLineup
    $env:TT_LOG_STDOUT = $prevStdout
    $env:LOCALAPPDATA = $prevAppData
}

# Belt and braces: nothing named tt_terminal may survive this script. A process
# that outlives it is not a cosmetic leak - it is the mutex that stops the
# terminal being started again.
$leftover = @(Get-Process -Name 'tt_terminal' -ErrorAction SilentlyContinue)
foreach ($p in $leftover) {
    "killing leftover tt_terminal pid $($p.Id)"
    Stop-Process -Id $p.Id -Force -Confirm:$false -ErrorAction SilentlyContinue
}

# stderr is normally empty (GLFW init failures land there); keep it only if used.
if ((Test-Path $errFile) -and (Get-Item $errFile).Length -eq 0) {
    Remove-Item $errFile -Force -ErrorAction SilentlyContinue
}

# --- report ---------------------------------------------------------------------
# 2 unless the child genuinely reported a status. An unreadable exit code must
# never fall through as 0: the whole point of this script is that a scheduled
# job can trust its status, so "I don't know" has to be a failure, loudly.
$exit = 2
if ($proc -and $proc.HasExited) {
    $code = $proc.ExitCode
    if ($null -ne $code) { $exit = [int]$code }
    else { "WARNING: the child's exit code could not be read - reporting 2." }
}
""
$report = @(Select-String -Path $LogFile -Pattern '^dryrun:' -ErrorAction SilentlyContinue |
            ForEach-Object { $_.Line })
if ($report.Count) { $report } else { "(no 'dryrun:' lines in $LogFile - the run produced no report)" }
""
"$([DateTime]::Now.ToString('yyyy-MM-dd HH:mm:ss')) dry run finished, exit=$exit"
exit $exit
