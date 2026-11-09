"""Headless WP-U1 import/parity/GC acceptance; paths are explicit command-line options."""
import unreal

args = unreal.SystemLibrary.get_command_line()
import shlex
options = dict(arg[1:].split("=", 1) for arg in shlex.split(args) if arg.startswith("-") and "=" in arg)
folder = options["SCPackage"]
world = unreal.EditorLoadingAndSavingUtils.load_map("/Game/Maps/Viewer")
if "SCVerifyTar" in options:
    if not unreal.SCImportLibrary.verify_package(world, folder, options["SCVerifyTar"]):
        raise RuntimeError("WP-U1 package verification failed; see LogSCImport")
else:
    vehicle = unreal.SCImportLibrary.import_folder(world, folder, True)
    if vehicle is None:
        raise RuntimeError("WP-U1 import failed")
unreal.log("WP-U1 HEADLESS_PYTHON_OK")
