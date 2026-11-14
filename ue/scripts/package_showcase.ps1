# Package a showcase build: the viewer with one cooked hangar and one cooked ship, started with
# -SCLoad=<ship> -SCEnvironment=<hangar> (staged as UECommandLine.txt), which stream the cooked levels.
# Cook both first (ue/scripts/cook_hangars.py, ue/scripts/cook_ships.py).
#  -Platform Wasm (UE 5.9 WebGPU, out/web/<ship>): no Data.p4k and no scbridge library (SC_WITH_FFI=0); serve it with
#     <Out>/Play_SCViewer.bat and open it in Chrome. -BakedLighting packages the hangar's Lightmass variant (<level>_Lit,
#     bake_hangar_lighting.py) and adds -SCBakedLighting, which streams it.
#  -Platform Win64 (standalone directory, out/win64/<ship>): Nanite, Lumen GI and reflections with hardware ray tracing
#     at the viewer's -SCMaxQuality (Cinematic, hit lighting; SCLook::ApplyMaxQuality) and DLAA (the NVIDIA DLSS plugin at
#     100 %, ue/README.md); run <Out>\Windows\SCViewer.exe.
# Usage: ue/scripts/package_showcase.ps1 -Platform Wasm|Win64 -Ship DRAK_Golem -Hangar hightech_a/hangar_smltop_001
#        [-Config Development|Shipping] [-BakedLighting] [-Out <dir>] [-Engine <UE 5.9>]
param(
    [Parameter(Mandatory = $true)][ValidateSet('Wasm', 'Win64')][string]$Platform,
    [Parameter(Mandatory = $true)][string]$Ship,
    [Parameter(Mandatory = $true)][string]$Hangar,
    [string]$Out = '',
    [string]$Engine = 'C:\Program Files\Epic Games\UE_5.9',
    [string]$Config = 'Development',
    [switch]$BakedLighting
)
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$Project = Join-Path $Root 'ue\SCViewer\SCViewer.uproject'
if (-not $Out) { $Out = Join-Path $Root ("out\" + $(if ($Platform -eq 'Wasm') { 'web' } else { 'win64' }) + "\$Ship") }
if ($Platform -eq 'Wasm' -and -not $env:WASM_EMSDK_ROOT -and -not $env:EMSDK) {
    throw 'Set WASM_EMSDK_ROOT (or EMSDK) to an emsdk with Emscripten 6.0.10 activated.'
}
if ($Platform -eq 'Win64' -and -not (Test-Path (Join-Path $Root 'ue\Plugins\NVIDIA\DLSS'))) {
    Write-Warning 'NVIDIA DLSS is not installed (scripts/install_dlss.ps1): the build anti-aliases with TSR instead of DLAA.'
}

# The cooked levels, named as the viewer looks them up (USCViewerSubsystem::CookedShipLevel / CookedEnvironmentLevel).
$Style, $HangarName = $Hangar.Replace('\', '/').Split('/')
$ShipLevel = '/Game/SC/Ships/Maps/' + ($Ship -replace '[^A-Za-z0-9_]', '_')
$HangarLevel = "/Game/SC/Hangars/Maps/${Style}_$HangarName"
$Extra = ''
if ($BakedLighting) { $HangarLevel += '_Lit'; $Extra += ' -SCBakedLighting' }
if ($Platform -eq 'Win64') { $Extra += ' -SCMaxQuality' }
foreach ($Level in @($ShipLevel, $HangarLevel)) {
    $File = Join-Path $Root ('ue\SCViewer\Content' + $Level.Substring(5).Replace('/', '\') + '.umap')
    if (-not (Test-Path $File)) { throw "$Level is not cooked ($File): run cook_ships.py / cook_hangars.py (bake_hangar_lighting.py for _Lit) first." }
}
# The environment argument is the hangar's object-container path; only its last two parts name the cooked level.
$Environment = "Data/ObjectContainers/PU/loc/mod/common/hangar/$Style/$HangarName.socpak"
$Maps = "/Game/Maps/Viewer+$ShipLevel+$HangarLevel"

& (Join-Path $Engine 'Engine\Build\BatchFiles\RunUAT.bat') BuildCookRun "-project=$Project" "-platform=$Platform" "-clientconfig=$Config" `
    -build -cook -stage -pak -iostore -package -archive "-archivedirectory=$Out" "-map=$Maps" -nocompileeditor -unattended -utf8output `
    "-cmdline=-SCLoad=$Ship -SCEnvironment=$Environment$Extra"
if ($LASTEXITCODE -ne 0) { throw "BuildCookRun failed ($LASTEXITCODE)" }
if ($Platform -eq 'Win64') {
    # A Windows build does not read the staged command line (UECommandLine.txt is the web page's): the launcher passes it.
    # SCViewer.exe on its own is the plain viewer (it opens Data.p4k when one is installed).
    $Launcher = "@echo off`r`nrem SCViewer showcase: $Ship in $Hangar ($Config).`r`nstart `"`" `"%~dp0SCViewer.exe`" -SCLoad=$Ship -SCEnvironment=$Environment$Extra %*`r`n"
    [IO.File]::WriteAllText((Join-Path $Out 'Play_SCViewer.bat'), $Launcher)
}
Write-Output "PACKAGED $Out"
