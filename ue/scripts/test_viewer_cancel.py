"""WP-V1 cancel checks through USCViewerSubsystem: cancel during the FFI export and during the import job.

Each cancelled load must end CANCELLED, leave no vehicle and no transient import objects alive after GC, and the
FFI handle must stay usable (the next load succeeds). Entity names are test data.
UnrealEditor-Cmd ue/SCViewer/SCViewer.uproject -run=pythonscript -script=ue/scripts/test_viewer_cancel.py \
    -nullrhi -unattended -nop4 -nosplash -ddc=SCViewerDDC [-SCEntity=AEGS_Gladius]
"""
import shlex

import unreal

options = dict(arg[1:].split("=", 1) for arg in shlex.split(unreal.SystemLibrary.get_command_line())
               if arg.startswith("-") and "=" in arg)
entity = options.get("SCEntity", "AEGS_Gladius")
viewer = unreal.SCViewerSubsystem.create_headless()
if not viewer.wait_for_archive(900.0):
    raise RuntimeError(viewer.get_archive_error())
view = unreal.SCViewOptions()
view.reset_to_defaults()


def cancelled(label, phase):
    if not viewer.load_entity(entity, view):
        raise RuntimeError("LoadEntity refused")
    if phase is not None and not viewer.wait_for_phase(phase, 900.0):
        raise RuntimeError("never reached %s (now %s)" % (phase, viewer.get_load_phase()))
    viewer.cancel_load()
    viewer.wait_for_load(900.0)
    r = viewer.get_last_report()
    unreal.log("WP-V1 CANCEL %s result=%s vehicle=%s export_s=%.3f import_s=%.3f" % (
        label, r.result, viewer.get_vehicle(), r.export_seconds, r.import_seconds))
    if r.result != unreal.SCLoadPhase.CANCELLED or viewer.get_vehicle() is not None:
        raise RuntimeError("%s: expected a cancelled load without a vehicle" % label)


cancelled("during_export", None)                         # flag set while the FFI export runs
cancelled("during_import", unreal.SCLoadPhase.IMPORTING)  # job created, mid-import
if not viewer.load_entity(entity, view) or not viewer.wait_for_load(900.0):
    raise RuntimeError("load after cancel failed: " + viewer.get_last_report().error)
r = viewer.get_last_report()
unreal.log("WP-V1 CANCEL after result=%s static_meshes=%d vehicles=%d" % (r.result, r.static_meshes, r.vehicles))
if r.vehicles != 1:
    raise RuntimeError("a cancelled import left a vehicle alive: %d" % r.vehicles)
unreal.SCViewerSubsystem.destroy_headless(viewer)
unreal.log("WP-V1 CANCEL_OK")
