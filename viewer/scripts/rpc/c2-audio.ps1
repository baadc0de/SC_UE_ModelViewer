# WP-A1 A1-1 verification: the C2's audio in first person (RPC driver), with submix recordings as evidence.
#   Import-Module viewer/scripts/rpc/SCRpc.psm1
#   $p = Start-SCViewer -Port 8771 -Extra '-SCLoad=CRUS_Starlifter_C2 -SCEnvironment=<hangar socpak> -SCCamera=first'
#   Invoke-SC /wait @{ until = 'idle'; timeout_s = 1500 }
#   viewer/scripts/rpc/c2-audio.ps1 -Port 8771 -Out <dir under out/audio>
# 1. Reverb probes: placed ambience off, one door sound posted in front of the listener in rooms of different
#    environments, each recorded to reverb-<room>.wav (the tail after the sound ends measures the room's reverb), and
#    again with the aux sends off to reverb-<room>-dry.wav (the dry path alone; the difference is the room's reverb).
# 2. The walk: from the top deck into the lift (call button, door), down to the lower deck, out into the hold, recorded
#    to c2-walk.wav; marks.json gives each segment's start (s from the recording start) for the level analysis.
# -Trace (A1-2): the walk also writes limiter-trace.csv (master and main output per 10 ms), mix-trace.csv (every bus
# effect chain per render buffer: limiter gain, meters, side-chain gains, EQ bands) and logs AUDIO_HEALTH for it.
# Positions, views and stop numbers are c2-lift.ps1's (LIVE 4.10.2 C2). The trigger named below is only the probe's
# test sound; the viewer's code names none.
param([int]$Port = 8771, [Parameter(Mandatory)][string]$Out, [switch]$Trace)
Import-Module "$PSScriptRoot\SCRpc.psm1" -Force
Set-SCRpcPort $Port
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
function C([string]$Cmd) { (Invoke-SC /console @{ cmd = $Cmd }).lines }
function WaitLift([int]$Timeout = 90) {
    $w = Invoke-SC /wait @{ until = 'lift_idle'; timeout_s = $Timeout } -TimeoutSec ($Timeout + 30)
    "lift_idle ok=$($w.ok) waited=$($w.waited_s) state=$($w.lifts[0].state) at=$($w.lifts[0].at)"
}
function Aim([double]$Yaw, [double[]]$Pitches, [string]$Panel) {
    foreach ($p in $Pitches) {
        [void](C "sc.FP.Look $Yaw $p"); Start-Sleep -Milliseconds 250
        $probe = C 'sc.FP.Probe' | Select-Object -First 1
        if ($probe -match "panel=$Panel") { return ($probe -replace ' chain=.*', '') }
    }
    throw "panel $Panel not found at yaw $Yaw"
}
function Row([int]$Index) {
    C "sc.Lift.PanelAim 0 $Index" | Select-String 'panel_aim'; Start-Sleep -Milliseconds 2500
    (C 'sc.FP.Probe' | Select-Object -First 1) -replace ' chain=.*', ''
}
function Watch([int]$Stop) {
    $deadline = (Get-Date).AddSeconds(90); $last = ''
    while ((Get-Date) -lt $deadline) {
        $l = (Invoke-SC /state).lifts[0]
        if ($l.state -ne $last) { "  $(Get-Date -Format HH:mm:ss.fff) $($l.state) s=$($l.s_m)"; $last = $l.state }
        if ($l.state -eq 'Open' -and $l.at -eq $Stop) { return }
        Start-Sleep -Milliseconds 80
    }
    throw "lift did not open at stop $Stop"
}
$CallHold = '4d52b73d-3b67-9a88-213d-f4c8017747a4'; $CallTop = '49b375f1-d5fb-1ee4-5f46-9a8fcffce2b2'
$Sweep = @(-6, -8.5, -10, -12, -14, -16)
$Probe = 'Play_SSMP_CRUS_StarLifter_Elevator_Door_Top_Open'

C 'sc.Audio.Unfocused 1' # the off-screen viewer has no focus; UE mutes an unfocused window
C 'sc.Audio.Rooms'

# ---------------------------------------------------------------------------------------- 1. reverb probes per room
[void](C 'sc.Audio.Ambience 0'); [void](C 'sc.Audio.Stop'); Start-Sleep 1
$spawns = C 'sc.FP.Spawns' | Select-String 'FP_SPAWN_LIST' | ForEach-Object { $_.Line }
# Where each probe stands: an interior spawn by its label, or a vehicle-relative floor hint (cm) where the spawn finds no
# clear capsule (c2-lift.ps1's hold landing; the top deck's interior spawn point).
# The top deck's floor hint is the hold landing's x / y moved onto the top deck in front of the lift (the carriage's top
# stop stands at z -60 cm, capsule centre).
$at = [ordered]@{ bridge_c2 = 'spawn'; top_deck_c2 = 'sc.FP.Teleport -300 -450 -150'; hold_c2 = 'sc.FP.Teleport -300 -400 -887'; systems_c2 = 'spawn' }
foreach ($room in $at.Keys) {
    if ($at[$room] -eq 'spawn') {
        $line = $spawns | Where-Object { $_ -match "kind=Interior" -and $_ -match $room } | Select-Object -First 1
        if (-not $line) { "no interior spawn for $room"; continue }
        [void](C "sc.FP.Spawn $([int]($line -replace '.*index=(\d+).*', '$1'))")
    } else { [void](C $at[$room]) }
    Start-Sleep 2; C 'sc.FP.Where' | Select-Object -First 1
    foreach ($pass in @('', '-dry')) {
        [void](C "sc.Audio.Sends $(if ($pass) { 0 } else { 1 })")
        [void](C 'sc.Audio.Record start 15'); Start-Sleep -Milliseconds 300
        [void](C 'sc.Audio.Meter start'); [void](C "sc.Audio.Play $Probe"); Start-Sleep 9
        C 'sc.Audio.Meter stop' | Select-String 'AUDIO_METER bus' | Where-Object { $_ -notmatch 'peak_db=-200' }
        C "sc.Audio.Record stop $Out\reverb-$room$pass.wav" | Select-String 'AUDIO_RECORD'
    }
    [void](C 'sc.Audio.Sends 1')
    C 'sc.Audio.List' | Select-Object -First 3
}
[void](C 'sc.Audio.Ambience 1')

