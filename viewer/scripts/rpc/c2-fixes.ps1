# C2 fixes verification (RPC driver): the C2 Hercules in hangar_xltop_001, off screen, first person.
#   Import-Module viewer/scripts/rpc/SCRpc.psm1
#   $p = Start-SCViewer -Port 8771 -ShotDir <dir> -Log <log> -Extra '-SCLoad=CRUS_Starlifter_C2 -SCEnvironment=<hangar socpak> -SCCamera=first'
#   Invoke-SC /wait @{ until = 'idle'; timeout_s = 1500 }
#   viewer/scripts/rpc/c2-fixes.ps1 -Port 8771 -Log <log> [-From <step>]
# 1-3 doors: the rear ramp at spawn (closed, the data's IsOpened), opened and closed by its own press (DOOR_POSE traces: no
#   jump of its joint), 4-5 the front: at spawn, then the front hatch's press opening the doors and the ramp through its
#   item-port links, and its panel, 6 the hold's light panel: each row aimed and pressed (LIGHT_PANEL_PRESS,
#   LIGHT_GROUP_STATE), the hold seen after each (region brightness), 7 the lift: top deck -> Exit Ship and lower deck ->
#   Exit Ship from where the rider was caught before (LIFT_RIDE_LEG spread). Camera points are vehicle-relative cm and
#   degrees that frame the parts (found with sc.FP.Probe / sc.Lift.Panels); nothing the viewer computes reads them.
param([int]$Port = 8771, [int]$From = 1, [string]$Log = 'c2fix-run1.log',
      [string]$ShotDir = "$PSScriptRoot\..\..\..\out\c2-fixes\run1")
Import-Module "$PSScriptRoot\SCRpc.psm1" -Force
Set-SCRpcPort $Port
Add-Type -AssemblyName System.Drawing
$LogPath = (Resolve-Path "$PSScriptRoot\..\..\Saved\Logs").Path + "\$Log"
function C([string]$Cmd) { (Invoke-SC /console @{ cmd = $Cmd }).lines }
function Shot([string]$Name, [switch]$Ui) { [void](Invoke-SC /screenshot @{ name = $Name; ui = [bool]$Ui }); $Name }
$script:N = 0
function Step([string]$Text) { $script:N++; $run = $script:N -ge $From; if ($run) { Write-Host "== $($script:N) $Text" }; $run }
function DoorIndex([string]$Label) {
    $l = C 'sc.FP.Spawns' | Select-String 'FP_DOOR_LIST' | Where-Object { $_ -match [regex]::Escape($Label) } | Select-Object -First 1
    [int]("$l" -replace '.*index=(\d+) .*', '$1')
}
function Pose([int]$Door) { (C "sc.Doors.Pose $Door" | Select-String 'DOOR_POSE' | Select-Object -First 1) -replace '.*DOOR_POSE ', '' }
function Trace([int]$Door, [double]$Seconds, [string]$Tag, [double[]]$ShotAt) {
    # The door's joint every ~100 ms while it moves, screenshots at the given times; the largest step between samples
    # against the median step (a snap is one step far larger than the rest).
    $t0 = Get-Date; $prev = $null; $steps = @(); $i = 0; $lines = @()
    while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
        $t = ((Get-Date) - $t0).TotalSeconds
        $p = Pose $Door
        $m = [regex]::Match($p, 'tip=([-\d.]+),([-\d.]+),([-\d.]+)') # a point on the joint's frame: it moves when a hinge turns
        $v = [double[]]@($m.Groups[1].Value, $m.Groups[2].Value, $m.Groups[3].Value)
        if ($prev) { $steps += [math]::Sqrt(($v[0]-$prev[0])*($v[0]-$prev[0]) + ($v[1]-$prev[1])*($v[1]-$prev[1]) + ($v[2]-$prev[2])*($v[2]-$prev[2])) }
        $prev = $v; $lines += ("{0,6:N2}s {1}" -f $t, $p)
        if ($i -lt $ShotAt.Count -and $t -ge $ShotAt[$i]) { [void](Shot "$Tag-$('{0:N1}' -f $ShotAt[$i])s"); $i++ }
        Start-Sleep -Milliseconds 100
    }
    $sorted = $steps | Sort-Object; $median = if ($sorted.Count) { $sorted[[int]($sorted.Count / 2)] } else { 0 }
    $lines | Select-Object -First 3; '   ...'; $lines | Select-Object -Last 2
    "TRACE $Tag samples=$($lines.Count) max_step_cm=$('{0:N1}' -f ($steps | Measure-Object -Maximum).Maximum) median_step_cm=$('{0:N1}' -f $median)"
}
function Brightness([string]$Name) {
    # Mean luminance (sRGB 0-255, Rec. 709 weights) of the whole frame and of its upper and lower halves.
    $f = Join-Path $ShotDir "$Name.png"
    for ($w = 0; $w -lt 40 -and -not (Test-Path $f); $w++) { Start-Sleep -Milliseconds 250 }
    $b = [System.Drawing.Bitmap]::FromFile($f); $sum = @(0.0, 0.0); $n = @(0, 0)
    for ($y = 0; $y -lt $b.Height; $y += 8) { for ($x = 0; $x -lt $b.Width; $x += 8) {
        $c = $b.GetPixel($x, $y); $l = 0.2126 * $c.R + 0.7152 * $c.G + 0.0722 * $c.B; $h = [int]($y -ge $b.Height / 2)
        $sum[$h] += $l; $n[$h]++ } }
    $b.Dispose()
    "BRIGHTNESS $Name all=$('{0:N1}' -f (($sum[0] + $sum[1]) / ($n[0] + $n[1]))) upper=$('{0:N1}' -f ($sum[0] / $n[0])) lower=$('{0:N1}' -f ($sum[1] / $n[1]))"
}

