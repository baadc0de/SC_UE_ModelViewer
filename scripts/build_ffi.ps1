# Build and stage the Win64 FFI library/header (Windows counterpart of scripts/build_ffi.sh). Default: debug.
# Usage: scripts/build_ffi.ps1 [-Release] [-CheckHeader]
param([switch]$Release, [switch]$CheckHeader)
# Native tools report through exit codes (cargo writes progress to stderr, which Windows PowerShell 5.1 would turn
# into terminating errors under ErrorActionPreference=Stop); cmdlets below pass -ErrorAction Stop themselves.
$PSDefaultParameterValues['*:ErrorAction'] = 'Stop'
Set-Location (Split-Path -Parent $PSScriptRoot)
$env:CARGO_TARGET_DIR = (Join-Path (Get-Location) 'target')
$profileArgs = @(); $profile = 'debug'
if ($Release) { $profileArgs = @('--release'); $profile = 'release' }
function Invoke-Checked { & $args[0] $args[1..($args.Count - 1)]; if ($LASTEXITCODE -ne 0) { throw "failed: $args" } }
$thirdParty = 'ue/Plugins/SCBridge/Source/ThirdParty/SCBridgeLib'
if ($CheckHeader) {
    Invoke-Checked cargo run --locked @profileArgs -p scbridge-ffi --example header -- --check
    if ((Get-FileHash crates/scbridge-ffi/include/scbridge.h).Hash -ne (Get-FileHash "$thirdParty/include/scbridge.h").Hash) {
        throw 'staged scbridge.h differs from crates/scbridge-ffi/include/scbridge.h'
    }
    return
}
Write-Host "Building SCBridge FFI profile: $profile"
Invoke-Checked cargo run --locked @profileArgs -p scbridge-ffi --example header
Invoke-Checked cargo build --locked @profileArgs -p scbridge-ffi --example discover
# Build consumers together: a stale CLI embeds a different exporter/cache identity.
Invoke-Checked cargo build --locked @profileArgs -p scbridge-ffi -p scviewer-cli
$out = "target/$profile"
$lib = "$thirdParty/lib/Win64"
New-Item -ItemType Directory -Force "$thirdParty/include", $lib | Out-Null
Copy-Item crates/scbridge-ffi/include/scbridge.h "$thirdParty/include/scbridge.h"
Copy-Item "$out/scbridge.dll", "$out/scbridge.dll.lib" $lib
if (Test-Path "$out/scbridge.pdb") { Copy-Item "$out/scbridge.pdb" $lib }
# UBT copies the DLL into the binaries only when it builds; refresh stale copies so a launch without a rebuild
# picks up the new exporter (and its cache identity).
foreach ($bin in 'ue/SCViewer/Binaries/Win64', 'ue/Plugins/SCBridge/Binaries/Win64') {
    if (Test-Path "$bin/scbridge.dll") { Copy-Item "$out/scbridge.dll" $bin }
}
Get-Item "$lib/scbridge.dll" | ForEach-Object { "{0}: {1} bytes" -f $_.FullName, $_.Length }
