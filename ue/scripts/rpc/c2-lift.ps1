# WP-P5 P5-1 verification: the C2 Hercules internal lift, operated from its real buttons in first person (RPC driver).
#   Import-Module ue/scripts/rpc/SCRpc.psm1
#   $p = Start-SCViewer -Port 8771 -ShotDir <dir> -Extra '-SCLoad=CRUS_Starlifter_C2 -SCEnvironment=<hangar socpak> -SCCamera=first'
#   Invoke-SC /wait @{ until = 'idle'; timeout_s = 1500 }
#   ue/scripts/rpc/c2-lift.ps1 -Port 8771 [-From <step>]
# Positions are vehicle-relative cm and views vehicle-relative degrees, found with sc.FP.Probe / sc.Screens on the LIVE
# 4.10.2 C2 (they frame the buttons; nothing in the lift reads them). The ground call button is a helper without geometry,
# pressed with sc.Lift.Send until WP-I1 makes helper-only panels pressable. Screenshots go to the viewer's -SCShotDir.
# -From resumes at a numbered step, with the lift and the player where the previous step left them.
param([int]$Port = 8771, [int]$From = 1)
Import-Module "$PSScriptRoot\SCRpc.psm1" -Force
Set-SCRpcPort $Port
function C([string]$Cmd) { (Invoke-SC /console @{ cmd = $Cmd }).lines }
function Shot([string]$Name, [switch]$Ui) { [void](Invoke-SC /screenshot @{ name = $Name; ui = [bool]$Ui }) }
$script:N = 0
function Step([string]$Text) { $script:N++; $run = $script:N -ge $From; if ($run) { Write-Host "== $($script:N) $Text" }; $run }
function WaitLift([int]$Timeout = 90) {
    $w = Invoke-SC /wait @{ until = 'lift_idle'; timeout_s = $Timeout } -TimeoutSec ($Timeout + 30)
    "lift_idle ok=$($w.ok) waited=$($w.waited_s) state=$($w.lifts[0].state) at=$($w.lifts[0].at)"
}
function Aim([double]$Yaw, [double[]]$Pitches, [string]$Panel) {
    # Sweep the pitch until the crosshair finds the panel (its own collision, else its bounds). A new focus starts at the
    # point's primary interaction.
    foreach ($p in $Pitches) {
        [void](C "sc.FP.Look $Yaw $p"); Start-Sleep -Milliseconds 250
        $probe = C 'sc.FP.Probe' | Select-Object -First 1
        if ($probe -match "panel=$Panel") { return ($probe -replace ' chain=.*', '') }
    }
    throw "panel $Panel not found at yaw $Yaw"
}
function Choose([int]$Index) { for ($i = 0; $i -lt $Index; $i++) { C 'sc.FP.Key MouseScrollDown 1' | Select-String 'FP_PANEL_CHOICE' } }
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
$CallHold = '4d52b73d-3b67-9a88-213d-f4c8017747a4'; $CallTop = '49b375f1-d5fb-1ee4-5f46-9a8fcffce2b2'
$Panel = '4e867e5a-7436-4974-761c-883613ff8998'; $Ground = '49bd738e-4318-f2b7-487a-8fee884c3a93'
$Sweep = @(-6, -8.5, -10, -12, -14, -16)
$PanelSweep = @(-30, -35, -40, -45, -50, -55, -60, -65, -70, -75, -80, -85)
# Floor panel choices in authored order: 0 Top Deck (primary), 1 Lower Deck, 2 Exit Ship.

