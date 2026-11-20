# SCRpc: PowerShell client of the viewer's RPC harness (WP-H1; docs/work-packages/WP-H1-rpc-harness.md).
#   Import-Module ue/scripts/rpc/SCRpc.psm1
#   $p = Start-SCViewer -Extra '-SCCargoScenario=c2_hangar'      # launches UnrealEditor -game with -SCRpc, waits for /health
#   Invoke-SC /wait @{ until = 'settled'; timeout_s = 600 }
#   Invoke-SC /cargo/transfer @{ from = 'elevator'; to = 'ship'; commodity = 'organic'; scu = 32 }
#   Stop-SCViewer $p
# Every call returns the parsed JSON answer; an HTTP error answer (ok=false) is returned too, not thrown.

$script:Base = 'http://127.0.0.1:8770'

function Set-SCRpcPort([int]$Port) { $script:Base = "http://127.0.0.1:$Port" }

function Invoke-SC {
    param([Parameter(Mandatory)][string]$Path, [hashtable]$Body = @{}, [int]$TimeoutSec = 900)
    $json = $Body | ConvertTo-Json -Depth 8 -Compress
    try {
        Invoke-RestMethod -Method Post -Uri "$script:Base$Path" -Body $json -ContentType 'application/json' -TimeoutSec $TimeoutSec
    } catch {
        $r = $_.Exception.Response
        if ($r -and $_.ErrorDetails.Message) { return $_.ErrorDetails.Message | ConvertFrom-Json }
        [pscustomobject]@{ ok = $false; error = "transport: $($_.Exception.Message)" }
    }
}

# Hold the machine's UE test slot across a build or a complete viewer session. The OS releases the
# file handle if the driver exits. All worktrees use the same path for this user.
function Enter-SCUELease {
    param([ValidateRange(1, 3600)][int]$TimeoutSec = 1200)
    $lockPath = Join-Path ([IO.Path]::GetTempPath()) 'SCViewer-Unreal.lock'
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $lease = $null
    while (-not $lease) {
        try { $lease = [IO.File]::Open($lockPath, 'OpenOrCreate', 'ReadWrite', 'None') }
        catch [IO.IOException] {
            # ERROR_SHARING_VIOLATION and ERROR_LOCK_VIOLATION mean another driver owns the slot.
            if (($_.Exception.HResult -band 0xffff) -notin 32, 33) { throw }
            if ($timer.Elapsed.TotalSeconds -ge $TimeoutSec) { throw "Unreal slot still in use after ${TimeoutSec}s ($lockPath)" }
            Start-Sleep -Milliseconds 250
        }
    }
    $lease
}

function Invoke-SCUELease {
    param([Parameter(Mandatory)][scriptblock]$Run, [ValidateRange(1, 3600)][int]$TimeoutSec = 1200)
    $lease = Enter-SCUELease -TimeoutSec $TimeoutSec
    try { & $Run } finally { $lease.Dispose() }
}

# Use /wait for the server's built-in conditions. Use this for a feature condition (such as a
# specific ship being ready) that /wait cannot assert. The predicate must return one boolean.
function Wait-SCRpc {
    param([Parameter(Mandatory)][string]$Path, [hashtable]$Body = @{},
          [Parameter(Mandatory)][scriptblock]$Condition, [System.Diagnostics.Process]$Process,
          [ValidateRange(1, 3600)][int]$TimeoutSec = 60, [ValidateRange(25, 5000)][int]$IntervalMs = 150)
    $timer = [Diagnostics.Stopwatch]::StartNew()
    do {
        if ($Process -and $Process.HasExited) { throw "viewer exited while waiting on $Path (code $($Process.ExitCode))" }
        $remainingSec = [math]::Max(1, [math]::Ceiling($TimeoutSec - $timer.Elapsed.TotalSeconds))
        $last = Invoke-SC -Path $Path -Body $Body -TimeoutSec ([math]::Min(3, $remainingSec))
        if ($last.ok -and (& $Condition $last)) { return $last }
        $remainingMs = $TimeoutSec * 1000 - $timer.ElapsedMilliseconds
        if ($remainingMs -gt 0) { Start-Sleep -Milliseconds ([math]::Min($IntervalMs, $remainingMs)) }
    } while ($timer.Elapsed.TotalSeconds -lt $TimeoutSec)
    $detail = if ($last.error) { $last.error } else { ($last | ConvertTo-Json -Depth 4 -Compress) }
    throw [TimeoutException]::new("timed out after ${TimeoutSec}s waiting on ${Path}. Last response: $detail")
}
function Find-SCFfmpeg {
    # ffmpeg for /record: on PATH, else a winget install (its PATH change does not reach shells started before it).
    $c = Get-Command ffmpeg -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    $w = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\*FFmpeg*\*\bin\ffmpeg.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($w) { return $w.FullName }
    $null
}

