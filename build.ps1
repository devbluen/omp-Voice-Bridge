<#
.SYNOPSIS
    Builds the Voice Bridge server plugin and/or client mod and collects the files in dist\.

.EXAMPLE
    .\build.ps1                          # server + client, Release, with tests
    .\build.ps1 client                   # only voice-bridge.asi
    .\build.ps1 server -NoTests          # only the plugin, skip the tests
    .\build.ps1 -Version 1.2.0           # set the version (saved in the VERSION file)
    .\build.ps1 -Clean -Package          # rebuild from scratch and create the .zip files
    .\build.ps1 client -GameDir "C:\Games\GTA San Andreas"
    .\build.ps1 server -ServerDir "D:\Samp\my-server"
#>
[CmdletBinding()]
param(
    [ValidateSet('all', 'server', 'client')]
    [string]$Target = 'all',

    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')]
    [string]$Config = 'Release',

    # New version (MAJOR.MINOR.PATCH). Saved in the VERSION file, so later
    # builds keep it. Without it, the version in VERSION is used.
    [string]$Version = '',

    # Delete the build folders first.
    [switch]$Clean,

    # Skip the server unit tests.
    [switch]$NoTests,

    # Also create the release .zip files in dist\.
    [switch]$Package,

    # Copy voice-bridge.asi into this GTA San Andreas folder.
    [string]$GameDir = '',

    # Copy the plugin and includes into this server folder (open.mp or SA-MP).
    [string]$ServerDir = ''
)

$ErrorActionPreference = 'Stop'
$Root = $PSScriptRoot
$Dist = Join-Path $Root 'dist'
$started = Get-Date

function Step([string]$text) { Write-Host "`n==> $text" -ForegroundColor Cyan }
function Done([string]$text) { Write-Host "    $text" -ForegroundColor Green }
function Fail([string]$text) { Write-Host "`nERROR: $text" -ForegroundColor Red; exit 1 }

function Invoke-Tool([string]$exe, [string[]]$arguments) {
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) { Fail "$exe $($arguments -join ' ') failed (exit code $LASTEXITCODE)" }
}

function Build-Preset([string]$preset, [string[]]$targets) {
    $buildDir = Join-Path $Root "build\$preset"
    if ($Clean -and (Test-Path $buildDir)) {
        Step "Cleaning build\$preset"
        Remove-Item -Recurse -Force $buildDir
    }
    Step "Configuring $preset"
    $configure = @('--preset', $preset, '--log-level=WARNING', '-Wno-deprecated', '-Wno-dev')
    $configure += "-DVOICE_BRIDGE_VERSION=$Version"
    Invoke-Tool 'cmake' $configure

    Step "Building $preset ($Config)"
    $build = @('--build', $buildDir, '--config', $Config, '--parallel')
    foreach ($t in $targets) { $build += @('--target', $t) }
    Invoke-Tool 'cmake' $build
}

# --- Checks -------------------------------------------------------------------

if ($PSVersionTable.PSVersion.Major -ge 6 -and -not $IsWindows) {
    Fail 'This script builds the Windows binaries. On Linux use ./build.sh'
}
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    Fail 'CMake was not found. Install it from https://cmake.org/download or with Visual Studio ("C++ CMake tools for Windows").'
}

# --- Version ------------------------------------------------------------------

