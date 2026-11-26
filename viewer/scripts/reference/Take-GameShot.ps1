# Take-GameShot: files a Star Citizen screenshot as the game half of a reference pair (WP-Q2,
# docs/work-packages/WP-Q2-game-reference-capture.md). Three steps per picture:
#   viewer/scripts/reference/Take-GameShot.ps1 -Arm                      # notes the pictures already in LIVE\ScreenShots
#   (press the game's screenshot key, Print Screen by default)
#   viewer/scripts/reference/Take-GameShot.ps1 -Id levski-medium-shield-straight -Note 'Levski, hangar 03, helmet on'
# Filing takes the newest picture that was not there when armed and copies it to out/reference/<id>/game-<variant>.<ext>
# with game-<variant>.json (source, time, note); a key press that wrote nothing is an error, never an older picture.
# -Screen grabs the primary display instead (borderless window mode; no arming), for when the game's key gives nothing.
# Input and screenshots only: nothing here reads or touches the game process.
param([string]$Id = '', [string]$Variant = 'a', [string]$Note = '', [switch]$Arm, [switch]$Screen,
      [string]$Game = 'C:\Program Files\Roberts Space Industries\StarCitizen\LIVE', [string]$Out = '')
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..\..\..").Path
. "$PSScriptRoot\ReferenceShots.ps1"
$Out = if ($Out) { $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Out) } else { Join-Path $root 'out\reference' }
$images = '.jpg', '.jpeg', '.png', '.bmp'
$utf8 = New-Object System.Text.UTF8Encoding $false
$shots = Join-Path $Game 'ScreenShots'
$armed = Join-Path $Out 'armed.txt' # the game's pictures when armed, one "path|write time" per line
function Get-GamePictures { @(Get-ChildItem -LiteralPath $shots -File -ErrorAction SilentlyContinue | Where-Object { $_.Extension -in $images }) }
function Get-Key($File) { "$($File.FullName)|$($File.LastWriteTimeUtc.Ticks)" }

if ($Arm) {
    New-Item -ItemType Directory -Force $Out | Out-Null
    [System.IO.File]::WriteAllLines($armed, [string[]]@(Get-GamePictures | ForEach-Object { Get-Key $_ }), $utf8)
    "armed: press the screenshot key, then file with -Id"
    return
}
if (-not $Id) { throw 'pass -Id <shot> to file a picture, or -Arm before pressing the screenshot key' }
if ($Id -notin (Read-ReferenceShots (Join-Path $root 'viewer\reference\shots.json')).id) { throw "no such shot in viewer/reference/shots.json: $Id" }
if (-not (Test-ReferenceName $Variant)) { throw 'variant: lower-case letters and digits joined by dashes' }
$dir = Join-Path $Out $Id
New-Item -ItemType Directory -Force $dir | Out-Null

if ($Screen) {
    Add-Type -AssemblyName System.Drawing, System.Windows.Forms
    $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
    $g.Dispose()
    $dest = Join-Path $dir "game-$Variant.png"
    $tmp = "$dest.tmp"
    $bmp.Save($tmp, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    $source = "screen $($b.Width)x$($b.Height)"
} else {
    if (-not (Test-Path -LiteralPath $armed)) { throw 'not armed: run with -Arm before pressing the screenshot key' }
    $before = [System.IO.File]::ReadAllLines($armed, $utf8)
    $pic = Get-GamePictures | Where-Object { (Get-Key $_) -notin $before } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $pic) { throw "no new game screenshot in $shots since -Arm: the screenshot key wrote nothing (arm, press, file again; or use -Screen)" }
    $dest = Join-Path $dir "game-$Variant$($pic.Extension.ToLower())"
    $tmp = "$dest.tmp"
    Copy-Item -LiteralPath $pic.FullName -Destination $tmp -Force
    $source = $pic.FullName
}
# Replace this variant's earlier picture (any image extension), nothing else.
Get-ChildItem -LiteralPath $dir -File | Where-Object { $_.BaseName -eq "game-$Variant" -and $_.Extension -in $images } | Remove-Item
Move-Item -LiteralPath $tmp -Destination $dest
if (-not $Screen) { Remove-Item -LiteralPath $armed } # one key press per arming
$meta = [ordered]@{ id = $Id; variant = $Variant; source = $source; note = $Note; taken = (Get-Date).ToString('o') } | ConvertTo-Json
[System.IO.File]::WriteAllText((Join-Path $dir "game-$Variant.json"), $meta, $utf8)
"game shot $Id/$Variant -> $dest"
