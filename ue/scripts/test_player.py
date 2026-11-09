"""WP-P2b commandlet gate: player animator, weapon states and (with -SCPlayerPackage=<kit tar>) the kit rig."""
import unreal
assert "-SCPlayerTests" in unreal.SystemLibrary.get_command_line()
if not unreal.SCImportLibrary.run_unit_tests():
    raise RuntimeError("WP-P2 player checks failed")
unreal.log("WP-P2 HEADLESS_OK")
