"""Create M_SC_DisplayMapping: the game's SDR display mapping as a UE tonemapper replacement.

Run with UnrealEditor-Cmd -run=pythonscript. The material is a post-process material at "Replacing the
Tonemapper": it reads the scene colour (PostProcessInput0) and the combined bloom (PostProcessInput2),
applies the eye-adaptation exposure, the game's display curve 1 and its sRGB encode
(ue/verification/WP-U3b.md §13). MaxDisplayNits / RefWhiteScale / ToeScale are set at run time from the
StarEngineRender fixture (SCViewerExposure.cpp); the asset's defaults are deliberately unusable.

The Custom node is a port of three shader functions of the game. The fixture pins their token hashes and
scripts/verify_star_engine_render.py re-checks them against the installed game; this script refuses to
build when the fixture no longer carries the hashes the port was written against.
"""
import json
import os
import unreal

PORTED = {  # function -> sha256 of the tokens the port below was written from (StarEngineRender.json)
    "HuePreservingShoulder": "765ad7fe9ff17ee77f7ff1f13107cf9fe9d4cf8e656393e11debae841dbd345b",
    "GetLuminance": "eab8d913434ab6867181ae9c532ac58f21003e7f8c6b4556281300f07bd6220f",
    "ToSRGB": "8c9a59f93c51fd030960b1cfff84ba735e9faa86b35993a420140f081ccfde59",
}
fixture_path = os.path.join(unreal.Paths.project_plugins_dir(), "SCBridge/Resources/StarEngineRender.json")
if not os.path.exists(fixture_path):  # the plugin lives beside the project (ue/Plugins)
    fixture_path = os.path.join(unreal.Paths.project_dir(), "../Plugins/SCBridge/Resources/StarEngineRender.json")
with open(fixture_path, encoding="utf-8") as handle:
    functions = json.load(handle)["shader_functions"]
changed = [name for name, digest in PORTED.items() if functions[name]["sha256"] != digest]
if changed:
    raise RuntimeError("the game's %s changed: port the new function before rebuilding the material" % ", ".join(changed))

# Stock tonemapper composition (PostProcessTonemap.usf): (scene + bloom) * OneOverPreExposure * exposure. The
# material translator already multiplies every PostProcessInput lookup of a material at this location by
# View.OneOverPreExposure (FHLSLMaterialTranslator::SceneTextureLookup), so only the exposure remains here.
# The curve and the encode follow the game's functions statement by statement; a Custom node cannot declare
# functions, so the three bodies are inlined.
CODE = """
float3 Colour = max((Scene.rgb + Bloom.rgb) * Exposure, 0.0);
// HuePreservingShoulder(Colour, MaxDisplayNits, RefWhiteScale, ToeScale)
const float RolloffRange = 0.5;
const float ToeRange = 0.15;
const float ToeStrength = 1.0 + 0.1 * ToeScale;
const float Luminance = dot(Colour, float3(0.2126, 0.7152, 0.0722)); // GetLuminance
const float RefWhite = lerp(1.0, RefWhiteScale, pow(saturate(Luminance), 0.25));
const float MaxDisplayLum = (MaxDisplayNits / RefWhite) / 80.0;
const float Peak = max(max(max(Colour.x, Colour.y), Colour.z), 1e-6);
float3 Ratios = Colour / Peak;
const float Knee = MaxDisplayLum - RolloffRange;
const float T = (Peak - Knee) / (MaxDisplayLum - Knee);
const float Toe = lerp(pow(Peak, ToeStrength), Peak, smoothstep(0.0, ToeRange, Peak));
const float CompressedPeak = Peak < Knee ? Toe : Knee + (MaxDisplayLum - Knee) * (1.0 - exp(-T));
float3 Desaturation = float3(3.0, 3.0, 3.0);
Desaturation.rg = lerp(Desaturation.rg, float2(2.0, 2.2), saturate(max(Ratios.r, Ratios.g) - Ratios.b * 3.0));
Desaturation.rg = lerp(Desaturation.rg, float2(5.0, 2.0), saturate(Ratios.b - max(Ratios.r * 3.0, Ratios.g)));
const float Average = dot(Colour, float3(0.3333, 0.3333, 0.3333));
Ratios = lerp(Ratios, 1.0, pow(Average / ((MaxDisplayLum - 0.2) + Average), Desaturation));
const float3 Display = saturate(CompressedPeak * Ratios * RefWhite); // DisplayMapping: saturate, then ToSRGB
return select(Display < 0.0031308, 12.92 * Display, 1.055 * pow(Display, 1.0 / 2.4) - 0.055);
"""

