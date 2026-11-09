"""Recreate the synthetic Viewer smoke scene; run inside Unreal's Python host."""
import unreal

levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
assets = unreal.AssetToolsHelpers.get_asset_tools()
assert levels.new_level('/Game/Maps/Viewer')


def spawn(cls, label, location, rotation=unreal.Rotator()):
    actor = actors.spawn_actor_from_class(cls, unreal.Vector(*location), rotation)
    actor.set_actor_label(label)
    return actor


sky = spawn(unreal.SkyLight, 'Sky light', (0, 0, 800))
sky.light_component.set_editor_property('mobility', unreal.ComponentMobility.MOVABLE)
sky.light_component.set_editor_property('real_time_capture', True)
sun = spawn(unreal.DirectionalLight, 'Sun', (0, 0, 600), unreal.Rotator(-45, -30, 0))
sun.light_component.set_editor_property('mobility', unreal.ComponentMobility.MOVABLE)
sun.light_component.set_editor_property('intensity', 5.0)
spawn(unreal.SkyAtmosphere, 'Sky atmosphere', (0, 0, 0))

cube = unreal.EditorAssetLibrary.duplicate_asset('/Engine/BasicShapes/Cube', '/Game/Smoke/NaniteCube')
settings = cube.get_editor_property("nanite_settings")
settings.enabled = True
cube.set_editor_property("nanite_settings", settings)
unreal.EditorAssetLibrary.save_loaded_asset(cube)

floor = spawn(unreal.StaticMeshActor, 'Neutral collision floor', (0, 0, -55))
floor.static_mesh_component.set_static_mesh(cube)
floor.set_actor_scale3d(unreal.Vector(30, 30, 1))
floor.static_mesh_component.set_collision_profile_name('BlockAll')
for index, position in enumerate([(-350, 450, 50), (0, 450, 100), (350, 450, 150)]):
    actor = spawn(unreal.StaticMeshActor, 'Nanite cube ' + str(index), position)
    actor.static_mesh_component.set_static_mesh(cube)
    actor.set_actor_scale3d(unreal.Vector(1 + index, 1 + index, 1 + index))

material = assets.create_asset('Emissive', '/Game/Smoke', unreal.Material, unreal.MaterialFactoryNew())
colour = unreal.MaterialEditingLibrary.create_material_expression(material, unreal.MaterialExpressionConstant3Vector)
colour.set_editor_property('constant', unreal.LinearColor(8.0, 2.0, 0.5))
unreal.MaterialEditingLibrary.connect_material_property(colour, '', unreal.MaterialProperty.MP_EMISSIVE_COLOR)
unreal.MaterialEditingLibrary.recompile_material(material)
unreal.EditorAssetLibrary.save_loaded_asset(material)
sphere = spawn(unreal.StaticMeshActor, 'Emissive sphere', (250, 150, 100))
sphere.static_mesh_component.set_static_mesh(unreal.load_asset('/Engine/BasicShapes/Sphere'))
sphere.static_mesh_component.set_material(0, material)
spawn(unreal.PlayerStart, 'Viewer start', (900, -1200, 850), unreal.Rotator(-28, 125, 0))
assert levels.save_current_level()
unreal.log('WP-U0 VIEWER_MAP_OK')
