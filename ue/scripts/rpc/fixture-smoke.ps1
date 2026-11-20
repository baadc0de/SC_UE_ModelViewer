# Time launch and first usable scene. Supply -Project to reuse another worktree's already-built editor target.
# Fixtures cover different runtime paths; the smaller scenes do not exercise the XL hangar or C2 interactions.
[CmdletBinding()]
param(
    [ValidateSet('Viewer', 'Cutlass', 'C2XL')][string[]]$Fixture = @('Cutlass'),
    [string]$Project = '',
    [string]$Engine = '',
    [string]$OutDir = '',
    [ValidateRange(1024, 65535)][int]$Port = 8776,
    [ValidateRange(1, 3600)][int]$TimeoutSec = 600
)
$ErrorActionPreference = 'Stop'
Import-Module "$PSScriptRoot\SCRpc.psm1" -Force
if (-not $OutDir) { $OutDir = Join-Path "$PSScriptRoot\..\..\..\out" ("fixture-smoke-" + (Get-Date -Format yyyyMMdd-HHmmss)) }
$OutDir = (New-Item -ItemType Directory -Force $OutDir).FullName

$fixtures = @{
    Viewer = @{ entity = ''; environment = '' }
    Cutlass = @{ entity = 'DRAK_Cutlass_Black'; environment = '' }
    C2XL = @{ entity = 'CRUS_Starlifter_C2'; environment = 'Data/ObjectContainers/PU/loc/mod/common/hangar/hightech_a/hangar_xltop_001.socpak' }
}

Invoke-SCUELease {
    foreach ($name in $Fixture) {
        $expected = $fixtures[$name]
        $viewerArgs = @('-SCDefaults')
        if ($expected.entity) { $viewerArgs += "-SCLoad=$($expected.entity)" }
        if ($expected.environment) { $viewerArgs += "-SCEnvironment=$($expected.environment)" }

        $process = $null
        $log = Join-Path $OutDir "$name.log"
        $startArgs = @{
            Project = $Project; Port = $Port; ResX = 960; ResY = 540
            StartTimeoutSec = $TimeoutSec; Log = $log; Arguments = $viewerArgs
        }
        if ($Engine) { $startArgs.Engine = $Engine }
        $timer = [Diagnostics.Stopwatch]::StartNew()
        try {
            $process = Start-SCViewer @startArgs
            $healthSeconds = $timer.Elapsed.TotalSeconds
            $state = Wait-SCRpc -Path /state -Process $process -TimeoutSec $TimeoutSec -Condition {
                param($reply)
                if ($reply.load.phase -eq 'Failed') { throw "ship load failed; see $log" }
                $reply.idle -and $reply.load.entity -eq $expected.entity -and
                    (!$expected.entity -or $reply.load.phase -eq 'Succeeded') -and
                    $reply.load.environment -eq $expected.environment
            }
            $readySeconds = $timer.Elapsed.TotalSeconds
            $result = [ordered]@{
                fixture = $name; health_s = [math]::Round($healthSeconds, 2)
                ready_s = [math]::Round($readySeconds, 2); after_health_s = [math]::Round($readySeconds - $healthSeconds, 2)
                load = $state.load; log = $log
            }
        } finally {
            if ($process) { $shutdown = Stop-SCViewer $process -GraceSec 15 }
        }
        if ($shutdown -ne 'exit=0') { throw "viewer shutdown failed ($shutdown); see $log" }
        if ($expected.environment -and -not (Select-String -LiteralPath $log -Pattern 'ENVIRONMENT_(IMPORT seconds|COOKED level)=' -Quiet)) {
            throw "environment readiness was not confirmed; see $log"
        }
        $result.stop_s = [math]::Round($timer.Elapsed.TotalSeconds - $readySeconds, 2)
        $result | ConvertTo-Json -Depth 5 -Compress | Tee-Object -FilePath (Join-Path $OutDir "$name.json")
    }
}
