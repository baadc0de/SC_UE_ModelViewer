"""WP-V1 cancel and teardown checks through USCViewerSubsystem (headless commandlet).

1. Cancel during the FFI export: the load ends CANCELLED without a vehicle.
2. Cancel mid-import: pumped until >= 3 static meshes exist while worker tasks are still running; after the cancel
   the subsystem collects garbage and every object the import had created (partial actor + session packages) is gone.
3. The handle stays usable: a full load succeeds and exactly one vehicle is alive.
4. Teardown with pending work: a new import is stopped mid-flight by DestroyHeadless (the real Deinitialize path);
   nothing it or the loaded vehicle owned survives GC.
Entity names are test data.
UnrealEditor-Cmd ue/SCViewer/SCViewer.uproject -run=pythonscript -script=ue/scripts/test_viewer_cancel.py \
    -nullrhi -unattended -nop4 -nosplash -ddc=SCViewerDDC [-SCEntity=AEGS_Gladius]
"""
import shlex

import unreal

options = dict(arg[1:].split("=", 1) for arg in shlex.split(unreal.SystemLibrary.get_command_line())
               if arg.startswith("-") and "=" in arg)
entity = options.get("SCEntity", "AEGS_Gladius")
MIN_MESHES = 3
viewer = unreal.SCViewerSubsystem.create_headless()
if not viewer.wait_for_archive(900.0):
    raise RuntimeError(viewer.get_archive_error())
view = unreal.SCViewOptions()
view.reset_to_defaults()


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def report(label):
    r = viewer.get_last_report()
    unreal.log("WP-V1 CANCEL %s result=%s vehicle=%s meshes_built_at_cancel=%d workers_pending_at_cancel=%s "
               "partial_objects=%d partial_alive=%d vehicles=%d" % (
                   label, r.result, viewer.get_vehicle(), r.meshes_built_at_cancel, r.workers_pending_at_cancel,
                   r.partial_objects, r.partial_objects_alive, r.vehicles))
    return r


# 1. during the export
check(viewer.load_entity(entity, view), "LoadEntity refused")
viewer.cancel_load()
viewer.wait_for_load(900.0)
r = report("during_export")
check(r.result == unreal.SCLoadPhase.CANCELLED and viewer.get_vehicle() is None, "export cancel did not cancel")

# 2. mid-import, with meshes built and workers pending
check(viewer.load_entity(entity, view), "LoadEntity refused")
check(viewer.wait_for_imported_meshes(MIN_MESHES, 900.0), "never reached a mid-import state with pending workers")
viewer.cancel_load()
viewer.wait_for_load(900.0)
r = report("during_import")
check(r.result == unreal.SCLoadPhase.CANCELLED and viewer.get_vehicle() is None, "import cancel did not cancel")
check(r.meshes_built_at_cancel >= MIN_MESHES and r.workers_pending_at_cancel, "cancel was not mid-import")
check(r.partial_objects > 0, "the partial import was not tracked")
check(r.partial_objects_alive == 0, "%d partial import objects survived GC" % r.partial_objects_alive)
check(r.vehicles == 0, "a vehicle survived the cancel")

# 3. the session still works
check(viewer.load_entity(entity, view) and viewer.wait_for_load(900.0), "load after cancel failed")
r = report("after")
check(r.vehicles == 1, "expected one vehicle, got %d" % r.vehicles)

# 4. teardown while an import of different options is mid-flight (not a reuse of the loaded one)
view.collision = not view.collision
check(viewer.load_entity(entity, view), "LoadEntity refused")
check(viewer.wait_for_imported_meshes(MIN_MESHES, 900.0), "never reached a mid-import state for teardown")
alive = unreal.SCViewerSubsystem.destroy_headless(viewer)
unreal.log("WP-V1 CANCEL teardown_with_pending_work alive_after_gc=%d" % alive)
check(alive == 0, "%d objects survived teardown" % alive)
unreal.log("WP-V1 CANCEL_OK")
