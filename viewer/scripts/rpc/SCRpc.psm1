# SCRpc: PowerShell client of the viewer's RPC harness (WP-H1; docs/work-packages/WP-H1-rpc-harness.md).
#   Import-Module viewer/scripts/rpc/SCRpc.psm1
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
          [int]$StartTimeoutSec = 300)
    $editor = "$Engine\Engine\Binaries\Win64\UnrealEditor.exe"
    if (-not (Test-Path $editor)) { throw "no UnrealEditor.exe under $Engine (pass -Engine or set UE_ENGINE_ROOT)" }
    Set-SCRpcPort $Port
    $root = (Resolve-Path "$PSScriptRoot\..\..\..").Path
    $proj = "$root\viewer\SCViewer.uproject"
    $argv = @("`"$proj`"", '-game', '-windowed', "-resx=$ResX", "-resy=$ResY", '-nosplash', "-SCRpc=$Port", "-log=$Log")
    if (-not $Visible) { $argv += '-AgentMode' }
    $ffmpeg = Find-SCFfmpeg
    if ($ffmpeg) { $argv += "-SCFfmpeg=`"$ffmpeg`"" }
    if ($ShotDir) { $argv += "-SCShotDir=$ShotDir" }
    if ($Extra) { $argv += ($Extra -split ' ' | Where-Object { $_ }) }
    $style = if ($Visible) { 'Normal' } else { 'Minimized' }
    # From the repository root: the archive's processed cache goes to ./cache of the working directory (gitignored there).
    $p = Start-Process -FilePath $editor -ArgumentList $argv -PassThru -WindowStyle $style -WorkingDirectory $root
    $deadline = (Get-Date).AddSeconds($StartTimeoutSec)
    while ((Get-Date) -lt $deadline) {
        if ($p.HasExited) { throw "viewer exited during start (code $($p.ExitCode))" }
        try { $h = Invoke-RestMethod -Uri "$script:Base/health" -TimeoutSec 15; if ($h.ok) { return $p } } catch { }
        Start-Sleep -Milliseconds 500
    }
    throw "viewer did not answer /health within $StartTimeoutSec s"
}

function Stop-SCViewer {
    # Asks the viewer to quit; kills that process (by its id only) if it has not exited within the grace period.
    param([Parameter(Mandatory)][System.Diagnostics.Process]$Process, [int]$GraceSec = 60)
    if (-not $Process.HasExited) { Invoke-SC /quit | Out-Null }
    if (-not $Process.WaitForExit($GraceSec * 1000)) { $Process.Kill(); return 'killed' }
    "exit=$($Process.ExitCode)"
}

function Invoke-SCRender {
    param([Parameter(Mandatory)][hashtable]$Request)
    $deadline = if ($Request.ContainsKey('timeout_s')) { [int][math]::Ceiling($Request.timeout_s) } else { 3600 }
    Invoke-SC /render $Request -TimeoutSec ($deadline + 30)
}

Export-ModuleMember -Function Invoke-SCRender, Invoke-SC, Set-SCRpcPort, Start-SCViewer, Stop-SCViewer, Find-SCFfmpeg
