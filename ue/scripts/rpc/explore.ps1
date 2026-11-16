# Exploratory stability run over the RPC harness (WP-H1): a seeded random sequence of incremental scene changes (ship
# instances spawned and despawned, boxes added and removed, transfers started and cancelled, grids extended and
# retracted), some waited on until the scene settles and some fired back to back, with the viewer's anomalies, its
# liveness and its log checked after every step. Writes <OutDir>/steps.ndjson, report.json and screenshots.
#   powershell -File ue/scripts/rpc/explore.ps1 -Scenario c2_hangar -Steps 40 -Seed 1
# Holds out/locks/ue.lock (one UE build or game at a time) for the run.
param(
    [string]$Scenario = 'c2_hangar',
    [int]$Steps = 40,
    [int]$Seed = 1,
    [string]$OutDir = '',
    [int]$Port = 8770,
    [int]$MaxExtraShips = 2,
    [double]$WaitShare = 0.5,       # share of steps followed by a wait until settled (the rest run back to back)
    [int]$ShotEvery = 10,
    [string[]]$Boxes = @('Carryable_TBO_FL_8SCU_Commodity_Organic', 'Carryable_TBO_FL_16SCU_Commodity_Organic', 'Carryable_TBO_FL_32SCU_Commodity_Organic'),
    [string]$Commodity = 'organic',
    [switch]$Video,                # record the run (10 fps, 960 wide) to <OutDir>/run.mp4
    [switch]$Visible,              # show the viewer window (default: rendered off screen, agent mode)
    [string]$ExtraArgs = ''        # more viewer switches (e.g. -SCStateLogMaxMB=1)
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..\..\..").Path
Import-Module "$PSScriptRoot\SCRpc.psm1" -Force
if (-not $OutDir) { $OutDir = "$root\out\h1\explore-$Scenario-s$Seed-$(Get-Date -Format yyyyMMdd-HHmmss)" }
New-Item -ItemType Directory -Force $OutDir | Out-Null
$stepsFile = "$OutDir\steps.ndjson"
$rng = [System.Random]::new($Seed)
function Pick($list) { if (-not $list -or @($list).Count -eq 0) { return $null }; @($list)[$rng.Next(@($list).Count)] }
function Chance([double]$p) { $rng.NextDouble() -lt $p }
function Note($o) { ($o | ConvertTo-Json -Depth 8 -Compress) | Add-Content -Path $stepsFile -Encoding utf8 }

while ($true) { try { New-Item -ItemType Directory "$root\out\locks\ue.lock" -ErrorAction Stop | Out-Null; break } catch { Start-Sleep 20 } }
"explore.ps1 $Scenario seed $Seed (pid $PID)" | Out-File "$root\out\locks\ue.lock\owner" -Encoding ascii
$log = "H1_explore_$Scenario`_s$Seed.log"
$report = [ordered]@{ scenario = $Scenario; seed = $Seed; steps = 0; failures = @(); anomalies = @(); actions = @{}; errors = @{} }
$p = $null
try {
    $t0 = Get-Date
    $noFloor = $false
    $p = Start-SCViewer -Extra "-SCCargoScenario=$Scenario $ExtraArgs" -Port $Port -Log $log -ShotDir $OutDir -Visible:$Visible
    $w = Invoke-SC /wait @{ until = 'settled'; timeout_s = 1200 }
    $report.start_s = [int]((Get-Date) - $t0).TotalSeconds
    Note @{ step = 0; action = 'start'; ok = $w.ok; waited_s = $w.waited_s; reasons = $w.reasons }
    if (-not $w.ok) { throw "the scenario did not settle: $($w | ConvertTo-Json -Compress)" }
    Invoke-SC /screenshot @{ name = '000-start' } | Out-Null
    if ($Video) { $report.video_start = Invoke-SC /record @{ action = 'start'; path = (Join-Path $OutDir 'run.mp4'); fps = 10; width = 960; max_s = 3600 } }

    for ($i = 1; $i -le $Steps; $i++) {
        if ($p.HasExited) { $report.failures += "viewer exited before step $i (code $($p.ExitCode))"; break }
        $s = Invoke-SC /state
        if ($s.ok -eq $false) { $report.failures += "no state before step $i : $($s.error)"; break }
        $extra = @($s.vehicles | Where-Object { $_.id -gt 0 })
        $grids = @($s.cargo.grids | Where-Object { $_.access -eq 'Open' })
        $stored = @($s.cargo.boxes | Where-Object { $_.state -eq 'Stored' })
        $moving = @($s.cargo.boxes | Where-Object { $_.state -ne 'Stored' -and $_.state -ne 'Reserved' })
        $active = @($s.cargo.jobs | Where-Object { $_.state -notin 'Done', 'Failed', 'Cancelled' })
        $closable = @($s.cargo.grids | Where-Object { $_.movable })
        # Weighted choice among the actions that make sense now.
        $menu = @()
        if ($extra.Count -lt $MaxExtraShips -and -not $noFloor) { $menu += @('ship_spawn') * 2 }
        if ($extra.Count -gt 0) { $menu += 'ship_despawn' }
        if ($grids.Count -gt 0) { $menu += @('cargo_spawn') * 3 }
        if ($stored.Count -gt 0) { $menu += @('transfer') * 4; $menu += 'cargo_remove' }
        if ($moving.Count -gt 0) { $menu += 'cargo_remove_moving' }
        if ($active.Count -gt 0) { $menu += 'cancel' }
        if ($closable.Count -gt 0) { $menu += 'access' }
        $action = Pick $menu
        $req = @{}; $path = ''
        switch ($action) {
            'ship_spawn' { $path = '/ship/spawn'; $req = @{ entity = 'loaded'; wait = $true; timeout_s = 900 } }
            'ship_despawn' { $path = '/ship/despawn'; $req = @{ id = (Pick $extra).id } }
            'cargo_spawn' { $g = Pick $grids; $path = '/cargo/spawn'; $req = @{ grid = "$($g.id)"; entity = (Pick $Boxes); count = 1 + $rng.Next(3); commodity = $Commodity; label = 'Organic' } }
            'cargo_remove' { $path = '/cargo/remove'; $req = @{ box = (Pick $stored).id } }
            'cargo_remove_moving' { $path = '/cargo/remove'; $req = @{ box = (Pick $moving).id; force = $true } }
            'transfer' {
                $b = Pick $stored
                $to = Pick @($grids | Where-Object { $_.id -ne $b.grid })
                if (-not $to) { $action = 'transfer_none'; break }
                $path = '/cargo/transfer'; $req = @{ from = "$($b.grid)"; to = "$($to.id)"; commodity = $b.commodity; scu = $b.scu * (1 + $rng.Next(2)); owner = $b.owner }
            }
            'cancel' { $path = '/cargo/cancel'; $req = @{ job = (Pick $active).id } }
            'access' { $g = Pick $closable; $path = '/cargo/access'; $req = @{ grid = "$($g.id)"; open = ($g.access -ne 'Open') } }
        }
        if (-not $path) { Note @{ step = $i; action = $action; skipped = $true }; continue }
        $t = Get-Date
        $r = Invoke-SC $path $req
        $ms = [int]((Get-Date) - $t).TotalMilliseconds
        $report.actions[$action] = 1 + [int]$report.actions[$action]
        if ($action -eq 'ship_spawn' -and "$($r.error)" -like 'no free floor*') { $noFloor = $true } # the hangar is full: stop asking
        if ($r.ok -eq $false) { $report.errors[$action] = 1 + [int]$report.errors[$action] }
        $waited = $null
        if (Chance $WaitShare) { $waited = Invoke-SC /wait @{ until = 'settled'; timeout_s = 600 } }
        $h = Invoke-SC /health
        $alive = -not $p.HasExited
        Note @{ step = $i; action = $action; path = $path; req = $req; ok = $r.ok; error = $r.error; warning = $r.warning; ms = $ms; job = $r.job; boxes = $r.boxes; removed = $r.removed
                waited = if ($waited) { @{ ok = $waited.ok; s = $waited.waited_s; reasons = $waited.reasons; unsettled = $waited.unsettled } } else { $null }
                alive = $alive; anomalies_total = $h.anomalies_total; reasons = $h.reasons }
        $report.steps = $i
        if (-not $alive) { $report.failures += "viewer died at step $i ($action $path): exit $($p.ExitCode)"; break }
        if ($h.ok -ne $true) { $report.failures += "no health after step $i : $($h.error)"; break }
        if ($waited -and -not $waited.ok) { $report.failures += "step $i ($action): not settled within 600 s: $($waited.reasons -join ',') $($waited.unsettled -join ',')" }
        if ($ShotEvery -gt 0 -and $i % $ShotEvery -eq 0) {
            Invoke-SC /camera @{ focus = 'all' } | Out-Null
            Invoke-SC /wait @{ until = 'settled'; timeout_s = 120 } | Out-Null
            Invoke-SC /screenshot @{ name = ('{0:D3}-{1}' -f $i, $action) } | Out-Null
        }
    }
    if (-not $p.HasExited) {
        $final = Invoke-SC /wait @{ until = 'settled'; timeout_s = 900 }
        $s = Invoke-SC /state
        $report.final = @{ settled = $final.ok; reasons = $final.reasons; anomalies = $s.anomalies
                           jobs_active = @($s.cargo.jobs | Where-Object { $_.state -notin 'Done', 'Failed', 'Cancelled' }).Count
                           drones_away = @($s.cargo.drones | Where-Object { $_.state -ne 'Docked' }).Count
                           boxes = @($s.cargo.boxes).Count; vehicles = @($s.vehicles).Count }
        if (-not $final.ok) { $report.failures += "did not settle at the end: $($final.reasons -join ',')" }
        if (@($s.anomalies).Count) { $report.failures += "anomalies at the end: $($s.anomalies -join ',')" }
        Invoke-SC /camera @{ focus = 'all' } | Out-Null
        Invoke-SC /screenshot @{ name = '999-end' } | Out-Null
        if ($Video) { $report.video = Invoke-SC /record @{ action = 'stop'; timeout_s = 900 } }
        $report.stop = Stop-SCViewer $p
    }
} catch {
    $report.failures += "driver: $($_.Exception.Message)"
} finally {
    if ($p -and -not $p.HasExited) { $p.Kill(); $report.stop = 'killed' }
    Remove-Item -Recurse -Force "$root\out\locks\ue.lock" -ErrorAction SilentlyContinue
    $gameLog = "$root\ue\SCViewer\Saved\Logs\$log"
    if (Test-Path $gameLog) {
        $report.crash = [bool](Select-String -Path $gameLog -Pattern 'Assertion failed|Fatal error|Unhandled Exception' -Quiet)
        $report.ensures = @(Select-String -Path $gameLog -Pattern 'Ensure condition failed' | Select-Object -First 5 | ForEach-Object { $_.Line })
        $report.overlaps = @(Select-String -Path $gameLog -Pattern 'CARGO_DRONE_OVERLAP').Count
        if ($report.crash) { $report.failures += 'crash in the game log' }
    }
    # This run's state log: the current file and the ones it rotated into (older runs' files have their own start).
    $logs = @(Get-ChildItem "$root\ue\SCViewer\Saved\Logs\SCState*.ndjson" | Where-Object { $_.LastWriteTime -ge $t0 })
    $report.state_logs = @($logs | ForEach-Object { "$($_.Name) $($_.Length)" })
    if ($logs) {
        $report.anomalies = @(Select-String -Path $logs.FullName -Pattern '"ev":"anomaly"' | ForEach-Object { ($_.Line | ConvertFrom-Json).key } | Sort-Object -Unique)
        $logs | ForEach-Object { Copy-Item $_.FullName "$OutDir\$($_.Name)" }
    }
    $report.game_log = $gameLog
    $report | ConvertTo-Json -Depth 6 | Set-Content "$OutDir\report.json" -Encoding utf8
    $report | ConvertTo-Json -Depth 6
}
