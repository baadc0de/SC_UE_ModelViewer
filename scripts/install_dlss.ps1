# Install NVIDIA's DLSS Unreal plugin into ue/Plugins/NVIDIA (gitignored: NVIDIA's license does not allow committing it)
# from the zip downloaded at https://developer.nvidia.com/rtx/dlss, and make it compile on UE 5.9.
#
# Copies DLSS and its one dependency, StreamlineNGXCommon (the other plugins in the zip are not used), drops the
# prebuilt editor modules and intermediates (built for the zip's engine version; UBT rebuilds them) and keeps NVIDIA's
# runtime DLLs (Binaries/ThirdParty). NVIDIA has no UE 5.9 release yet; its newest (8.7.2, UE 5.8) needs nine format
# arguments cast for 5.9's compile-time format-string check (enum / int64 values passed to %d / %u). Each replacement
# must match exactly once or the script stops: a newer plugin may not need them, or may need others.
#
# Usage: powershell -File scripts/install_dlss.ps1 -Zip <path to ..._DLSS..._Plugin_vX.Y.Z.zip>
param([Parameter(Mandatory = $true)][string]$Zip)
$PSDefaultParameterValues['*:ErrorAction'] = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $repo 'ue\Plugins\NVIDIA'
$work = Join-Path ([IO.Path]::GetTempPath()) ('dlss-' + [guid]::NewGuid().ToString('N'))

Write-Host "Extracting $Zip"
Expand-Archive -LiteralPath $Zip -DestinationPath $work
New-Item -ItemType Directory -Force $dest | Out-Null
foreach ($plugin in 'DLSS', 'StreamlineNGXCommon') {
    $src = Join-Path $work "Plugins\$plugin"
    if (-not (Test-Path (Join-Path $src "$plugin.uplugin"))) { throw "$plugin not found in $Zip" }
    $target = Join-Path $dest $plugin
    if (Test-Path $target) { Remove-Item -LiteralPath $target -Recurse -Force }
    Copy-Item -LiteralPath $src -Destination $target -Recurse
    foreach ($stale in 'Binaries\Win64', 'Intermediate') {
        $p = Join-Path $target $stale
        if (Test-Path $p) { Remove-Item -LiteralPath $p -Recurse -Force }
    }
    Write-Host "Installed $plugin -> $target"
}
Remove-Item -LiteralPath $work -Recurse -Force

# UE 5.9 format-string fixes: file relative to DLSS\Source, exact old text, new text, expected matches.
$fixes = @(
    @('DLSSBlueprint\Private\DLSSLibrary.cpp', 'with an invalid DLSSMode enum value (%d)', 'with an invalid DLSSMode enum value (%lld)', 2),
    @('DLSSBlueprint\Private\DLSSLibrary.cpp', 'InDLSSQualityMode from the higher level code"), InDLSSQualityMode);', 'InDLSSQualityMode from the higher level code"), int32(InDLSSQualityMode));', 1),
    @('DLSSBlueprint\Private\DLSSLibrary.cpp', 'QueryDLSSSRSupport() %u "), DLSSModule->QueryDLSSSRSupport());', 'QueryDLSSSRSupport() %u "), uint32(DLSSModule->QueryDLSSSRSupport()));', 1),
    @('DLSS\Private\DLSSUpscaler.cpp', 'TEXT("%u is not a valid Quality mode"), DLSSQualityMode);', 'TEXT("%u is not a valid Quality mode"), uint32(DLSSQualityMode));', 1),
    @('DLSS\Private\DLSSUpscaler.cpp', 'GetMaxUpsampleResolutionFraction(), DLSSQualityMode);', 'GetMaxUpsampleResolutionFraction(), int32(DLSSQualityMode));', 1),
    @('DLSS\Private\DLSSUpscaler.cpp', 'TEXT("%u is not a valid Quality mode"), Quality);', 'TEXT("%u is not a valid Quality mode"), uint32(Quality));', 4)
)
foreach ($f in $fixes) {
    $file = Join-Path $dest "DLSS\Source\$($f[0])"
    $text = [IO.File]::ReadAllText($file)
    $count = ([regex]::Matches($text, [regex]::Escape($f[1]))).Count
    if ($count -ne $f[3]) { throw "UE 5.9 fix: expected $($f[3]) x '$($f[1])' in $($f[0]), found $count" }
    [IO.File]::WriteAllText($file, $text.Replace($f[1], $f[2]))
}
Write-Host "Applied $($fixes.Count) UE 5.9 format-string fixes."

# The plugins declare the zip's engine version (5.8.0). An interactive editor asks whether to load them anyway; an
# unattended one (the cook of a packaged build) answers No and skips them, so their global shaders are not cooked and the
# packaged game, which loads them, stops at start ("Missing global shader FVelocityCombineCS"). Declare 5.9.
foreach ($plugin in 'DLSS', 'StreamlineNGXCommon') {
    $file = Join-Path $dest "$plugin\$plugin.uplugin"
    $text = [IO.File]::ReadAllText($file)
    $count = ([regex]::Matches($text, '"EngineVersion": "5\.8\.0"')).Count
    if ($count -ne 1) { throw "UE 5.9 fix: expected one EngineVersion 5.8.0 in $plugin.uplugin, found $count" }
    [IO.File]::WriteAllText($file, $text.Replace('"EngineVersion": "5.8.0"', '"EngineVersion": "5.9.0"'))
}
Write-Host "Declared UE 5.9 in DLSS.uplugin and StreamlineNGXCommon.uplugin. Build SCViewerEditor to compile the plugin."
