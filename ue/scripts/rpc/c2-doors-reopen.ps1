# C2 doors opened again (RPC driver): the C2 Hercules in hangar_xltop_001, off screen, first person.
#   Import-Module ue/scripts/rpc/SCRpc.psm1
#   $p = Start-SCViewer -Port 8771 -ShotDir <dir> -Log <log> -Extra '-SCLoad=CRUS_Starlifter_C2 -SCEnvironment=<hangar socpak> -SCCamera=first'
#   Invoke-SC /wait @{ until = 'idle'; timeout_s = 1500 }
#   ue/scripts/rpc/c2-doors-reopen.ps1 -Port 8771 -Log <log> -ShotDir <dir> [-Cycles 3] [-Sensor <label>] [-Pad <label>]
# 1 the doors: FP_DOOR_LIST, the proximity sensors (sc.Doors.Sensors: box, exit delay, doors); 2 a sensor door (the first
#   door labelled -Sensor that a sensor drives): Cycles times the player walks in (put in the sensor's box), stays until it
#   is open, leaves (put 15 m above, outside every sensor) until it has closed; 3 a pad door (the first labelled -Pad that no
#   sensor drives): Cycles presses to open and to close (sc.FP.Door, the pads' path). The door is traced every tick
#   (sc.Doors.Trace: DOOR_TICK recorded state, clip, playhead, joint tip; DOOR_AUDIO_EVENT), each start logs its clip and
#   start frame (DOOR_CLIP_START rig=), the sounds played are AUDIO_PLAY, DOOR_POSE is polled every ~50 ms and the voices
#   0.5 s in (sc.Audio.List: the sound's distance and gain at the listener; the pad door's phases stand at its joint). A
#   phase passes (PHASE_OK) when its rig had exactly one clip start, at the start of play (frame 0 forward, the last frame
#   backwards), the ticks ran it to its end, its clip crossed an audio event whose trigger then played (AUDIO_PLAY, not
#   AUDIO_SKIP) and the recorded state ends as the phase's; the script exits 1 when a phase failed. Screenshots of the
#   sensor door's second opening. An older build without sc.Doors.Sensors / the trace falls back to probing each
#   candidate's joint (a first cycle of every door) and to the polled samples.
param([int]$Port = 8771, [string]$Log = 'doors-reopen.log', [int]$Cycles = 3,
      [string]$Sensor = 'CRUS Starlifter CircleDoor', [string]$Pad = 'crus starlifter door component 4m',
      [string]$ShotDir = "$PSScriptRoot\..\..\..\out\doors-reopen")
