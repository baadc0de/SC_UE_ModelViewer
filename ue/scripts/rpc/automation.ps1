# Run selected automation groups in one commandlet session. Only asset-free tests belong in the inner loop.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidatePattern('^[A-Za-z0-9_.]+$')][string[]]$Filter,
    [string]$Project = "$PSScriptRoot\..\..\SCViewer\SCViewer.uproject",
    [string]$Engine = $(if ($env:UE_ENGINE_ROOT) { $env:UE_ENGINE_ROOT } else { Join-Path $env:USERPROFILE 'UnrealEngine-5.9-src' }),
    [string]$Log = '',
    [ValidateRange(1, 3600)][int]$TimeoutSec = 180,
    [switch]$Build
)
$ErrorActionPreference = 'Stop'
Import-Module "$PSScriptRoot\SCRpc.psm1" -Force
$projectPath = (Resolve-Path -LiteralPath $Project).Path
$root = (Resolve-Path (Join-Path (Split-Path -Parent $projectPath) '..\..')).Path
$editor = Join-Path $Engine 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
if (-not (Test-Path -LiteralPath $editor)) { throw "could not find $editor" }
if (-not $Log) { $Log = Join-Path $root ("out\automation-" + (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '.log') }
$Log = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Log)
New-Item -ItemType Directory -Force (Split-Path -Parent $Log) | Out-Null
if (Test-Path -LiteralPath $Log) { throw "log already exists: $Log" }

Invoke-SCUELease {
    if ($Build) {
        $buildBat = Join-Path $Engine 'Engine\Build\BatchFiles\Build.bat'
        & $buildBat SCViewerEditor Win64 Development "-Project=$projectPath" -WaitMutex
        if ($LASTEXITCODE -ne 0) { throw "Unreal build exited with code $LASTEXITCODE" }
    }
    $timer = [Diagnostics.Stopwatch]::StartNew()
    # Use one Automation queue with '+' between groups. Apply Now and RunTests in the same
    # command because a second Automation command reinitializes the discovery timer.
    $groups = $Filter -join '+'
    $argv = @("`"$projectPath`"", '-unattended', '-nullrhi', '-nosound', '-nop4', '-nosplash',
        "-abslog=`"$Log`"", "-ExecCmds=`"Automation Now; RunTests $groups`"",
        '-TestExit="Automation Test Queue Empty"')
    $process = Start-Process -FilePath $editor -ArgumentList $argv -PassThru -WindowStyle Hidden -WorkingDirectory $root
    if (-not $process.WaitForExit($TimeoutSec * 1000)) {
        $process.Kill()
        if (-not $process.WaitForExit(10000)) { throw "Unreal automation PID $($process.Id) did not terminate" }
        throw "automation timed out after ${TimeoutSec}s; see $Log"
    }
    $summary = Select-String -LiteralPath $Log -Pattern 'Automation Test Queue Empty [1-9]\d* tests performed\.' | Select-Object -Last 1
    $failed = @(Select-String -LiteralPath $Log -Pattern 'Test Completed\. Result=\{(Fail|Error)\}')
    if ($process.ExitCode -ne 0 -or !$summary -or $failed.Count) {
        $failed | ForEach-Object { Write-Warning $_.Line }
        throw "automation failed (process exit $($process.ExitCode)); see $Log"
    }
    $successful = @(Select-String -LiteralPath $Log -Pattern 'Test Completed\. Result=\{Success\}.* Path=\{([^}]+)\}' |
        ForEach-Object { $_.Matches[0].Groups[1].Value })
    foreach ($group in $Filter) {
        $prefix = "$group."
        if (!($successful | Where-Object { $_ -eq $group -or $_.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) } |
                Select-Object -First 1)) {
            throw "no successful automation test matched $group; see $Log"
        }
    }
    [pscustomobject]@{
        filter = $groups; elapsed_s = [math]::Round($timer.Elapsed.TotalSeconds, 2)
        completed = $successful.Count
        log = $Log
    }
}
