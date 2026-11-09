"""Run the editor-only Interchange baseline against an explicit GLB; never saves assets."""
import shlex
import unreal
options = dict(arg[1:].split("=", 1) for arg in shlex.split(unreal.SystemLibrary.get_command_line()) if arg.startswith("-") and "=" in arg)
if not unreal.SCInterchangeBaseline.import_glb(options["SCBaseline"]):
    raise RuntimeError("Interchange/direct baseline mismatch; see SC_BASELINE log")
unreal.log("WP-U1 INTERCHANGE_BASELINE_OK")
