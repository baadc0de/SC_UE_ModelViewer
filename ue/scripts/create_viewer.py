"""Create the Viewer level and its lighting presets; run inside Unreal's Python host (UnrealEditor-Cmd -run=pythonscript).

/Game/Maps/Viewer is the default environment: a closed, neutral-grey hangar lit from the ceiling at a standard
working illuminance. /Game/Maps/Presets/Viewer_Daylight and Viewer_Space are selectable alternatives (generated,
untracked). Every number is derived from a physical reference or from the largest test ship's logged bounds, never
tuned to a ship's look; the reasoning is recorded in ue/verification/WP-U3b.md §11.9.

Regeneration: delete Content/Maps/Viewer.umap and Content/Maps/Presets/ first (maps are never overwritten);
shared assets under /Game/Smoke are reused when present.
"""
import math
import runpy
from pathlib import Path

import unreal

levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
assets = unreal.AssetToolsHelpers.get_asset_tools()
library = unreal.EditorAssetLibrary
configure_exposure = runpy.run_path(str(Path(__file__).with_name("configure_viewer_exposure.py")),
                                    run_name="sc_viewer_exposure")["configure_exposure"]

# --- Hangar geometry (cm) -------------------------------------------------------------------------------------------
# Largest test ship (Cutlass Black, BOUNDS_CM in the import log): 35.7 x 26.1 x 10.8 m, bounds radius 22.8 m; the
# importer lifts every model 1 m above the floor. Review cameras orbit the bounds centre at <= 1.6 radii.
HANGAR_HALF_SPAN = 4000.0      # 80 m x 80 m floor: >= 20 m clear around the largest footprint, cameras stay inside
HANGAR_HEIGHT = 4000.0         # 40 m clear height: top-down camera at 1.3 radii is ~36 m above the floor
FLOOR_TOP = -5.0               # existing floor slab top (centre -55 cm, 1 m thick)
WALL = 100.0                   # 1 m thick walls / ceiling (Nanite cube, 100 cm)
GREY_REFLECTANCE = 0.18        # photographic middle grey: the auto-exposure target reflectance
GREY_ROUGHNESS = 0.8           # matte, so the walls do not mirror the ship

# --- Hangar lighting --------------------------------------------------------------------------------------------------
TARGET_LUX = 400.0             # horizontal illuminance on the floor: EN 12464-1 industrial work areas 300-500 lux
PANEL_GRID = 4                 # 4 x 4 ceiling fixtures over the 80 m span (20 m pitch)
PANEL_SIZE = (400.0, 100.0)    # 4 m x 1 m rect panels (row high-bay fixtures); only shadow softness depends on it


def panel_candela():
    """Normal luminous intensity per Lambertian panel so that the mean direct illuminance over the central
    40 m x 40 m (where ships stand) is TARGET_LUX: E(p) = sum_k I0 * cos(theta_k)^2 / d_k^2 (panel emits with
    cos theta, floor receives with cos theta; far field, panel size << 40 m height). Lumen's inter-reflection off
    the 18 % grey box adds on top."""
    pitch = 2 * HANGAR_HALF_SPAN / PANEL_GRID / 100.0
    centres = [(-HANGAR_HALF_SPAN / 100.0 + pitch * (i + 0.5)) for i in range(PANEL_GRID)]
    h = HANGAR_HEIGHT / 100.0
    samples = [(-20.0 + 5.0 * i, -20.0 + 5.0 * j) for i in range(9) for j in range(9)]
    def unit_e(x, y):
        total = 0.0
        for cx in centres:
            for cy in centres:
                d2 = (x - cx) ** 2 + (y - cy) ** 2 + h * h
                total += (h * h / d2) / d2
        return total
    mean = sum(unit_e(x, y) for x, y in samples) / len(samples)
    return TARGET_LUX / mean, centres, [unit_e(x, y) * TARGET_LUX / mean for x, y in ((0, 0), (20, 20), (39, 39))]


