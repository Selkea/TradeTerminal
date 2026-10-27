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
