# Exercise the live Ship Link API on the smallest loaded package that has door clips.
# Start a single viewer with -SCRpc and load a suitable fixture first. A Cutlass Black in the default
# Viewer map exercises doors without loading the XL hangar. Pass -Screenshots for Slate evidence.
[CmdletBinding()]
param(
    [int]$Port = 8770,
    [int]$Ship = 0,
    [int]$TimeoutSeconds = 120,
    [switch]$Screenshots,
    [switch]$RequireExterior
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
$door = $start.doors | Where-Object { $_.ship -eq $Ship -and $_.group -eq 'Exterior' } | Select-Object -First 1
if (-not $door) {
    if ($RequireExterior) { throw 'The ready ship has no classified exterior doors.' }
    $door = $start.doors | Where-Object { $_.ship -eq $Ship } | Select-Object -First 1
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
$refusal = Invoke-SC /ship/door @{ ship = $Ship; door = $id; action = 'open'; as = 'ship-link-smoke-visitor' }
if ($refusal.ok -or $refusal.error -ne 'Locked') { throw "Expected access denial, got $($refusal | ConvertTo-Json -Compress)." }
"locked: $($locked | ConvertTo-Json -Compress -Depth 5)"

if ($Screenshots) {
    Call '/console' @{ cmd = 'sc.MobiGlas 1' } | Out-Null
    Call '/console' @{ cmd = 'sc.MobiGlas.App ShipLink' } | Out-Null
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
$light = $start.lights | Where-Object { $_.ship -eq $Ship -and $_.modes -contains 'Off' -and $_.modes.Count -gt 1 } | Select-Object -First 1
if ($light) {
    $on = $light.modes | Where-Object { $_ -ne 'Off' } | Select-Object -First 1
    foreach ($mode in @('Off', $on)) {
        Call '/ship/lights' @{ ship = $Ship; group = $light.id; mode = $mode; as = $owner } | Out-Null
        $present = (Call '/ship/state').ship.lights | Where-Object { $_.ship -eq $Ship -and $_.id -eq $light.id }
        if ($present.state -ne $mode) { throw "Light group $($light.id) did not enter $mode." }
    }
}
"PASS ship=$Ship door=$id light_tested=$([bool]$light) elapsed_s=$([math]::Round($total.Elapsed.TotalSeconds, 2))"