# --- Presets: auto-exposure bounds in EV100 ---------------------------------------------------------------------------
# Hangar bounds from the picks measured in this hangar (§11.9): ship exteriors EV100 7.3-7.9, darkest ship
# interior (Cutlass bay) -2.1 -> one stop below the darkest interior, two above the brightest exterior.
PRESETS = {
    "Hangar": dict(path="/Game/Maps/Viewer", enclosure=True, sun_lux=None, atmosphere=False, ev=(-3.0, 10.0)),
    # Clear-sky sun 100 klux at 45 deg + sky (~86 klux horizontal): predicted EV100 13.5, +/-3.
    "Daylight": dict(path="/Game/Maps/Presets/Viewer_Daylight", enclosure=False, sun_lux=100000.0, atmosphere=True,
                     ev=(10.0, 16.0)),
    # No sun, no atmosphere: the ship's own lights set the exposure.
    "Space": dict(path="/Game/Maps/Presets/Viewer_Space", enclosure=False, sun_lux=None, atmosphere=False,
                  ev=(-6.0, 6.0)),
}


def spawn(cls, label, location, rotation=None):
    actor = actors.spawn_actor_from_class(cls, unreal.Vector(*location), rotation or unreal.Rotator())
    actor.set_actor_label(label)
    return actor


def nanite_cube():
    path = "/Game/Smoke/NaniteCube"
    if library.does_asset_exist(path):
        return unreal.load_asset(path)
    cube = library.duplicate_asset("/Engine/BasicShapes/Cube", path)
    settings = cube.get_editor_property("nanite_settings")
    settings.enabled = True
    cube.set_editor_property("nanite_settings", settings)
    library.save_loaded_asset(cube)
    return cube


def constant_material(name, connect):
    path = "/Game/Smoke/" + name
    if library.does_asset_exist(path):
        return unreal.load_asset(path)
    material = assets.create_asset(name, "/Game/Smoke", unreal.Material, unreal.MaterialFactoryNew())
    connect(material)
    unreal.MaterialEditingLibrary.recompile_material(material)
    library.save_loaded_asset(material)
    return material


def emissive(material):
    colour = unreal.MaterialEditingLibrary.create_material_expression(material, unreal.MaterialExpressionConstant3Vector)
    colour.set_editor_property("constant", unreal.LinearColor(8.0, 2.0, 0.5))
    unreal.MaterialEditingLibrary.connect_material_property(colour, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)


def grey(material):
    colour = unreal.MaterialEditingLibrary.create_material_expression(material, unreal.MaterialExpressionConstant3Vector)
    colour.set_editor_property("constant", unreal.LinearColor(GREY_REFLECTANCE, GREY_REFLECTANCE, GREY_REFLECTANCE))
    unreal.MaterialEditingLibrary.connect_material_property(colour, "", unreal.MaterialProperty.MP_BASE_COLOR)
    rough = unreal.MaterialEditingLibrary.create_material_expression(material, unreal.MaterialExpressionConstant)
    rough.set_editor_property("r", GREY_ROUGHNESS)
    unreal.MaterialEditingLibrary.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)


def box(label, cube, material, centre, size):
    actor = spawn(unreal.StaticMeshActor, label, centre)
    actor.static_mesh_component.set_static_mesh(cube)
    actor.set_actor_scale3d(unreal.Vector(*(s / 100.0 for s in size)))
    actor.static_mesh_component.set_collision_profile_name("BlockAll")
    if material:
        actor.static_mesh_component.set_material(0, material)
    return actor


