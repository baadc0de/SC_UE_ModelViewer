# Run against one already-running viewer. Quick mode needs only the game world.
param([int]$Port = 8770, [switch]$Acceptance, [int]$FrameLimit = 0, [string]$CompareWith = '', [string]$Tag = (Get-Date -Format 'yyyyMMdd-HHmmss'))
$ErrorActionPreference = 'Stop'
Import-Module "$PSScriptRoot/SCRpc.psm1" -Force
Set-SCRpcPort $Port
$root = (Resolve-Path "$PSScriptRoot/../../..").Path
$dir = Join-Path $root "out/h2/$Tag"
$count = if ($Acceptance) { 150 } else { 8 }
$orbitFrames = $count
if ($FrameLimit) {
    if ($FrameLimit -lt 2 -or $FrameLimit -gt $count) { throw "FrameLimit must be between 2 and $count" }
    $count = $FrameLimit
}
$fps = 30
$center = if ($Acceptance) { 'vehicle' } else { @(0, 0, 1) }
$worldCenter = @(0, 0, 1)
$width = if ($Acceptance) { 1920 } else { 320 }
$height = if ($Acceptance) { 1080 } else { 180 }
$orbit = @{ center = $center; radius = 25; height = 5; fromDeg = 0; toDeg = 90; seconds = ($orbitFrames / $fps) }
$req = @{ path = $dir; frames = $count; fps = $fps; width = $width; height = $height
          outputs = $(if ($Acceptance) { @('png16','exr') } else { @('png16','exr','png8') })
          setup = @(@{ orbit = $orbit }); steps = @(@{ at = 0; orbit = $orbit }) }
if (!$Acceptance) { $req.warmup_frames = 2 }
if ($Acceptance) {
    $ready = Invoke-SC /wait @{ until = 'settled'; timeout_s = 600 }
    if (!$ready.ok) { throw $ready.error }
    $s = Invoke-SC /state
    if (!$s.ok) { throw $s.error }
    if (!($s.vehicles | Where-Object { $_.id -eq 0 -and $_.entity -eq 'DRAK_Cutlass_Black' })) {
        $load = Invoke-SC /load @{ entity = 'DRAK_Cutlass_Black' }
        if (!$load.ok) { throw $load.error }
        $ready = Invoke-SC /wait @{ until = 'settled'; timeout_s = 600 }
        if (!$ready.ok) { throw $ready.error }
    }
    $state = Invoke-SC /state
    $vehicle = $state.vehicles | Where-Object { $_.id -eq 0 -and $_.entity -eq 'DRAK_Cutlass_Black' } | Select-Object -First 1
    if (!$state.ok -or !$vehicle -or $vehicle.pos.Count -ne 3) { throw 'Cannot resolve the Cutlass orbit centre' }
    $worldCenter = $vehicle.pos
}
$result = Invoke-SCRender $req
if (!$result.ok) { throw $result.error }
$sidecar = Get-Content -Raw -LiteralPath $result.sidecar | ConvertFrom-Json
if ($sidecar.frames.Count -ne $count) { throw "expected $count sidecar frames" }
if ($sidecar.fps -ne $fps) { throw "expected $fps fps in the sidecar" }
$geometry = & "$PSScriptRoot/orbit_metrics.ps1" -Sidecar $result.sidecar -Center $worldCenter
$result | Add-Member -NotePropertyName 'orbit_geometry' -NotePropertyValue $geometry
if (!$geometry.passed) { throw "Camera timing or pose diverged from the authored orbit: $($geometry | ConvertTo-Json -Compress)" }
foreach ($format in $req.outputs) {
    if ((Get-ChildItem -File (Join-Path $dir $format)).Count -ne $count) { throw "bad frame count for $format" }
}
$ff = Find-SCFfmpeg
if (!$ff) { throw 'ffmpeg is required to verify image differences' }
$pattern = Join-Path $dir 'png16/%06d.png'
function Read-ImageMetric([string[]]$FfmpegArgs, [string]$Metric) {
    $lines = & $ff @FfmpegArgs 2>&1
    if ($LASTEXITCODE -ne 0) { throw ($lines -join [Environment]::NewLine) }
    foreach ($line in $lines) {
        if ("$line" -match "lavfi\.signalstats\.$Metric=(\S+)") {
            [double]::Parse($Matches[1], [cultureinfo]::InvariantCulture)
        }
    }
}
# All three channels are rearranged into a grey strip, retaining the source precision.
$stack = 'format=gbrp16le,extractplanes=g+b+r[gg][bb][rr];[gg][bb][rr]hstack=inputs=3'
$filter = "$stack,tblend=all_mode=difference,format=gray16le,scale=in_range=full:out_range=full,format=yuv444p16le,signalstats,metadata=print"
$diff = @(Read-ImageMetric @('-hide_banner','-nostdin','-i',$pattern,'-vf',$filter,'-f','null','-') 'YAVG')
if ($diff.Count -ne $count - 1) { throw "expected $($count - 1) consecutive image comparisons, got $($diff.Count)" }
$result | Add-Member -NotePropertyName 'image_step_luma16' -NotePropertyValue $diff
# Adjacent image differences reflect lighting and scene detail. Use them to detect exact repeats.
$repeatedFrames = @(for ($i = 0; $i -lt $diff.Count; $i++) { if ($diff[$i] -eq 0) { $i + 1 } })
$result | Add-Member -NotePropertyName 'repeated_image_frames' -NotePropertyValue $repeatedFrames
$result | Add-Member -NotePropertyName 'wall_s_per_frame' -NotePropertyValue ($result.render_s / $count)
$result | Add-Member -NotePropertyName 'holds_total' -NotePropertyValue (($sidecar.frames | Measure-Object -Property holds -Sum).Sum)
if ($CompareWith) {
    $other = Join-Path (Resolve-Path $CompareWith) 'png16/%06d.png'
    # Raw gbrp16 differences remain per channel, full range; stats take a maximum over every pixel.
    $filter = '[0:v]format=gbrp16le[a];[1:v]format=gbrp16le[b];[a][b]blend=all_mode=difference,extractplanes=g+b+r[g][b2][r];[g][b2][r]hstack=inputs=3,scale=in_range=full:out_range=full,format=yuv444p16le,signalstats,metadata=print'
    $max = @(Read-ImageMetric @('-hide_banner','-nostdin','-i',$pattern,'-i',$other,'-filter_complex',$filter,'-f','null','-') 'YMAX')
    if ($max.Count -ne $count) { throw "comparison supplied $($max.Count) matching frames, expected $count" }
    $result | Add-Member -NotePropertyName 'comparison_peak' -NotePropertyValue (($max | Measure-Object -Maximum).Maximum / 65535)
}
$result | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $dir 'report.json')
if ($repeatedFrames.Count) { throw "Identical adjacent RGB frames at indices $($repeatedFrames -join ', ')" }
$result