Import-Module "$PSScriptRoot\SCRpc.psm1" -Force
Set-SCRpcPort $Port
$LogPath = (Resolve-Path "$PSScriptRoot\..\..\SCViewer\Saved\Logs").Path + "\$Log"
function C([string]$Cmd) { (Invoke-SC /console @{ cmd = $Cmd }).lines }
function Shot([string]$Name) { [void](Invoke-SC /screenshot @{ name = $Name }); $Name }
function Pose([int]$Door) { "$(C "sc.Doors.Pose $Door" | Select-String 'DOOR_POSE' | Select-Object -First 1)" -replace '.*DOOR_POSE ', '' }
function Field([string]$Line, [string]$Name) { [regex]::Match($Line, "(?:^| )$Name=(\S+)").Groups[1].Value }
function LogSize() { (Get-Item $LogPath).Length }
function LogSince([long]$From) {
    # The log written since byte From (the viewer keeps the file open for writing).
    $s = [System.IO.File]::Open($LogPath, 'Open', 'Read', 'ReadWrite')
    try { [void]$s.Seek($From, 'Begin'); (New-Object System.IO.StreamReader($s)).ReadToEnd() -split "`r?`n" } finally { $s.Dispose() }
}
function Phase([int]$Door, [string]$Tag, [bool]$WantOpen, [scriptblock]$Act, [double]$Seconds, [double[]]$ShotAt = @()) {
    # Act starts the phase; the door is then sampled until it rests in the wanted state (or Seconds pass). About 0.5 s in, the
    # playing voices (sc.Audio.List): the door sound's gain where the listener stands.
    $owner = Field (Pose $Door) 'owner'; $rig = Field (Pose $Door) 'rig'
    $mark = LogSize
    $t0 = Get-Date; & $Act
    $samples = @(); $i = 0; $restSince = $null; $voices = $null
    while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
        $t = ((Get-Date) - $t0).TotalSeconds
        if (-not $voices -and $t -ge 0.5) { $voices = (C 'sc.Audio.List') -join "`n" }
        $p = Pose $Door; $samples += [pscustomobject]@{ t = $t; line = $p; open = Field $p 'open'; clip = Field $p 'clip'; frame = [double](Field $p 'frame') }
        if ($i -lt $ShotAt.Count -and $t -ge $ShotAt[$i]) { [void](Shot "$Tag-$('{0:N1}' -f $ShotAt[$i])s"); $i++ }
        $resting = (Field $p 'clip') -eq '-' -and ((Field $p 'open') -eq '1') -eq $WantOpen
        if ($resting -and -not $restSince) { $restSince = $t }
        if (-not $resting) { $restSince = $null }
        if ($restSince -and $t - $restSince -gt 0.6 -and $i -ge $ShotAt.Count) { break }
        Start-Sleep -Milliseconds 50
    }
    Start-Sleep -Milliseconds 400
    $since = @(LogSince $mark)
    $ticks = @($since | Where-Object { $_ -match "DOOR_TICK door=$Door " } | ForEach-Object { $_ -replace '^.*?DOOR_TICK', 'DOOR_TICK' })
    $events = @($since | Where-Object { $_ -match "DOOR_AUDIO_EVENT door=$Door " } | ForEach-Object { $_ -replace '^.*?DOOR_AUDIO_EVENT', 'DOOR_AUDIO_EVENT' })
    $starts = @($since | Where-Object { $_ -match 'DOOR_CLIP_START' -and $_.Contains("rig=$rig ") } | ForEach-Object { $_ -replace '^.*?DOOR_CLIP_START', 'DOOR_CLIP_START' })
    $triggers = @($events | ForEach-Object { Field $_ 'trigger' } | Select-Object -Unique)
    # The door's sounds played (not skipped): AUDIO_PLAY at the door's rig owner with a trigger this door's clips crossed
    # (an older build without the trace: any AUDIO_PLAY at the owner).
    $sounds = @($since | Where-Object { $_ -match 'AUDIO_PLAY ' -and $_.Contains($owner) -and (-not $Traced -or $triggers -contains (Field ($_ -replace '^.*?AUDIO_PLAY ', '') 'trigger')) } |
        ForEach-Object { ($_ -replace '^.*?(AUDIO_)', '$1') -replace ' media=\d+', '' -replace ' bus=.*', '' })
    $gains = @(foreach ($tr in $triggers) { [regex]::Matches("$voices", "#\d+ $([regex]::Escape($tr)) `"[^`"]*`" dist_m=(\S+) gain_db=(\S+)") | ForEach-Object { "$tr dist_m=$($_.Groups[1].Value) gain_db=$($_.Groups[2].Value)" } })
    $playing = @($samples | Where-Object { $_.clip -ne '-' })
    $endState = (($samples | Select-Object -Last 1).open -eq '1') -eq $WantOpen
    # From the start pose: exactly one clip start on the door's rig, at the start of play (frame 0 forward, the last frame
    # backwards); the ticks advance it to its end (the last DOOR_TICK ended=1). An older build without the trace: the polled
    # samples must show the clip at two playheads.
    $fromStart = -not $Traced -or $starts.Count -eq 1
    foreach ($s in $starts) {
        $from = [double](Field $s 'from'); $last = [double](Field $s 'last'); $speed = [double](Field $s 'speed')
        if ($(if ($speed -lt 0) { $from -lt $last - 0.5 } else { $from -gt 0.5 })) { $fromStart = $false }
    }
    $advanced = if ($Traced) { $ticks.Count -ge 2 -and (Field ($ticks | Select-Object -Last 1) 'ended') -eq '1' }
                else { $playing.Count -ge 2 -and ($playing | Select-Object -Last 1).frame -ne ($playing | Select-Object -First 1).frame }
    $ok = $advanced -and $fromStart -and $endState -and $sounds.Count -gt 0 -and (-not $Traced -or $events.Count -ge 1)
    if (-not $ok) { $script:Failed++ }
    foreach ($s in $samples) { "{0,6:N2}s {1}" -f $s.t, ($s.line -replace ' owner=\S+', '') }
    ($starts + $ticks + $events + $sounds) | ForEach-Object { "   $_" }
    $gains | ForEach-Object { "   VOICE_AT_0.5s $_" }
    "PHASE {0} door={1} want_open={2} samples={3} playing_samples={4} clip_starts={5} from_start={6} ticks={7} ended={8} audio_events={9} sounds={10} end_state_ok={11} {12}" -f $Tag, $Door,
        [int]$WantOpen, $samples.Count, $playing.Count, $starts.Count, [int]$fromStart, $ticks.Count, [int]$advanced, $events.Count, $sounds.Count, [int]$endState,
        $(if ($ok) { 'PHASE_OK' } else { 'PHASE_FAIL' })
}
$script:Failed = 0

