# Run against one already-running viewer. Quick mode needs only the game world.
param([int]$Port = 8770, [switch]$Acceptance, [string]$CompareWith = '', [string]$Tag = (Get-Date -Format 'yyyyMMdd-HHmmss'))
$ErrorActionPreference = 'Stop'
Import-Module "$PSScriptRoot/SCRpc.psm1" -Force
Set-SCRpcPort $Port
$root = (Resolve-Path "$PSScriptRoot/../../..").Path
$dir = Join-Path $root "out/h2/$Tag"
$count = if ($Acceptance) { 150 } else { 8 }
$fps = 30
$center = if ($Acceptance) { 'vehicle' } else { @(0, 0, 1) }
$width = if ($Acceptance) { 1920 } else { 320 }
$height = if ($Acceptance) { 1080 } else { 180 }
$orbit = @{ center = $center; radius = 25; height = 5; fromDeg = 0; toDeg = 90; seconds = ($count / $fps) }
$req = @{ path = $dir; frames = $count; fps = $fps; width = $width; height = $height
          warmup_frames = $(if ($Acceptance) { 32 } else { 2 }); outputs = @('png16','exr','png8')
          setup = @(@{ orbit = $orbit }); steps = @(@{ at = 0; orbit = $orbit }) }
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
}
$result = Invoke-SCRender $req
if (!$result.ok) { throw $result.error }
$sidecar = Get-Content -Raw -LiteralPath $result.sidecar | ConvertFrom-Json
if ($sidecar.frames.Count -ne $count) { throw "expected $count sidecar frames" }
for ($i = 0; $i -lt $count; $i++) {
    $f = $sidecar.frames[$i]
    if ($f.index -ne $i -or [math]::Abs($f.t - $i / $fps) -gt 0.000001) { throw "bad time or index at $i" }
    if ($i -gt 0) {
        $delta = ($f.rot[1] - $sidecar.frames[$i-1].rot[1] + 540) % 360 - 180
        if ([math]::Abs($delta - 90 / $count) -gt 0.05 * 90 / $count) { throw "bad camera step at $i" }
    }
}
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
        if ("$line" -match "lavfi\.signalstats\.$Metric=([\d.]+)") {
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
$stats = $diff | Measure-Object -Average -Minimum -Maximum
$pass = $stats.Minimum -gt 0 -and ($stats.Maximum - $stats.Minimum) -lt 0.05 * $stats.Average
$result | Add-Member -NotePropertyName 'image_evenness_pass' -NotePropertyValue $pass
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
$result
