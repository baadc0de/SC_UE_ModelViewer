# Package the viewer (SCViewer game target) for Win64: a standalone directory that opens the viewer on a double click of
# SCViewer.exe. It imports from the user's own Star Citizen installation at run time (Data.p4k, found in the standard RSI
# install locations), through the archive library (scbridge.dll, staged as a runtime dependency of SCBridgeImport).
# A game target has no editor modules: meshes are built at run time without Nanite, distance fields or Lumen cards
# (SCMeshBuild: "disabled:non_editor_host"); see viewer/README.md "Packaged viewer".
# The engine is the SCViewer fork of UE 5.9 built from source (plan D17): -Engine, else $env:UE_ENGINE_ROOT, else
# %USERPROFILE%\UnrealEngine-5.9-src. The FFI library must be built first (scripts/build_ffi.ps1 [-Release]).
# Usage: viewer/scripts/package_viewer.ps1 [-Config Development|Shipping] [-Out <dir>] [-Engine <UE 5.9 source fork>]
param(
    [string]$Config = 'Development',
    [string]$Out = '',
    [string]$Engine = $(if ($env:UE_ENGINE_ROOT) { $env:UE_ENGINE_ROOT } else { Join-Path $env:USERPROFILE 'UnrealEngine-5.9-src' })
)
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$Project = Join-Path $Root 'viewer\SCViewer.uproject'
if (-not $Out) { $Out = Join-Path $Root "out\win64\viewer-$($Config.ToLower())" }
$Out = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Out)
if (-not (Test-Path (Join-Path $Engine 'Engine\Build\BatchFiles\RunUAT.bat'))) {
    throw "No UE 5.9 source fork at $Engine (pass -Engine or set UE_ENGINE_ROOT)."
}
if (-not (Test-Path (Join-Path $Root 'plugins\SCBridgeImport\Source\ThirdParty\SCBridgeLib\lib\Win64\scbridge.dll'))) {
    throw 'No scbridge.dll: run scripts/build_ffi.ps1 first.'
}
& (Join-Path $Engine 'Engine\Build\BatchFiles\RunUAT.bat') BuildCookRun "-project=$Project" -platform=Win64 "-clientconfig=$Config" `
    -build -cook -stage -pak -iostore -package -archive "-archivedirectory=$Out" -map=/Game/Maps/Viewer -nocompileeditor -unattended -utf8output
if ($LASTEXITCODE -ne 0) { throw "BuildCookRun failed ($LASTEXITCODE)" }
Write-Output "PACKAGED $Out"
