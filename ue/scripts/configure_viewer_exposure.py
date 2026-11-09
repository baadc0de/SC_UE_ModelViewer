"""Set reproducible Viewer exposure bounds; run standalone to update an existing map."""
import unreal

EXPOSURE_LABEL = "Viewer exposure bounds"
# Default (hangar) bounds in EV100; reasoning in ue/verification/WP-U3b.md §11.9.
HANGAR_EV100 = (-3.0, 10.0)


def configure_exposure(actors, ev_min=HANGAR_EV100[0], ev_max=HANGAR_EV100[1]):
    matches = [actor for actor in actors.get_all_level_actors()
               if isinstance(actor, unreal.PostProcessVolume)
               and actor.get_actor_label() == EXPOSURE_LABEL]
    assert len(matches) <= 1, "Duplicate Viewer exposure policy volumes"
    volume = matches[0] if matches else actors.spawn_actor_from_class(
        unreal.PostProcessVolume, unreal.Vector(0, 0, 0))
    volume.set_actor_label(EXPOSURE_LABEL)
    volume.set_editor_property("unbound", True)
    volume.set_editor_property("enabled", True)
    volume.set_editor_property("blend_weight", 1.0)
    volume.set_editor_property("priority", 10.0)
    settings = volume.get_editor_property("settings")
    for name, value in (("auto_exposure_min_brightness", ev_min),
                        ("auto_exposure_max_brightness", ev_max),
                        ("auto_exposure_bias", 0.0),
                        ("auto_exposure_method", unreal.AutoExposureMethod.AEM_HISTOGRAM)):
        settings.set_editor_property("override_" + name, True)
        settings.set_editor_property(name, value)
    volume.set_editor_property("settings", settings)
    unreal.log(f"VIEWER_EXPOSURE unbound=1 min_ev100={ev_min:g} max_ev100={ev_max:g} bias=0 method=histogram")
    return volume


if __name__ == "__main__":
    unreal.EditorLoadingAndSavingUtils.load_map("/Game/Maps/Viewer")
    configure_exposure(unreal.get_editor_subsystem(unreal.EditorActorSubsystem))
    assert unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).save_current_level()
    unreal.log("VIEWER_EXPOSURE_SAVED")
