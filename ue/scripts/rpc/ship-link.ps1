# Exercise the live Ship Link API on the smallest loaded package that has door clips.
# Start a single viewer with -SCRpc and load a suitable fixture first. A Cutlass Black in the default
# Viewer map exercises doors without loading the XL hangar. Pass -Screenshots for Slate evidence.
[CmdletBinding()]
param(
    [int]$Port = 8770,
    [int]$Ship = 0,
    [int]$TimeoutSeconds = 120,
    [switch]$Screenshots,
    [switch]$RequireExterior,
    [switch]$RequirePad,
    [switch]$C2Cargo
)
$ErrorActionPreference = 'Stop'
Import-Module "$PSScriptRoot\SCRpc.psm1" -Force
Set-SCRpcPort $Port

function Call([string]$Path, [hashtable]$Body = @{}) {
    $response = Invoke-SC $Path $Body -TimeoutSec ($TimeoutSeconds + 10)
    if (-not $response.ok) { throw "$Path failed: $($response.error)" }
    $response
}

function Wait-State([scriptblock]$Ready, [string]$Description) {
    $elapsed = [Diagnostics.Stopwatch]::StartNew()
    do {
        $snapshot = (Call '/ship/state').ship
        if (& $Ready $snapshot) { return $snapshot }
        Start-Sleep -Milliseconds 100
    } while ($elapsed.Elapsed.TotalSeconds -lt $TimeoutSeconds)
    throw "Timed out waiting for $Description. Last state: $($snapshot | ConvertTo-Json -Compress -Depth 8)"
}

function Get-Door($State, [int]$Id) {
    $State.doors | Where-Object { $_.ship -eq $Ship -and $_.id -eq $Id } | Select-Object -First 1
}

$total = [Diagnostics.Stopwatch]::StartNew()
$start = Wait-State { param($s) $s.pairings | Where-Object { $_.ship -eq $Ship } } 'ship pairing'
$pairing = $start.pairings | Where-Object { $_.ship -eq $Ship } | Select-Object -First 1
$candidates = $start.doors | Where-Object { $_.ship -eq $Ship -and (!$RequirePad -or $_.pads.Count -gt 0) }
$door = $candidates | Where-Object group -eq 'Exterior' | Select-Object -First 1
if (-not $door) {
    if ($RequireExterior) { throw 'The ready ship has no classified exterior doors.' }
    $door = $candidates | Select-Object -First 1
}
if (-not $door) { throw 'The ready ship has no Ship Link doors.' }
$id = $door.id
$owner = $pairing.actor
"ship=$Ship entity=$($pairing.entity) owner=$owner door=$id group=$($door.group) evidence=$($door.group_source)"

Call '/ship/door' @{ ship = $Ship; door = $id; action = 'close'; as = $owner } | Out-Null
Wait-State { param($s) $d = Get-Door $s $id; $d -and -not $d.open -and -not $d.moving } 'closed door' | Out-Null
Call '/ship/door' @{ ship = $Ship; door = $id; action = 'lock'; as = $owner } | Out-Null
$locked = Get-Door (Call '/ship/state').ship $id
if (-not $locked.locked -or $locked.open -or $locked.moving) { throw 'Lock did not hold a closed, stationary door.' }
if ($RequirePad -and !($locked.pads -match 'DoorLockState.DoorLocked')) {
    throw "Door $id has no pad showing its locked render: $($locked | ConvertTo-Json -Compress)"
}
$refusal = Invoke-SC /ship/door @{ ship = $Ship; door = $id; action = 'open'; as = 'ship-link-smoke-visitor' }
if ($refusal.ok -or $refusal.error -ne 'Locked') { throw "Expected access denial, got $($refusal | ConvertTo-Json -Compress)." }
"locked: $($locked | ConvertTo-Json -Compress -Depth 5)"