if (Step 'stops') { C 'sc.Lift.List' | Select-String 'LIFT_LIST|LIFT_STOP' }
if (Step 'hold: the real call button (the carriage is already there: only its doors open)') {
    [void](C 'sc.FP.Teleport -300 -400 -887'); Start-Sleep 1
    Aim -58.9 $Sweep $CallHold; Shot 'a1-hold-call-button'
    C 'sc.FP.Interact' | Select-String 'FP_PANEL_PRESS'; WaitLift
    [void](C 'sc.FP.Look -90 0'); Start-Sleep 0.5; Shot 'a2-hold-doors-open'
}
if (Step 'walk in') { [void](C 'sc.FP.Look -90 0'); [void](C 'sc.FP.Move 1 0 1.2'); Start-Sleep 2.5; C 'sc.Lift.Where' | Select-Object -Last 1 }
if (Step 'floor panel: Top Deck (the primary interaction)') {
    Aim -90 $PanelSweep $Panel; Shot 'b1-floor-panel-prompt' -Ui
    C 'sc.FP.Interact' | Select-String 'FP_PANEL_PRESS'; [void](C 'sc.FP.Look 90 0')
    Watch 'b2-mid-travel' 2 @(8.5, 10.5, 12.5); Start-Sleep 0.5; Shot 'b3-top-open-from-inside'
}
if (Step 'walk out onto the top deck') { [void](C 'sc.FP.Look 90 0'); [void](C 'sc.FP.Move 1 0 1.3'); Start-Sleep 2.5; C 'sc.Lift.Where' | Select-Object -Last 1 }
if (Step 'send the empty carriage down') { C 'sc.Lift.Request 0 1' | Select-String 'LIFT_CMD'; WaitLift; [void](C 'sc.FP.Look -90 0'); Start-Sleep 0.5; Shot 'c1-top-shaft-empty' }
if (Step 'top: the real call button brings it back up') {
    Aim -63.2 $Sweep $CallTop; Shot 'c2-top-call-button'
    C 'sc.FP.Interact' | Select-String 'FP_PANEL_PRESS'; Watch 'c3-arriving' 2 @(); [void](C 'sc.FP.Look -90 0'); Start-Sleep 0.4; Shot 'c4-top-arrived-open'
}
if (Step 'walk in, ride down to Lower Deck') {
    [void](C 'sc.FP.Look -90 0'); [void](C 'sc.FP.Move 1 0 1.25'); Start-Sleep 2.5
    Aim -90 $PanelSweep $Panel | Out-Null; Choose 1
    C 'sc.FP.Interact' | Select-String 'FP_PANEL_PRESS'; [void](C 'sc.FP.Look 90 0'); Watch 'c5-descending' 1 @(11.5)
}
if (Step 'Exit Ship: the belly hatch opens, the carriage descends to the ground') {
    Aim -90 $PanelSweep $Panel | Out-Null; Choose 2
    Shot 'd1-exit-ship-chosen' -Ui; C 'sc.FP.Interact' | Select-String 'FP_PANEL_PRESS'; [void](C 'sc.FP.Look 90 -30')
    Watch 'd2-through-hatch' 0 @(4.5, 2.2); C 'sc.FP.Where'
}
if (Step 'step off onto the hangar floor, the open hatch from below') {
    [void](C 'sc.FP.Look 90 0'); [void](C 'sc.FP.Move 1 0 0.8'); Start-Sleep 2; C 'sc.FP.Where'
    [void](C 'sc.FP.Torch 1'); [void](C 'sc.FP.Look -90 35'); Start-Sleep 0.6; Shot 'd3-hatch-open-from-ground'
    [void](C 'sc.FP.Look -90 -5'); Start-Sleep 0.4; Shot 'd4-carriage-at-ground'; [void](C 'sc.FP.Torch 0')
}
if (Step 'carriage up (Arrived at 1 closes the hatch), doors closed, then the ground call (sc.Lift.Send: the button is a helper)') {
    C 'sc.Lift.Request 0 1' | Select-String 'LIFT_CMD'; WaitLift; C 'sc.Lift.Doors 0 close' | Select-String 'LIFT_CMD'; WaitLift; Start-Sleep 5
    C "sc.Lift.Send $Ground Descend" | Select-String 'LINK_DELIVER'; Watch 'e1-ground-call' 0 @(3.4)
}
if (Step 'over the carriage, ride up to Lower Deck') {
    [void](C 'sc.FP.Teleport -300 -784 -1505'); Start-Sleep 1; C 'sc.FP.Where'
    Aim -90 $PanelSweep $Panel | Out-Null; Choose 1
    C 'sc.FP.Interact' | Select-String 'FP_PANEL_PRESS'; [void](C 'sc.FP.Look 90 0'); Watch 'e2-ride-up' 1 @(3.4); C 'sc.FP.Where'
}
if (Step 'done') { C 'sc.Lift.Where' }