mel = unreal.MaterialEditingLibrary
path = "/SCBridge/Materials/M_SC_DisplayMapping"
material = unreal.load_asset(path)
if not material:
    material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        "M_SC_DisplayMapping", "/SCBridge/Materials", unreal.Material, unreal.MaterialFactoryNew())
material.set_editor_property("material_domain", unreal.MaterialDomain.MD_POST_PROCESS)
material.set_editor_property("blendable_location", unreal.BlendableLocation.BL_REPLACING_TONEMAPPER)
mel.delete_all_material_expressions(material)

custom = mel.create_material_expression(material, unreal.MaterialExpressionCustom, -200, 0)
custom.set_editor_property("description", "Star Engine display mapping (curve 1)")
custom.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT3)
custom.set_editor_property("code", CODE.strip())
names = ["Scene", "Bloom", "Exposure", "MaxDisplayNits", "RefWhiteScale", "ToeScale"]
inputs = []
for name in names:
    entry = unreal.CustomInput()
    entry.set_editor_property("input_name", name)
    inputs.append(entry)
custom.set_editor_property("inputs", inputs)

viewport_uv = mel.create_material_expression(material, unreal.MaterialExpressionScreenPosition, -1000, -200)
UV_INPUT = ""  # the expression's only input (the viewport UV)

def scene_texture(texture_id, filtered, y):
    node = mel.create_material_expression(material, unreal.MaterialExpressionSceneTexture, -700, y)
    node.set_editor_property("scene_texture_id", texture_id)
    node.set_editor_property("filtered", filtered)
    # A connected viewport UV makes the translator clamp the lookup to the input's bilinear viewport
    # (ClampSceneTextureUV); the default lookup is unclamped and bleeds at the viewport's last row and column.
    assert mel.connect_material_expressions(viewport_uv, "ViewportUV", node, UV_INPUT)
    return node

def scalar(name, default, y):
    node = mel.create_material_expression(material, unreal.MaterialExpressionScalarParameter, -700, y)
    node.set_editor_property("parameter_name", name)
    node.set_editor_property("default_value", default)
    return node

scene = scene_texture(unreal.SceneTextureId.PPI_POST_PROCESS_INPUT0, False, -300)
bloom = scene_texture(unreal.SceneTextureId.PPI_POST_PROCESS_INPUT2, True, -100)  # half resolution: bilinear
exposure = mel.create_material_expression(material, unreal.MaterialExpressionEyeAdaptation, -700, 100)
assert mel.connect_material_expressions(scene, "Color", custom, "Scene")
assert mel.connect_material_expressions(bloom, "Color", custom, "Bloom")
assert mel.connect_material_expressions(exposure, "", custom, "Exposure")
# Not the game's values: the run-time instance supplies them (a zero display peak renders black).
assert mel.connect_material_expressions(scalar("MaxDisplayNits", 0.0, 200), "", custom, "MaxDisplayNits")
assert mel.connect_material_expressions(scalar("RefWhiteScale", 1.0, 300), "", custom, "RefWhiteScale")
assert mel.connect_material_expressions(scalar("ToeScale", 0.0, 400), "", custom, "ToeScale")
assert mel.connect_material_property(custom, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
mel.recompile_material(material)
unreal.EditorAssetLibrary.save_loaded_asset(material)
unreal.log("WP-U3b DISPLAY_MAPPING_MATERIAL_OK")