if ($Screenshots) {
    Call '/console' @{ cmd = 'sc.MobiGlas 1' } | Out-Null
    Call '/console' @{ cmd = 'sc.MobiGlas.App ShipLink' } | Out-Null
    Wait-State { param($s) $s.ui.ready -and $s.ui.door_labels -gt 0 } 'presented Ship Link layout' | Out-Null
    Call '/wait' @{ until = 'settled'; timeout_s = $TimeoutSeconds } | Out-Null
    Call '/screenshot' @{ name = 'ship-link-locked'; ui = $true } | Select-Object path
}
Call '/ship/door' @{ ship = $Ship; door = $id; action = 'unlock'; as = $owner } | Out-Null
Call '/ship/door' @{ ship = $Ship; door = $id; action = 'open'; as = $owner } | Out-Null
if ($Screenshots) {
    $pending = Get-Door (Call '/ship/state').ship $id
    if ($pending.moving) {
        Call '/screenshot' @{ name = 'ship-link-opening'; ui = $true } | Select-Object path
    }
}
Wait-State { param($s) $d = Get-Door $s $id; $d -and $d.open -and -not $d.moving } 'opened door' | Out-Null
if ($door.group -eq 'Exterior') {
    Call '/ship/secure' @{ ship = $Ship; as = $owner } | Out-Null
    Wait-State { param($s) $d = Get-Door $s $id; $d -and $d.locked -and -not $d.open -and -not $d.moving } 'secure ship' | Out-Null
} else {
    Call '/ship/door' @{ ship = $Ship; door = $id; action = 'close'; as = $owner } | Out-Null
    Wait-State { param($s) $d = Get-Door $s $id; $d -and -not $d.open -and -not $d.moving } 'reclosed door' | Out-Null
}
$light = $start.lights | Where-Object { $_.ship -eq $Ship -and $_.modes -contains 'Off' -and $_.modes.Count -gt 1 } |
    Sort-Object light_count -Descending | Select-Object -First 1
if ($light) {
    $on = $light.modes | Where-Object { $_ -ne 'Off' } | Select-Object -First 1
    foreach ($mode in @('Off', $on)) {
        Call '/ship/lights' @{ ship = $Ship; group = $light.id; mode = $mode; as = $owner } | Out-Null
        $present = (Call '/ship/state').ship.lights | Where-Object { $_.ship -eq $Ship -and $_.id -eq $light.id }
        if ($present.state -ne $mode) { throw "Light group $($light.id) did not enter $mode." }
    }
}
if ($C2Cargo) {
    if ($door.group -ne 'Exterior') { throw 'The C2 cargo check expects an exterior door.' }
    Call '/console' @{ cmd = 'sc.MobiGlas 0' } | Out-Null
    Call '/camera' @{ mode = 'first' } | Out-Null
    Call '/console' @{ cmd = 'sc.FP.Spawn interior:0' } | Out-Null
    Call '/ship/door' @{ ship = $Ship; door = $id; action = 'open'; local = $true } | Out-Null
    Wait-State { param($s) $d = Get-Door $s $id; $d.open -and $d.locked -and !$d.moving } 'owner egress without unlocking' | Out-Null
    Call '/camera' @{ mode = 'orbit' } | Out-Null
    Call '/ship/secure' @{ ship = $Ship; as = $owner } | Out-Null
    Wait-State { param($s) $d = Get-Door $s $id; $d.locked -and !$d.open -and !$d.moving } 'resecuring after egress' | Out-Null
    $job = Call '/cargo/transfer' @{ from = 'elevator'; to = 'ship'; commodity = 'organic'; scu = 16; owner = $owner }
    $opening = Wait-State { param($s) $s.doors | Where-Object { $_.ship -eq $Ship -and $_.cargo_access } } 'owner cargo unlocking a hold door'
    $temporarilyUnlocked = @($opening.doors | Where-Object { $_.ship -eq $Ship -and $_.cargo_access })
    $finished = Call '/wait' @{ until = 'job'; job = $job.job; timeout_s = $TimeoutSeconds }
    if ($finished.job_state -ne 'Done') { throw "Owner cargo failed: $($finished.job_reason)" }
    foreach ($hold in $temporarilyUnlocked) {
        $holdId = $hold.id
        Wait-State { param($s) $d = Get-Door $s $holdId; $d.locked -and !$d.open -and !$d.moving } 'cargo hold relocking' | Out-Null
    }
    "local egress and owner cargo relock passed, job=$($job.job) doors=$($temporarilyUnlocked.id -join ',')"
}
"PASS ship=$Ship door=$id light_tested=$([bool]$light) elapsed_s=$([math]::Round($total.Elapsed.TotalSeconds, 2))"
