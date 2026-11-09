"""WP-V1 headless entry: drive USCViewerSubsystem.LoadEntity through the FFI without a viewport.

The archive is auto-discovered (sc_open(NULL)); the cache is <launch dir>/cache unless -SCCacheDir=<dir>.
Loads -SCEntity (default: the Gladius class used by the WP-V1 acceptance), then each -SCAlso=<a,b> entity, then the
first entity again, asserting that every load succeeds, that a replaced vehicle leaves no live objects after GC and
that the object counts return to the first load's values; finally checks reuse of an identical request.
Test data (entity names) lives here, never in the UI.

UnrealEditor-Cmd ue/SCViewer/SCViewer.uproject -run=pythonscript -script=ue/scripts/ue_load_gladius.py \
    -nullrhi -unattended -nop4 -nosplash -ddc=SCViewerDDC [-SCEntity=AEGS_Gladius] [-SCAlso=RSI_Scorpius]
"""
import shlex

import unreal

options = dict(arg[1:].split("=", 1) for arg in shlex.split(unreal.SystemLibrary.get_command_line())
               if arg.startswith("-") and "=" in arg)
first = options.get("SCEntity", "AEGS_Gladius")
also = [e for e in options.get("SCAlso", "").split(",") if e]

viewer = unreal.SCViewerSubsystem.create_headless()
if not viewer.wait_for_archive(900.0):
    raise RuntimeError("archive did not open: " + viewer.get_archive_error())

view = unreal.SCViewOptions()
view.reset_to_defaults()
view.lod = int(options.get("SCLod", 1))
view.mip = int(options.get("SCMip", 2))

COUNTS = ("static_meshes", "skeletal_meshes", "textures", "material_instances", "vehicles")


def load(entity):
    if not viewer.load_entity(entity, view):
        raise RuntimeError("LoadEntity refused " + entity)
    viewer.wait_for_load(1800.0)
    r = viewer.get_last_report()
    counts = {k: r.get_editor_property(k) for k in COUNTS}
    unreal.log("WP-V1 LOAD entity=%s result=%s reused=%s export_s=%.3f cache_hit=%s import_s=%.3f build_s=%.3f "
               "collision_s=%.3f total_s=%.3f assets=%d previous_objects=%d previous_alive=%d counts=%s" % (
                   entity, r.result, r.reused, r.export_seconds, r.export_cache_hit, r.import_seconds,
                   r.build_seconds, r.collision_seconds, r.total_seconds, r.assets, r.previous_objects,
                   r.previous_objects_alive, counts))
    if r.result != unreal.SCLoadPhase.SUCCEEDED:
        raise RuntimeError("load failed: %s %s" % (entity, r.error))
    if r.previous_objects_alive != 0:
        raise RuntimeError("previous vehicle leaked %d objects" % r.previous_objects_alive)
    if counts["vehicles"] != 1:
        raise RuntimeError("expected exactly one live vehicle, got %d" % counts["vehicles"])
    return r, counts


_, first_counts = load(first)
for entity in also:
    report, _ = load(entity)
    if report.previous_objects == 0:
        raise RuntimeError("the previous vehicle was not tracked for release")
_, again_counts = load(first)
if also and again_counts != first_counts:
    raise RuntimeError("object counts differ after reloading %s: %s vs %s" % (first, first_counts, again_counts))
reused, _ = load(first)
if not reused.reused:
    raise RuntimeError("identical entity + options were not reused")
vehicle = viewer.get_vehicle()
unreal.log("WP-V1 HEADLESS_LOAD_OK entity=%s components=%d clips=%d" % (
    first, len(vehicle.get_components_by_class(unreal.SceneComponent)), len(vehicle.clip_catalog.clips)))
unreal.SCViewerSubsystem.destroy_headless(viewer)
unreal.log("WP-V1 HEADLESS_TEARDOWN_OK")
