#!/usr/bin/env python3
"""Print a one-screen summary of a .glb: counts of nodes/meshes/materials/
textures/skins/animations, extensions used, and animation names.
Usage: python3 -I scripts/glbinfo.py file.glb"""
import json, struct, sys

def main(path: str) -> None:
    with open(path, "rb") as f:
        magic, version, length = struct.unpack("<III", f.read(12))
        if magic != 0x46546C67:
            sys.exit(f"{path}: not a GLB (magic {magic:#x})")
        chunk_len, chunk_type = struct.unpack("<II", f.read(8))
        doc = json.loads(f.read(chunk_len))
    for key in ("nodes", "meshes", "materials", "textures", "images", "skins", "animations", "scenes"):
        print(f"{key:12s} {len(doc.get(key, []))}")
    print("extensionsUsed:", doc.get("extensionsUsed"))
    print("animations:", [a.get("name") for a in doc.get("animations", [])])
    print("asset.extras keys:", list((doc.get("asset", {}).get("extras") or {}).keys()))

if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
