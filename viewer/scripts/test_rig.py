"""WP-U3 package rig/clip/light verification. Optional -SCPoseReference=GLB -SCPoseClip=id."""
import shlex
import unreal
args = dict(word[1:].split("=", 1) for word in shlex.split(unreal.SystemLibrary.get_command_line()) if word.startswith("-") and "=" in word)
unreal.EditorLoadingAndSavingUtils.load_map("/Game/Maps/Viewer")
world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
vehicle = unreal.SCImportLibrary.import_folder(world, args["SCPackage"])
if not vehicle or not unreal.SCImportLibrary.verify_rig(vehicle, args.get("SCPoseReference", ""), args.get("SCPoseClip", "None")):
    raise RuntimeError("WP-U3 rig verification failed")
unreal.log("WP-U3 RIG_TESTS_OK")
unreal.SCImportLibrary.unload_package(world)
