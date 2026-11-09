"""Check the saved synthetic map in the headless Python commandlet."""
import unreal

levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
assert levels.load_level('/Game/Maps/Viewer')
actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
by_label = {actor.get_actor_label(): actor for actor in actors}
expected = {'Sky light', 'Sun', 'Sky atmosphere', 'Neutral collision floor',
            'Nanite cube 0', 'Nanite cube 1', 'Nanite cube 2', 'Emissive sphere', 'Viewer start'}
assert expected <= set(by_label), expected - set(by_label)
for label in ('Neutral collision floor', 'Nanite cube 0', 'Nanite cube 1', 'Nanite cube 2'):
    component = by_label[label].static_mesh_component
    assert component.static_mesh.get_editor_property('nanite_settings').enabled, label
floor = by_label['Neutral collision floor'].static_mesh_component
assert str(floor.get_collision_profile_name()) == 'BlockAll'
assert floor.get_collision_enabled() != unreal.CollisionEnabled.NO_COLLISION
assert by_label['Emissive sphere'].static_mesh_component.get_material(0).get_path_name() == '/Game/Smoke/Emissive.Emissive'
unreal.log('WP-U0 VIEWER_VERIFY_OK expected_actors=' + str(len(expected)))
