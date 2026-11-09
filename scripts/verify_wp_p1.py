#!/usr/bin/env python3
"""Check WP-P1 additive item types and scoped Mannequin references against live DataCore.

Run with python3 -I because these packages are exported data. The optional live
lookup uses the existing CLI query (no game-specific category/name tables).
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import subprocess
import struct

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("packages", type=Path, nargs="+")
parser.add_argument("--datacore", type=Path, help="StarBreaker CLI for independent live item-type validation")
args = parser.parse_args()
authored = {}
if args.datacore:
    output = subprocess.run([str(args.datacore.resolve()), "dcb", "query",
        "EntityClassDefinition.Components[SAttachableComponentParams].AttachDef.Type"],
        check=True, capture_output=True, text=True).stdout
    for line in output.splitlines():
        if "\t" in line:
            name, value = line.split("\t", 1)
            authored[name.removeprefix("EntityClassDefinition.")] = json.loads(value) if value.startswith(('"', "null")) else value
for root in args.packages:
    scenes = list(root.glob("Packages/*/scene.json"))
    assert len(scenes) == 1, (root, "expected one scene")
    scene = json.loads(scenes[0].read_bytes())
    counts = Counter()
    compared = 0
    for child in scene["children"]:
        assert "item_type" in child, child["entity_name"]
        item_type = child["item_type"]
        assert item_type is None or isinstance(item_type, str) and item_type
        counts[item_type] += 1
        name = child["entity_name"].removeprefix("EntityClassDefinition.")
        if args.datacore and item_type is not None:
            assert name in authored, (name, "type missing from independent DataCore query")
            assert item_type == authored[name], (name, item_type, authored[name])
            compared += 1
    clips = scene["root_entity"].get("animations", [])
    interior_meshes = {placement["cgf_path"].replace("\\", "/").lower().removeprefix("data/"): placement["mesh_asset"]
                       for container in scene.get("interiors", []) for placement in container.get("placements", [])}
    scoped = 0
    interior_receivers = 0
    for clip in clips:
        if not clip["name"].startswith("item_"):
            continue
        scoped += 1
        sidecar = json.loads((scenes[0].parent / clip["sidecar"]).read_bytes())
        assert sidecar.get("owner_entity")
        owner_path = sidecar["owner_entity"].replace("\\", "/").lower().removeprefix("data/")
        if owner_path in interior_meshes and sidecar.get("fragments"):
            glb = (root / interior_meshes[owner_path]).read_bytes()
            document = json.loads(glb[20:20 + struct.unpack_from("<I", glb, 12)[0]])
            nodes = {node.get("name", "").lower() for node in document.get("nodes", [])}
            receivers = {channel.get("source_node_name", "").lower()
                         for value in sidecar["bones"].values()
                         for channel in (value if isinstance(value, list) else [value])}
            assert (receivers - {""}) & nodes, (owner_path, "interior controller lost its receiving hierarchy")
            interior_receivers += 1
        for fragment in sidecar.get("fragments") or []:
            for animation in fragment.get("animations") or []:
                # A fragment can reference unsupported clips, but its own receiving clip must use the same ID namespace.
                assert animation["name"].startswith(clip["name"].split("__", 1)[0] + "__")
    print(f"P1_CONTRACT root={root} children={sum(counts.values())} types={dict(counts)} scoped_clips={scoped} interior_receivers={interior_receivers} compared={compared} live={bool(authored)}")
