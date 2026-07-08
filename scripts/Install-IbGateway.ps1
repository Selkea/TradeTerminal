<#
    Install-IbGateway.ps1 - install IB Gateway (stable) + IBC for the TWS
    socket route. Idempotent; safe to re-run.

    IB Gateway is IBKR's lightweight desktop app that serves the TWS socket
    API (port 4002 paper / 4001 live). IBC (IbcAlpha/IBC) automates its login
    dialog. NOTE: IBC cannot generate TOTP codes - a 2FA-enrolled login shows
    the security-code dialog once per login window (type it via RDP); IB
    Gateway's auto-restart then keeps the session up for ~a week.
#>
$ErrorActionPreference = "Stop"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

function Step($msg) { Write-Host "`n=== $msg ===" -ForegroundColor Cyan }

# --- IB Gateway (stable standalone) -------------------------------------------
Step "IB Gateway"
$jts = "C:\Jts\ibgateway"

# IBC expects the install under the NUMERIC version dir (C:\Jts\ibgateway\1030
# style). Detect the version from the launch jar and normalize the dir name.
function Get-GwVersion($dir) {
    $jar = Get-ChildItem (Join-Path $dir "jars") -Filter "*launch-*.jar" -ErrorAction SilentlyContinue |
           Select-Object -First 1
    if ($jar -and $jar.Name -match '(\d{3,4})') { return $Matches[1] }
    return $null
}
function Normalize-GwDir {
    foreach ($d in @(Get-ChildItem $jts -Directory -ErrorAction SilentlyContinue)) {
        if ($d.Name -match '^\d+$') { continue }
        $ver = Get-GwVersion $d.FullName
        if ($ver -and -not (Test-Path (Join-Path $jts $ver))) {
            Rename-Item $d.FullName $ver
            Write-Host "  normalized $($d.Name) -> $ver"
        }
    }
}

$numeric = @(Get-ChildItem $jts -Directory -ErrorAction SilentlyContinue |
             Where-Object { $_.Name -match '^\d+$' })
if ($numeric.Count -eq 0) { Normalize-GwDir }
$numeric = @(Get-ChildItem $jts -Directory -ErrorAction SilentlyContinue |
             Where-Object { $_.Name -match '^\d+$' })
if ($numeric.Count -gt 0) {
    Write-Host "  already installed: $($numeric[0].FullName)"
} else {
    $exe = Join-Path $env:TEMP "ibgateway-stable.exe"
    curl.exe -sL -o $exe "https://download2.interactivebrokers.com/installers/ibgateway/stable-standalone/ibgateway-stable-standalone-windows-x64.exe"
    if ($LASTEXITCODE -ne 0) { throw "IB Gateway download failed" }
    # install4j silent install NEEDS an explicit -dir (bare -q no-ops).
    Start-Process $exe -ArgumentList "-q", "-dir", "$jts\_install" -Wait
    if (-not (Test-Path "$jts\_install")) { throw "IB Gateway install produced nothing" }
    Normalize-GwDir
    Write-Host "  installed"
}

# --- IBC (login automation) ----------------------------------------------------
Step "IBC"
$ibcDir = "C:\IBC"
if (Test-Path (Join-Path $ibcDir "IBC.jar")) {
    Write-Host "  already installed: $ibcDir"
} else {
    $rel = Invoke-RestMethod "https://api.github.com/repos/IbcAlpha/IBC/releases/latest"
    $asset = $rel.assets | Where-Object { $_.name -like "IBCWin*.zip" } | Select-Object -First 1
    if (-not $asset) { throw "could not resolve an IBCWin release asset" }
    $zip = Join-Path $env:TEMP $asset.name
    curl.exe -sL -o $zip $asset.browser_download_url
    if ($LASTEXITCODE -ne 0) { throw "IBC download failed" }
    Expand-Archive -Path $zip -DestinationPath $ibcDir -Force
    Remove-Item $zip
    Write-Host "  installed: $ibcDir ($($rel.tag_name))"
}

Step "Done"
Write-Host @"
Next: scripts\Start-IbGateway.ps1 starts IB Gateway via IBC using the active
saved account (paper/live mode + port follow the account). On a 2FA-enrolled
login, type the authenticator code into the gateway dialog via RDP when it
appears; the session then survives daily auto-restarts for ~a week.
"@
