"""Load the extracted package through Blender's actual pure-Python package reader."""
import hashlib
import importlib.util
import json
from pathlib import Path
import sys

root = Path(sys.argv[1]).resolve()
source = Path(__file__).resolve().parents[3] / "vendor/StarBreaker/blender_addon/starbreaker_addon/manifest.py"
spec = importlib.util.spec_from_file_location("scbridge_blender_manifest", source)
module = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = module
spec.loader.exec_module(module)
scene, = root.glob("Packages/**/scene.json")
bundle = module.PackageBundle.load(scene)
assert bundle.scene_path == scene
stamp = json.loads((root / "Data/UI/Generated/.export_stamp.json").read_text())
assert stamp["schema_version"] == 2
assert stamp["archive_build_id"] and stamp["exporter"]["source_sha256"]
assert not any(key in stamp for key in ("written_at_epoch_s", "binary_built_at_epoch_s", "git_describe"))
for relative, expected in stamp["generated_files"].items():
    path = (root / relative).resolve()
    assert path.is_relative_to(root)
    assert hashlib.sha256(path.read_bytes()).hexdigest() == expected, relative
print("Blender PackageBundle.load: PASS; deterministic stamp and generated hashes: PASS")
