"""Create the Viewer level and its lighting presets; run inside Unreal's Python host (UnrealEditor-Cmd -run=pythonscript).

/Game/Maps/Viewer is the default environment: a closed hangar designed for the look (plan D15, ue/verification/LOOK-3.md):
dark floor with a lighter landing pad, structural wall pillars and a ceiling truss, three shadowed key light bars over
the pad and a cooler wall fill at about a third of the key (adjudicated 3:1), amber accent strips; no longer a uniform
18 % grey box. /Game/Maps/Presets/Viewer_Daylight and Viewer_Space are selectable alternatives (generated,
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

# --- Designed hangar (look, plan D15; viewer choices, not game data) -------------------------------------------------
PAD_HALF = 1500.0              # 30 m x 30 m landing pad: the largest footprint (Cutlass 35.7 x 26.1 m) mostly on it
KEY_LUX = 350.0                # mean horizontal illuminance on the pad from the key bars
KEY_BARS_X = (-1000.0, 0.0, 1000.0)
KEY_BAR = (60.0, 2400.0)       # 0.6 m x 24 m bars along Y (width, length)
KEY_HEIGHT = 2800.0            # hung 28 m above the floor, under the truss
KEY_KELVIN = 5500.0
FILL_RATIO = 1.0 / 3.0         # fill : key on the subject (adjudicated 3:1)
FILL_PANEL = (600.0, 200.0)    # 6 m x 2 m wall panels, two per wall at +-15 m, 8 m up
FILL_HEIGHT = 800.0
FILL_KELVIN = 7000.0
ACCENT_NITS = 300.0            # amber floor-level wall strips
SURFACES = {                   # name: (linear base colour, roughness)
    "M_HangarFloor": ((0.12, 0.12, 0.125), 0.5),
    "M_HangarPad": ((0.22, 0.22, 0.22), 0.6),
    "M_HangarPaint": ((0.55, 0.40, 0.05), 0.5),
    "M_HangarWall": ((0.25, 0.25, 0.26), 0.7),
    "M_HangarStructure": ((0.045, 0.045, 0.05), 0.45),
}


def key_bar_candela():
    """Normal intensity per key bar (Lambertian, facing down) so the mean direct illuminance over the pad is KEY_LUX.
    Each bar is summed as 12 point segments: at 28 m the 24 m bar is not a point."""
    h = KEY_HEIGHT / 100.0
    segments = [(x / 100.0, (-KEY_BAR[1] / 2 + KEY_BAR[1] * (k + 0.5) / 12) / 100.0) for x in KEY_BARS_X for k in range(12)]
    pad = PAD_HALF / 100.0
    samples = [(-pad + pad * 2 * (i + 0.5) / 9, -pad + pad * 2 * (j + 0.5) / 9) for i in range(9) for j in range(9)]
    def unit_e(x, y):  # illuminance per candela of each bar (split over its 12 segments)
        return sum((h * h / ((x - sx) ** 2 + (y - sy) ** 2 + h * h)) / ((x - sx) ** 2 + (y - sy) ** 2 + h * h)
                   for sx, sy in segments) / 12
    mean = sum(unit_e(x, y) for x, y in samples) / len(samples)
    return KEY_LUX / mean


def fill_panel_candela():
    """Normal intensity per fill panel: the two panels of one wall give FILL_RATIO * KEY_LUX on a vertical plane at the
    pad centre, 2 m up, facing that wall."""
    d_wall = HANGAR_HALF_SPAN / 100.0
    dz = (FILL_HEIGHT - 200.0) / 100.0
    d2 = d_wall ** 2 + 15.0 ** 2 + dz ** 2
    cos2 = d_wall * d_wall / d2  # emitter cosine x receiver cosine (both along the wall normal)
    return FILL_RATIO * KEY_LUX / 2 * d2 / cos2


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


def surface(colour, roughness):
    def connect(material):
        c = unreal.MaterialEditingLibrary.create_material_expression(material, unreal.MaterialExpressionConstant3Vector)
        c.set_editor_property("constant", unreal.LinearColor(*colour))
        unreal.MaterialEditingLibrary.connect_material_property(c, "", unreal.MaterialProperty.MP_BASE_COLOR)
        r = unreal.MaterialEditingLibrary.create_material_expression(material, unreal.MaterialExpressionConstant)
        r.set_editor_property("r", roughness)
        unreal.MaterialEditingLibrary.connect_material_property(r, "", unreal.MaterialProperty.MP_ROUGHNESS)
    return connect


def glow(rgb_nits):
    """Black, rough surface emitting rgb_nits cd/m2 (emissive 1 = 1 cd/m2 with the extended luminance range)."""
    def connect(material):
        surface((0.02, 0.02, 0.02), 0.6)(material)
        c = unreal.MaterialEditingLibrary.create_material_expression(material, unreal.MaterialExpressionConstant3Vector)
        c.set_editor_property("constant", unreal.LinearColor(*rgb_nits))
        unreal.MaterialEditingLibrary.connect_material_property(c, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    return connect


def kelvin_rgb(kelvin):
    """Linear sRGB of a black body (Tanner Helland fit, normalised to max 1): fixture face colour matching the light."""
    t = kelvin / 100.0
    r = 255.0 if t <= 66 else 329.698727446 * (t - 60) ** -0.1332047592
    g = 99.4708025861 * math.log(t) - 161.1195681661 if t <= 66 else 288.1221695283 * (t - 60) ** -0.0755148492
    b = 255.0 if t >= 66 else (0.0 if t <= 19 else 138.5177312231 * math.log(t - 10) - 305.0447927307)
    srgb = [min(255.0, max(0.0, v)) / 255.0 for v in (r, g, b)]
    lin = [c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4 for c in srgb]
    m = max(lin)
    return tuple(c / m for c in lin)


def rect_light(label, centre, rotation, size, candela, kelvin, reach):
    light = spawn(unreal.RectLight, label, centre, rotation)
    component = light.rect_light_component
    component.set_editor_property("mobility", unreal.ComponentMobility.MOVABLE)
    component.set_editor_property("intensity_units", unreal.LightUnits.CANDELAS)
    component.set_editor_property("intensity", candela)
    component.set_editor_property("source_width", size[0])
    component.set_editor_property("source_height", size[1])
    component.set_editor_property("attenuation_radius", reach)
    component.set_editor_property("use_temperature", True)
    component.set_editor_property("temperature", kelvin)
    component.set_editor_property("cast_shadows", True)
    return light


def fixture_face(label, cube, material, centre, size):
    """The visible glowing face of a fixture: lit by its rect light already, so kept out of Lumen (no double count)."""
    actor = box(label, cube, material, centre, size)
    component = actor.static_mesh_component
    component.set_editor_property("cast_shadow", False)
    component.set_editor_property("affect_dynamic_indirect_lighting", False)
    component.set_collision_profile_name("NoCollision")
    return actor


def designed_hangar(cube, mats):
    """Pad, structure, key bars, wall fill, accents (constants above). Walls, floor and ceiling are built by build()."""
    floor = FLOOR_TOP
    box("Landing pad", cube, mats["M_HangarPad"], (0, 0, floor + 1), (2 * PAD_HALF, 2 * PAD_HALF, 2))
    for k, (cx, cy, sx, sy) in enumerate(((0, PAD_HALF, 2 * PAD_HALF, 30), (0, -PAD_HALF, 2 * PAD_HALF, 30),
                                         (PAD_HALF, 0, 30, 2 * PAD_HALF), (-PAD_HALF, 0, 30, 2 * PAD_HALF))):
        box(f"Pad stripe {k}", cube, mats["M_HangarPaint"], (cx, cy, floor + 2.5), (sx, sy, 1))
    span, top = 2 * HANGAR_HALF_SPAN, floor + HANGAR_HEIGHT
    # Pillars every 10 m on every wall, a dark skirt band 3 m up, a 10 m ceiling truss.
    for axis in ("X", "Y"):
        for sign in (1, -1):
            for k in range(9):
                along = -HANGAR_HALF_SPAN + 1000.0 * k
                at = sign * (HANGAR_HALF_SPAN - 75.0)
                centre = (at, along, floor + HANGAR_HEIGHT / 2) if axis == "X" else (along, at, floor + HANGAR_HEIGHT / 2)
                size = (150.0, 120.0, HANGAR_HEIGHT) if axis == "X" else (120.0, 150.0, HANGAR_HEIGHT)
                box(f"Pillar {axis}{'+' if sign > 0 else '-'}{k}", cube, mats["M_HangarStructure"], centre, size)
            at = sign * (HANGAR_HALF_SPAN - 15.0)
            centre = (at, 0, floor + 300.0) if axis == "X" else (0, at, floor + 300.0)
            size = (30.0, span, 150.0) if axis == "X" else (span, 30.0, 150.0)
            box(f"Skirt {axis}{'+' if sign > 0 else '-'}", cube, mats["M_HangarStructure"], centre, size)
            at = sign * (HANGAR_HALF_SPAN - 40.0)
            centre = (at, 0, floor + 40.0) if axis == "X" else (0, at, floor + 40.0)
            size = (10.0, span - 400.0, 6.0) if axis == "X" else (span - 400.0, 10.0, 6.0)
            fixture = box(f"Accent {axis}{'+' if sign > 0 else '-'}", cube, mats["M_HangarAccent"], centre, size)
            fixture.static_mesh_component.set_editor_property("cast_shadow", False)
    for k in range(1, 8):
        along = -HANGAR_HALF_SPAN + 1000.0 * k
        box(f"Truss X{k}", cube, mats["M_HangarStructure"], (along, 0, top - 60.0), (60.0, span, 100.0))
        box(f"Truss Y{k}", cube, mats["M_HangarStructure"], (0, along, top - 60.0), (span, 60.0, 100.0))
    reach = math.sqrt(span ** 2 * 2 + HANGAR_HEIGHT ** 2)
    key = key_bar_candela()
    for k, x in enumerate(KEY_BARS_X):
        z = floor + KEY_HEIGHT
        box(f"Key housing {k}", cube, mats["M_HangarStructure"], (x, 0, z + 25.0), (KEY_BAR[0] + 30.0, KEY_BAR[1] + 30.0, 40.0))
        fixture_face(f"Key face {k}", cube, mats["M_HangarKeyFace"], (x, 0, z + 4.0), (KEY_BAR[0], KEY_BAR[1], 2.0))
        rect_light(f"Key light {k}", (x, 0, z), unreal.Rotator(pitch=-90.0, yaw=0.0, roll=0.0),
                   (KEY_BAR[1], KEY_BAR[0]), key, KEY_KELVIN, reach)
    fill = fill_panel_candela()
    for axis, sign, yaw in (("X", 1, 180.0), ("X", -1, 0.0), ("Y", 1, -90.0), ("Y", -1, 90.0)):
        for k, along in enumerate((-1500.0, 1500.0)):
            at = sign * (HANGAR_HALF_SPAN - 160.0)
            centre = (at, along, floor + FILL_HEIGHT) if axis == "X" else (along, at, floor + FILL_HEIGHT)
            face = (at + sign * 5.0, along, floor + FILL_HEIGHT) if axis == "X" else (along, at + sign * 5.0, floor + FILL_HEIGHT)
            face_size = (2.0, FILL_PANEL[0], FILL_PANEL[1]) if axis == "X" else (FILL_PANEL[0], 2.0, FILL_PANEL[1])
            fixture_face(f"Fill face {axis}{'+' if sign > 0 else '-'}{k}", cube, mats["M_HangarFillFace"], face, face_size)
            rect_light(f"Fill light {axis}{'+' if sign > 0 else '-'}{k}", centre, unreal.Rotator(pitch=0.0, yaw=yaw, roll=0.0),
                       FILL_PANEL, fill, FILL_KELVIN, reach)
    unreal.log(f"VIEWER_HANGAR designed key_bars={len(KEY_BARS_X)} key_candela_each={key:.0f} key_lux_pad={KEY_LUX:.0f} "
               f"fill_panels=8 fill_candela_each={fill:.0f} fill_lux_vertical={FILL_RATIO * KEY_LUX:.0f} accent_nits={ACCENT_NITS:.0f}")


def box(label, cube, material, centre, size):
    actor = spawn(unreal.StaticMeshActor, label, centre)
    actor.static_mesh_component.set_static_mesh(cube)
    actor.set_actor_scale3d(unreal.Vector(*(s / 100.0 for s in size)))
    actor.static_mesh_component.set_collision_profile_name("BlockAll")
    if material:
        actor.static_mesh_component.set_material(0, material)
    return actor


def build(name, preset, cube, grey_material, emissive_material, hangar):
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
    box("Neutral collision floor", cube, hangar["M_HangarFloor"] if preset["enclosure"] else grey_material,
        (0, 0, FLOOR_TOP - WALL / 2), (span, span, WALL))
    if preset["enclosure"]:
        mid_z = FLOOR_TOP + HANGAR_HEIGHT / 2
        outer = span + 2 * WALL
        for sign, axis in ((1, "X"), (-1, "X"), (1, "Y"), (-1, "Y")):
            offset = sign * (HANGAR_HALF_SPAN + WALL / 2)
            centre = (offset, 0, mid_z) if axis == "X" else (0, offset, mid_z)
            size = (WALL, outer, HANGAR_HEIGHT) if axis == "X" else (outer, WALL, HANGAR_HEIGHT)
            box(f"Hangar wall {'+' if sign > 0 else '-'}{axis}", cube, hangar["M_HangarWall"], centre, size)
        ceiling_z = FLOOR_TOP + HANGAR_HEIGHT
        box("Hangar ceiling", cube, hangar["M_HangarStructure"], (0, 0, ceiling_z + WALL / 2), (outer, outer, WALL))
        designed_hangar(cube, hangar)
    # Synthetic test props (WP-U0), moved off the landing pad into a corner bay.
    for index, position in enumerate([(-3200, 2800, 50), (-2850, 2800, 100), (-2500, 2800, 150)]):
        cube_actor = spawn(unreal.StaticMeshActor, "Nanite cube " + str(index), position)
        cube_actor.static_mesh_component.set_static_mesh(cube)
        cube_actor.set_actor_scale3d(unreal.Vector(1 + index, 1 + index, 1 + index))
    sphere = spawn(unreal.StaticMeshActor, "Emissive sphere", (-3200, 3300, 100))
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
hangar_assets = {name: constant_material(name, surface(*spec)) for name, spec in SURFACES.items()}
key_face_nits = key_bar_candela() / (KEY_BAR[0] * KEY_BAR[1] / 1e4)
fill_face_nits = fill_panel_candela() / (FILL_PANEL[0] * FILL_PANEL[1] / 1e4)
hangar_assets["M_HangarKeyFace"] = constant_material("M_HangarKeyFace", glow(tuple(c * key_face_nits for c in kelvin_rgb(KEY_KELVIN))))
hangar_assets["M_HangarFillFace"] = constant_material("M_HangarFillFace", glow(tuple(c * fill_face_nits for c in kelvin_rgb(FILL_KELVIN))))
hangar_assets["M_HangarAccent"] = constant_material("M_HangarAccent", glow((ACCENT_NITS, ACCENT_NITS * 0.45, ACCENT_NITS * 0.06)))
for preset_name, preset_settings in PRESETS.items():
    build(preset_name, preset_settings, cube_asset, grey_asset, emissive_asset, hangar_assets)
unreal.log("WP-U0 VIEWER_MAP_OK")
