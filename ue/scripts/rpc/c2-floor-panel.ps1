# WP-P5 floor panel verification: the C2 lift's floor-select panel (the viewer's own UI on the inner panel's screen face),
# operated in first person (RPC driver).
#   Import-Module ue/scripts/rpc/SCRpc.psm1
#   $p = Start-SCViewer -Port 8771 -ShotDir <dir> -Extra '-SCLoad=CRUS_Starlifter_C2 -SCEnvironment=<hangar socpak> -SCCamera=first'
#   Invoke-SC /wait @{ until = 'idle'; timeout_s = 1500 }
#   ue/scripts/rpc/c2-floor-panel.ps1 -Port 8771 [-From <step>] [-Through <step>] [-NoShots]
# The view is turned with sc.Lift.PanelAim (an option's centre, or a content point of the face) and E is sc.FP.Interact,
# the same press path as the key. Positions are vehicle-relative cm (as c2-lift.ps1). Screenshots go to -SCShotDir.
param([int]$Port = 8771, [ValidateRange(1, 10)][int]$From = 1,
      [ValidateRange(1, 10)][int]$Through = 10, [switch]$NoShots)
if ($Through -lt $From) { throw '-Through must be at or after -From' }
Import-Module "$PSScriptRoot\SCRpc.psm1" -Force
Set-SCRpcPort $Port
function C([string]$Cmd) { (Invoke-SC /console @{ cmd = $Cmd }).lines }
function Shot([string]$Name, [switch]$Ui) {
    if (!$NoShots) { [void](Invoke-SC /screenshot @{ name = $Name; ui = [bool]$Ui }) }
}
$script:N = 0
function Step([string]$Text) { $script:N++; $run = $script:N -ge $From -and $script:N -le $Through; if ($run) { Write-Host "== $($script:N) $Text" }; $run }
function WaitLift([int]$Timeout = 90) {
    $w = Invoke-SC /wait @{ until = 'lift_idle'; timeout_s = $Timeout } -TimeoutSec ($Timeout + 30)
    "lift_idle ok=$($w.ok) waited=$($w.waited_s) state=$($w.lifts[0].state) at=$($w.lifts[0].at)"
}
function Probe { (C 'sc.FP.Probe' | Select-Object -First 1) -replace ' chain=.*', '' }
function WaitRow([string]$Target, [string]$Interaction) {
    $aim = C "sc.Lift.PanelAim 0 $Target" | Select-String 'panel_aim'
    if ("$aim" -notmatch 'panel_aim panel=0 ') { throw "cannot aim at floor panel: $aim" }
    Write-Host $aim
    $wantedChoice = "choice=$([regex]::Escape($Interaction))(?= )"
    try {
        $reply = Wait-SCRpc -Path /console -Body @{ cmd = 'sc.FP.Probe' } -TimeoutSec 1 -IntervalMs 50 -Condition {
            param($state)
            [bool]($state.lines | Select-String $wantedChoice -Quiet)
        }
        return ($reply.lines | Select-String 'FP_PROBE' | Select-Object -First 1) -replace ' chain=.*', ''
    } catch [TimeoutException] {
        # The held view can settle off the option. AimRow will try nearby points.
    }
}
function AimRow([int]$Row, [string]$Interaction) {
    # Ask the game to use the authored row centre. Read the rectangles for nudges only if head movement settles off target.
    $probe = WaitRow "$Row" $Interaction
    if ($probe) { return $probe }
    if (!$script:PanelRows) { $script:PanelRows = C 'sc.Lift.Panels' | Select-String 'FLOOR_PANEL_ROW panel=0 ' }
    $rect = ($script:PanelRows | Select-String "option=$Row ") -replace '.* rect=', ''
    $bounds = [regex]::Match($rect, '([\d.]+),([\d.]+)\.\.([\d.]+),([\d.]+)')
    if (!$bounds.Success) { throw "floor panel row $Row has no rectangle" }
    $centreY = ([double]$bounds.Groups[2].Value + [double]$bounds.Groups[4].Value) / 2
    foreach ($dy in -0.05, 0.05, -0.09, 0.09) {
        $probe = WaitRow "0.5 $($centreY + $dy)" $Interaction
        if ($probe) { return $probe }
    }
    throw "row $Row not under the crosshair"
}
function Aim([string]$Target) {
    # sc.Lift.PanelAim 0 <row> | 0 <x> <y>: the view is held on the point (turned again each tick as the eye moves) until
    # sc.FP.Look; the highlight follows the crosshair.
    C "sc.Lift.PanelAim 0 $Target" | Select-String 'panel_aim'; Start-Sleep -Milliseconds 2500
    Probe
}
function Watch([string]$Tag, [int]$Stop, [double[]]$ShotAtS) {
    # Follow the lift until it stands open at Stop; screenshots when the carriage passes the given path positions (m).
    $deadline = (Get-Date).AddSeconds(90); $last = ''; $i = 0
    while ((Get-Date) -lt $deadline) {
        $l = (Invoke-SC /state).lifts[0]
        if ($l.state -ne $last) { "  $(Get-Date -Format HH:mm:ss.fff) $($l.state) s=$($l.s_m)"; $last = $l.state }
        if ($l.state -eq 'Moving' -and $i -lt $ShotAtS.Count -and [math]::Abs($l.s_m - $ShotAtS[$i]) -lt 1.2) { Shot "$Tag-$i"; $i++ }
        if ($l.state -eq 'Open' -and $l.at -eq $Stop) { return }
        Start-Sleep -Milliseconds 80
    }
    throw "lift did not open at stop $Stop"
}
function OnCarriage {
    # The middle of the carriage, wherever it is (floor hint 40 cm under its node), as c2-lift.ps1.
    $z = [double](((C 'sc.Lift.Where' | Select-String 'LIFT_WHERE lift=0' | Select-Object -First 1) -replace '.* z_cm=(\S+) .*', '$1'))
    $teleport = C "sc.FP.Teleport -300 -784 $($z - 40)"
    if (!($teleport | Select-String 'FP_CMD teleport ok=1' -Quiet)) { throw "teleport onto the carriage failed: $teleport" }
    $rider = Wait-SCRpc -Path /console -Body @{ cmd = 'sc.Lift.Where' } -TimeoutSec 5 -Condition {
        param($reply)
        [bool]($reply.lines | Select-String 'LIFT_WHERE player base=\S+ lift=0 ' -Quiet)
    }
    $rider.lines | Select-String 'LIFT_WHERE player'
}
$Rows = @('RequestFloor1', 'RequestFloor0', 'RequestSurface') # the rows' interactions (sc.Lift.Panels): Top Deck, Lower Deck, Exit Ship
function Press([int]$Row, [string]$Tag) {
    AimRow $Row $Rows[$Row]; Shot "$Tag-aimed" -Ui
    C 'sc.FP.Interact' | Select-String 'FLOOR_PANEL_PRESS|FP_PANEL_PRESS|LINK_SEND'
    # The call before the leg starts (the queue wait): the floor's steady yellow bar, the view held off the rows.
    [void](C "sc.Lift.PanelAim 0 -0.3 0.5"); Start-Sleep -Milliseconds 1500; Shot "$Tag-called"
}

