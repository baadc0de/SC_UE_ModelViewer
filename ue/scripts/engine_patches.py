#!/usr/bin/env python3
"""Apply, check or revert the viewer's engine shader patches (ue/EnginePatches) in the installed Unreal Engine.

The engine is a binary install, so only shaders can be patched; they recompile on the next editor or cook run.
Each patched engine file keeps its original beside it as <name>.scviewer-orig; `apply` always starts from that
original, so re-applying picks up changes to the patch. Constants are taken from the game data in
ue/Plugins/SCBridge/Resources/StarEngineRender.json (re-verified by scripts/verify_star_engine_render.py).

  engine_patches.py apply | check | revert      (engine root: $UE_ENGINE_ROOT or ~/UnrealEngine/5.9)
"""
import json
import os
import shutil
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
PATCHES = REPO / "ue" / "EnginePatches"
FIXTURE = REPO / "ue" / "Plugins" / "SCBridge" / "Resources" / "StarEngineRender.json"
ENGINE = Path(os.environ.get("UE_ENGINE_ROOT", Path.home() / "UnrealEngine" / "5.9"))
MEGALIGHTS = ENGINE / "Engine" / "Shaders" / "Private" / "MegaLights"
MARK = "// SCViewer patch (ue/EnginePatches)"
ORIG = ".scviewer-orig"

# WP-L2: Star Engine SSDO for lights that cast no shadow (MegaLights shading pass) and for ambient light (the Lumen
# indirect composite, Substrate path).
SHADING = MEGALIGHTS / "MegaLightsShading.usf"
COMPOSITE = ENGINE / "Engine" / "Shaders" / "Private" / "DiffuseIndirectComposite.usf"
SHADING_EDITS = [
    # (anchor, text inserted before it, text inserted after it[, replacement of the anchor])
    ('#include "MegaLightsShading.ush"\n', "", f'#include "SCSSDO.ush" {MARK}\n'),
    (
        "\t\t\t\t\twhile (NextLocalLightIndex < MAX_LOCAL_LIGHT_INDEX)\n",
        f"\t\t\t\t\t{MARK}: the pixel's SSDO, computed on the first light that needs it.\n"
        "\t\t\t\t\tFSCSSDO SCSSDOData = (FSCSSDO)0;\n"
        "\t\t\t\t\tbool bSCSSDOValid = false;\n\n",
        "",
    ),
    (
        "\t\t\t\t\t\t\t\tconst float Lum = Luminance(SplitLighting.DiffuseLighting.xyz + SplitLighting.SpecularLighting.xyz) * View.PreExposure;\n",
        f"\t\t\t\t\t\t\t\t{MARK}: TiledShading.cfx scales the light term of a light that is not a shadow caster, or the\n"
        "\t\t\t\t\t\t\t\t// sun, by CalculateSSDO. r.GeneralPurposeTweak scales the amount (development A/B; 1 in shipping).\n"
        "\t\t\t\t\t\t\t\tconst float SCSSDOAmount = SC_SSDO_AMOUNT_DIRECT * View.GeneralPurposeTweak;\n"
        "\t\t\t\t\t\t\t\tif (SCSSDOAmount > 0.0f && (!UnpackCastShadow(ForwardLightData) || !LightData.bRadialLight))\n"
        "\t\t\t\t\t\t\t\t{\n"
        "\t\t\t\t\t\t\t\t\tif (!bSCSSDOValid)\n"
        "\t\t\t\t\t\t\t\t\t{\n"
        "\t\t\t\t\t\t\t\t\t\tSCSSDOData = SCComputeSSDO(ScreenCoord, Material.Depth, Material.WorldNormal, TranslatedWorldPosition);\n"
        "\t\t\t\t\t\t\t\t\t\tbSCSSDOValid = true;\n"
        "\t\t\t\t\t\t\t\t\t}\n"
        "\t\t\t\t\t\t\t\t\tconst float3 SCToLight = LightData.bRadialLight ? LightData.TranslatedWorldPosition - TranslatedWorldPosition : LightData.Direction * 1e8f;\n"
        "\t\t\t\t\t\t\t\t\tconst float SCLightDist = max(length(SCToLight), 1e-4f);\n"
        "\t\t\t\t\t\t\t\t\tconst float SCCameraDist = max(length(TranslatedWorldPosition - View.TranslatedWorldCameraOrigin), 1e-4f);\n"
        "\t\t\t\t\t\t\t\t\tconst float SCOccl = SCCalculateSSDO(SCLightDist, SCCameraDist, SCToLight / SCLightDist, SCSSDOData, SCSSDOAmount);\n"
        "\t\t\t\t\t\t\t\t\tSplitLighting.DiffuseLighting *= SCOccl;\n"
        "\t\t\t\t\t\t\t\t\tSplitLighting.SpecularLighting *= SCOccl;\n"
        "\t\t\t\t\t\t\t\t}\n\n",
        "",
    ),
    (
        "\t\t\t\t\t\tSpecularLighting /= Factors.Specular;\n",
        "",
        f"\n\t\t\t\t\t\t{MARK}: debug view (development). r.GeneralPurposeTweak2 2 = the SSDO AO (1 - occlusion),\n"
        "\t\t\t\t\t\t// 3 = the bent normal, as demodulated diffuse light (no specular).\n"
        "\t\t\t\t\t\tif (View.GeneralPurposeTweak2 >= 1.5f)\n"
        "\t\t\t\t\t\t{\n"
        "\t\t\t\t\t\t\tconst FSCSSDO SCDebug = SCComputeSSDO(ScreenCoord, Material.Depth, Material.WorldNormal, TranslatedWorldPosition);\n"
        "\t\t\t\t\t\t\tDiffuseLighting = (View.GeneralPurposeTweak2 >= 2.5f ? SCDebug.ConeDir * 0.5f + 0.5f : (1.0f - SCDebug.Occlusion).xxx) * View.PreExposure;\n"
        "\t\t\t\t\t\t\tSpecularLighting = 0.0f;\n"
        "\t\t\t\t\t\t}\n",
    ),
]

