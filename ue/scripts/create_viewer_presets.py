"""WP-U3b: derive lighting-preset variants of the Viewer map from standard illuminance values.

Run with UnrealEditor-Cmd -run=pythonscript after create_viewer.py. Each preset duplicates
/Game/Maps/Viewer into /Game/Maps/Presets/Viewer_<Preset> and changes only the environment
lighting and the auto-exposure bounds (EV100); the ship and its lights are never touched.
Values are textbook illuminance levels, not tuned to any ship (ue/verification/WP-U3b.md §11.4):
  daylight  clear-sky sun 100 klux at 45 deg + atmosphere sky   (horizontal ~86 klux)
  hangar    overhead fixtures ~300 lux at 70 deg + sky fill     (horizontal ~300-400 lux)
  space     no sun, no atmosphere; sky light captures black     (ship lights only)
"""
import unreal

PRESETS = {
    # name: (sun lux or None, sun pitch, keep atmosphere, (min EV100, max EV100))
    "Daylight": (100000.0, -45.0, True, (10.0, 16.0)),
    "Hangar": (300.0, -70.0, True, (2.0, 8.0)),
    "Space": (None, -45.0, False, (-6.0, 6.0)),
}
EXPOSURE_LABEL = "Viewer exposure bounds"
# Measurement variants (Viewer_<Preset>_FixedEV): auto exposure pinned (min = max) to the EV100 predicted from
# the preset's horizontal illuminance E, calibrated on the pick measured in the original map
# (EV100 -1.56 at its 5 lux sun, 30 deg elevation -> 2.5 lux): EV = -1.56 + log2(E / 2.5 lux).
# Pinning keeps on/off pairs at one exposure so pool contrast can be read from the images.
FIXED_EV = {"Daylight": -1.56 + __import__("math").log2(86000 / 2.5),   # 70.7 klux sun + ~15 klux sky
            "Hangar": -1.56 + __import__("math").log2(282 / 2.5)}       # 300 lux at 70 deg

actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)


def by_label(label):
    hits = [a for a in actors.get_all_level_actors() if a.get_actor_label() == label]
    assert len(hits) == 1, f"expected one actor labelled {label!r}, found {len(hits)}"
    return hits[0]


VARIANTS = [(n, v, n) for n, v in PRESETS.items()] + \
    [(n + "_FixedEV", PRESETS[n][:3] + ((ev, ev),), n) for n, ev in FIXED_EV.items()]
for name, (lux, pitch, atmosphere, (ev_min, ev_max)), _ in VARIANTS:
    path = f"/Game/Maps/Presets/Viewer_{name}"
    # Regenerate from scratch: delete ue/SCViewer/Content/Maps/Presets/ before running (the commandlet cannot
    # delete a map package it has registered).
    assert not unreal.EditorAssetLibrary.does_asset_exist(path), f"{path} exists; delete Content/Maps/Presets first"
    assert unreal.EditorAssetLibrary.duplicate_asset("/Game/Maps/Viewer", path)
    unreal.EditorLoadingAndSavingUtils.load_map(path)
    sun = by_label("Sun")
    unreal.log(f"VIEWER_PRESET_BASE sun_rotation={sun.get_actor_rotation()} sun_lux={sun.light_component.get_editor_property('intensity')}")
    if lux is None:
        actors.destroy_actor(sun)
    else:
        sun.light_component.set_editor_property("intensity", lux)
        rot = sun.get_actor_rotation()
        sun.set_actor_rotation(unreal.Rotator(pitch=pitch, yaw=rot.yaw, roll=0.0), False)
    if not atmosphere:
        actors.destroy_actor(by_label("Sky atmosphere"))
    volume = by_label(EXPOSURE_LABEL)
    settings = volume.get_editor_property("settings")
    for prop, value in (("auto_exposure_min_brightness", ev_min), ("auto_exposure_max_brightness", ev_max)):
        settings.set_editor_property("override_" + prop, True)
        settings.set_editor_property(prop, value)
    volume.set_editor_property("settings", settings)
    assert levels.save_current_level()
    unreal.log(f"VIEWER_PRESET {name} path={path} sun_lux={lux} pitch={pitch} atmosphere={int(atmosphere)} "
               f"ev100={ev_min}..{ev_max}")
unreal.log("VIEWER_PRESETS_OK")