if (Step 'lifts, panels, rows') { C 'sc.Lift.List' | Select-String 'LIFT_LIST|LIFT_STOP|LIFT_PANEL'; C 'sc.Lift.Panels' }
if (Step 'onto the carriage at the hold') {
    $camera = (Invoke-SC /state).camera.mode
    if ($camera -ne 'FirstPerson') {
        $first = Invoke-SC /camera @{ mode = 'first' }
        if (!$first.ok) { throw "cannot enter first person: $($first.error)" }
    }
    OnCarriage
    WaitLift
}
if (Step 'aim at each row: the highlight follows the crosshair') {
    foreach ($r in 0, 1, 2) { AimRow $r $Rows[$r]; Shot "a$r-aim-row-$r"; Shot "a$r-aim-row-$r-ui" -Ui }
}
if (Step 'off the rows and off the face: nothing highlighted') {
    Aim '0.5 0.955'; Shot 'b1-status-bar'
    Aim '0.5 -0.4'; Shot 'b2-off-face-above'
    Aim '1.6 0.5'; Shot 'b3-off-face-beside'
    C 'sc.FP.Interact' | Select-String 'PANEL_PRESS|FP_CMD'
    # Off the face: nothing highlighted, and E queued no request.
    $panel = C 'sc.Lift.Panels' | Select-String 'FLOOR_PANEL index' | Select-Object -First 1; $panel
    $where = C 'sc.Lift.Where' | Select-String 'LIFT_WHERE lift=0' | Select-Object -First 1; $where
    if ("$panel" -notmatch 'highlight=-1 ') { throw 'a row is highlighted with the crosshair off the face' }
    if ("$where" -notmatch 'queue=\[\]') { throw 'E off the face queued a request' }
}
if (Step 'E on TOP DECK: the lift goes up; the panel marks the call and the travel') {
    Press 0 'c1-top'
    Watch 'c2-top-travel' 2 @(9.0, 11.5); Start-Sleep 0.5
    Aim '-0.3 0.5'; Shot 'c3-top-arrived'; C 'sc.Lift.Panels' | Select-String 'FLOOR_PANEL index'
}
if (Step 'E on LOWER DECK') {
    OnCarriage
    Press 1 'd1-lower'
    Watch 'd2-lower-travel' 1 @(10.5); Start-Sleep 0.5
    Aim '-0.3 0.5'; Shot 'd3-lower-arrived'; C 'sc.Lift.Panels' | Select-String 'FLOOR_PANEL index'
}
if (Step 'E on EXIT SHIP: the hatch opens, down to the ground') {
    OnCarriage
    Press 2 'e1-exit'
    Watch 'e2-exit-travel' 0 @(4.5); Start-Sleep 0.5
    Aim '-0.3 0.5'; Shot 'e3-exit-arrived'; C 'sc.Lift.Panels' | Select-String 'FLOOR_PANEL index'
}
if (Step 'back up: E on LOWER DECK') {
    OnCarriage
    Press 1 'f1-lower'
    Watch 'f2-lower-travel' 1 @(3.4); Start-Sleep 0.5
    Aim '-0.3 0.5'; Shot 'f3-lower-arrived'
}
if (Step 'legibility at arm''s length: the panel from where the rider stands, nothing aimed') {
    OnCarriage
    Aim '0.5 -0.4' | Out-Null
    [void](C 'sc.FP.Look -90 -45'); Start-Sleep 0.5; Shot 'g1-arms-length'
    [void](C 'sc.FP.Look -90 -35'); Start-Sleep 0.5; Shot 'g2-arms-length-wider'
    AimRow 0 $Rows[0] | Out-Null; Shot 'g3-arms-length-top-highlighted'
}
if (Step 'done') { C 'sc.Lift.Where'; C 'sc.Lift.Panels' }