COMPOSITE_EDITS = [
    ('#include "BlueNoise.ush"\n', "", f'#include "MegaLights/SCSSDO.ush" {MARK}\n'),
    (
        "\tSubstrate_for(uint ClosureIndex = 0, ClosureIndex < SubstratePixelHeader.ClosureCount, ++ClosureIndex)\n",
        f"\t{MARK}: the pixel's SSDO, computed on the first closure that needs it.\n"
        "\tFSCSSDO SCSSDOData = (FSCSSDO)0;\n"
        "\tbool bSCSSDOValid = false;\n\n",
        "",
    ),
    (
        "\t\tconst FShadingOcclusion Occlusion = GetShadingOcclusion(PixelPos, LumenBufferCoord, ClosureIndex, V, SubstrateBSDFContext.N, Roughness, DiffuseColor, InAO, bHasBackfaceDiffuse);\n",
        "",
        f"\t\t{MARK}: MergedTiledShading.cfi occludes the ambient (probe) light with the SSDO instead of Lumen's\n"
        "\t\t// short-range AO: diffuse x (1 - occlusion * r_ssdoAmountAmbient), rough specular x DeriveSpecularOcclusion.\n"
        "\t\t// r.GeneralPurposeTweak 0 restores Lumen's short-range AO (development A/B; 1 in shipping).\n"
        "\t\t#if DIM_APPLY_DIFFUSE_INDIRECT == DIM_APPLY_DIFFUSE_INDIRECT_SCREEN_PROBE_GATHER\n"
        "\t\tif (SC_SSDO_AMOUNT_AMBIENT * View.GeneralPurposeTweak > 0.0f)\n"
        "\t\t{\n"
        "\t\t\tif (!bSCSSDOValid)\n"
        "\t\t\t{\n"
        "\t\t\t\tSCSSDOData = SCComputeSSDOAtPixel(PixelPos, SubstrateBSDFContext.N);\n"
        "\t\t\t\tbSCSSDOValid = true;\n"
        "\t\t\t}\n"
        "\t\t\tconst float SCAmbient = saturate(1.0f - SCSSDOData.Occlusion * SC_SSDO_AMOUNT_AMBIENT * View.GeneralPurposeTweak);\n"
        "\t\t\tconst float SCReflection = saturate(1.0f - SCSSDOData.Occlusion * SC_SSDO_AMOUNT_REFLECTION * View.GeneralPurposeTweak);\n"
        "\t\t\tOcclusion.DiffuseOcclusion = SCAmbient;\n"
        "\t\t\tOcclusion.SpecularOcclusion = SCDeriveSpecularOcclusion(dot(SubstrateBSDFContext.N, V), SCReflection, 1.0f - Roughness);\n"
        "\t\t}\n"
        "\t\t#endif\n",
        "\t\tFShadingOcclusion Occlusion = GetShadingOcclusion(PixelPos, LumenBufferCoord, ClosureIndex, V, SubstrateBSDFContext.N, Roughness, DiffuseColor, InAO, bHasBackfaceDiffuse);\n",
    ),
]



