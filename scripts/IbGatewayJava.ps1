<#
    IbGatewayJava.ps1 - how IBC finds java.exe, in one place.

    WHY THIS FILE EXISTS. On 2026-10-27 the 10.45 -> 10.50.1f upgrade took the
    box out of the market for a whole session. Update-IbGateway.ps1 installed
    10.50 correctly, Start-IbGateway.ps1 killed the working 10.45 gateway as
    designed, and then IBC said:

        Error: Can't find suitable Java installation

    and nothing came up. Not a dialog change, not IBC's age, not the rename -
    JAVA DISCOVERY. IBC\scripts\StartIBC.bat looks for a JVM in this order
    (lines 404-423 of IBC 3.24.1):

        1. $env:JAVA_PATH, if it contains java.exe
        2. <install>\.install4j\pref_jre.cfg  -> that path + \bin
        3. <install>\.install4j\inst_jre.cfg  -> that path + \bin
        4. %PROGRAMDATA%\Oracle\Java\javapath
        5. give up

    10.45 was fine because its installer registered a SHARED JRE and wrote
    pref_jre.cfg pointing at it:

        C:\Program Files\Common Files\i4j_jres\Oda-...\17.0.16.0.101-zulu

    10.50 BUNDLES its JRE inside the install directory instead, and writes NO
    pref_jre.cfg at all. Its inst_jre.cfg points at the installer's own temp
    directory:

        c:\users\administrator\appdata\local\install4j\t\e4jd590.tmp_dir.../jre

    which is deleted the moment the installer exits. So steps 2-4 all miss, even
    though a perfectly good java.exe is sitting in <install>\jre\bin. The
    gateway's own ibgateway.exe finds it (install4j resolves it relative to the
    launcher), which is why running the exe by hand "worked" and made this look
    like an IBC-vs-10.50 incompatibility. It is not. IBC simply has to be TOLD.

    THE LESSON, which is the same one this project keeps relearning: a check
    that is narrower than the invariant it claims. Verify-IbGateway.ps1 checked
    the build NUMBER and called that a verified gateway. A build number says
    nothing about whether the thing can be launched, and the one check that
    would have caught this - "can IBC resolve a JVM for the build the launcher
    would pick?" - is three lines long and was not there.

    Dot-source this from any script that launches or checks the gateway:

        . (Join-Path $PSScriptRoot 'IbGatewayJava.ps1')
#>

# The JRE a build ships inside its own install directory, or $null.
#
# Preferred over everything else when it exists: it is the JVM IB built and
# tested that specific build against, it moves with the install, and it cannot
# be pulled out from under us by an unrelated install4j uninstall tidying up the
# shared i4j_jres registry.
function Get-IbcBundledJava {
    param([Parameter(Mandatory = $true)][string]$InstallDir)
    $bin = Join-Path $InstallDir 'jre\bin'
    if (Test-Path (Join-Path $bin 'java.exe')) { return $bin }
    return $null
}

# What java.exe WOULD be used for $InstallDir, and where it came from.
#
# Returns a PSCustomObject: Path (the folder containing java.exe, or $null) and
# Source (bundled | pref_jre | inst_jre | oracle | none). Deliberately a mirror
# of StartIBC.bat's own order with the bundled JRE prepended, so a caller can
# ask "will IBC be able to start this?" and get the launcher's real answer
# rather than a second opinion.
function Resolve-IbcJava {
    param([Parameter(Mandatory = $true)][string]$InstallDir)

    $bundled = Get-IbcBundledJava $InstallDir
    if ($bundled) {
        return [pscustomobject]@{ Path = $bundled; Source = 'bundled' }
    }

    # Both .cfg files hold a JRE HOME, one line, no quotes; IBC appends \bin and
    # then re-tests for java.exe - a stale path is skipped, not fatal, which is
    # exactly how 10.50's dead temp-dir entry slipped through silently.
    $i4j = Join-Path $InstallDir '.install4j'
    foreach ($cfg in 'pref_jre.cfg', 'inst_jre.cfg') {
        $p = Join-Path $i4j $cfg
        if (-not (Test-Path $p)) { continue }
        # NOT $home - that is a PowerShell automatic variable (the user profile
        # path). Assigning it here would only shadow it function-locally, but it
        # is the same trap that bit $args in Update-IbGateway.ps1.
        $jreHome = (Get-Content $p -ErrorAction SilentlyContinue | Select-Object -First 1)
        if (-not $jreHome) { continue }
        $bin = Join-Path $jreHome.Trim() 'bin'
        if (Test-Path (Join-Path $bin 'java.exe')) {
            return [pscustomobject]@{ Path = $bin; Source = $cfg.Replace('.cfg', '') }
        }
    }

    $oracle = Join-Path $env:PROGRAMDATA 'Oracle\Java\javapath'
    if (Test-Path (Join-Path $oracle 'java.exe')) {
        return [pscustomobject]@{ Path = $oracle; Source = 'oracle' }
    }

    return [pscustomobject]@{ Path = $null; Source = 'none' }
}

