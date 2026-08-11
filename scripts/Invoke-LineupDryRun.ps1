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
    refused before it started, did not finish inside -TimeoutMinutes, or ended
    in a way this script could not read a status from; 3 another tt_terminal is
    already running.

    IT KILLS ONLY THE PROCESS IT STARTED. If any other tt_terminal exists it
    refuses (3) and touches nothing. Until 0.20.0 the cleanup ended with a sweep
    that killed every process NAMED tt_terminal, and on 2026-08-11 that reaped
    the operator's freshly launched GUI mid-run - which then rotated the
    production optimizer.log on startup and destroyed the 2026-08-10 baseline.

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
#
# REFUSE, never kill. This script does not get to decide that somebody else's
# terminal is expendable; it cannot tell a stale leftover from the instance that
# is trading the account right now.
$existing = @(Get-Process -Name 'tt_terminal' -ErrorAction SilentlyContinue)
if ($existing.Count -gt 0) {
    "REFUSING to start: tt_terminal is already running (pid $($existing.Id -join ', '))."
    "Stop it YOURSELF if it is safe to - this script will not touch a process it did"
    "not start. This run needs the single-instance mutex and the API client ids."
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
$script:ourPid = 0   # the ONE pid this script is allowed to kill
$timedOut = $false   # declared out here so the report below can trust it

# Kill the child THIS script started, by pid, and nothing else. Idempotent.
#
# WHY THIS IS A FUNCTION AND WHY IT CHECKS THE NAME. The cleanup used to end with
# a "belt and braces" sweep that killed every process named tt_terminal, on the
# theory that no such process may survive the script. On 2026-08-11 that sweep
# reaped the operator's freshly launched GUI:
#
#     killing leftover tt_terminal pid 8988
#
# The GUI was not this script's to kill and had nothing to do with the run; the
# collateral damage went further than the process, because its startup rotated
# the production optimizer.log and destroyed 11.25 MB of 2026-08-10 baseline
# evidence. Leaving a stray process behind is a nuisance that a human can see and
# fix. Killing an unrelated instance is silent, and on a trading day it can be
# the instance holding the live session.
#
# The name re-check is not paranoia about our own pid, it is about pid REUSE:
# between the child exiting and this running, Windows can hand its number to
# something else entirely, and Stop-Process does not care what it points at.
function Stop-OurChild {
    if (-not $script:ourPid) { return }
    $p = Get-Process -Id $script:ourPid -ErrorAction SilentlyContinue
    if (-not $p) { return }
    if ($p.Name -ne 'tt_terminal') {
        "NOT killing pid $($script:ourPid): it is now '$($p.Name)', not our tt_terminal (pid reuse)."
        return
    }
    Stop-Process -Id $script:ourPid -Force -Confirm:$false -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
    if (Get-Process -Id $script:ourPid -ErrorAction SilentlyContinue) {
        "WARNING: pid $($script:ourPid) survived the kill - it still holds the single-instance mutex."
    }
}

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
    # The pid we launched, latched the instant we have it. Every kill below is
    # scoped to THIS number and nothing else. See the cleanup section.
    $script:ourPid = $proc.Id
    "  pid:     $($script:ourPid)"

    # --- wait, with a hard wall -------------------------------------------------
    # WaitForExit on the .NET object, NOT Wait-Process: Wait-Process throws
    # "cannot find a process with the process identifier" when the child has
    # already exited, which is indistinguishable from its timeout exception - so
    # a build that finished fast would be reported, and killed, as a hang. This
    # returns $false only for a real timeout, and returns the instant the child
    # exits so a fast build is not rounded up to a poll interval.
    $timedOut = -not $proc.WaitForExit($TimeoutMinutes * 60 * 1000)

    if ($timedOut) {
        "TIMED OUT after $TimeoutMinutes minutes - killing pid $($script:ourPid)."
        # The app's own shutdown watchdog force-exits after 8s, but that only
        # helps once it has decided to quit. A wedged build never does, and a
        # leftover window-less tt_terminal.exe holds the single-instance mutex
        # and blocks every future launch - the documented auto-restart failure.
        Stop-OurChild
    }
} finally {
    # Leave no stray process behind, on ANY exit path from the block above -
    # including Ctrl-C and an exception between launch and wait. Scoped to the
    # pid we started; see Stop-OurChild.
    Stop-OurChild
    $env:TT_AUTORUN_LINEUP = $prevLineup
    $env:TT_LOG_STDOUT = $prevStdout
    $env:LOCALAPPDATA = $prevAppData
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
if ($timedOut) {
    # A timeout is documented as 2 and must STAY 2. We killed the child, so it
    # has an exit code now - whatever Windows gives a force-terminated process
    # (-1, i.e. 255 through a shell). Reading it back would report a hung build
    # under a code that means nothing, and a negative one at that: `exit -1`
    # leaves 255 for the scheduled job to interpret.
    "TIMED OUT - reporting 2 regardless of the killed child's exit code."
} elseif ($proc -and $proc.HasExited) {
    $code = $proc.ExitCode
    if ($null -ne $code) {
        # Clamp to a byte. The contract is 0/1/2/3; anything else came from a
        # crash (an unhandled SEH code is a large negative) and `exit` would
        # truncate it silently - possibly to 0. Report 2: unknown is failure.
        if ($code -ge 0 -and $code -le 255) { $exit = [int]$code }
        else {
            "WARNING: the child exited with $code (a crash, not one of 0/1/2/3) - reporting 2."
        }
    } else {
        "WARNING: the child's exit code could not be read - reporting 2."
    }
} elseif ($proc) {
    "WARNING: the child is still running after cleanup - reporting 2."
}
""
$report = @(Select-String -Path $LogFile -Pattern '^dryrun:' -ErrorAction SilentlyContinue |
            ForEach-Object { $_.Line })
if ($report.Count) { $report } else { "(no 'dryrun:' lines in $LogFile - the run produced no report)" }
""
"$([DateTime]::Now.ToString('yyyy-MM-dd HH:mm:ss')) dry run finished, exit=$exit"
exit $exit