New-Item -ItemType Directory -Force $ShotDir | Out-Null
[void](C 'sc.Audio.Log 1'); [void](C 'sc.Audio.Unfocused 1')
"== 1 doors"
$list = C 'sc.FP.Spawns' | Select-String 'FP_DOOR_LIST'
$list | ForEach-Object { "$_" -replace '.*FP_DOOR_LIST', 'FP_DOOR_LIST' }
function Indices([string]$Label) { @($list | Where-Object { $_ -match "label=`"$([regex]::Escape($Label))`"" } | ForEach-Object { [int]("$_" -replace '.*index=(\d+) .*', '$1') }) }
function JointOf([int]$Door) { [double[]]((Field (Pose $Door) 'pos') -split ',') }
function Vec([string]$Text) { [double[]]($Text -split ',') }
function Put([double[]]$P) { [void](C ("sc.FP.Place {0} {1} {2}" -f $P[0], $P[1], $P[2])) }
[void](C 'sc.FP.Noclip 1')
$sensors = @(C 'sc.Doors.Sensors' | Select-String 'DOOR_SENSOR ' | ForEach-Object { "$_" -replace '.*DOOR_SENSOR', 'DOOR_SENSOR' })
$sensors
$Traced = $sensors.Count -gt 0 # a build with sc.Doors.Sensors has the trace (DOOR_TICK, DOOR_AUDIO_EVENT, DOOR_CLIP_START rig=) too
$sensorDoors = @(); $boxes = @{}
foreach ($s in $sensors) {
    foreach ($d in ((Field $s 'doors') -split ',')) {
        if ($d -match '^\d+$') { $sensorDoors += [int]$d; if (-not $boxes.ContainsKey([int]$d)) { $boxes[[int]$d] = $s } }
    }
}
if (-not $sensors) {
    # An older build: which candidates a sensor opens, the player at each joint for 1.5 s, then 15 m above it until the
    # door has closed (a first cycle of each candidate).
    foreach ($d in (Indices $Sensor) + (Indices $Pad)) {
        $j = JointOf $d
        Put $j; Start-Sleep 1.5
        $opened = (Field (Pose $d) 'open') -eq '1'
        Put ([double[]]@($j[0], $j[1], ($j[2] + 1500)))
        for ($w = 0; $w -lt 100 -and (((Field (Pose $d) 'open') -eq '1') -or (Field (Pose $d) 'clip') -ne '-'); $w++) { Start-Sleep -Milliseconds 100 }
        "SENSOR_PROBE door=$d joint=$($j -join ',') opened_by_sensor=$([int]$opened) after=$('{0:N1}' -f ($w / 10))s $(Pose $d)"
        if ($opened) { $sensorDoors += $d }
    }
}
$SensorDoor = (Indices $Sensor | Where-Object { $sensorDoors -contains $_ } | Select-Object -First 1)
$PadDoor = (Indices $Pad | Where-Object { $sensorDoors -notcontains $_ } | Select-Object -First 1)
"sensor door=$SensorDoor pad door=$PadDoor"

"== 2 sensor door $SensorDoor, $Cycles cycles: walk in, wait open, leave, wait closed"
$j = JointOf $SensorDoor
# In: the sensor box's centre (the joint without a box). The second opening is seen from inside the box, 30 cm from its
# end along the way through the door (the horizontal axis across the joint's offset from the box centre: a sliding leaf's
# joint sits beside the doorway), looking at the box centre. Out: 15 m above, outside every sensor.
$in = $j; $view = $null
if ($boxes.ContainsKey($SensorDoor)) {
    $centre = Vec (Field $boxes[$SensorDoor] 'centre'); $half = Vec (Field $boxes[$SensorDoor] 'half')
    $in = $centre
    $axis = if ([math]::Abs($j[0] - $centre[0]) -le [math]::Abs($j[1] - $centre[1])) { 0 } else { 1 }
    $view = [double[]]@($centre[0], $centre[1], $centre[2])
    $view[$axis] = $centre[$axis] - [math]::Max(0, $half[$axis] - 30)
}
$out = [double[]]@($in[0], $in[1], ($in[2] + 1500))
[void](C "sc.Doors.Trace $SensorDoor")
for ($c = 1; $c -le $Cycles; $c++) {
    $shots = @(); $at = $in
    if ($c -eq 2 -and $view) {
        $shots = @(0.05, 0.3, 0.6, 1.0, 1.6, 2.4); $at = $view
        [void](C ("sc.FP.Look {0} 0" -f ([math]::Atan2($in[1] - $view[1], $in[0] - $view[0]) * 180 / [math]::PI)))
    }
    Phase $SensorDoor "sensor-c$c-open" $true { Put $at } 12 $shots
    Phase $SensorDoor "sensor-c$c-close" $false { Put $out } 20
}
"== 3 pad door $PadDoor, $Cycles cycles: press to open, press to close (the listener at the door's joint)"
Put (JointOf $PadDoor); Start-Sleep 0.5
[void](C "sc.Doors.Trace $PadDoor")
for ($c = 1; $c -le $Cycles; $c++) {
    Phase $PadDoor "pad-c$c-open" $true { [void](C "sc.FP.Door $PadDoor") } 12
    Phase $PadDoor "pad-c$c-close" $false { [void](C "sc.FP.Door $PadDoor") } 12
}
[void](C 'sc.Doors.Trace -1')
"== summary"
"FAILED_PHASES $script:Failed"
if ($script:Failed) { exit 1 }
