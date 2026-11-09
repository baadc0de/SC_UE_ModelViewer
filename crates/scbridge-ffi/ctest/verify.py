"""Validate export structure; game-build-specific counts are informational only."""
import json
from pathlib import Path
import struct
import sys
import tarfile

root = Path(sys.argv[1])
with tarfile.open(root / "ffi.tar") as archive:
    entries = archive.getmembers()
    assert entries and all(entry.type == tarfile.REGTYPE for entry in entries)
    paths = [entry.name for entry in entries]
    assert paths[0].startswith("Packages/") and paths[0].endswith("/scene.json")
    assert paths[1:] == sorted(paths[1:])
    for entry in entries:
        if not entry.name.endswith(".glb"):
            continue
        data = archive.extractfile(entry).read()
        assert len(data) >= 20 and data[:4] == b"glTF", entry.name
        version, length, json_size, kind = struct.unpack_from("<IIII", data, 4)
        assert version == 2 and length == len(data) and kind == 0x4e4f534a
        geometry = json.loads(data[20:20 + json_size])
        if "meshes" not in geometry:
            # Transform-only assembly helpers are valid GLBs too.
            assert geometry.get("nodes"), entry.name
            continue
        primitives = [p for mesh in geometry["meshes"] for p in mesh["primitives"]]
        assert primitives and all(geometry["accessors"][p["attributes"]["POSITION"]]["count"] > 0
                                  for p in primitives), entry.name
scene = json.loads((root / "extracted" / paths[0]).read_text())
clips = json.loads((root / "clips.json").read_text())["clips"]
lights = json.loads((root / "lights.json").read_text())["lights"]
assert isinstance(lights, list)
assert {c["id"] for c in clips} == {c["name"] for c in scene["root_entity"]["animations"]}
owners = [scene["root_entity"], *scene["children"], *scene["interiors"]]
assert all(isinstance(owner["lights"], list) for owner in owners)
counts = (sum(p.endswith(".glb") for p in paths), sum(p.endswith(".materials.json") for p in paths),
          sum(p.endswith(".png") for p in paths), len(clips), len(lights))
print(f"tar: {len(paths)} regular files; scene.json first; remaining paths sorted")
print("Informational counts: %d GLBs / %d sidecars / %d PNGs / %d clips / %d lights" % counts)
print("Structural invariants: PASS")
