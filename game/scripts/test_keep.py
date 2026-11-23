"""WP-V1 editor-host Keep check: load an entity through the viewer subsystem, keep it as assets, reload them.

Keep is UnrealCitizen's editor module (UnrealCitizenEditor, WP-O1-2), so the check runs in the game project.
Writes /Game/SC/<entity>[_n]/ (gitignored, game-derived); delete game/Content/SC/<entity>* afterwards.
UnrealEditor-Cmd game/UnrealCitizen.uproject -run=pythonscript -script=game/scripts/test_keep.py \
    -nullrhi -unattended -nop4 -nosplash -ddc=SCViewerDDC [-SCEntity=AEGS_Gladius] [-SCPrepared] [-SCClip=door_open]
-SCPrepared: the entity has a current prepared level, which must be what loads (its clip bindings carry tracks and no
sequences), e.g. -SCEntity=DRAK_Golem after the golem_showcase prep; its rigs and clip sequences are checked instead of
static meshes. -SCClip=<name>: that clip (in the Keep run and the -SCKeepReload run) resolves through the kept catalog
(its catalog id, exactly) to its primary sequence (its first binding's), on its binding mesh's own skeleton, with bone
keys.
"""
import shlex

import unreal

options = dict(arg[1:].split("=", 1) for arg in shlex.split(unreal.SystemLibrary.get_command_line())
               if arg.startswith("-") and "=" in arg)
entity = options.get("SCEntity", "AEGS_Gladius")
prepared = any(arg == "-SCPrepared" for arg in shlex.split(unreal.SystemLibrary.get_command_line()))
clip_name = options.get("SCClip")


def find_clip(catalog, name):
    """The catalog's clip whose id is `name` exactly (an item's clips carry an owner prefix: item_<hash>__door_open); None
    when there is none; an error when several match."""
    found = [clip for clip in catalog.get_editor_property("clips") if str(clip.get_editor_property("id")) == name]
    if len(found) > 1:
        raise RuntimeError("clip id %s is ambiguous (%d clips)" % (name, len(found)))
    return found[0] if found else None


def check_kept_clip(root, name):
    """The kept catalog's clip `name`: its primary sequence, on its first binding mesh's own skeleton, with bone keys."""
    catalog = unreal.load_asset(root.rstrip("/") + "/Clips")
    clip = find_clip(catalog, name) if catalog else None
    if clip is None:
        raise RuntimeError("clip %s not in the kept catalog" % name)
    sequence = clip.get_editor_property("sequence")
    bindings = clip.get_editor_property("bindings")
    mesh = bindings[0].get_editor_property("mesh") if bindings else None
    skeleton = sequence.get_editor_property("skeleton") if sequence else None
    tracks = unreal.AnimationLibrary.get_animation_track_names(sequence) if sequence else []
    frames = unreal.AnimationLibrary.get_num_frames(sequence) if sequence else 0
    unreal.log("WP-V1 KEEP_CLIP %s sequence=%s skeleton=%s mesh=%s tracks=%d frames=%d" % (
        name, sequence.get_path_name() if sequence else None, skeleton.get_path_name() if skeleton else None,
        mesh.get_path_name() if mesh else None, len(tracks), frames))
    primary = bindings[0].get_editor_property("sequences")[0] if bindings and bindings[0].get_editor_property("sequences") else None
    if not sequence or sequence != primary or not mesh or skeleton != mesh.skeleton or not tracks or frames < 1:
        raise RuntimeError("kept clip %s: no primary sequence (its first binding's) on its mesh's skeleton with bone keys" % name)


