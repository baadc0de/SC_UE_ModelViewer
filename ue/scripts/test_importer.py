"""Run synchronous synthetic SCBridge automation tests without starting editor UI."""
import unreal
if "-SCPackage=" in unreal.SystemLibrary.get_command_line():
    unreal.EditorLoadingAndSavingUtils.load_map("/Game/Maps/Viewer")
if not unreal.SCImportLibrary.run_unit_tests():
    raise RuntimeError("SCBridge unit tests failed")
unreal.log("WP-U1 UNIT_TESTS_OK")
