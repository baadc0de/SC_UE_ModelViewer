# Package the web build (UE 5.9 Wasm / WebGPU): package_showcase.ps1 -Platform Wasm (see there).
# Usage: ue/scripts/package_web.ps1 -Ship DRAK_Golem -Hangar hightech_a/hangar_smltop_001 [-BakedLighting] [-Config Shipping]
#        [-Out out/web/<ship>] [-Engine <UE 5.9>]
# Then serve: <Out>/Play_SCViewer.bat (or serve.py --root <Out>/Wasm) and open it in Chrome.
param(
    [Parameter(Mandatory = $true)][string]$Ship,
    [Parameter(Mandatory = $true)][string]$Hangar,
    [string]$Out = '',
    [string]$Engine = 'C:\Program Files\Epic Games\UE_5.9',
    [string]$Config = 'Development',
    [switch]$BakedLighting
)
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'package_showcase.ps1') -Platform Wasm @PSBoundParameters
