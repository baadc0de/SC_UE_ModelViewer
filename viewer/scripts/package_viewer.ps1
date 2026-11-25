# Package the viewer (SCViewer game target) for Win64: a standalone directory that opens the viewer on a double click of
# SCViewer.exe. It imports from the user's own Star Citizen installation at run time (Data.p4k, found in the standard RSI
# install locations or chosen in the app), through the archive library (scbridge.dll, staged as a runtime dependency of
# SCBridgeImport). A game target has no editor modules: meshes are built at run time without Nanite, distance fields or
# Lumen cards (SCMeshBuild: "disabled:non_editor_host"). Its cache is the "cache" folder beside SCViewer.exe.
# -Release makes the public download (docs/viewer/USER_GUIDE.md): Shipping, no debug symbols, the Visual C++ runtime
# installer staged, and out/release/SCViewer-<Version>-win64.zip with one SCViewer folder (SCViewer.exe, README.txt).
# The engine is the SCViewer fork of UE 5.9 built from source (plan D17): -Engine, else $env:UE_ENGINE_ROOT, else
# %USERPROFILE%\UnrealEngine-5.9-src. The FFI library must be built first (scripts/build_ffi.ps1 [-Release]).
# Usage: viewer/scripts/package_viewer.ps1 [-Config Development|Shipping] [-Out <dir>] [-Engine <dir>]
#        viewer/scripts/package_viewer.ps1 -Release -Version 0.1.0
param(
    [string]$Config = 'Development',
    [string]$Out = '',
    [switch]$Release,
    [string]$Version = '',
    [string]$Engine = $(if ($env:UE_ENGINE_ROOT) { $env:UE_ENGINE_ROOT } else { Join-Path $env:USERPROFILE 'UnrealEngine-5.9-src' })
)
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$Project = Join-Path $Root 'viewer\SCViewer.uproject'
if ($Release) {
    if (-not $Version) { throw '-Release needs -Version (for example 0.1.0).' }
    $Config = 'Shipping'
}
if (-not $Out) { $Out = Join-Path $Root "out\win64\viewer-$($Config.ToLower())" }
$Out = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Out)
if (-not (Test-Path (Join-Path $Engine 'Engine\Build\BatchFiles\RunUAT.bat'))) {
    throw "No UE 5.9 source fork at $Engine (pass -Engine or set UE_ENGINE_ROOT)."
}
if (-not (Test-Path (Join-Path $Root 'plugins\SCBridgeImport\Source\ThirdParty\SCBridgeLib\lib\Win64\scbridge.dll'))) {
    throw 'No scbridge.dll: run scripts/build_ffi.ps1 first.'
}
$Extra = @()
if ($Release) { $Extra += @('-nodebuginfo', '-prereqs') }
& (Join-Path $Engine 'Engine\Build\BatchFiles\RunUAT.bat') BuildCookRun "-project=$Project" -platform=Win64 "-clientconfig=$Config" `
    -build -cook -stage -pak -iostore -package -archive "-archivedirectory=$Out" -map=/Game/Maps/Viewer -nocompileeditor -unattended -utf8output @Extra
if ($LASTEXITCODE -ne 0) { throw "BuildCookRun failed ($LASTEXITCODE)" }
Write-Output "PACKAGED $Out"
if (-not $Release) { return }

# The zip: the archived package without what a run leaves behind (the cache, Saved) or what users do not need.
$Stage = Join-Path $Root "out\release\stage\SCViewer"
if (Test-Path $Stage) { Remove-Item -Recurse -Force $Stage }
New-Item -ItemType Directory -Force $Stage | Out-Null
$From = if (Test-Path (Join-Path $Out 'Windows\SCViewer.exe')) { Join-Path $Out 'Windows' } else { $Out }
robocopy $From $Stage /E /NFL /NDL /NJH /NJS /XD cache Saved /XF *.pdb Manifest_*.txt | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)" }
Copy-Item (Join-Path $Root 'viewer\release\README.txt') (Join-Path $Stage 'README.txt')
Copy-Item (Join-Path $Root 'LICENSE') (Join-Path $Stage 'LICENSE.txt')
$Zip = Join-Path $Root "out\release\SCViewer-$Version-win64.zip"
if (Test-Path $Zip) { Remove-Item -Force $Zip }
Push-Location (Split-Path $Stage)
try { & tar.exe -a -c -f $Zip SCViewer; if ($LASTEXITCODE -ne 0) { throw "zip failed ($LASTEXITCODE)" } } finally { Pop-Location }
$MB = [math]::Round((Get-Item $Zip).Length / 1MB)
Write-Output "RELEASE_ZIP $Zip ${MB} MB"
