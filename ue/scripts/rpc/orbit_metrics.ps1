# Check a linear orbit's frame schedule, position and yaw against the request and independently supplied centre.
param([Parameter(Mandatory)][string]$Sidecar, [Parameter(Mandatory)][double[]]$Center)
$ErrorActionPreference = 'Stop'
$shot = Get-Content -Raw -LiteralPath $Sidecar | ConvertFrom-Json
$orbit = $shot.request.steps[0].orbit
$fps = $shot.request.fps
if ($Center.Count -ne 3 -or !$orbit -or $orbit.seconds -le 0 -or $orbit.fromDeg -eq $orbit.toDeg -or
    $fps -le 0 -or $shot.fps -ne $fps -or $shot.frames.Count -lt 2 -or $shot.frames.Count -ne $shot.request.frames) {
    throw 'Expected a three-component centre and a multi-frame linear orbit'
}
$step = ($orbit.toDeg - $orbit.fromDeg) / ($orbit.seconds * $fps)
$tolerance = [math]::Abs($step * 0.05)
$indicesMatch = $true
$timeError = 0.0
$stepError = 0.0
$yawStepError = 0.0
$pathError = 0.0
$angleError = 0.0
$previousAngle = 0.0
$previousYaw = 0.0
for ($i = 0; $i -lt $shot.frames.Count; $i++) {
    $frame = $shot.frames[$i]
    $indicesMatch = $indicesMatch -and $frame.index -eq $i
    if ($null -eq $frame.t) { throw "Missing timestamp at frame $i" }
    $timeError = [math]::Max($timeError, [math]::Abs($frame.t - $i / $fps))
    $x = $frame.at[0] - $Center[0]
    $y = $frame.at[1] - $Center[1]
    $angle = [math]::Atan2($y, $x) * 180 / [math]::PI
    $yaw = $frame.rot[1]
    $expected = $orbit.fromDeg + $i * $step
    $angleError = [math]::Max($angleError, [math]::Abs([math]::IEEERemainder($angle - $expected, 360)))
    if ($i -gt 0) {
        $stepError = [math]::Max($stepError, [math]::Abs([math]::IEEERemainder($angle - $previousAngle - $step, 360)))
        $yawStepError = [math]::Max($yawStepError, [math]::Abs([math]::IEEERemainder($yaw - $previousYaw - $step, 360)))
    }
    $previousAngle = $angle
    $previousYaw = $yaw
    $radialError = [math]::Sqrt($x * $x + $y * $y) - $orbit.radius
    $heightError = $frame.at[2] - $Center[2] - $orbit.height
    $pathError = [math]::Max($pathError, [math]::Sqrt($radialError * $radialError + $heightError * $heightError))
}
[pscustomobject]@{
    expected_step_deg = $step
    indices_match = $indicesMatch
    max_time_error_s = $timeError
    max_step_error_deg = $stepError
    max_yaw_step_error_deg = $yawStepError
    max_angle_error_deg = $angleError
    max_radius_height_error_m = $pathError
    # Position metadata is rounded to millimetres. Allow one millimetre on each reported coordinate.
    passed = $indicesMatch -and $timeError -le 0.000001 -and $stepError -lt $tolerance -and
        $yawStepError -lt $tolerance -and $angleError -lt $tolerance -and $pathError -le 0.002
}