<#
    THE SECOND 10.50 PROBLEM, found 2026-10-28: once IBC could find Java, the
    gateway came up - and then failed to load its own settings on EVERY start:

        We are having trouble loading your settings, they may be corrupted.
        File does not exist C:\Jts\<userdir>\ibg.xml

    The file existed. It had been written 11 seconds earlier, by the same build,
    on the way down into its nightly auto-restart. The message is false; the
    cause is the JRE. 10.50 bundles JAVA 25, and JDK 24 tightened the XML
    parser's limits (JDK-8343006): elementAttributeLimit 10000 -> 200,
    totalEntitySizeLimit 50,000,000 -> 100000, maxGeneralEntitySizeLimit
    unlimited -> 100000. The gateway's settings file is bigger than that.

    IB knows. Its own install4j launcher raises all three again through the
    javaOptions variable in <install>\.install4j\i4jparams.conf:

        -Djdk.xml.elementAttributeLimit=10000
        -Djdk.xml.maxGeneralEntitySizeLimit=0
        -Djdk.xml.totalEntitySizeLimit=0

    IBC 3.24.1 never uses that launcher. It builds its own java command line and
    takes exactly one option from i4jparams.conf (-DjxBrowserKey), so the parser
    runs on Java 25's strict defaults and rejects every settings file - and the
    gateway runs on FACTORY DEFAULTS after every start. IBC quietly re-applies
    the API port and Read-Only flag, so trading kept working, which is precisely
    why this looked cosmetic. It was not: every setting IBC does not manage was
    reset on every launch, and IBC's log said so - "Read-Only API checkbox is
    NOW set to: false", where 10.45 had always said "already set".

    10.45 never showed it because it ran on a shared Zulu 17, whose limits are
    the old loose ones. 0.51.0 pointed IBC at 10.50's bundled JRE, which was
    necessary - 10.50 cannot run without it - and is what exposed this.

    The fix forwards the build's OWN values, read from its own i4jparams.conf,
    through JDK_JAVA_OPTIONS: the one variable StartIBC.bat leaves alone (it
    blanks JAVA_TOOL_OPTIONS at line 446) and the route publicly A/B-tested on
    this exact build (extrange/ibkr-docker #158). Verified on the VPS's own
    Java 25: with the variable set, java -XshowSettings:properties reports all
    three; without it, none. System properties outrank the JRE's
    conf\jaxp.properties, so nothing inside the install has to be edited.

    IBC 3.24.2 (2026-08-21, the real final IbcAlpha release - not 3.24.1, as
    these notes long assumed) forwards the whole javaOptions value itself. When
    the box moves to it this becomes redundant, and harmless.
#>

# The -Djdk.xml.* options a build's OWN launcher passes, as one string, or $null.
#
# Only tokens of the exact shape -Djdk.xml.<name>=<word> are taken, and only when
# the token ENDS cleanly (whitespace, closing quote or end of line). A value
# carrying an XML escape such as &amp; therefore fails to match at all instead of
# being silently truncated into a different value. This also guarantees nothing
# else on that line - it carries a licence key - can leak into the environment.
function Get-IbcJdkXmlOptions {
    param([Parameter(Mandatory = $true)][string]$InstallDir)
    $conf = Join-Path $InstallDir '.install4j\i4jparams.conf'
    if (-not (Test-Path $conf)) { return $null }
    $line = Select-String -Path $conf -Pattern 'name="javaOptions"' -ErrorAction SilentlyContinue |
            Select-Object -First 1
    if (-not $line) { return $null }
    $toks = @([regex]::Matches($line.Line, '-Djdk\.xml\.[A-Za-z]+=[A-Za-z0-9.]+(?=[\s"]|$)') |
              ForEach-Object { $_.Value })
    if ($toks.Count -eq 0) { return $null }
    return ($toks -join ' ')
}

# The gateway's own verdict on the LAST start, read from IBC's log: did it fail
# to load its settings? Returns a PSCustomObject with State = loaded |
# defaults | unknown, and Detail.
#
# Scoped to the segment after the last "IBC: Starting Gateway", which IBC logs
# once per JVM start - including the nightly soft auto-restart, which appends to
# the same file rather than starting a new one. A segment with no
# "Login has completed" yet is UNKNOWN, not loaded: the dialog appears just
# before that line, so an absent dialog proves nothing until the login is past it.
function Get-IbcSettingsLoadState {
    param([Parameter(Mandatory = $true)][string]$Build)
    $logDir = Join-Path $env:LOCALAPPDATA 'TradeTerminal\ibc\logs'
    $log = Get-ChildItem $logDir -Filter "IBC-*_GATEWAY-${Build}_*.txt" -ErrorAction SilentlyContinue |
           Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $log) {
        return [pscustomobject]@{ State = 'unknown'; Detail = "no IBC log for build $Build under $logDir" }
    }
    $lines = @(Get-Content $log.FullName -ErrorAction SilentlyContinue)
    $start = -1
    for ($i = $lines.Count - 1; $i -ge 0; $i--) {
        if ($lines[$i] -match 'IBC: Starting Gateway') { $start = $i; break }
    }
    if ($start -lt 0) {
        return [pscustomobject]@{ State = 'unknown'; Detail = "$($log.Name) has no 'Starting Gateway' line" }
    }
    $seg = $lines[$start..($lines.Count - 1)]
    $when = if ($lines[$start] -match '^(\S+ \S+)') { $Matches[1] } else { '?' }
    if ($seg -match 'trouble loading your settings') {
        return [pscustomobject]@{ State = 'defaults'; Detail = "start at $when (IBC clock) showed the settings-load dialog - running on FACTORY DEFAULTS" }
    }
    if (-not ($seg -match 'Login has completed')) {
        return [pscustomobject]@{ State = 'unknown'; Detail = "start at $when has not completed login yet" }
    }
    return [pscustomobject]@{ State = 'loaded'; Detail = "start at $when (IBC clock) loaded its settings - no dialog" }
}
