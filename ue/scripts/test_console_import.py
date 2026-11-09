"""Verify sc.Import preserves quoted folder paths containing spaces."""
import shlex
import unreal

options = dict(arg[1:].split("=", 1) for arg in shlex.split(unreal.SystemLibrary.get_command_line()) if arg.startswith("-") and "=" in arg)
folder = options["SCConsolePackage"]
world = unreal.EditorLoadingAndSavingUtils.load_map("/Game/Maps/Viewer")
unreal.SystemLibrary.execute_console_command(world, f'sc.Import "{folder}"')
vehicles = unreal.GameplayStatics.get_all_actors_of_class(world, unreal.SCVehicle)
if len(vehicles) != 1:
    raise RuntimeError(f"Expected one vehicle after quoted console import, got {len(vehicles)}")
unreal.SCImportLibrary.unload_package(world)
unreal.log("WP-U1 QUOTED_CONSOLE_OK")
