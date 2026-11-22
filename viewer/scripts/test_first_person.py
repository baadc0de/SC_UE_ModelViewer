"""WP-P1 commandlet gate. The C++ suite imports -SCPackage and runs real physics queries."""
import unreal
assert "-SCFirstPersonTests" in unreal.SystemLibrary.get_command_line()
if not unreal.SCImportLibrary.run_unit_tests():
    raise RuntimeError("WP-P1 first-person checks failed")
unreal.log("WP-P1 HEADLESS_OK")
