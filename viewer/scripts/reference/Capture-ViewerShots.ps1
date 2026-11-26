# Capture-ViewerShots: the viewer half of a game reference pair (WP-Q2, docs/work-packages/WP-Q2-game-reference-capture.md).
# For every shot of the list (viewer/reference/shots.json): sets the hangar and the vehicle, runs the console steps, puts the
# fly camera at the shot's pose and saves out/reference/<id>/viewer.png with viewer.json (pose, commit, time) beside it.
#   viewer/scripts/reference/Capture-ViewerShots.ps1                      # every shot, own viewer (off screen), quits after
#   viewer/scripts/reference/Capture-ViewerShots.ps1 -Id levski-medium-shield-close -Attach -Port 8770   # a running viewer
# Run from anywhere: the viewer starts from the repository root (its processed cache follows the working directory).
param([string]$Shots = '', [string[]]$Id = @(), [int]$Port = 8775, [switch]$Attach, [string]$Extra = '', [string]$Out = '')
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..\..\..").Path
. "$PSScriptRoot\ReferenceShots.ps1"
if (-not $Shots) { $Shots = Join-Path $root 'viewer\reference\shots.json' }
# Absolute: the viewer resolves the screenshot path against its own working directory, not this shell's.
$Out = if ($Out) { $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Out) } else { Join-Path $root 'out\reference' }
Import-Module "$root\viewer\scripts\rpc\SCRpc.psm1" -Force
Set-SCRpcPort $Port

$list = Read-ReferenceShots $Shots
if ($Id) {
    $unknown = $Id | Where-Object { $_ -notin $list.id }
    if ($unknown) { throw "no such shot: $($unknown -join ', ')" }
    $list = $list | Where-Object { $_.id -in $Id }
}
# Fewest reloads: shots of one hangar and vehicle together, in list order otherwise.
$list = @($list | Group-Object { "$($_.viewer.environment)|$($_.viewer.load)" } | ForEach-Object { $_.Group })

function Wait-For([string]$Until, [int]$TimeoutSec = 900) {
    $w = Invoke-SC /wait @{ until = $Until; timeout_s = $TimeoutSec } -TimeoutSec ($TimeoutSec + 30)
    if (-not $w.ok) { throw "viewer not $($Until): $($w.error)" }
}
function Assert-Ok($Answer, [string]$What) { if (-not $Answer.ok) { throw "$What failed: $($Answer.error)" } }

$commit = (git -C $root rev-parse --short HEAD).Trim()
$dirty = [bool](git -C $root status --porcelain --untracked-files=no -- plugins viewer game)
$proc = $null
if (-not $Attach) {
    # Never drive (or quit) a viewer that already owns the port: Start-SCViewer's /health check cannot tell them apart.
    $busy = try { (Invoke-RestMethod -Uri "http://127.0.0.1:$Port/health" -TimeoutSec 3).ok } catch { $false }
    if ($busy) { throw "a viewer already answers on port ${Port}: pass -Attach to use it, or another -Port" }
    $started = Start-SCViewer -Port $Port -Extra $Extra -Log 'SCReference.log'
    # Another viewer can still win the port between the probe and the start: drive only the process this script started.
    $owner = (Invoke-SC /health).pid
    if ($owner -ne $started.Id) {
        if (-not $started.HasExited) { $started.Kill() }
        throw "port $Port answers from process $owner, not the viewer started here ($($started.Id)): left it alone"
    }
    $proc = $started
}
try {
    Wait-For idle
    $environment = $null; $vehicle = $null
    foreach ($s in $list) {
        $v = $s.viewer
        if ($v.environment -and $v.environment -ne $environment) {
            Assert-Ok (Invoke-SC /environment @{ folder = $v.environment }) "environment $($v.environment)"
            Wait-For idle
            Start-Sleep 5 # the hangar's own placement settles after idle; a /load sent before it is dropped
            # /environment answers when the request is taken; a failed hangar ends idle too, without an environment.
            $st = (Invoke-SC /state).load
            if ($st.environment -ne $v.environment -or -not $st.environment_ready) {
                throw "environment $($v.environment) did not load (request '$($st.environment)', ready $($st.environment_ready))"
            }
            $environment = $v.environment; $vehicle = $null
        }
        if ($v.load -and $v.load -ne $vehicle) {
            Assert-Ok (Invoke-SC /load @{ entity = $v.load } -TimeoutSec 900) "load $($v.load)"
            Wait-For idle
            # /load answers when the request is taken; a failed load ends idle too.
            $main = (Invoke-SC /state).vehicles | Where-Object { $_.id -eq 0 } | Select-Object -First 1
            if (-not $main -or $main.entity -ne $v.load -or $main.state -ne 'ready') {
                throw "load $($v.load) did not finish: $(if ($main) { "$($main.entity) $($main.state) $($main.error)" } else { 'no vehicle' })"
            }
            $vehicle = $v.load
        }
        foreach ($cmd in @($v.console)) {
            if ($cmd) { Assert-Ok (Invoke-SC /console @{ cmd = $cmd }) "console '$cmd'"; Start-Sleep -Milliseconds 400 }
        }
        $cam = @{}
        foreach ($p in $v.camera.PSObject.Properties) { $cam[$p.Name] = $p.Value }
        Assert-Ok (Invoke-SC /camera $cam) "camera of $($s.id)"
        Wait-For settled 300 # streaming and shaders at the new pose
        $settle = if ($v.settle_s) { [double]$v.settle_s } else { 5 }
        Start-Sleep -Milliseconds ([int]($settle * 1000)) # exposure and temporal history settle at the new pose
        $dir = Join-Path $Out $s.id
        New-Item -ItemType Directory -Force $dir | Out-Null
        $png = Join-Path $dir 'viewer.png'
        Assert-Ok (Invoke-SC /screenshot @{ path = $png; ui = $false }) "screenshot of $($s.id)"
        $meta = [ordered]@{ id = $s.id; camera = $v.camera; environment = $v.environment; load = $v.load; console = $v.console
                            commit = $commit; dirty = $dirty; extra = $Extra; taken = (Get-Date).ToString('o') } | ConvertTo-Json -Depth 6
        [System.IO.File]::WriteAllText((Join-Path $dir 'viewer.json'), $meta, (New-Object System.Text.UTF8Encoding $false))
        "shot $($s.id) -> $png"
    }
} finally {
    if ($proc) { "viewer $(Stop-SCViewer $proc)" }
}
