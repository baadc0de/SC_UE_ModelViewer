# Package the web build (UE 5.9 Wasm / WebGPU): the viewer with one cooked hangar and one cooked ship, no Data.p4k and
# no scbridge library (SC_WITH_FFI=0). Cook both first (ue/scripts/cook_hangars.py, ue/scripts/cook_ships.py); the
# viewer starts with -SCLoad=<ship> -SCEnvironment=<hangar>, which stream the cooked levels.
# -BakedLighting packages the hangar's Lightmass variant (<level>_Lit, ue/scripts/bake_hangar_lighting.py) and starts the
# viewer with -SCBakedLighting, which streams it.
# Usage: ue/scripts/package_web.ps1 -Ship DRAK_Golem -Hangar hightech_a/hangar_smltop_001 [-BakedLighting] [-Out out/web/<ship>]
#        [-Engine <UE 5.9>]
# Then serve: <Out>/Play_SCViewer.bat (or serve.py --root <Out>/Wasm) and open it in Chrome.
param(
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
if (-not $Out) { $Out = Join-Path $Root "out\web\$Ship" }
if (-not $env:WASM_EMSDK_ROOT -and -not $env:EMSDK) { throw 'Set WASM_EMSDK_ROOT (or EMSDK) to an emsdk with Emscripten 6.0.10 activated.' }

# The cooked levels, named as the viewer looks them up (USCViewerSubsystem::CookedShipLevel / CookedEnvironmentLevel).
$Style, $HangarName = $Hangar.Replace('\', '/').Split('/')
$ShipLevel = '/Game/SC/Ships/Maps/' + ($Ship -replace '[^A-Za-z0-9_]', '_')
$HangarLevel = "/Game/SC/Hangars/Maps/${Style}_$HangarName"
$Lighting = ''
if ($BakedLighting) { $HangarLevel += '_Lit'; $Lighting = ' -SCBakedLighting' }
foreach ($Level in @($ShipLevel, $HangarLevel)) {
    $File = Join-Path $Root ('ue\SCViewer\Content' + $Level.Substring(5).Replace('/', '\') + '.umap')
    if (-not (Test-Path $File)) { throw "$Level is not cooked ($File): run cook_ships.py / cook_hangars.py (bake_hangar_lighting.py for _Lit) first." }
}
# The environment argument is the hangar's object-container path; only its last two parts name the cooked level.
$Environment = "Data/ObjectContainers/PU/loc/mod/common/hangar/$Style/$HangarName.socpak"
$Maps = "/Game/Maps/Viewer+$ShipLevel+$HangarLevel"

& (Join-Path $Engine 'Engine\Build\BatchFiles\RunUAT.bat') BuildCookRun "-project=$Project" -platform=Wasm "-clientconfig=$Config" `
    -build -cook -stage -pak -iostore -package -archive "-archivedirectory=$Out" "-map=$Maps" -nocompileeditor -unattended -utf8output `
    "-cmdline=-SCLoad=$Ship -SCEnvironment=$Environment$Lighting"
if ($LASTEXITCODE -ne 0) { throw "BuildCookRun failed ($LASTEXITCODE)" }
Write-Output "PACKAGED $Out (serve: $Out\Play_SCViewer.bat)"
