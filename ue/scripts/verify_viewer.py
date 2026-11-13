"""Check the saved synthetic map in the headless Python commandlet."""
import unreal

levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
assert levels.load_level('/Game/Maps/Viewer')
actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
by_label = {actor.get_actor_label(): actor for actor in actors}
# WP-U3b §11.9 / LOOK-3: the default Viewer is a closed designed hangar (no sun, no atmosphere): three key bars over the
# landing pad and eight wall fill panels, all shadowed rect lights.
expected = {'Sky light', 'Neutral collision floor', 'Hangar wall +X', 'Hangar wall -X', 'Hangar wall +Y',
            'Hangar wall -Y', 'Hangar ceiling', 'Nanite cube 0', 'Nanite cube 1', 'Nanite cube 2',
            'Emissive sphere', 'Viewer start', 'Landing pad'} | {f'Key light {k}' for k in range(3)} | \
           {f'Fill light {a}{s}{k}' for a in 'XY' for s in '+-' for k in range(2)}
assert expected <= set(by_label), expected - set(by_label)
assert 'Sun' not in by_label and 'Sky atmosphere' not in by_label
for label in ('Neutral collision floor', 'Hangar ceiling', 'Nanite cube 0', 'Nanite cube 1', 'Nanite cube 2'):
    component = by_label[label].static_mesh_component
    assert component.static_mesh.get_editor_property('nanite_settings').enabled, label
floor = by_label['Neutral collision floor'].static_mesh_component
assert str(floor.get_collision_profile_name()) == 'BlockAll'
assert floor.get_collision_enabled() != unreal.CollisionEnabled.NO_COLLISION
for label in [l for l in expected if l.startswith(('Key light', 'Fill light'))]:
    assert by_label[label].rect_light_component.get_editor_property('cast_shadows'), label
assert by_label['Emissive sphere'].static_mesh_component.get_material(0).get_path_name() == '/Game/Smoke/Emissive.Emissive'
unreal.log('WP-U0 VIEWER_VERIFY_OK expected_actors=' + str(len(expected)))
