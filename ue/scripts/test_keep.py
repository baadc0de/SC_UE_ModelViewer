"""WP-V1 editor-host Keep check: load an entity through the viewer subsystem, keep it as assets, reload them.

Writes /Game/SC/<entity>[_n]/ (gitignored, game-derived); delete ue/SCViewer/Content/SC/<entity>* afterwards.
UnrealEditor-Cmd ue/SCViewer/SCViewer.uproject -run=pythonscript -script=ue/scripts/test_keep.py \
    -nullrhi -unattended -nop4 -nosplash -ddc=SCViewerDDC [-SCEntity=AEGS_Gladius]
"""
import shlex

import unreal

options = dict(arg[1:].split("=", 1) for arg in shlex.split(unreal.SystemLibrary.get_command_line())
               if arg.startswith("-") and "=" in arg)
entity = options.get("SCEntity", "AEGS_Gladius")


def reload_from_disk(root):
    """Fresh-process check (-SCKeepReload=<root>): every kept asset loads; meshes carry kept materials."""
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    registry.scan_paths_synchronous([root], True)
    assets = registry.get_assets_by_path(root, recursive=True)
    failed, meshes, bound, textures_with_source, rigs_ok = [], 0, 0, 0, 0
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
            rigs_ok += asset.skeleton is not None
    unreal.log("WP-V1 KEEP_DISK root=%s assets=%d failed=%d static_meshes=%d with_kept_materials=%d textures_sized=%d "
               "skeletal_with_skeleton=%d" % (root, len(assets), len(failed), meshes, bound, textures_with_source, rigs_ok))
    if failed or not assets:
        raise RuntimeError("kept assets failed to load: %s" % failed[:5])
    unreal.log("WP-V1 KEEP_DISK_OK")


if "SCKeepReload" in options:
    reload_from_disk(options["SCKeepReload"])
    raise SystemExit(0)
viewer = unreal.SCViewerSubsystem.create_headless()
if not viewer.wait_for_archive(900.0):
    raise RuntimeError(viewer.get_archive_error())
view = unreal.SCViewOptions()
view.reset_to_defaults()
if not viewer.load_entity(entity, view) or not viewer.wait_for_load(1800.0):
    raise RuntimeError("load failed: " + viewer.get_last_report().error)
if not viewer.can_keep():
    raise RuntimeError("Keep handler not bound (editor module not loaded?)")
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
mesh = unreal.load_asset(str(meshes[0].package_name))
mat = mesh.get_material(0)
unreal.log("WP-V1 KEEP_RELOAD mesh=%s nanite=%s material=%s" % (
    mesh.get_path_name(), mesh.get_editor_property("nanite_settings").enabled, mat.get_path_name() if mat else None))
if not meshes or not kinds.get("Texture2D") or not kinds.get("MaterialInstanceConstant") or not kinds.get("AnimSequence"):
    raise RuntimeError("missing asset kinds: %s" % kinds)
unreal.log("WP-V1 KEEP_OK")
