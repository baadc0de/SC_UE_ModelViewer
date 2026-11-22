# List the authored exhaust effects used by the supplied decomposed ship packages for SCFxBuild.
param([Parameter(Mandatory)][string[]]$Package)

$ErrorActionPreference = 'Stop'
$references = [Collections.Generic.SortedSet[string]]::new([StringComparer]::Ordinal)
foreach ($path in $Package) {
    $item = Get-Item -LiteralPath $path
    if ($item.Extension -eq '.json') {
        $json = Get-Content -LiteralPath $item.FullName -Raw
    } else {
        $reader = [IO.BinaryReader]::new($item.OpenRead())
        try {
            # scviewer-core writes every package's scene.json as its first entry.
            $header = $reader.ReadBytes(512)
            if ($header.Length -ne 512) { throw "Incomplete package $path" }
            $name = [Text.Encoding]::UTF8.GetString($header, 0, 100).Trim([char]0)
            if ([IO.Path]::GetFileName($name) -ne 'scene.json') { throw "Expected scene.json first in $path" }
            $octalSize = [Text.Encoding]::ASCII.GetString($header, 124, 12).Trim([char[]]@(0, 32))
            $size = [Convert]::ToInt32($octalSize, 8)
            if ($size -lt 2 -or $size -gt 32MB) { throw "Unexpected scene.json size in $path" }
            $bytes = $reader.ReadBytes($size)
            if ($bytes.Length -ne $size) { throw "Incomplete scene.json in $path" }
            $json = [Text.Encoding]::UTF8.GetString($bytes)
        } finally {
            $reader.Dispose()
        }
    }
    $scene = $json | ConvertFrom-Json -Depth 100
    foreach ($child in $scene.children) {
        foreach ($effect in $child.thruster_vfx.effects) {
            if ($effect.reference) { [void]$references.Add($effect.reference) }
        }
    }
}
if ($references.Count -eq 0) { throw 'No authored thruster effects found. Export the ship with attachments.' }
$references | Write-Output
