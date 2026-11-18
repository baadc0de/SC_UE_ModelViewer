# Package the Win64 showcase: the viewer with one cooked hangar and one cooked ship, started with
# -SCLoad=<ship> -SCEnvironment=<hangar>, which stream the cooked levels. Cook both first (ue/scripts/cook_hangars.py,
# ue/scripts/cook_ships.py). A standalone directory (out/win64/<ship>): Nanite, Lumen GI and reflections with hardware
# ray tracing at the viewer's -SCMaxQuality (Cinematic, hit lighting; SCLook::ApplyMaxQuality) and DLAA (the NVIDIA DLSS
# plugin at 100 %, ue/README.md). <Out>\Play_SCViewer.bat starts it as the showcase (-SCShowcase: no viewer panels, first
# person with the player character; C third person, Z orbit, T teleport, F torch, LMB fire, RMB aim). The player
# character is exported from Data.p4k at start (an installed Star Citizen): -PlayerLoadout picks the game loadout,
# -PlayerWeapons keeps only those of its weapons (entity class names), and the kept weapons' spare magazines are
# carried on the armour.
# The engine is the SCViewer fork of UE 5.9 built from source (plan D17): -Engine, else $env:UE_ENGINE_ROOT, else
# %USERPROFILE%\UnrealEngine-5.9-src.
# Usage: ue/scripts/package_showcase.ps1 -Ship DRAK_Golem -Hangar hightech_a/hangar_smltop_001
#        [-Config Development|Shipping] [-PlayerLoadout <p4k path>] [-PlayerWeapons <entity>,...]
#        [-Out <dir>] [-Engine <UE 5.9 source fork>]
param(
    [ValidateSet('Win64')][string]$Platform = 'Win64',
    [Parameter(Mandatory = $true)][string]$Ship,
    [Parameter(Mandatory = $true)][string]$Hangar,
    [string]$Out = '',
    [string]$Engine = $(if ($env:UE_ENGINE_ROOT) { $env:UE_ENGINE_ROOT } else { Join-Path $env:USERPROFILE 'UnrealEngine-5.9-src' }),
    [string]$Config = 'Development',
    [string]$PlayerLoadout = '',
    [string[]]$PlayerWeapons = @()
)
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$Project = Join-Path $Root 'ue\SCViewer\SCViewer.uproject'
if (-not $Out) { $Out = Join-Path $Root "out\win64\$Ship" }
if (-not (Test-Path (Join-Path $Engine 'Engine\Build\BatchFiles\RunUAT.bat'))) {
    throw "No UE 5.9 source fork at $Engine (pass -Engine or set UE_ENGINE_ROOT)."
}
if (-not (Test-Path (Join-Path $Root 'ue\Plugins\NVIDIA\DLSS'))) {
    Write-Warning 'NVIDIA DLSS is not installed (scripts/install_dlss.ps1): the build anti-aliases with TSR instead of DLAA.'
}

# The cooked levels, named as the viewer looks them up (USCViewerSubsystem::CookedShipLevel / CookedEnvironmentLevel).
$Style, $HangarName = $Hangar.Replace('\', '/').Split('/')
$ShipLevel = '/Game/SC/Ships/Maps/' + ($Ship -replace '[^A-Za-z0-9_]', '_')
$HangarLevel = "/Game/SC/Hangars/Maps/${Style}_$HangarName"
$Extra = ' -SCMaxQuality'
foreach ($Level in @($ShipLevel, $HangarLevel)) {
    $File = Join-Path $Root ('ue\SCViewer\Content' + $Level.Substring(5).Replace('/', '\') + '.umap')
    if (-not (Test-Path $File)) { throw "$Level is not cooked ($File): run cook_ships.py / cook_hangars.py first." }
}
# The environment argument is the hangar's object-container path; only its last two parts name the cooked level.
$Environment = "Data/ObjectContainers/PU/loc/mod/common/hangar/$Style/$HangarName.socpak"
$Maps = "/Game/Maps/Viewer+$ShipLevel+$HangarLevel"

& (Join-Path $Engine 'Engine\Build\BatchFiles\RunUAT.bat') BuildCookRun "-project=$Project" "-platform=$Platform" "-clientconfig=$Config" `
    -build -cook -stage -pak -iostore -package -archive "-archivedirectory=$Out" "-map=$Maps" -nocompileeditor -unattended -utf8output `
    "-cmdline=-SCLoad=$Ship -SCEnvironment=$Environment$Extra"
if ($LASTEXITCODE -ne 0) { throw "BuildCookRun failed ($LASTEXITCODE)" }
# A Windows build does not read the staged command line (UECommandLine.txt): the launcher passes it.
# SCViewer.exe on its own is the plain viewer (it opens Data.p4k when one is installed).
$Player = ' -SCShowcase -SCCamera=first'
if ($PlayerLoadout) { $Player += " -SCPlayerLoadout=$PlayerLoadout" }
if ($PlayerWeapons.Count) { $Player += ' -SCPlayerWeapons=' + ($PlayerWeapons -join ',') }
$Launcher = "@echo off`r`nrem SCViewer showcase: $Ship in $Hangar ($Config).`r`nstart `"`" `"%~dp0SCViewer.exe`" -SCLoad=$Ship -SCEnvironment=$Environment$Extra$Player %*`r`n"
[IO.File]::WriteAllText((Join-Path $Out 'Play_SCViewer.bat'), $Launcher)
Write-Output "PACKAGED $Out"