$Rear = DoorIndex 'Rear Ramp'; $Bottom = DoorIndex 'Door Bottom'
# The hold's light panel (sc.Lift.Panels: its index, face centre and facing, vehicle frame) and a view of it from Cm in front.
$panels = C 'sc.Lift.Panels'
$LightPanel = [int]("$($panels | Select-String 'FLOOR_PANEL index=[0-9]+ kind=buttons entity=42b0ae77' | Select-Object -First 1)" -replace '.*FLOOR_PANEL index=([0-9]+) .*', '$1')
$m = [regex]::Match((($panels | Select-String "FLOOR_PANEL_AT index=$LightPanel ") -replace '.*centre=', ''), '([-0-9.]+),([-0-9.]+),([-0-9.]+) normal=([-0-9.]+),([-0-9.]+),([-0-9.]+)')
$PC = 1..3 | ForEach-Object { [double]$m.Groups[$_].Value }; $PN = 4..6 | ForEach-Object { [double]$m.Groups[$_].Value }
function ViewPanel([double]$Cm) {
    # Eye about 70 cm over the capsule centre: the capsule a little below the face, looking straight at it.
    [void](C 'sc.FP.Noclip 1')
    [void](C ("sc.FP.Place {0} {1} {2}" -f ($PC[0] + $PN[0] * $Cm), ($PC[1] + $PN[1] * $Cm), ($PC[2] - 70)))
    [void](C ("sc.FP.Look {0} 0" -f ([math]::Atan2(-$PN[1], -$PN[0]) * 180 / [math]::PI))); Start-Sleep 1.5
}
function Crop([string]$Name, [double]$Fraction = 0.25, [int]$Scale = 3) {
    # The centre of a shot (Fraction of each side), enlarged: the small panel faces.
    $f = Join-Path $ShotDir "$Name.png"
    for ($w = 0; $w -lt 40 -and -not (Test-Path $f); $w++) { Start-Sleep -Milliseconds 250 }
    $b = [System.Drawing.Bitmap]::FromFile($f); $cw = [int]($b.Width * $Fraction); $ch = [int]($b.Height * $Fraction)
    $o = New-Object System.Drawing.Bitmap ($cw * $Scale), ($ch * $Scale); $g = [System.Drawing.Graphics]::FromImage($o)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.DrawImage($b, (New-Object System.Drawing.Rectangle 0, 0, ($cw * $Scale), ($ch * $Scale)), (New-Object System.Drawing.Rectangle ([int](($b.Width - $cw) / 2)), ([int](($b.Height - $ch) / 2)), $cw, $ch), [System.Drawing.GraphicsUnit]::Pixel)
    $o.Save((Join-Path $ShotDir "$Name-crop.png")); $g.Dispose(); $o.Dispose(); $b.Dispose(); "$Name-crop"
}
if (Step 'rear ramp at spawn: closed (IsOpened false), its transition held at the start') {
    C 'sc.FP.Spawns' | Select-String 'FP_DOOR_LIST' | Select-String 'Rear Ramp|Front|Door Bottom'
    "rear=$Rear bottom=$Bottom"; Pose $Rear
    [void](C 'sc.FP.Noclip 1'); [void](C 'sc.FP.Place -6500 1200 -300'); [void](C 'sc.FP.Look -15 -5'); Start-Sleep 2; Shot 'a0-rear-spawn'
}
if (Step 'rear ramp: opened by its press (the pads call the same ToggleDoor), traced') {
    [void](C "sc.FP.Door $Rear"); Trace $Rear 18 'a1-rear-opening' @(0.1, 1.5, 4, 9, 17.5)
}
if (Step 'rear ramp: closed again (its transition backwards), and turned around mid-stroke') {
    [void](C "sc.FP.Door $Rear"); Trace $Rear 6 'a2-rear-closing' @(0.1, 3)
    [void](C "sc.FP.Door $Rear"); Trace $Rear 3 'a3-rear-turned-open' @(0.1)
    [void](C "sc.FP.Door $Rear"); Trace $Rear 12 'a4-rear-closed-again' @(11.5)
}
if (Step 'front at spawn: doors closed, ramp stowed; the front hatch panels') {
    [void](C 'sc.FP.Noclip 1'); [void](C 'sc.FP.Place 4300 900 -950'); [void](C 'sc.FP.Look 195 6'); Start-Sleep 2; Shot 'b0-front-spawn'
    C 'sc.Screens.States' | Select-String 'Door_Bottom'
    ViewPanel 90; Shot 'b0b-hold-front-panels-spawn'; Crop 'b0b-hold-front-panels-spawn' 0.4 2
}
if (Step 'front: the hatch press opens the hatch, the doors and the ramp (item-port links)') {
    [void](C 'sc.FP.Noclip 1'); [void](C 'sc.FP.Place 4300 900 -950'); [void](C 'sc.FP.Look 195 6'); Start-Sleep 1.5 # the step 4 view, outside
    C "sc.FP.Door $Bottom" | Select-String 'DOOR_PORT_LINK|FIRST_PERSON_DOOR'
    Trace $Bottom 14 'b1-front-opening' @(0.5, 5, 13.5)
    [void](C 'sc.FP.Place 2600 -150 -1200'); [void](C 'sc.FP.Look 180 25'); Start-Sleep 2; Shot 'b2-front-ramp-from-ground'
    ViewPanel 90; Shot 'b3-hold-front-panels-opened'; Crop 'b3-hold-front-panels-opened' 0.4 2
}
if (Step 'hold light panel: each row aimed and pressed; the hold after each') {
    "light panel $LightPanel centre=$($PC -join ',') normal=$($PN -join ',')"
    # Before any press: the hold and the panel as imported.
    ViewPanel 35; Shot 'c-0-before'; Crop 'c-0-before'
    $yaw = [math]::Atan2(-$PN[1], -$PN[0]) * 180 / [math]::PI + 180
    [void](C ("sc.FP.Look {0} -8" -f $yaw)); Start-Sleep 2; Shot 'c-0-before-hold'; Brightness 'c-0-before-hold'
    foreach ($s in @(@(2, 'emergency'), @(1, 'auxiliary'), @(0, 'on'))) {
        $o = $s[0]; $name = $s[1]
        # The player 35 cm in front of the face (it is 4.6 x 8.1 cm), the crosshair held on the row.
        ViewPanel 35
        C "sc.Lift.PanelAim $LightPanel $o" | Select-String 'panel_aim'; Start-Sleep 2.5
        (C 'sc.FP.Probe' | Select-Object -First 1) -replace ' chain=.*', ''
        Shot "c-$name-1-aimed"; Crop "c-$name-1-aimed"
        C 'sc.FP.Interact' | Select-String 'LIGHT_PANEL_PRESS|LIGHT_GROUP_STATE'
        Start-Sleep 1.5; Shot "c-$name-2-pressed"; Crop "c-$name-2-pressed"
        # The hold behind the player, from the panel's place (the lights the panel switched).
        [void](C ("sc.FP.Look {0} -8" -f $yaw)); Start-Sleep 2; Shot "c-$name-3-hold"; Brightness "c-$name-3-hold"
    }
    C 'sc.Lights.Groups' | Select-String 'LIGHT_GROUP '
}
if (Step 'lift: top deck -> Exit Ship and lower deck -> Exit Ship from where the rider was caught') {
    # Leaving noclip needs a clear capsule: the teleport onto the carriage below comes first in every ride.
    function Ride([int]$Stop, [double]$X, [double]$Y, [string]$Tag) {
        C "sc.Lift.Request 0 $Stop" | Out-Null; [void](Invoke-SC /wait @{ until = 'lift_idle'; timeout_s = 90 } -TimeoutSec 120)
        $z = [double](((C 'sc.Lift.Where' | Select-String 'LIFT_WHERE lift=0' | Select-Object -First 1) -replace '.* z_cm=(\S+) .*', '$1'))
        [void](C "sc.FP.Teleport $X $Y $($z - 40)"); [void](C 'sc.FP.Noclip 0'); Start-Sleep 1.5; C 'sc.FP.Where'
        [void](C 'sc.FP.Look 90 -20')
        C 'sc.Lift.Send 4e867e5a-7436-4974-761c-883613ff8998 RequestSurface' | Select-String 'LINK_SEND'
        $t0 = Get-Date; $shot = 0
        while (((Get-Date) - $t0).TotalSeconds -lt 40) {
            $s = (C 'sc.Lift.Where') -join ' '
            if ($s -match 'state=Moving' -and $shot -eq 0 -and $s -match ' z_cm=(-[89]\d\d)\.') { [void](Shot "$Tag-1-passing-hold"); $shot++ }
            if ($s -match 'state=Open' -and $s -match ' at=0 ') { break }
            Start-Sleep -Milliseconds 100
        }
        Start-Sleep 0.5; Shot "$Tag-2-arrived"; C 'sc.Lift.Where' | Select-String 'player'
    }
    Ride 2 -300 -700 'd-top-y700'
    Ride 2 -204 -764 'd-top-x204'
    Ride 1 -300 -700 'd-lower-y700'
    Ride 1 -204 -764 'd-lower-x204'
    Ride 1 -300 -784 'd-lower-centre'
}
if (Step 'rider check: every leg ridden at a steady height on the carriage (LIFT_RIDE_LEG)') {
    # Steady: the feet's height over the carriage anchor varies less than 3 cm over the leg and the base never left the carriage.
    Select-String -Path $LogPath -Pattern 'LIFT_RIDE_LEG' | ForEach-Object {
        $l = $_.Line.Substring($_.Line.IndexOf('LIFT_RIDE_LEG'))
        $spread = [double]($l -replace '.*spread_cm=(\S+) .*', '$1'); $off = [int]($l -replace '.*off_carriage=(\d+).*', '$1')
        "{0} {1}" -f $(if ($spread -lt 3 -and $off -eq 0) { 'RIDE_OK  ' } else { 'RIDE_FAIL' }), $l
    }
}
if (Step 'logs') {
    Select-String -Path $LogPath -Pattern 'LIFT_RIDE_LEG|LIFT_PATH|LIGHT_PANEL_BUILD|DOOR_PORT_LINK|EXPLORATION spawns|CARGO_GRID id=[34]|CARGO_GRID_ENV_ACCESS' |
        ForEach-Object { $i = $_.Line.IndexOf(': Display: '); if ($i -ge 0) { $_.Line.Substring($i + 11) } else { $_.Line } } | Select-Object -Last 40
}
