"""Read untrusted game exports in isolated Python; assert LIVE 4.10.2 smoke fixture."""
import json
from pathlib import Path
import sys
import tarfile

root = Path(sys.argv[1])
raw = (root / "ffi.tar").read_bytes()
offset = 0
paths = []
while any(raw[offset:offset + 512]):
    header = raw[offset:offset + 512]
    assert len(header) == 512 and header[156:157] == b"0"
    assert header[257:263] == b"ustar\0"
    name = header[:100].split(b"\0", 1)[0].decode("utf-8")
    prefix = header[345:500].split(b"\0", 1)[0].decode("utf-8")
    paths.append(f"{prefix}/{name}" if prefix else name)
    size = int(header[124:136].rstrip(b"\0 "), 8)
    offset += 512 + ((size + 511) // 512) * 512
assert raw[offset:] == b"\0" * 1024
assert paths[0].startswith("Packages/") and paths[0].endswith("/scene.json")
assert paths[1:] == sorted(paths[1:])
with tarfile.open(root / "ffi.tar") as archive:
    assert paths == archive.getnames()
scene = json.loads((root / "extracted" / paths[0]).read_text())
clips = json.loads((root / "clips.json").read_text())["clips"]
lights = json.loads((root / "lights.json").read_text())["lights"]
assert {c["id"] for c in clips} == {c["name"] for c in scene["root_entity"]["animations"]}
exported_lights = [light for owner in [scene["root_entity"], *scene["children"], *scene["interiors"]]
                   for light in owner["lights"]]
identity = lambda light: (light["source_container_index"], light["source_light_index"])
assert {identity(light) for light in exported_lights} == {identity(light) for light in lights}
counts = (sum(p.endswith(".glb") for p in paths), sum(p.endswith(".materials.json") for p in paths),
          sum(p.endswith(".png") for p in paths), len(clips), len(lights))
assert counts == (45, 23, 366, 63, 77), counts
assert len(exported_lights) == 77
print(f"ustar: {len(paths)} regular files; scene.json first; name/prefix reader agrees with tarfile")
print("Gladius: 45 GLBs / 23 sidecars / 366 PNGs / 63 clips / 77 lights; catalogue joins PASS")