# ---------------------------------------------------------------------------------------- 2. the walk
"== to the top deck through the lift (setup, not recorded)"
[void](C 'sc.FP.Teleport -300 -400 -887'); Start-Sleep 1
Aim -58.9 $Sweep $CallHold; C 'sc.FP.Interact' | Select-String 'FP_PANEL_PRESS'; WaitLift
[void](C 'sc.FP.Look -90 0'); [void](C 'sc.FP.Move 1 0 1.2'); Start-Sleep 2.5
Row 0; C 'sc.FP.Interact' | Select-String 'FLOOR_PANEL_PRESS'; [void](C 'sc.FP.Look 90 0'); Watch 2
[void](C 'sc.FP.Look 90 0'); [void](C 'sc.FP.Move 1 0 1.3'); Start-Sleep 2.5; WaitLift 60
[void](C 'sc.FP.Move 1 0 2.0'); Start-Sleep 3 # further out onto the top deck
C 'sc.Lift.Doors 0 close' | Select-String 'LIFT_CMD'; WaitLift 60 # the carriage waits at the top deck, doors closed
$marks = [ordered]@{}
[void](C 'sc.Audio.Health reset')
if ($Trace) { C 'sc.Audio.LimiterTrace start' | Select-String 'AUDIO_LIMITER_TRACE'; C 'sc.Audio.MixTrace start' | Select-String 'AUDIO_MIX_TRACE' }
$t0 = Get-Date
[void](C 'sc.Audio.Record start 150')
function Mark([string]$Name) { $marks[$Name] = [math]::Round(((Get-Date) - $t0).TotalSeconds, 2); "mark $Name $($marks[$Name])" }
# The bus levels of a segment (AUDIO_METER per bus, the loudest first).
function Meter([string]$What) {
    C 'sc.Audio.Meter stop' | Select-String 'AUDIO_METER bus' | Where-Object { $_ -notmatch 'peak_db=-200' } |
        Sort-Object { [double](($_ -replace '.*rms_db=(\S+).*', '$1')) } -Descending | Select-Object -First 8 | ForEach-Object { "  $What $_" }
}
try {
    Mark 'top_deck'; [void](C 'sc.Audio.Meter start'); Start-Sleep 8; C 'sc.Audio.List' | Select-Object -First 1; Meter 'top_deck'
    C 'sc.Audio.List' | Select-String 'gain_db' | Sort-Object { [double](($_ -replace '.*gain_db=(\S+).*', '$1')) } -Descending | Select-Object -First 8
    Mark 'walk_to_lift'; [void](C 'sc.FP.Look -90 0'); [void](C 'sc.FP.Move 1 0 1.6'); Start-Sleep 2.5
    # The top call button's press path (its exported links, as E on it does): the press sound, then the doors open.
    Mark 'call_button'; C "sc.Lift.Send $CallTop Descend" | Select-String 'LIFT_CMD|LINK_SEND'; Watch 2
    Mark 'doors_open'; Start-Sleep 2
    Mark 'walk_in'; [void](C 'sc.FP.Look -90 0'); [void](C 'sc.FP.Move 1 0 1.6'); Start-Sleep 2.5; C 'sc.Lift.Where' | Select-String 'player base'
    Mark 'floor_panel'; Row 1 | Out-Null; C 'sc.FP.Interact' | Select-String 'FLOOR_PANEL_PRESS'
    Mark 'ride_down'; [void](C 'sc.FP.Look 90 0'); Watch 1
    Mark 'hold_doors_open'; Start-Sleep 1.5
    Mark 'walk_out'; [void](C 'sc.FP.Move 1 0 1.3'); Start-Sleep 2.5
    Mark 'hold'; [void](C 'sc.Audio.Meter start'); Start-Sleep 8; C 'sc.Audio.List' | Select-Object -First 1; Meter 'hold'
    C 'sc.Audio.List' | Select-String 'gain_db' | Sort-Object { [double](($_ -replace '.*gain_db=(\S+).*', '$1')) } -Descending | Select-Object -First 8
    Mark 'end'
} finally {
    C "sc.Audio.Record stop $Out\c2-walk.wav" | Select-String 'AUDIO_RECORD'
    if ($Trace) {
        C "sc.Audio.LimiterTrace stop $Out\limiter-trace.csv" | Select-String 'AUDIO_LIMITER_TRACE'
        C "sc.Audio.MixTrace stop $Out\mix-trace.csv" | Select-String 'AUDIO_MIX_TRACE'
        C 'sc.Audio.Rtpc' | Select-String 'AUDIO_RTPC'
    }
    C 'sc.Audio.Health' | Select-String 'AUDIO_HEALTH'
    $marks | ConvertTo-Json | Set-Content -Encoding utf8 "$Out\marks.json"
    $marks
}
