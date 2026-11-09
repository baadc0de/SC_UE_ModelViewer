"""Run synchronous synthetic SCBridge automation tests without starting editor UI."""
import unreal
if "-SCPackage=" in unreal.SystemLibrary.get_command_line():
    unreal.EditorLoadingAndSavingUtils.load_map("/Game/Maps/Viewer")
    volumes = [actor for actor in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
               if isinstance(actor, unreal.PostProcessVolume) and actor.get_actor_label() == "Viewer exposure bounds"]
    assert len(volumes) == 1, "Viewer must have exactly one exposure policy volume"
    volume = volumes[0]
    settings = volume.get_editor_property("settings")
    assert volume.get_editor_property("unbound") and volume.get_editor_property("enabled")
    assert volume.get_editor_property("blend_weight") == 1.0
    assert settings.get_editor_property("override_auto_exposure_min_brightness")
    assert settings.get_editor_property("override_auto_exposure_max_brightness")
    assert settings.get_editor_property("auto_exposure_min_brightness") == -3.0
    assert settings.get_editor_property("auto_exposure_max_brightness") == 6.0
    assert settings.get_editor_property("auto_exposure_bias") == 0.0
    assert settings.get_editor_property("auto_exposure_method") == unreal.AutoExposureMethod.AEM_HISTOGRAM
    unreal.log("VIEWER_EXPOSURE_VERIFY passed=1 unbound=1 min_ev100=-3 max_ev100=6 bias=0 method=histogram")
if not unreal.SCImportLibrary.run_unit_tests():
    raise RuntimeError("SCBridge unit tests failed")
unreal.log("WP-U1 UNIT_TESTS_OK")