$versionFile = Join-Path $Root 'VERSION'
if ($Version) {
    if ($Version -notmatch '^(\d+)\.(\d+)\.(\d+)$') {
        Fail "-Version must look like 1.2.3 (got '$Version')"
    }
    # The client reports MAJOR*10000 + MINOR*100 + PATCH in a 16-bit field.
    if ([int]$Matches[1] -gt 6 -or [int]$Matches[2] -gt 99 -or [int]$Matches[3] -gt 99) {
        Fail "Version $Version is out of range: MAJOR 0-6, MINOR 0-99, PATCH 0-99"
    }
    [IO.File]::WriteAllText($versionFile, "$Version`n")
    $include = Join-Path $Root 'include\voice-bridge.inc'
    $text = [IO.File]::ReadAllText($include)
    $text = $text -replace '#define VOICE_BRIDGE_INCLUDE_VERSION "[^"]*"', "#define VOICE_BRIDGE_INCLUDE_VERSION `"$Version`""
    [IO.File]::WriteAllText($include, $text)
}
else {
    if (-not (Test-Path $versionFile)) { Fail 'VERSION file not found; run with -Version 1.0.0' }
    $Version = ([IO.File]::ReadAllText($versionFile)).Trim()
}
Write-Host "Voice Bridge $Version" -ForegroundColor White

$buildServer = $Target -in 'all', 'server'
$buildClient = $Target -in 'all', 'client'

# --- Build --------------------------------------------------------------------

Push-Location $Root
try {
    if ($buildServer) {
        Build-Preset 'windows-server' @()
        if (-not $NoTests) {
            Step 'Running server tests'
            Invoke-Tool 'ctest' @('--test-dir', 'build\windows-server', '-C', $Config, '--output-on-failure')
        }
    }
    if ($buildClient) {
        Build-Preset 'windows-client' @('voice-bridge-client')
    }
}
finally {
    Pop-Location
}

# --- Collect ------------------------------------------------------------------

Step 'Collecting files in dist\'
$serverOut = Join-Path $Dist 'server'
$clientOut = Join-Path $Dist 'client'

if ($buildServer) {
    $dll = Join-Path $Root "build\windows-server\plugins\$Config\voice-bridge.dll"
    if (-not (Test-Path $dll)) { Fail "voice-bridge.dll not found at $dll" }
    if (Test-Path $serverOut) { Remove-Item -Recurse -Force $serverOut }
    New-Item -ItemType Directory -Force "$serverOut\components", "$serverOut\plugins", "$serverOut\include" | Out-Null
    Copy-Item $dll "$serverOut\components\"
    Copy-Item $dll "$serverOut\plugins\"
    Copy-Item "$Root\build\windows-server\pawno\include\*.inc" "$serverOut\include\"
    Copy-Item "$Root\packaging\SERVER-README.txt" "$serverOut\README.txt"
    Copy-Item "$Root\LICENSE" $serverOut
    Done 'dist\server\components\voice-bridge.dll   (open.mp)'
    Done 'dist\server\plugins\voice-bridge.dll      (SA-MP)'
    Done 'dist\server\include\*.inc'
}

if ($buildClient) {
    $asi = Join-Path $Root "build\windows-client\client\$Config\voice-bridge.asi"
    if (-not (Test-Path $asi)) { Fail "voice-bridge.asi not found at $asi" }
    if (Test-Path $clientOut) { Remove-Item -Recurse -Force $clientOut }
    New-Item -ItemType Directory -Force $clientOut | Out-Null
    Copy-Item $asi $clientOut
    Copy-Item "$Root\packaging\CLIENT-README.txt" "$clientOut\LEIA-ME.txt"
    Copy-Item "$Root\LICENSE" $clientOut
    Done 'dist\client\voice-bridge.asi'
}

if ($Package) {
    Step 'Creating packages'
    $tag = $Version
    if ($buildServer) {
        $zip = Join-Path $Dist "voice-bridge-server-windows-x86-$tag.zip"
        Remove-Item $zip -ErrorAction SilentlyContinue
        Compress-Archive -Path "$serverOut\*" -DestinationPath $zip
        Done (Split-Path $zip -Leaf)
    }
    if ($buildClient) {
        $zip = Join-Path $Dist "voice-bridge-client-$tag.zip"
        Remove-Item $zip -ErrorAction SilentlyContinue
        Compress-Archive -Path "$clientOut\*" -DestinationPath $zip
        Done (Split-Path $zip -Leaf)
    }
}

# --- Install ------------------------------------------------------------------

if ($GameDir) {
    Step "Installing the client in $GameDir"
    if (-not (Test-Path (Join-Path $GameDir 'gta_sa.exe'))) { Fail "gta_sa.exe not found in $GameDir" }
    if (-not $buildClient) { Fail '-GameDir needs the client: run with target "all" or "client"' }
    Copy-Item "$clientOut\voice-bridge.asi" $GameDir -Force
    if (Test-Path (Join-Path $GameDir 'sampvoice.asi')) {
        Write-Host '    sampvoice.asi is also installed: remove it, Voice Bridge replaces it.' -ForegroundColor Yellow
    }
    Done 'voice-bridge.asi copied'
}

if ($ServerDir) {
    Step "Installing the plugin in $ServerDir"
    if (-not $buildServer) { Fail '-ServerDir needs the server: run with target "all" or "server"' }
    $isOpenMp = (Test-Path (Join-Path $ServerDir 'omp-server.exe')) -or (Test-Path (Join-Path $ServerDir 'components'))
    $folder = if ($isOpenMp) { 'components' } else { 'plugins' }
    New-Item -ItemType Directory -Force (Join-Path $ServerDir $folder) | Out-Null
    Copy-Item "$serverOut\$folder\voice-bridge.dll" (Join-Path $ServerDir $folder) -Force
    $includeDir = @('qawno\include', 'pawno\include') | ForEach-Object { Join-Path $ServerDir $_ } | Where-Object { Test-Path $_ } | Select-Object -First 1
    if ($includeDir) {
        Copy-Item "$serverOut\include\*.inc" $includeDir -Force
        Done "includes copied to $includeDir"
    }
    Done "voice-bridge.dll copied to $folder\"
    if (-not $isOpenMp) { Write-Host '    SA-MP: add "voice-bridge" to the plugins line of server.cfg.' -ForegroundColor Yellow }
}

$elapsed = [int]((Get-Date) - $started).TotalSeconds
Write-Host "`nVoice Bridge $Version built in $elapsed s. Files are in $Dist" -ForegroundColor Green
