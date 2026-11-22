# Score two completed render_check captures from one viewer session and display mode.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Reference,
    [Parameter(Mandatory)][string]$Candidate,
    [Parameter(Mandatory)][string]$Tolerance,
    [switch]$Calibrate
)
$ErrorActionPreference = 'Stop'
$a = (Resolve-Path -LiteralPath $Reference).Path
$b = (Resolve-Path -LiteralPath $Candidate).Path
if ($a -eq $b) { throw 'Use two independent captures' }

function Sort-JsonFields($value) {
    if ($null -ne $value -and $value.GetType() -eq [System.Management.Automation.PSCustomObject]) {
        $fields = [ordered]@{}
        foreach ($property in ($value.PSObject.Properties | Sort-Object Name)) {
            $fields[$property.Name] = Sort-JsonFields $property.Value
        }
        return $fields
    }
    if ($value -is [array]) {
        return ,@($value | ForEach-Object { Sort-JsonFields $_ })
    }
    $value
}

function Read-Fixture($sidecar) {
    $request = $sidecar.request | Select-Object -Property * -ExcludeProperty path, warmup_frames
    $request | Add-Member -NotePropertyName warmup_frames -NotePropertyValue $sidecar.warmup_frames
    [ordered]@{
        request = Sort-JsonFields $request
        cvars = Sort-JsonFields $sidecar.cvars
        anti_aliasing_method = $sidecar.frames[0].anti_aliasing_method
        display_method = $sidecar.frames[0].display_method
        ray_tracing_allowed = $sidecar.frames[0].ray_tracing_allowed
    }
}

function Read-Capture([string]$dir) {
    $s = Get-Content -Raw -LiteralPath (Join-Path $dir 'render.json') | ConvertFrom-Json
    if (!$s.ok -or $s.running -or !$s.frames.Count -or $s.frames.Count -ne $s.request.frames -or
        $s.outputs -notcontains 'png16') { throw "Incomplete PNG16 capture: $dir" }
    if ($null -eq $s.warmup_frames -or
        ($null -ne $s.request.warmup_frames -and $s.request.warmup_frames -ne $s.warmup_frames)) {
        throw "Invalid effective warm-up in capture: $dir"
    }
    $pngDir = Join-Path $dir 'png16'
    $files = @(Get-ChildItem -LiteralPath $pngDir -Filter '*.png' -File | Sort-Object Name)
    if ($files.Count -ne $s.frames.Count) { throw "Wrong PNG16 file count: $dir" }
    $header = [byte[]]::new(26)
    for ($i = 0; $i -lt $files.Count; $i++) {
        if ($s.frames[$i].index -ne $i -or $files[$i].Name -ne ('{0:D6}.png' -f $i)) {
            throw "Missing or out-of-order frame ${i}: $dir"
        }
        $file = $files[$i].OpenRead()
        try {
            if ($file.Read($header, 0, $header.Length) -ne $header.Length -or
                [Text.Encoding]::ASCII.GetString($header, 12, 4) -cne 'IHDR' -or
                $header[24] -ne 16 -or $header[25] -notin 2, 6) {
                throw "Expected a 16-bit RGB PNG: $($files[$i].FullName)"
            }
            $width = [Net.IPAddress]::NetworkToHostOrder([BitConverter]::ToInt32($header, 16))
            $height = [Net.IPAddress]::NetworkToHostOrder([BitConverter]::ToInt32($header, 20))
            if ($width -ne $s.request.width -or $height -ne $s.request.height) {
                throw "PNG dimensions differ from the render request: $($files[$i].FullName)"
            }
        } finally { $file.Dispose() }
    }
    $s
}

$referenceCapture = Read-Capture $a
$repeat = Read-Capture $b
$referenceHash = (Get-FileHash -LiteralPath (Join-Path $a 'render.json')).Hash
$candidateHash = (Get-FileHash -LiteralPath (Join-Path $b 'render.json')).Hash
if ($referenceHash -eq $candidateHash) { throw 'Use two independent captures' }
$fixture = Read-Fixture $referenceCapture
$fixtureJson = $fixture | ConvertTo-Json -Depth 12 -Compress
if ($referenceCapture.commit -cne $repeat.commit -or $referenceCapture.engine -cne $repeat.engine -or
    $fixtureJson -cne ((Read-Fixture $repeat) | ConvertTo-Json -Depth 12 -Compress)) {
    throw 'Render request, build, or rendering settings differ'
}
$frameFields = @('index', 't', 'at', 'rot', 'fov', 'anti_aliasing_method', 'display_method',
    'ray_tracing_allowed', 'ssdo_direct', 'ssdo_ambient', 'ssdo_reflections')
