"""Synthetic structural regressions, independent of any installed game build."""
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import tarfile
import tempfile
import unittest


class VerifyTest(unittest.TestCase):
    def check_export(self, vertices=3, lights=True, helper=False):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            scene = {"root_entity": {"animations": [], "lights": []}, "children": [], "interiors": []}
            geometry = {"asset": {"version": "2.0"}, "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
                        "accessors": [{"count": vertices}]}
            if helper:
                geometry = {"asset": {"version": "2.0"}, "nodes": [{"name": "synthetic_helper"}]}
            chunk = json.dumps(geometry).encode()
            chunk += b" " * (-len(chunk) % 4)
            glb = struct.pack("<4sIIII", b"glTF", 2, 20 + len(chunk), len(chunk), 0x4e4f534a) + chunk
            files = {"Packages/synthetic/scene.json": json.dumps(scene).encode(), "Data/synthetic.glb": glb}
            with tarfile.open(root / "ffi.tar", "w", format=tarfile.USTAR_FORMAT) as archive:
                for name, data in files.items():
                    entry = tarfile.TarInfo(name)
                    entry.size = len(data)
                    archive.addfile(entry, io.BytesIO(data))
                    path = root / "extracted" / name
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes(data)
            tar_path = root / "ffi.tar"
            size = sum(512 + ((len(data) + 511) // 512) * 512 for data in files.values()) + 1024
            tar_path.write_bytes(tar_path.read_bytes()[:size])
            (root / "clips.json").write_text('{"clips": []}')
            (root / "lights.json").write_text('{"lights": []}' if lights else '{}')
            return subprocess.run([sys.executable, "-I", str(Path(__file__).with_name("verify.py")), str(root)],
                                  capture_output=True, text=True)

    def test_counts_can_change(self):
        result = self.check_export()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_empty_glb_is_rejected(self):
        self.assertNotEqual(self.check_export(vertices=0).returncode, 0)

    def test_transform_only_glb_is_valid(self):
        result = self.check_export(helper=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_missing_lights_list_is_rejected(self):
        self.assertNotEqual(self.check_export(lights=False).returncode, 0)


if __name__ == "__main__":
    unittest.main()
