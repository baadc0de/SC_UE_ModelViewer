# Package the Win64 showcase: UnrealCitizen running one prepared scenario (game/Config/Scenarios/<name>.json), started
# with -Scenario=<name>, which streams the scenario's prepared levels. Prepare it first (WP-O1-4, the prep step):
#   UnrealEditor-Cmd game/UnrealCitizen.uproject -run=SCPrepare -Scenario=<name> -AllowCommandletRendering -unattended
# A standalone directory (out/win64/<scenario>): Nanite, Lumen GI and reflections with hardware ray tracing at
# -SCMaxQuality (Cinematic, hit lighting; SCLook::ApplyMaxQuality) and DLAA (the NVIDIA DLSS plugin at 100 %,
# viewer/README.md). <Out>\Play_UnrealCitizen.bat starts it as the showcase (-SCShowcase: no viewer chrome; the scenario's
# camera; C third person, Z orbit, T teleport, F torch, LMB fire, RMB aim). The player character is exported from Data.p4k
# at start (an installed Star Citizen; WP-O1-4b prepares it): -PlayerLoadout picks the game loadout, -PlayerWeapons keeps
# only those of its weapons (entity class names), and the kept weapons' spare magazines are carried on the armour.
# The engine is the SCViewer fork of UE 5.9 built from source (plan D17): -Engine, else $env:UE_ENGINE_ROOT, else
# %USERPROFILE%\UnrealEngine-5.9-src.
# Usage: game/scripts/package_showcase.ps1 -Scenario golem_showcase
#        [-Config Development|Shipping] [-PlayerLoadout <p4k path>] [-PlayerWeapons <entity>,...]
#        [-Out <dir>] [-Engine <UE 5.9 source fork>]
param(
    [ValidateSet('Win64')][string]$Platform = 'Win64',
    [Parameter(Mandatory = $true)][string]$Scenario,
    [string]$Out = '',
    [string]$Engine = $(if ($env:UE_ENGINE_ROOT) { $env:UE_ENGINE_ROOT } else { Join-Path $env:USERPROFILE 'UnrealEngine-5.9-src' }),
    [string]$Config = 'Development',
    [string]$PlayerLoadout = '',
    [string[]]$PlayerWeapons = @()
)
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$Project = Join-Path $Root 'game\UnrealCitizen.uproject'
if (-not $Out) { $Out = Join-Path $Root "out\win64\$Scenario" }
if (-not (Test-Path (Join-Path $Engine 'Engine\Build\BatchFiles\RunUAT.bat'))) {
    throw "No UE 5.9 source fork at $Engine (pass -Engine or set UE_ENGINE_ROOT)."
}
if (-not (Test-Path (Join-Path $Root 'plugins\NVIDIA\DLSS'))) {
    Write-Warning 'NVIDIA DLSS is not installed (scripts/install_dlss.ps1): the build anti-aliases with TSR instead of DLAA.'
}

# The scenario's record (FUCPrepared, Content/SCPrepared/<name>.json) names its prepared levels; it and each level carry the
# prep format (FUCPrepared::FormatVersion; a level's <level>.cookformat beside its .umap). An older preparation keeps
# outdated screens and pooled meshes, so it is refused rather than packaged.
$Prepare = "UnrealEditor-Cmd game\UnrealCitizen.uproject -run=SCPrepare -Scenario=$Scenario -AllowCommandletRendering -unattended"
$Header = Join-Path $Root 'game\Source\UnrealCitizen\Public\UCScenario.h'
$Format = [int]([regex]::Match((Get-Content -Raw $Header), 'FormatVersion = (\d+);').Groups[1].Value)
if (-not $Format) { throw "No FormatVersion in $Header" }
$RecordFile = Join-Path $Root "game\Content\SCPrepared\$Scenario.json"
if (-not (Test-Path $RecordFile)) { throw "Scenario $Scenario is not prepared (no $RecordFile): run $Prepare" }
$Record = Get-Content -Raw $RecordFile | ConvertFrom-Json
if ([int]$Record.format -ne $Format) { throw "Scenario $Scenario was prepared with format $($Record.format), current $Format`: run $Prepare" }
$Definition = Join-Path $Root "game\Config\Scenarios\$Scenario.json"
if ((Test-Path $Definition) -and ([IO.File]::ReadAllText($Definition) -ne $Record.definition)) {
    throw "Scenario $Scenario changed since it was prepared: run $Prepare"
}
$Levels = @($Record.vehicle_level) + @($Record.environment_level | Where-Object { $_ })
foreach ($Level in $Levels) {
    $File = Join-Path $Root ('game\Content' + $Level.Substring(5).Replace('/', '\') + '.umap')
    if (-not (Test-Path $File)) { throw "$Level is not prepared ($File): run $Prepare" }
    $Stamp = [IO.Path]::ChangeExtension($File, '.cookformat')
    $Have = if (Test-Path $Stamp) { [int](Get-Content -Raw $Stamp).Trim() } else { 0 }
    if ($Have -ne $Format) { throw "$Level is stale (format $Have, current $Format): run $Prepare -Force" }
}
$Maps = '/Game/Maps/Viewer+' + ($Levels -join '+')
$Extra = ' -SCMaxQuality'

# The built FX must carry the current generated contract (WP-E1 E1-6 review): systems built for an older runtime contract load but
# play with stale behaviour, so they are refused here (rebuild them with -run=SCFxBuild).
$Editor = Join-Path $Engine 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
& $Editor $Project -run=SCFxBuild -VerifyContract -unattended -nosplash -nopause
if ($LASTEXITCODE -ne 0) { throw "Stale FX under /Game/SC/FX (SC_FX_STALE in the log): rebuild them with -run=SCFxBuild first." }

& (Join-Path $Engine 'Engine\Build\BatchFiles\RunUAT.bat') BuildCookRun "-project=$Project" "-platform=$Platform" "-clientconfig=$Config" `
    -build -cook -stage -pak -iostore -package -archive "-archivedirectory=$Out" "-map=$Maps" -nocompileeditor -unattended -utf8output `
    "-cmdline=-Scenario=$Scenario$Extra"
if ($LASTEXITCODE -ne 0) { throw "BuildCookRun failed ($LASTEXITCODE)" }
# A Windows build does not read the staged command line (UECommandLine.txt): the launcher passes it.
$Player = ' -SCShowcase'
if ($PlayerLoadout) { $Player += " -SCPlayerLoadout=$PlayerLoadout" }
if ($PlayerWeapons.Count) { $Player += ' -SCPlayerWeapons=' + ($PlayerWeapons -join ',') }
$Launcher = "@echo off`r`nrem UnrealCitizen showcase: scenario $Scenario ($Config).`r`nstart `"`" `"%~dp0UnrealCitizen.exe`" -Scenario=$Scenario$Extra$Player %*`r`n"
[IO.File]::WriteAllText((Join-Path $Out 'Play_UnrealCitizen.bat'), $Launcher)
Write-Output "PACKAGED $Out"
