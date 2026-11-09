#!/usr/bin/env python3
"""Validate WP-R5 exports against an unchanged baseline and print live evidence.

Run with python3 -I: inputs are generated/exported data. Named material selectors
are the WP's acceptance probes, never production classification rules. Numeric
emission expectations are derived from authored attributes, not copied game data.
"""
import argparse
import collections
import json
from pathlib import Path
import struct


def scene(root):
    paths = list((root / "Packages").rglob("scene.json"))
    assert len(paths) == 1, "provide a single-package export root"
    return json.loads(paths[0].read_bytes())


def inventory(root, package):
    paths = [p for p in root.rglob("*") if p.is_file()]
    return {
        "files": len(paths),
        "glbs": sum(p.suffix == ".glb" for p in paths),
        "pngs": sum(p.suffix == ".png" for p in paths),
        "sidecars": sum(p.name.endswith(".materials.json") for p in paths),
        "children": len(package["children"]),
        "interiors": len(package["interiors"]),
        "clips": len(package["root_entity"].get("animations", [])),
    }


def topology(path):
    data = path.read_bytes()
    assert data[:4] == b"glTF"
    size, kind = struct.unpack_from("<II", data, 12)
    assert kind == 0x4E4F534A
    doc = json.loads(data[20:20 + size])
    return {key: len(doc.get(key, [])) for key in ("nodes", "meshes", "materials", "skins", "accessors")}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before", type=Path)
    parser.add_argument("after", type=Path)
    args = parser.parse_args()
    old, new = scene(args.before), scene(args.after)
    assert new["version"] == 2
    assert inventory(args.before, old) == inventory(args.after, new)
    print("Unchanged package counts:", json.dumps(inventory(args.after, new), sort_keys=True))
    for path in args.after.rglob("*.glb"):
        assert topology(path) == topology(args.before / path.relative_to(args.after)), path
    print("All GLB topology counts unchanged; root:", topology(args.after / new["root_entity"]["mesh_asset"]))
    materials = []
    for path in args.after.rglob("*.materials.json"):
        doc = json.loads(path.read_bytes())
        assert doc["version"] == 2
        for material in doc["submaterials"]:
            attrs = {a["name"]: a["value"] for a in material["authored_attributes"]}
            glow = float(attrs.get("Glow", attrs.get("GlowAmount", "0")))
            emission = [float(v) for v in attrs.get("Emissive", "0,0,0").split(",")]
            diffuse = [float(v) for v in attrs.get("Diffuse", "1,1,1").split(",")]
            expected = [e * d * glow for e, d in zip(emission, diffuse)]
            actual = material["emissive"]["color"]
            assert all(abs(a - b) <= 1e-5 * max(1, abs(b)) for a, b in zip(actual, expected)), path
            materials.append(material)
    root = json.loads((args.after / new["root_entity"]["material_sidecar"]).read_bytes())
    by_name = {m["submaterial_name"]: m for m in root["submaterials"]}
    for name in ("emissives", "Headlight_Glow"):
        print(name, json.dumps(by_name[name]["emissive"], sort_keys=True))
    pipes = [m for m in materials if m["submaterial_name"] == "pipe mat 1"]
    assert pipes and all(m["emissive"]["color"] == [0, 0, 0] for m in pipes)
    print("pipe mat 1", pipes[0]["emissive"]["color"])
    for name, kind in (("pom", "relief_only"), ("decals", "color_sheet"), ("stencil", "stencil_palette")):
        assert by_name[name]["decal"]["kind"] == kind
        print(name, json.dumps(by_name[name]["decal"], sort_keys=True))
    groups = [new["root_entity"], *new["children"], *new["interiors"]]
    lights = [light for group in groups for light in group.get("lights", [])]
    ids = [(light["source_container_index"], light["source_light_index"]) for light in lights]
    assert len(ids) == len(set(ids)), "duplicate lights across entity/container arrays"
    assert len(lights) == sum(len(c["lights"]) for c in old["interiors"])
    print("Lights:", len(lights), "root:", len(new["root_entity"]["lights"]),
          "children:", sum(len(c["lights"]) for c in new["children"]),
          "interiors:", sum(len(c["lights"]) for c in new["interiors"]))
    print("Resolved helpers:", dict(collections.Counter(l.get("parent_node_name") for l in new["root_entity"]["lights"])))


if __name__ == "__main__":
    main()