for ($i = 0; $i -lt $referenceCapture.frames.Count; $i++) {
    $first = $referenceCapture.frames[$i] | Select-Object -Property $frameFields | ConvertTo-Json -Compress
    $second = $repeat.frames[$i] | Select-Object -Property $frameFields | ConvertTo-Json -Compress
    if ($first -cne $second) { throw "Camera, timing, or look differs at frame $i" }
    if ($repeat.frames[$i].ray_tracing_allowed -cne $fixture.ray_tracing_allowed) {
        throw "Ray-tracing permission changed at frame $i"
    }
}

if (!$Calibrate) {
    $limits = Get-Content -Raw -LiteralPath $Tolerance | ConvertFrom-Json
    if ($fixtureJson -cne ($limits.fixture | ConvertTo-Json -Depth 12 -Compress) -or
        $limits.calibration_sidecar_sha256.Count -ne 2 -or
        $limits.shot_mae_limit -le 0 -or $limits.worst_frame_mae_limit -lt $limits.shot_mae_limit -or
        $limits.worst_frame_mae_limit -gt 1) { throw 'Tolerance is invalid for this capture fixture' }
    if ($candidateHash -in $limits.calibration_sidecar_sha256) {
        throw 'The repeat candidate must be held out from calibration'
    }
}

Import-Module "$PSScriptRoot/SCRpc.psm1"
$ff = Find-SCFfmpeg
if (!$ff) { throw 'ffmpeg is required to compare PNG16 repeats' }
# Put the absolute R, G and B differences into a single full-range 16-bit luma plane.
$filter = '[0:v]format=gbrp16le[a];[1:v]format=gbrp16le[b];' +
    '[a][b]blend=all_mode=difference:shortest=1,extractplanes=g+b+r[green][blue][red];' +
    '[green][blue][red]hstack=inputs=3,scale=in_range=full:out_range=full,format=yuv444p16le,' +
    'signalstats,metadata=mode=print:file=-'
$measurements = & $ff -hide_banner -loglevel error -nostdin -start_number 0 -i (Join-Path $a 'png16/%06d.png') `
    -start_number 0 -i (Join-Path $b 'png16/%06d.png') -filter_complex $filter -f null -
if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed (exit $LASTEXITCODE)" }
$frameMeans = @(foreach ($line in $measurements) {
    if ($line -match '^lavfi\.signalstats\.YAVG=(.+)$') {
        [double]::Parse($Matches[1], [cultureinfo]::InvariantCulture) / 65535
    }
})
$framePeaks = @(foreach ($line in $measurements) {
    if ($line -match '^lavfi\.signalstats\.YMAX=(\d+)$') { [int]$Matches[1] / 65535 }
})
if ($frameMeans.Count -ne $referenceCapture.frames.Count -or $framePeaks.Count -ne $referenceCapture.frames.Count) {
    throw 'ffmpeg did not compare every expected frame'
}
$stats = $frameMeans | Measure-Object -Average -Maximum
$report = [ordered]@{
    reference = $a
    candidate = $b
    frames = $frameMeans.Count
    shot_mae = $stats.Average
    worst_frame_mae = $stats.Maximum
    worst_frame_index = [array]::IndexOf($frameMeans, $stats.Maximum)
    peak = ($framePeaks | Measure-Object -Maximum).Maximum
}

if ($Calibrate) {
    $limits = [ordered]@{
        fixture = $fixture
        calibration_warmup_frames = $referenceCapture.warmup_frames
        calibration_sidecar_sha256 = @($referenceHash, $candidateHash)
        baseline_shot_mae = $report.shot_mae
        baseline_worst_frame_mae = $report.worst_frame_mae
        shot_mae_limit = [math]::Max(1, [math]::Ceiling($report.shot_mae * 1.25 * 65535)) / 65535
        worst_frame_mae_limit = [math]::Max(1, [math]::Ceiling($report.worst_frame_mae * 1.25 * 65535)) / 65535
    }
    $limits | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $Tolerance -Encoding utf8
    [pscustomobject]$limits
} else {
    $report.shot_mae_limit = $limits.shot_mae_limit
    $report.worst_frame_mae_limit = $limits.worst_frame_mae_limit
    $report.passed = $report.shot_mae -le $report.shot_mae_limit -and
        $report.worst_frame_mae -le $report.worst_frame_mae_limit
    $report | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $b 'repeat-report.json') -Encoding utf8
    [pscustomobject]$report
    if (!$report.passed) { throw 'Live PNG16 repeat exceeded its calibrated tolerance' }
}
