# Build-ReferencePage: out/reference/index.html, every shot's viewer and game pictures side by side (WP-Q2,
# docs/work-packages/WP-Q2-game-reference-capture.md). Click a pair to flip between them in one frame (differences jump
# out); the notes Take-GameShot.ps1 filed are under each game picture. Local only: game pictures never leave out/.
param([string]$Out = '')
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..\..\..").Path
. "$PSScriptRoot\ReferenceShots.ps1"
$Out = if ($Out) { $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Out) } else { Join-Path $root 'out\reference' }
$list = Read-ReferenceShots (Join-Path $root 'viewer\reference\shots.json') # ids are checked: safe in paths, URLs and HTML
function Enc([string]$s) { [System.Net.WebUtility]::HtmlEncode($s) }
function Read-Json([string]$Path) { [System.IO.File]::ReadAllText($Path, [System.Text.Encoding]::UTF8) | ConvertFrom-Json }

$sections = foreach ($s in $list) {
    $dir = Join-Path $Out $s.id
    $viewer = if (Test-Path (Join-Path $dir 'viewer.png')) { "$($s.id)/viewer.png" }
    $meta = if (Test-Path (Join-Path $dir 'viewer.json')) { Read-Json (Join-Path $dir 'viewer.json') }
    $games = @(Get-ChildItem -LiteralPath $dir -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^game-[a-z0-9-]+\.(jpe?g|png|bmp)$' } | Sort-Object Name)
    $vcap = if ($meta) { "viewer $($meta.commit)$(if ($meta.dirty) { '+' }) $($meta.extra)" } else { 'viewer: not captured' }
    $pairs = if (-not $games) {
        "<div class='pair'><figure>$(if ($viewer) { "<img src='$viewer' alt='viewer'>" } else { "<div class='none'>no viewer shot</div>" })<figcaption>$(Enc $vcap)</figcaption></figure><figure><div class='none'>no game shot yet</div></figure></div>"
    } else {
        foreach ($g in $games) {
            $nf = Join-Path $dir ($g.BaseName + '.json')
            $note = if (Test-Path $nf) { (Read-Json $nf).note }
            "<div class='pair' tabindex='0'><figure>$(if ($viewer) { "<img src='$viewer' alt='viewer'>" } else { "<div class='none'>no viewer shot</div>" })<figcaption>$(Enc $vcap)</figcaption></figure>" +
            "<figure><img src='$($s.id)/$($g.Name)' alt='game'><figcaption>game $($g.BaseName.Substring(5)): $(Enc $note)</figcaption></figure></div>"
        }
    }
    @"
<section id='$($s.id)'>
<h2>$(Enc $s.title) <code>$($s.id)</code></h2>
<p>$(Enc $s.why)</p>
<details><summary>How it is framed</summary><p><b>Viewer:</b> $(Enc ($s.viewer.camera | ConvertTo-Json -Compress)) in $(Enc $s.viewer.environment) $(Enc $s.viewer.load)</p>
<p><b>Game:</b> $(Enc $s.game.where) $(Enc $s.game.frame) $(Enc $s.game.conditions)</p></details>
$($pairs -join "`n")
</section>
"@
}

$html = @"
<!doctype html>
<html lang='en'><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'>
<title>Game Reference Pairs</title>
<style>
:root { --bg: #f6f6f4; --fg: #1c1c1a; --muted: #6a6a64; --card: #ffffff; --line: #deded8; --accent: #2f6fb3; }
@media (prefers-color-scheme: dark) { :root:not([data-theme='light']) { --bg: #151514; --fg: #ececE8; --muted: #9a9a92; --card: #1f1f1d; --line: #33332f; --accent: #7fb0e6; } }
:root[data-theme='dark'] { --bg: #151514; --fg: #ececE8; --muted: #9a9a92; --card: #1f1f1d; --line: #33332f; --accent: #7fb0e6; }
body { margin: 0; padding: 24px 16px; background: var(--bg); color: var(--fg); font: 15px/1.5 system-ui, sans-serif; }
main { max-width: 1800px; margin: 0 auto; }
h1 { font-size: 22px; margin: 0 0 4px; } h2 { font-size: 17px; margin: 0 0 6px; } code { color: var(--muted); font-size: 13px; }
section { background: var(--card); border: 1px solid var(--line); border-radius: 8px; padding: 16px; margin: 16px 0; }
p { margin: 4px 0; color: var(--muted); } details { margin: 6px 0 12px; } summary { cursor: pointer; color: var(--accent); }
.pair { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; margin-top: 12px; cursor: pointer; outline: none; }
.pair.flip { grid-template-columns: 1fr; } .pair.flip figure:first-child { display: none; }
.pair.flip.show-viewer figure:first-child { display: block; } .pair.flip.show-viewer figure:last-child { display: none; }
figure { margin: 0; } img { width: 100%; height: auto; display: block; border-radius: 4px; background: #000; }
figcaption { font-size: 13px; color: var(--muted); margin-top: 4px; }
.none { aspect-ratio: 16 / 9; display: grid; place-items: center; border: 1px dashed var(--line); border-radius: 4px; color: var(--muted); }
@media (max-width: 700px) { .pair { grid-template-columns: 1fr; } }
</style></head>
<body><main>
<h1>Game reference pairs</h1>
<p>Viewer left, game right. Click a pair to show one picture at full width, then click again to flip between the two in the same place.</p>
$($sections -join "`n")
</main>
<script>
document.querySelectorAll('.pair').forEach(p => p.addEventListener('click', () => {
  if (!p.classList.contains('flip')) { p.classList.add('flip'); return; }
  p.classList.toggle('show-viewer');
}));
document.addEventListener('keydown', e => { if (e.key === 'Escape') document.querySelectorAll('.pair').forEach(p => p.classList.remove('flip', 'show-viewer')); });
</script>
</body></html>
"@
New-Item -ItemType Directory -Force $Out | Out-Null
$page = Join-Path $Out 'index.html'
[System.IO.File]::WriteAllText($page, $html, (New-Object System.Text.UTF8Encoding $false))
"page $page ($(@($list).Count) shots)"