def build(name, preset, cube, grey_material, emissive_material):
    assert not library.does_asset_exist(preset["path"]), preset["path"] + " exists; delete the map file to regenerate"
    assert levels.new_level(preset["path"])
    sky = spawn(unreal.SkyLight, "Sky light", (0, 0, 800))
    sky.light_component.set_editor_property("mobility", unreal.ComponentMobility.MOVABLE)
    # Real-time capture records only sky components (SkyAtmosphere, clouds, IsSky meshes): without an atmosphere it is
    # black and UE warns on screen. Indoors the sky light captures the lit enclosure once instead (captured scene),
    # which is what Lumen rays that miss geometry then see.
    sky.light_component.set_editor_property("real_time_capture", bool(preset["atmosphere"]))
    if preset["sun_lux"]:
        sun = spawn(unreal.DirectionalLight, "Sun", (0, 0, 600), unreal.Rotator(pitch=-45.0, yaw=-30.0, roll=0.0))
        sun.light_component.set_editor_property("mobility", unreal.ComponentMobility.MOVABLE)
        sun.light_component.set_editor_property("intensity", preset["sun_lux"])
    if preset["atmosphere"]:
        spawn(unreal.SkyAtmosphere, "Sky atmosphere", (0, 0, 0))
    span = 2 * HANGAR_HALF_SPAN
    box("Neutral collision floor", cube, grey_material, (0, 0, FLOOR_TOP - WALL / 2), (span, span, WALL))
    if preset["enclosure"]:
        mid_z = FLOOR_TOP + HANGAR_HEIGHT / 2
        outer = span + 2 * WALL
        for sign, axis in ((1, "X"), (-1, "X"), (1, "Y"), (-1, "Y")):
            offset = sign * (HANGAR_HALF_SPAN + WALL / 2)
            centre = (offset, 0, mid_z) if axis == "X" else (0, offset, mid_z)
            size = (WALL, outer, HANGAR_HEIGHT) if axis == "X" else (outer, WALL, HANGAR_HEIGHT)
            box(f"Hangar wall {'+' if sign > 0 else '-'}{axis}", cube, grey_material, centre, size)
        ceiling_z = FLOOR_TOP + HANGAR_HEIGHT
        box("Hangar ceiling", cube, grey_material, (0, 0, ceiling_z + WALL / 2), (outer, outer, WALL))
        candela, centres, check = panel_candela()
        reach = math.sqrt((2 * HANGAR_HALF_SPAN) ** 2 * 2 + HANGAR_HEIGHT ** 2)    # far floor corner, cm
        for i, cx in enumerate(centres):
            for j, cy in enumerate(centres):
                light = spawn(unreal.RectLight, f"Hangar light r{i}c{j}", (cx * 100.0, cy * 100.0, ceiling_z - 10.0),
                              unreal.Rotator(pitch=-90.0, yaw=0.0, roll=0.0))
                component = light.rect_light_component
                component.set_editor_property("mobility", unreal.ComponentMobility.MOVABLE)
                component.set_editor_property("intensity_units", unreal.LightUnits.CANDELAS)
                component.set_editor_property("intensity", candela)
                component.set_editor_property("source_width", PANEL_SIZE[0])
                component.set_editor_property("source_height", PANEL_SIZE[1])
                component.set_editor_property("attenuation_radius", reach)
        unreal.log(f"VIEWER_HANGAR panels={PANEL_GRID ** 2} candela_each={candela:.0f} direct_lux centre={check[0]:.0f} "
                   f"(20,20)m={check[1]:.0f} corner={check[2]:.0f} target_mean={TARGET_LUX:.0f}")
    for index, position in enumerate([(-350, 450, 50), (0, 450, 100), (350, 450, 150)]):
        cube_actor = spawn(unreal.StaticMeshActor, "Nanite cube " + str(index), position)
        cube_actor.static_mesh_component.set_static_mesh(cube)
        cube_actor.set_actor_scale3d(unreal.Vector(1 + index, 1 + index, 1 + index))
    sphere = spawn(unreal.StaticMeshActor, "Emissive sphere", (250, 150, 100))
    sphere.static_mesh_component.set_static_mesh(unreal.load_asset("/Engine/BasicShapes/Sphere"))
    sphere.static_mesh_component.set_material(0, emissive_material)
    spawn(unreal.PlayerStart, "Viewer start", (900, -1200, 850), unreal.Rotator(pitch=-28.0, yaw=125.0, roll=0.0))
    configure_exposure(actors, *preset["ev"])
    assert levels.save_current_level()
    unreal.log(f"VIEWER_PRESET {name} path={preset['path']} enclosure={int(preset['enclosure'])} "
               f"sun_lux={preset['sun_lux']} atmosphere={int(preset['atmosphere'])} ev100={preset['ev'][0]}..{preset['ev'][1]}")


cube_asset = nanite_cube()
grey_asset = constant_material("M_ViewerGrey", grey)
emissive_asset = constant_material("Emissive", emissive)
for preset_name, preset_settings in PRESETS.items():
    build(preset_name, preset_settings, cube_asset, grey_asset, emissive_asset)
unreal.log("WP-U0 VIEWER_MAP_OK")
