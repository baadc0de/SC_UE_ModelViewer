"""Create the reusable, synthetic gobo master. Run with UnrealEditor-Cmd -run=pythonscript -AllowCommandletRendering
(compiling the material needs a renderer: not -nullrhi)."""
import unreal
path = "/SCBridge/Materials/M_SC_LightFunction"
material = unreal.load_asset(path)
if not material:
    material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        "M_SC_LightFunction", "/SCBridge/Materials", unreal.Material, unreal.MaterialFactoryNew())
material.set_editor_property("material_domain", unreal.MaterialDomain.MD_LIGHT_FUNCTION)
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
# WP-L3: the game samples a projector's texture with a border sampler (TiledShading SpotTexArray, SampStateTrilinearBorder),
# so nothing reaches past the texture's square; the importer opens the UE cone to that square's corners. Zero outside
# UV [0, 1] here (the light-function pass without the atlas; the atlas path is patched in ue/scripts/engine_patches.py).
uv = unreal.MaterialEditingLibrary.create_material_expression(material, unreal.MaterialExpressionTextureCoordinate)
border = unreal.MaterialEditingLibrary.create_material_expression(material, unreal.MaterialExpressionCustom)
border.set_editor_property("code", "return all(UV == saturate(UV)) ? 1.0f : 0.0f;")
border.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT1)
border_input = unreal.CustomInput()
border_input.set_editor_property("input_name", "UV")
border.set_editor_property("inputs", [border_input])
assert unreal.MaterialEditingLibrary.connect_material_expressions(uv, "", border, "UV")
masked = unreal.MaterialEditingLibrary.create_material_expression(material, unreal.MaterialExpressionMultiply)
assert unreal.MaterialEditingLibrary.connect_material_expressions(luminance, "", masked, "A")
assert unreal.MaterialEditingLibrary.connect_material_expressions(border, "", masked, "B")
assert unreal.MaterialEditingLibrary.connect_material_property(masked, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
unreal.MaterialEditingLibrary.recompile_material(material)
unreal.EditorAssetLibrary.save_loaded_asset(material)
unreal.log("WP-U3 LIGHT_MATERIAL_OK")