# WP-L3: the range window of every local light is the game's GetLightForcedDistanceFade (ShadeLib), full strength until the
# last forcedFalloffFraction of the radius, then saturate((1 - d / R) / fraction)^2; UE's is (1 - (d / R)^4)^2, which
# already halves a light at 0.8 R. Shading (GetLocalLightAttenuation: deferred, MegaLights, Lumen, translucency, volumes)
# and the light grid's energy estimate (EstimateLocalLight, MegaLights sampling and culling) change together.
LIGHTING = ENGINE / "Engine" / "Shaders" / "Private" / "DeferredLightingCommon.ush"
CULLING = ENGINE / "Engine" / "Shaders" / "Private" / "LightCulling.ush"


def fade_rcp():
    fraction = float(json.loads(FIXTURE.read_text())["shader_constants"]["forcedFalloffFraction"]["value"])
    if not 0 < fraction <= 1:
        sys.exit(f"forcedFalloffFraction {fraction} is outside (0, 1] in {FIXTURE}")
    return f"{1.0 / fraction:.6f}f"


# WP-L3 also carries GetPhysicalLightAttenuation's tweak term: for a point or spot light the game's falloff is
# candela / (max(d, bulb) + offset)^2 with offset = bulb (1 - tweak) / tweak (FSCStarEngine::AttenuationOffset), UE's is
# candela / d^2. UE has no per-light field for the offset; the importer stores it in SoftSourceRadius, which reaches every
# light path (deferred uniforms, the light grid, Lumen, GPU scene) and which UE reads only as a soft specular radius
# (GetCapsule -> SphereSinAlphaSoft). The patch drops that reading and scales the light mask by d^2 / (max(d, bulb) +
# offset)^2. Rect lights are left alone (their SourceRadius is a half-width; their SoftSourceRadius is always 0).
OFFSET_MASK = (
    "\t\tif (LightData.SoftSourceRadius != 0 && !LightData.bRectLight)\n"
    "\t\t{\n"
    "\t\t\tconst float SCDistance = sqrt(DistanceSqr);\n"
    "\t\t\tLightMask *= Square( SCDistance / max( max(SCDistance, LightData.SourceRadius) + LightData.SoftSourceRadius, 1e-4f ) );\n"
    "\t\t}\n"
)


def lighting_edits():
    return [
        (
            "\t\tLightMask = Square( saturate( 1 - Square( DistanceSqr * Square(LightData.InvRadius) ) ) );\n",
            f"\t\t{MARK}: Star Engine GetLightForcedDistanceFade (WP-L3), in place of (1 - (d / R)^4)^2;\n"
            "\t\t// the factor is 1 / forcedFalloffFraction from StarEngineRender.json.\n",
            f"\t\t{MARK}: GetPhysicalLightAttenuation's tweak offset, carried in SoftSourceRadius (WP-L3).\n" + OFFSET_MASK,
            f"\t\tLightMask = Square( saturate( {fade_rcp()} * (1 - sqrt(DistanceSqr) * LightData.InvRadius) ) );\n",
        ),
        (
            "\tCapsule.SoftRadius = LightData.SoftSourceRadius;\n",
            f"\t{MARK}: SoftSourceRadius carries the game's attenuation offset (WP-L3), not a soft specular radius.\n",
            "",
            "\tCapsule.SoftRadius = 0;\n",
        ),
    ]


def culling_edits():
    return [
        (
            "\t\tLightFalloff = Pow2(saturate(1.0 - Pow2(NormalizeDistanceSquared))) / DistanceSquared;\n",
            f"\t\t{MARK}: the light's range window is the game's forced fade (WP-L3, DeferredLightingCommon.ush).\n",
            "",
            f"\t\tLightFalloff = Pow2(saturate({fade_rcp()} * (1.0 - sqrt(NormalizeDistanceSquared)))) / DistanceSquared;\n",
        ),
        (
            "\tfloat LightPower = ComputeLightPower(LightData);\n",
            "",
            f"\t{MARK}: the game's attenuation offset (WP-L3, DeferredLightingCommon.ush GetLocalLightAttenuation).\n"
            "\tif (LightData.FalloffExponent == 0 && LightData.SoftSourceRadius != 0 && !LightData.bRectLight)\n"
            "\t{\n"
            "\t\tconst float SCDistance = sqrt(max(DistanceSquared, 0.01));\n"
            "\t\tLightPower *= Pow2(SCDistance / max(max(SCDistance, LightData.SourceRadius) + LightData.SoftSourceRadius, 1e-4f));\n"
            "\t}\n",
        ),
    ]


LOOK = PATCHES / "look.json"