function Start-SCViewer {
    # Launches the viewer as a game with the RPC on; returns the process once /health answers (or throws).
    # Without -Visible the viewer runs in UE 5.9 agent mode: rendered off screen (no window to take focus), unattended,
    # no modal dialogs; screenshots and /record still capture the viewport.
    # -Engine: the SCViewer fork of UE 5.9 built from source (plan D17); default $env:UE_ENGINE_ROOT, else
    # %USERPROFILE%\UnrealEngine-5.9-src.
    param([string]$Extra = '', [int]$Port = 8770, [string]$Log = 'SCRpc.log', [string]$ShotDir = '', [switch]$Visible,
          [int]$ResX = 1920, [int]$ResY = 1080,
          [string]$Engine = $(if ($env:UE_ENGINE_ROOT) { $env:UE_ENGINE_ROOT } else { Join-Path $env:USERPROFILE 'UnrealEngine-5.9-src' }),
          [ValidateRange(1, 3600)][int]$StartTimeoutSec = 600, [string]$Project = '', [string[]]$Arguments = @())
    $editor = "$Engine\Engine\Binaries\Win64\UnrealEditor.exe"
    if (-not (Test-Path $editor)) { throw "no UnrealEditor.exe under $Engine (pass -Engine or set UE_ENGINE_ROOT)" }
    Set-SCRpcPort $Port
    $root = (Resolve-Path "$PSScriptRoot\..\..\..").Path
    $proj = if ($Project) { (Resolve-Path -LiteralPath $Project -ErrorAction Stop).Path } else { "$root\ue\SCViewer\SCViewer.uproject" }
    if ($Project) { $root = (Resolve-Path (Join-Path (Split-Path -Parent $proj) '..\..')).Path }
    $argv = @("`"$proj`"", '-game', '-windowed', "-resx=$ResX", "-resy=$ResY", '-nosplash', "-SCRpc=$Port")
    $argv += if ([IO.Path]::IsPathRooted($Log)) { "-abslog=`"$Log`"" } else { "-log=`"$Log`"" }
    if (-not $Visible) { $argv += '-AgentMode' }
    $ffmpeg = Find-SCFfmpeg
    if ($ffmpeg) { $argv += "-SCFfmpeg=`"$ffmpeg`"" }
    if ($ShotDir) { $argv += "-SCShotDir=$ShotDir" }
    if ($Extra) { $argv += ($Extra -split ' ' | Where-Object { $_ }) }
    foreach ($arg in $Arguments) {
        if ($arg.Contains('"')) { throw "Unexpected embedded quote in Unreal argument: $arg" }
        $argv += if ($arg -match '\s') { '"' + $arg + '"' } else { $arg }
    }
    $style = if ($Visible) { 'Normal' } else { 'Hidden' }
    # From the repository root: the archive's processed cache goes to ./cache of the working directory (gitignored there).
    $p = Start-Process -FilePath $editor -ArgumentList $argv -PassThru -WindowStyle $style -WorkingDirectory $root
    $timer = [Diagnostics.Stopwatch]::StartNew()
    try {
        do {
            if ($p.HasExited) { throw "viewer exited during start (code $($p.ExitCode)); see $Log" }
            $remaining = [math]::Max(1, [math]::Ceiling($StartTimeoutSec - $timer.Elapsed.TotalSeconds))
            try { $health = Invoke-RestMethod -Uri "$script:Base/health" -TimeoutSec ([math]::Min(2, $remaining)) }
            catch { $health = $null }
            if ($health.ok) {
                if ($health.pid -ne $p.Id) { throw "RPC port $Port belongs to viewer PID $($health.pid), expected $($p.Id)" }
                return $p
            }
            if ($timer.Elapsed.TotalSeconds -lt $StartTimeoutSec) { Start-Sleep -Milliseconds 150 }
        } while ($timer.Elapsed.TotalSeconds -lt $StartTimeoutSec)
        throw "viewer did not answer /health within ${StartTimeoutSec}s; see $Log"
    } catch {
        if (-not $p.HasExited) { $p.Kill() }
        [void]$p.WaitForExit(10000)
        throw
    }
}
function Stop-SCViewer {
    # Asks the viewer to quit; kills that process (by its id only) if it has not exited within the grace period.
    param([Parameter(Mandatory)][System.Diagnostics.Process]$Process, [int]$GraceSec = 60)
    if (-not $Process.HasExited) { Invoke-SC /quit -TimeoutSec ([math]::Max(1, [math]::Min(5, $GraceSec))) | Out-Null }
    if (-not $Process.WaitForExit($GraceSec * 1000)) {
        $Process.Kill()
        if (-not $Process.WaitForExit(10000)) { throw "viewer PID $($Process.Id) did not terminate" }
        return 'killed'
    }
    "exit=$($Process.ExitCode)"
}

Export-ModuleMember -Function Enter-SCUELease, Invoke-SCUELease, Wait-SCRpc, Invoke-SC, Set-SCRpcPort, Start-SCViewer, Stop-SCViewer, Find-SCFfmpeg