def reload_from_disk(root):
    """Fresh-process check (-SCKeepReload=<root>): every kept asset loads; meshes carry kept materials."""
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    registry.scan_paths_synchronous([root], True)
    assets = registry.get_assets_by_path(root, recursive=True)
    failed, meshes, bound, textures_with_source, rigs_ok = [], 0, 0, 0, 0
    sequences, sequences_on_kept, sequences_with_tracks = 0, 0, 0
    kept_rigs = root.rstrip("/") + "/Rigs/"
    for data in assets:
        asset = unreal.load_asset(str(data.package_name))
        if asset is None:
            failed.append(str(data.package_name))
            continue
        if isinstance(asset, unreal.StaticMesh):
            meshes += 1
            mats = [asset.get_material(i) for i in range(len(asset.static_materials))]
            bound += any(m and not m.get_path_name().startswith("/Engine/") for m in mats)
        elif isinstance(asset, unreal.Texture2D):
            textures_with_source += asset.blueprint_get_size_x() > 0
        elif isinstance(asset, unreal.SkeletalMesh):
            # its kept skeleton, saved beside it (not the import's transient one)
            rigs_ok += asset.skeleton is not None and asset.skeleton.get_path_name().startswith(kept_rigs)
        elif isinstance(asset, unreal.AnimSequence):
            # on a kept skeleton; the bound ones carry their bone tracks (a clip without receiving bones has none)
            sequences += 1
            skeleton = asset.get_editor_property("skeleton")
            sequences_on_kept += skeleton is not None and skeleton.get_path_name().startswith(kept_rigs)
            sequences_with_tracks += len(unreal.AnimationLibrary.get_animation_track_names(asset)) > 0
    unreal.log("WP-V1 KEEP_DISK root=%s assets=%d failed=%d static_meshes=%d with_kept_materials=%d textures_sized=%d "
               "skeletal_with_skeleton=%d sequences=%d on_kept_skeletons=%d with_tracks=%d" % (
                   root, len(assets), len(failed), meshes, bound, textures_with_source, rigs_ok, sequences, sequences_on_kept,
                   sequences_with_tracks))
    if failed or not assets:
        raise RuntimeError("kept assets failed to load: %s" % failed[:5])
    skeletal = sum(1 for d in assets if str(d.asset_class_path.asset_name) == "SkeletalMesh")
    if not skeletal or rigs_ok != skeletal:
        raise RuntimeError("kept rig meshes without their skeletons: %d of %d" % (skeletal - rigs_ok, skeletal))
    if not sequences or sequences_on_kept != sequences or not sequences_with_tracks:
        raise RuntimeError("kept sequences: %d, on kept skeletons %d, with tracks %d" % (sequences, sequences_on_kept, sequences_with_tracks))
    if clip_name:
        check_kept_clip(root, clip_name)
    unreal.log("WP-V1 KEEP_DISK_OK")


if "SCKeepReload" in options:
    reload_from_disk(options["SCKeepReload"])
    raise SystemExit(0)
viewer = unreal.SCViewerSubsystem.create_headless()
if not viewer.wait_for_archive(900.0):
    raise RuntimeError(viewer.get_archive_error())
# The viewer may start with its environment (the hangar setting), a load that refuses another: Keep needs no hangar.
viewer.set_environment("none")
view = unreal.SCViewOptions()
view.reset_to_defaults()
if not viewer.load_entity(entity, view) or not viewer.wait_for_load(1800.0):
    raise RuntimeError("load failed: " + viewer.get_last_report().error)
if not viewer.can_keep():
    raise RuntimeError("Keep handler not bound (editor module not loaded?)")
if prepared:
    # The prepared level is what loaded (not an archive import): saved rig meshes, bindings with no sequences.
    catalog = viewer.get_vehicle().get_editor_property("clip_catalog")
    bindings = [b for clip in catalog.get_editor_property("clips") for b in clip.get_editor_property("bindings")]
    live = [b for b in bindings if b.get_editor_property("sequences") or
            (b.get_editor_property("mesh") and b.get_editor_property("mesh").get_path_name().startswith("/Temp/"))]
    if not bindings or live:
        raise RuntimeError("-SCPrepared, but %d of %d bindings come from an import (is the prepared level current?)" % (len(live), len(bindings)))
    if clip_name and find_clip(catalog, clip_name) is None:
        raise RuntimeError("clip %s not in the prepared catalog" % clip_name)
kept = viewer.keep()
unreal.log("WP-V1 KEEP ok=%s saved=%d message=%s" % (kept.ok, kept.saved, kept.message))
if not kept.ok:
    raise RuntimeError(kept.message)
root = kept.root
registry = unreal.AssetRegistryHelpers.get_asset_registry()
assets = registry.get_assets_by_path(root, recursive=True)
kinds = {}
for data in assets:
    kinds[str(data.asset_class_path.asset_name)] = kinds.get(str(data.asset_class_path.asset_name), 0) + 1
unreal.log("WP-V1 KEEP_ASSETS root=%s count=%d kinds=%s" % (root, len(assets), kinds))
# Unload the live vehicle, then load kept assets back from disk.
unreal.SCViewerSubsystem.destroy_headless(viewer)
meshes = [d for d in assets if str(d.asset_class_path.asset_name) == "StaticMesh"]
# A live import keeps its static meshes, textures and materials; a prepared vehicle (-SCPrepared: its prepared level
# loaded) has those as assets already and keeps its rigs, their skeletons and the clip sequences built from its tracks.
if meshes:
    mesh = unreal.load_asset(str(meshes[0].package_name))
    mat = mesh.get_material(0)
    unreal.log("WP-V1 KEEP_RELOAD mesh=%s nanite=%s material=%s" % (
        mesh.get_path_name(), mesh.get_editor_property("nanite_settings").enabled, mat.get_path_name() if mat else None))
required = ["SkeletalMesh", "Skeleton", "AnimSequence"] if prepared else ["StaticMesh", "Texture2D", "MaterialInstanceConstant", "AnimSequence"]
if any(not kinds.get(kind) for kind in required):
    raise RuntimeError("missing asset kinds: %s" % kinds)
if clip_name:
    check_kept_clip(root, clip_name)
unreal.log("WP-V1 KEEP_OK")
