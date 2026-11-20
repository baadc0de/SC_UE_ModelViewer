# Exercise fleet retrieval, replacement refusal, free-floor placement and paint on one running viewer.
[CmdletBinding()]
param(
    [int]$Port = 8770,
    [Parameter(Mandatory)][string]$BayEntity,
    [Parameter(Mandatory)][string]$FloorEntity,
    [int]$TimeoutSeconds = 240,
    [switch]$Screenshots
)

$ErrorActionPreference = 'Stop'
Import-Module "$PSScriptRoot\SCRpc.psm1" -Force
Set-SCRpcPort $Port

function Call([string]$Path, [hashtable]$Body = @{}) {
    $response = Invoke-SC $Path $Body -TimeoutSec ($TimeoutSeconds + 10)
    if (-not $response.ok) { throw "$Path failed: $($response.error)" }
    $response
}

function Find-Row($Snapshot, [string]$Entity) {
    $Snapshot.garage.fleet | Where-Object entity -eq $Entity | Select-Object -First 1
}

function Wait-ForRow([string]$Entity, [string]$State) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    do {
        $row = Find-Row (Call /garage/fleet) $Entity
        if ($row -and (!$State -or $row.state -eq $State)) { return $row }
        if ($row.state -eq 'Unavailable') { throw "$Entity unavailable: $($row.reason)" }
        Start-Sleep -Milliseconds 150
    } while ($timer.Elapsed.TotalSeconds -lt $TimeoutSeconds)
    throw "Timed out waiting for $Entity in $State. Last row: $($row | ConvertTo-Json -Compress)"
}

function Test-Pairing([int]$Id, [bool]$Paired) {
    $pair = (Call /ship/state).ship.pairings | Where-Object ship -eq $Id | Select-Object -First 1
    if ([bool]$pair -ne $Paired) { throw "Unexpected pairing state for ship $Id. Expected $Paired." }
}

$timer = [Diagnostics.Stopwatch]::StartNew()
$bayRow = Wait-ForRow $BayEntity ''
$null = Wait-ForRow $FloorEntity 'Stored'
if ($bayRow.state -ne 'In hangar') {
    $null = Call /garage/retrieve @{ entity = $BayEntity; at = 'bay' }
    $bayRow = Wait-ForRow $BayEntity 'In hangar'
}
Test-Pairing $bayRow.ship $true
$occupied = Invoke-SC /garage/retrieve @{ entity = $FloorEntity; at = 'bay' }
if ($occupied.ok -or !$occupied.confirm_store -or $occupied.occupant -ne $bayRow.ship) {
    throw "An occupied bay should ask for confirmation: $($occupied | ConvertTo-Json -Compress)"
}
$null = Call /garage/retrieve @{ entity = $FloorEntity; at = 'floor' }
$floorRow = Wait-ForRow $FloorEntity 'In hangar'
Test-Pairing $floorRow.ship $true

if ($Screenshots) {
    $null = Call /console @{ cmd = 'sc.MobiGlas 1' }
    $null = Call /console @{ cmd = 'sc.MobiGlas.App Garage' }
    $null = Call /wait @{ until = 'settled'; timeout_s = $TimeoutSeconds }
    Call /screenshot @{ name = 'garage-fleet'; ui = $true } | Select-Object path
    $null = Call /console @{ cmd = 'sc.MobiGlas 0' }
}
$paint = $bayRow.paints | Where-Object { $_ -ne $bayRow.paint } | Select-Object -First 1
if ($paint) {
    $null = Call /garage/paint @{ ship = $bayRow.ship; paint = $paint }
    $repainted = Wait-ForRow $BayEntity 'In hangar'
    if ($repainted.paint -ne $paint) { throw "Paint did not apply: $($repainted.paint)." }
}
$null = Call /camera @{ mode = 'orbit' }
$null = Call /garage/store @{ ship = $bayRow.ship }
$null = Wait-ForRow $BayEntity 'Stored'
Test-Pairing $bayRow.ship $false
Test-Pairing $floorRow.ship $true
$null = Call /garage/retrieve @{ entity = $BayEntity; at = 'bay' }
$replacement = Wait-ForRow $BayEntity 'In hangar'
Test-Pairing $replacement.ship $true
if ($paint -and $replacement.paint -ne $paint) { throw "Paint did not survive retrieve: $($replacement.paint)." }

[pscustomobject]@{
    bay = $replacement; floor = $floorRow; paint_tested = [bool]$paint
    elapsed_s = [math]::Round($timer.Elapsed.TotalSeconds, 2)
} | ConvertTo-Json -Depth 5 -Compress
