# WP-I1-4 verification: the game's hover look on operating panels under the crosshair, and the interaction popout (its
# actions, the wheel's choice) on a door, a light panel's row and a usable (RPC driver).
#   Import-Module ue/scripts/rpc/SCRpc.psm1
#   $p = Start-SCViewer -Port 8772 -ShotDir <dir> -Extra '-SCLoad=CRUS_Starlifter_C2 -SCEnvironment=<hangar socpak> -SCCamera=first'
#   Invoke-SC /wait @{ until = 'idle'; timeout_s = 1800 }
#   ue/scripts/rpc/interaction-feedback.ps1 -Port 8772 -ShotDir <dir>
# Positions are vehicle-relative cm from the viewer's own listings (sc.Panels.Hover, sc.Lift.Panels); the view is held on
# a point with sc.FP.AimAt / sc.Lift.PanelAim, E is sc.FP.Interact and the wheel sc.FP.Key MouseScrollDown 1.
param([int]$Port = 8772, [string]$ShotDir = '', [int]$From = 1)
Import-Module "$PSScriptRoot\SCRpc.psm1" -Force
Set-SCRpcPort $Port
function C([string]$Cmd) { (Invoke-SC /console @{ cmd = $Cmd }).lines }
function Shot([string]$Name) { [void](Invoke-SC /screenshot @{ name = $Name; ui = $true }); $Name }
$script:N = 0
function Step([string]$Text) { $script:N++; $run = $script:N -ge $From; if ($run) { Write-Host "== $($script:N) $Text" }; $run }
function Vec([string]$Text, [string]$Key) {
    $m = [regex]::Match($Text, "$Key=V\(X=([-0-9.]+), Y=([-0-9.]+), Z=([-0-9.]+)\)")
    if (-not $m.Success) { throw "no $Key in: $Text" }
    1..3 | ForEach-Object { [double]$m.Groups[$_].Value }
}
function Prompt { (C 'sc.FP.Prompt' | Select-String 'FP_PROMPT_NOW' | Select-Object -First 1) -replace '.*FP_PROMPT_NOW ', '' }
function Stand([double[]]$Point, [double[]]$Normal, [double]$Cm) {
    # The capsule Cm in front of the point, its eye about level with it, the view held on it.
    [void](C 'sc.FP.Noclip 1')
    [void](C ("sc.FP.Place {0} {1} {2}" -f ($Point[0] + $Normal[0] * $Cm), ($Point[1] + $Normal[1] * $Cm), ($Point[2] - 64)))
    [void](C ("sc.FP.AimAt {0} {1} {2}" -f $Point[0], $Point[1], $Point[2])); Start-Sleep 2
}

$hover = C 'sc.Panels.Hover'
$hover | Select-String 'PANEL_AIM|USABLE' | Select-Object -First 12
C 'sc.Lift.Panels' | Select-String 'kind=buttons' | Select-Object -First 3

if (Step 'a door pad: its door button in the game hover look, the popout names the door and its action') {
    $pad = "$($hover | Select-String 'PANEL_AIM .*fallback=DoorButton' | Select-Object -First 1)"
    $pc = Vec $pad 'centre'; $pn = Vec $pad 'normal'
    Stand $pc $pn 70
    C 'sc.Panels.Hover' | Select-String "PANEL_AIM index=$([regex]::Match($pad, 'index=(\d+)').Groups[1].Value) "
    C 'sc.Screens.States' | Select-String 'hover:' | Select-Object -First 2
    "prompt: $(Prompt)"; Shot 'a1-pad-hovered'
    # The view above the pad: nothing hovered, no prompt from the pad.
    [void](C ("sc.FP.AimAt {0} {1} {2}" -f $pc[0], $pc[1], ($pc[2] + 35))); Start-Sleep 1.5
    C 'sc.Screens.States' | Select-String 'hover:' | Select-Object -First 1
    "prompt off the pad: $(Prompt)"; Shot 'a2-pad-rest'
}
if (Step 'a light panel: the aimed row in the game hover look (no frame); the popout names the row') {
    $panels = C 'sc.Lift.Panels'
    $index = [int]("$($panels | Select-String 'FLOOR_PANEL index=[0-9]+ kind=buttons' | Select-Object -First 1)" -replace '.*FLOOR_PANEL index=([0-9]+) .*', '$1')
    $m = [regex]::Match((($panels | Select-String "FLOOR_PANEL_AT index=$index ") -replace '.*centre=', ''), '([-0-9.]+),([-0-9.]+),([-0-9.]+) normal=([-0-9.]+),([-0-9.]+),([-0-9.]+)')
    $lc = 1..3 | ForEach-Object { [double]$m.Groups[$_].Value }; $ln = 4..6 | ForEach-Object { [double]$m.Groups[$_].Value }
    Stand $lc $ln 60
    foreach ($row in 0, 1, 2) {
        C "sc.Lift.PanelAim $index $row" | Select-String 'panel_aim'; Start-Sleep 2.5
        C 'sc.Lift.Panels' | Select-String "FLOOR_PANEL index=$index " | Select-Object -First 1
        "prompt: $(Prompt)"; Shot "b$row-light-row-$row"
    }
}
if (Step 'a usable (a bed or a seat of the interior): its actions listed, the wheel moves the choice, E is logged') {
    $usable = "$($hover | Select-String 'USABLE .*options=.*,' | Select-Object -First 1)"
    if (-not $usable) { $usable = "$($hover | Select-String 'USABLE ' | Select-Object -First 1)" }
    $usable
    $uc = Vec $usable 'centre'
    # From the side the bed is open to is not known here: try the four horizontal directions until the prompt names it.
    $entity = [regex]::Match($usable, 'entity=(\S+)').Groups[1].Value
    foreach ($dir in @(@(1, 0, 0), @(-1, 0, 0), @(0, 1, 0), @(0, -1, 0))) {
        Stand $uc $dir 120
        $p = Prompt
        if ($p -match "usable:$([regex]::Escape($entity))") { break }
    }
    "prompt: $p"; Shot 'c1-usable'
    C 'sc.FP.Key MouseScrollDown 1' | Select-String 'FP_CMD'
    "after the wheel: $(Prompt)"; Shot 'c2-usable-wheel'
    C 'sc.FP.Interact' | Select-String 'FP_USABLE|FP_CMD'
}
