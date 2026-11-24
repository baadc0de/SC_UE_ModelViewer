# Package the Win64 showcase: UnrealCitizen running one prepared scenario (game/Config/Scenarios/<name>.json), started
# with -Scenario=<name>, which streams the scenario's prepared levels. Prepare it first (WP-O1-4, the prep step):
#   UnrealEditor-Cmd game/UnrealCitizen.uproject -run=SCPrepare -Scenario=<name> -AllowCommandletRendering -unattended
# A standalone directory (out/win64/<scenario>): Nanite, Lumen GI and reflections with hardware ray tracing at
# -SCMaxQuality (Cinematic, hit lighting; SCLook::ApplyMaxQuality) and DLAA (the NVIDIA DLSS plugin at 100 %,
# viewer/README.md). <Out>\Play_UnrealCitizen.bat starts it as the showcase (-SCShowcase: no viewer chrome; the scenario's
# camera; C third person, Z orbit, T teleport, F torch, LMB fire, RMB aim). The player character is prepared with the
# scenario (WP-O1-4b-2: its own level, named by the record): the definition's "player" block picks the gender, the game
# loadout, the weapons kept (entity class names) and whether their spare magazines are carried on the armour.
# The engine is the SCViewer fork of UE 5.9 built from source (plan D17): -Engine, else $env:UE_ENGINE_ROOT, else
# %USERPROFILE%\UnrealEngine-5.9-src.
# Usage: game/scripts/package_showcase.ps1 -Scenario golem_showcase
#        [-Config Development|Shipping] [-Out <dir>] [-Engine <UE 5.9 source fork>]
param(
    [ValidateSet('Win64')][string]$Platform = 'Win64',
    [Parameter(Mandatory = $true)][string]$Scenario,
    [string]$Out = '',
    [string]$Engine = $(if ($env:UE_ENGINE_ROOT) { $env:UE_ENGINE_ROOT } else { Join-Path $env:USERPROFILE 'UnrealEngine-5.9-src' }),
    [string]$Config = 'Development'
)
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$Project = Join-Path $Root 'game\UnrealCitizen.uproject'
if (-not $Out) { $Out = Join-Path $Root "out\win64\$Scenario" }
# Absolute here: UAT resolves a relative -archivedirectory against the engine root, not this directory.
$Out = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Out)
# The archive directory is UAT's (DeploymentContext): -Out itself when a part of its path starts with the platform's name
# (Windows / Win64), else -Out\Windows.
$ArchiveDir = if ($Out.Split('\', [StringSplitOptions]::RemoveEmptyEntries) | Where-Object { $_ -like 'Windows*' -or $_ -like 'Win64*' }) { $Out } else { Join-Path $Out 'Windows' }
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
$Record = [IO.File]::ReadAllText($RecordFile) | ConvertFrom-Json
if ([int]$Record.format -ne $Format) { throw "Scenario $Scenario was prepared with format $($Record.format), current $Format`: run $Prepare" }
$Definition = Join-Path $Root "game\Config\Scenarios\$Scenario.json"
if (-not (Test-Path $Definition)) { throw "No scenario $Definition" }
# Character for character, line ends aside (a checkout can turn LF into CRLF), as the game compares (FUCPrepared::Problem).
function ConvertTo-Comparable([string]$Text) { $Text.Replace("`r`n", "`n") }
if ((ConvertTo-Comparable ([IO.File]::ReadAllText($Definition))) -cne (ConvertTo-Comparable $Record.definition)) { throw "Scenario $Scenario changed since it was prepared: run $Prepare" }
# Its cargo setup (Config/Cargo/<cargo>.json) as prepared: the packaged game has only the record's copy.
$CargoName = (ConvertFrom-Json $Record.definition).cargo
$CargoText = ''
if ($CargoName) {
    $CargoFile = Join-Path $Root "game\Config\Cargo\$CargoName.json"
    if (-not (Test-Path $CargoFile)) { throw "Scenario $Scenario names the cargo setup $CargoName, but there is no $CargoFile" }
    $CargoText = [IO.File]::ReadAllText($CargoFile)
}
if ((ConvertTo-Comparable $CargoText) -cne (ConvertTo-Comparable ([string]$Record.cargo_definition))) { throw "The cargo setup of scenario $Scenario changed since it was prepared: run $Prepare" }
if (-not $Record.player_level) { throw "Scenario $Scenario's record names no player body level: run $Prepare" }
if (-not $Record.traversal_clips) { throw "Scenario $Scenario's record names no traversal clips: run $Prepare" }
$Levels = @($Record.vehicle_level, $Record.player_level) + @($Record.environment_level | Where-Object { $_ })
# The cargo setup's prepared boxes (WP-O1-4b-3): a level per box entity, streamed as the box's template.
if ($Record.cargo_levels) { $Levels += @($Record.cargo_levels.PSObject.Properties | ForEach-Object { $_.Value }) }
# Its obstacles' prepared levels (WP-O1-4b-3c): streamed as extra instances.
if ($Record.obstacle_levels) { $Levels += @($Record.obstacle_levels.PSObject.Properties | ForEach-Object { $_.Value }) | Where-Object { $Levels -notcontains $_ } }
# The levels are cooked as maps; the traversal clips (an asset under /Game/SC/Players/Traversal, always cooked) and the music
# waves (assets under /Game/SC/Music, always cooked; WP-O1-4b-3c) are checked alike.
$Assets = @($Record.traversal_clips)
if ($Record.music_waves) { $Assets += @($Record.music_waves.PSObject.Properties | ForEach-Object { $_.Value }) }
foreach ($Level in $Levels + $Assets) {
    $Extension = if ($Assets -contains $Level) { '.uasset' } else { '.umap' }
    $File = Join-Path $Root ('game\Content' + $Level.Substring(5).Replace('/', '\') + $Extension)
    if (-not (Test-Path $File)) { throw "$Level is not prepared ($File): run $Prepare" }
    $Stamp = [IO.Path]::ChangeExtension($File, '.cookformat')
    # A complete stamp (FUCPrepared::IsLevelCurrent): the format on line 1, the archive build on line 2, the save's id on line 3.
    $Lines = if (Test-Path $Stamp) { @(Get-Content $Stamp) } else { @() }
    $Have = if ($Lines.Count -ge 1 -and $Lines[0].Trim() -match '^\d+$') { [int]$Lines[0].Trim() } else { 0 }
    if ($Have -ne $Format -or $Lines.Count -lt 3 -or -not $Lines[1].Trim() -or -not $Lines[2].Trim()) {
        throw "$Level is stale or not finished (stamp format $Have, current $Format): run $Prepare -Force"
    }
}
# The traversal clips were read from these very levels (FUCPrepared::TraversalSources): their stamp's fourth line names the
# ids (third lines) of the body, vehicle and environment levels; a level saved again since makes them stale.
function Get-StampLine([string]$Package, [int]$Index) {
    if (-not $Package) { return '' }
    $Stamp = Join-Path $Root ('game\Content' + $Package.Substring(5).Replace('/', '\') + '.cookformat')
    $Lines = if (Test-Path $Stamp) { @(Get-Content $Stamp) } else { @() }
    if ($Lines.Count -gt $Index) { $Lines[$Index].Trim() } else { '' }
}
$Sources = (Get-StampLine $Record.player_level 2) + '|' + (Get-StampLine $Record.vehicle_level 2) + '|' + (Get-StampLine $Record.environment_level 2)
if ((Get-StampLine $Record.traversal_clips 3) -cne $Sources) { throw "The traversal clips $($Record.traversal_clips) were read from other levels than the record's: run $Prepare" }
$Maps = '/Game/Maps/Viewer+' + ($Levels -join '+')
$Extra = ' -SCMaxQuality'

# The built FX must carry the current generated contract (WP-E1 E1-6 review): systems built for an older runtime contract load but
# play with stale behaviour, so they are refused here (rebuild them with -run=SCFxBuild).
$Editor = Join-Path $Engine 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
& $Editor $Project -run=SCFxBuild -VerifyContract -unattended -nosplash -nopause
if ($LASTEXITCODE -ne 0) { throw "Stale FX under /Game/SC/FX (SC_FX_STALE in the log): rebuild them with -run=SCFxBuild first." }
# The record runs as the game decides it (FUCPrepared::Problem: every level, box, obstacle, music wave and Garage detail).
& $Editor $Project -run=SCPrepare "-Scenario=$Scenario" -Check -unattended -nosplash -nopause
if ($LASTEXITCODE -ne 0) { throw "Scenario $Scenario's prepared record is stale (SC_PREPARE_CHECK in the log): run $Prepare" }

& (Join-Path $Engine 'Engine\Build\BatchFiles\RunUAT.bat') BuildCookRun "-project=$Project" "-platform=$Platform" "-clientconfig=$Config" `
    -build -cook -stage -pak -iostore -package -archive "-archivedirectory=$Out" "-map=$Maps" -nocompileeditor -unattended -utf8output `
    "-cmdline=-Scenario=$Scenario$Extra"
if ($LASTEXITCODE -ne 0) { throw "BuildCookRun failed ($LASTEXITCODE)" }
# The game has no archive library since WP-O1-4c-3b; archiving leaves files of an earlier package in place, so a
# scbridge.dll / .pdb from before the split would still ship: remove them from this game's binaries only (-Out may be
# shared with other packages, which need theirs).
foreach ($Stale in 'scbridge.dll', 'scbridge.pdb') {
    $Path = Join-Path $ArchiveDir "UnrealCitizen\Binaries\Win64\$Stale"
    if (Test-Path -LiteralPath $Path) { Remove-Item -LiteralPath $Path -Force; Write-Output "REMOVED_STALE $Path" }
}
# A Windows build does not read the staged command line (UECommandLine.txt): the launcher passes it.
$Launcher = "@echo off`r`nrem UnrealCitizen showcase: scenario $Scenario ($Config).`r`nstart `"`" `"%~dp0UnrealCitizen.exe`" -Scenario=$Scenario$Extra -SCShowcase %*`r`n"
[IO.File]::WriteAllText((Join-Path $ArchiveDir 'Play_UnrealCitizen.bat'), $Launcher)
Write-Output "PACKAGED $ArchiveDir"
