"""Create the reusable, synthetic gobo master. Run with UnrealEditor-Cmd -run=pythonscript -AllowCommandletRendering
(compiling the material needs a renderer: not -nullrhi)."""
import unreal
path = "/SCBridge/Materials/M_SC_LightFunction"
material = unreal.load_asset(path)
if not material:
    material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        "M_SC_LightFunction", "/SCBridge/Materials", unreal.Material, unreal.MaterialFactoryNew())
material.set_editor_property("material_domain", unreal.MaterialDomain.MD_LIGHT_FUNCTION)
# WP-E1 E1-1: a material outside the light-function atlas (HLSLMaterialTranslator bIsLightFunctionAtlasCompatible) loses its
# gobo in MegaLights and makes deferred lights fall back to the per-light pass. The graph samples the gobo at the light
# function's own coordinate, which is what the atlas renders the material in, so the material declares itself compatible.
material.set_editor_property("force_compatible_with_light_function_atlas", True)
unreal.MaterialEditingLibrary.delete_all_material_expressions(material)
sample = unreal.MaterialEditingLibrary.create_material_expression(material, unreal.MaterialExpressionTextureSampleParameter2D)
sample.set_editor_property("parameter_name", "Gobo")
sample.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
sample.set_editor_property("texture", unreal.load_asset("/SCBridge/Materials/Textures/T_SC_WhiteLinear"))
# Light-function UVs are the engine's projected light coordinates, including spot perspective.
# Rec.709 luminance preserves green/blue projectors as well as red ones.
weights = unreal.MaterialEditingLibrary.create_material_expression(material, unreal.MaterialExpressionConstant3Vector)
weights.set_editor_property("constant", unreal.LinearColor(0.2126, 0.7152, 0.0722, 1.0))
luminance = unreal.MaterialEditingLibrary.create_material_expression(material, unreal.MaterialExpressionDotProduct)
assert unreal.MaterialEditingLibrary.connect_material_expressions(sample, "RGB", luminance, "A")
assert unreal.MaterialEditingLibrary.connect_material_expressions(weights, "", luminance, "B")
# The game samples a projector's texture with a border sampler (TiledShading SpotTexArray, SampStateTrilinearBorder), so
# nothing reaches past the texture's square. The engine fork does that for every light function (WP-E1 E1-3: the importer
# sets LightFunctionAddress Border, black outside the projected [0, 1] square in the atlas and the per-light pass), so the
# material is the gobo's luminance only.
assert unreal.MaterialEditingLibrary.connect_material_property(luminance, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
unreal.MaterialEditingLibrary.recompile_material(material)
unreal.EditorAssetLibrary.save_loaded_asset(material)
unreal.log("WP-U3 LIGHT_MATERIAL_OK")
