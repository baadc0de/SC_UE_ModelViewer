"""Create the reusable, synthetic gobo master. Run with UnrealEditor-Cmd -run=pythonscript."""
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
assert unreal.MaterialEditingLibrary.connect_material_property(sample, "R", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
unreal.MaterialEditingLibrary.recompile_material(material)
unreal.EditorAssetLibrary.save_loaded_asset(material)
unreal.log("WP-U3 LIGHT_MATERIAL_OK")