def params():
    cvars = json.loads(FIXTURE.read_text())["exe_cvars"]
    # The look (plan D15) may switch the game's SSDO off: amounts 0 leave the patched passes inert (Lumen short-range AO).
    ssdo = json.loads(LOOK.read_text()).get("ssdo", True) if LOOK.exists() else True
    value = lambda name: float(cvars[name]["value"]) if ssdo or not name.startswith("r_ssdoAmount") else 0.0
    return (
        f"{MARK}: generated by ue/scripts/engine_patches.py from StarEngineRender.json; do not edit.\n"
        "#pragma once\n"
        f"#define SC_SSDO_RADIUS_METERS {value('r_ssdoRadius'):.6f}f  // r_ssdoRadius\n"
        f"#define SC_SSDO_AO_STRENGTH {value('r_ssdoStrength'):.6f}f  // r_ssdoStrength\n"
        f"#define SC_SSDO_AMOUNT_DIRECT {value('r_ssdoAmountDirect'):.6f}f  // r_ssdoAmountDirect\n"
        f"#define SC_SSDO_AMOUNT_AMBIENT {value('r_ssdoAmountAmbient'):.6f}f  // r_ssdoAmountAmbient\n"
        f"#define SC_SSDO_AMOUNT_REFLECTION {value('r_ssdoAmountReflection'):.6f}f  // r_ssdoAmountReflection\n"
        f"#define SC_SSDO_TEMPORAL 1\n"
    )


# WP-L3: a game projector is its texture sampled with a border sampler (TiledShading SpotTexArray,
# SampStateTrilinearBorder): nothing beyond the texture's square. UE's light-function atlas wraps spot UVs (frac), which
# tiles the gobo into the frustum corners the importer now opens the cone to; zero spot light functions outside [0, 1].
ATLAS = ENGINE / "Engine" / "Shaders" / "Private" / "LightFunctionAtlas" / "LightFunctionAtlasCommon.usf"
ATLAS_EDITS = [
    (
        "\tLightFunctionUV = frac(LightFunctionUV);\n",
        f"\t{MARK}: the game's projector border (WP-L3): a spot light function is zero outside its texture.\n"
        "\tconst float SCProjectorBorder = (Slot.LightType == LIGHT_TYPE_SPOT && any(LightFunctionUV != saturate(LightFunctionUV))) ? 0.0f : 1.0f;\n",
        "",
    ),
    (
        "\tFLightFunctionColor LightFunctionColor = SampleLightFunctionSlot(Slot, LightFunctionUV);\n",
        "",
        f"\tLightFunctionColor *= SCProjectorBorder; {MARK} (WP-L3)\n",
    ),
]

PATCHED = {SHADING: SHADING_EDITS, COMPOSITE: COMPOSITE_EDITS, LIGHTING: lighting_edits, CULLING: culling_edits, ATLAS: ATLAS_EDITS}


def patched(path, edits):
    edits = edits() if callable(edits) else edits
    orig = path.with_name(path.name + ORIG)
    text = (orig if orig.exists() else path).read_text()
    if MARK in text:
        sys.exit(f"{path} is patched but {orig.name} is missing; restore the engine file first")
    for anchor, before, after, *replacement in edits:
        if text.count(anchor) != 1:
            sys.exit(f"anchor not found exactly once in {path.name} (engine version changed?):\n{anchor}")
        text = text.replace(anchor, before + (replacement[0] if replacement else anchor) + after)
    return text


def wanted():
    """Map of engine path -> intended content."""
    files = {path: patched(path, edits) for path, edits in PATCHED.items()}
    files[MEGALIGHTS / "SCSSDO.ush"] = (PATCHES / "MegaLights" / "SCSSDO.ush").read_text()
    files[MEGALIGHTS / "SCSSDOParams.ush"] = params()
    return files

def main(argv):
    cmd = argv[1] if len(argv) > 1 else "check"
    if cmd == "revert":
        for path in PATCHED:
            orig = path.with_name(path.name + ORIG)
            if orig.exists():
                path.chmod(path.stat().st_mode | 0o200)
                shutil.move(orig, path)
        for name in ("SCSSDO.ush", "SCSSDOParams.ush"):
            (MEGALIGHTS / name).unlink(missing_ok=True)
        print("ENGINE_PATCHES reverted")
        return 0
    files = wanted()
    stale = [p for p, text in files.items() if not p.exists() or p.read_text() != text]
    if cmd == "check":
        for p in stale:
            print("STALE", p)
        print(f"ENGINE_PATCHES {'OK' if not stale else 'NOT APPLIED'}: {len(files) - len(stale)}/{len(files)} files current")
        return 1 if stale else 0
    if cmd != "apply":
        sys.exit(__doc__)
    for path in PATCHED:
        orig = path.with_name(path.name + ORIG)
        if not orig.exists():
            shutil.copy2(path, orig)
    for p in stale:
        if p.exists():
            p.chmod(p.stat().st_mode | 0o200)  # the installed engine's shaders are read-only
        p.write_text(files[p])
        print("WROTE", p)
    print(f"ENGINE_PATCHES applied ({len(stale)} changed)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
