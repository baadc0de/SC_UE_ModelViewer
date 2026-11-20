# Measure a linear orbit using the recorded camera positions and independently supplied world-space centre.
param([Parameter(Mandatory)][string]$Sidecar, [Parameter(Mandatory)][double[]]$Center)
$ErrorActionPreference = 'Stop'
$shot = Get-Content -Raw -LiteralPath $Sidecar | ConvertFrom-Json
$orbit = $shot.request.steps[0].orbit
if ($Center.Count -ne 3 -or !$orbit -or $orbit.seconds -le 0 -or $shot.frames.Count -lt 2) {
    throw 'Expected a three-component centre and a multi-frame linear orbit'
}
$step = ($orbit.toDeg - $orbit.fromDeg) / ($orbit.seconds * $shot.fps)
$stepError = 0.0
$pathError = 0.0
$angleError = 0.0
$previous = 0.0
foreach ($frame in $shot.frames) {
    $x = $frame.at[0] - $Center[0]
    $y = $frame.at[1] - $Center[1]
    $angle = [math]::Atan2($y, $x) * 180 / [math]::PI
    $expected = $orbit.fromDeg + $frame.index * $step
    $angleError = [math]::Max($angleError, [math]::Abs([math]::IEEERemainder($angle - $expected, 360)))
    if ($frame.index -gt 0) {
        $stepError = [math]::Max($stepError, [math]::Abs([math]::IEEERemainder($angle - $previous - $step, 360)))
    }
    $previous = $angle
    $radialError = [math]::Sqrt($x * $x + $y * $y) - $orbit.radius
    $heightError = $frame.at[2] - $Center[2] - $orbit.height
    $pathError = [math]::Max($pathError, [math]::Sqrt($radialError * $radialError + $heightError * $heightError))
}
[pscustomobject]@{
    expected_step_deg = $step
    max_step_error_deg = $stepError
    max_angle_error_deg = $angleError
    max_radius_height_error_m = $pathError
    # Position metadata is rounded to millimetres. Allow one millimetre on each reported coordinate.
    passed = $stepError -le [math]::Abs($step * 0.05) -and $angleError -le [math]::Abs($step * 0.05) -and $pathError -le 0.002
}
