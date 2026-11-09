#!/usr/bin/env python3
"""Validate WP-R5 exports against an unchanged baseline and print live evidence.

Run with python3 -I: inputs are generated/exported data. Named material selectors
are the WP's acceptance probes, never production classification rules. General
emission expectations are derived from authored attributes. The four
named strength checks are acceptance values supplied in the amended WP-R5 brief.
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


def gltf_document(path):
    data = path.read_bytes()
    assert data[:4] == b"glTF"
    size, kind = struct.unpack_from("<II", data, 12)
    assert kind == 0x4E4F534A
    doc = json.loads(data[20:20 + size])
    return doc


def topology(path):
    doc = gltf_document(path)
    return {key: len(doc.get(key, [])) for key in ("nodes", "meshes", "materials", "skins", "accessors")}


def placements(package):
    # Compare full authored placement transforms/paths per owning container,
    # preserving multiplicity (several fixtures may share geometry).
    return collections.Counter(json.dumps({"container": c["name"],
        "parent_entity": c.get("parent_entity_name"), "parent_node": c.get("parent_node_name"),
        "container_transform": c["container_transform"], "placement": p}, sort_keys=True)
        for c in package["interiors"] for p in c["placements"])


def all_lights(package):
    return [light for group in [package["root_entity"], *package["children"], *package["interiors"]]
            for light in group.get("lights", [])]


def check_glow_join(root, package, record_path):
    """Recheck live node/material joins against an exported raw DataCore record."""
    record = json.loads(record_path.read_bytes())
    component = next(c for c in record["_RecordValue_"]["Components"]
                     if c["_Type_"] == "SEntityComponentEffects")
    category = component["entityEffects"]["materialEffects"]
    assert category["enabled"]
    links = [(kind, i, j, link) for kind in ("tagEffects", "triggerEffects")
             for i, effect in enumerate(category[kind]) if effect["enabled"]
             for j, link in enumerate(effect["propertyLinks"]) if link["materialProperty"] == "Glow"]
    targets = [t for t in package["controls"]["engine_glow"]["targets"]
               if t["entity_name"] == record["_RecordName_"].removeprefix("EntityClassDefinition.")]
    assert targets and links
    for target in targets:
        gltf = gltf_document(root / target["mesh_asset"])
        sidecar = json.loads((root / target["material_sidecar"]).read_bytes())
        slot = next(i for i, m in enumerate(sidecar["submaterials"])
                    if m["index"] == target["source_material_index"])
        matches = [(node, primitive, kind, i, j) for node in gltf["nodes"] if "mesh" in node
                   for primitive in gltf["meshes"][node["mesh"]]["primitives"] if primitive.get("material") == slot
                   for kind, i, j, link in links if link["subMeshName"].casefold() == node["name"].casefold()]
        assert matches, f"target lacks enabled Glow link on its exact geometry node: {target}"
        for node, primitive, kind, i, j in matches:
            print("Glow join:", record["_RecordId_"], f"{kind}[{i}].propertyLinks[{j}].Glow",
                  node["name"], "source slot", target["source_material_index"],
                  target["submaterial_name"], "indices", gltf["accessors"][primitive["indices"]]["count"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before", type=Path)
    parser.add_argument("after", type=Path)
    parser.add_argument("--generic", action="store_true", help="omit Gladius-specific acceptance probes")
    parser.add_argument("--expect-engine-glow", choices=("present", "absent"), required=True)
    parser.add_argument("--glow-record", type=Path, help="raw DataCore entity JSON for live Glow-link join checks")
    args = parser.parse_args()
    old, new = scene(args.before), scene(args.after)
    assert new["version"] == 2
    assert placements(old) == placements(new), "interior placement identities/transforms changed"
    print("Placements unchanged:", sum(placements(new).values()))
    assert inventory(args.before, old) == inventory(args.after, new)
    print("Unchanged package counts:", json.dumps(inventory(args.after, new), sort_keys=True))
    for path in args.after.rglob("*.glb"):
        assert topology(path) == topology(args.before / path.relative_to(args.after)), path
    print("All GLB topology counts unchanged; root:", topology(args.after / new["root_entity"]["mesh_asset"]))
    before_control = bool(old.get("controls", {}).get("engine_glow", {}).get("targets"))
    control = new.get("controls", {}).get("engine_glow", {})
    assert bool(control.get("targets")) == (args.expect_engine_glow == "present")
    print("controls.engine_glow presence:", before_control, "->", bool(control.get("targets")),
          "targets:", len(control.get("targets", [])))
    if args.glow_record:
        check_glow_join(args.after, new, args.glow_record)
    materials = []
    black_with_glow = 0
    for path in args.after.rglob("*.materials.json"):
        doc = json.loads(path.read_bytes())
        assert doc["version"] == 2
        for material in doc["submaterials"]:
            attrs = {a["name"]: a["value"] for a in material["authored_attributes"]}
            glow = float(attrs.get("Glow", attrs.get("GlowAmount", "0")))
            emission = [float(v) for v in attrs.get("Emissive", "0,0,0").split(",")]
            tokens = set(attrs.get("StringGenMask", "").upper().split("%"))
            emittance = bool(tokens & {"EMITTANCE_MAP", "MACRO_EMISSIVE_MAP"})
            expected_tint = emission if any(emission) else [1,1,1]
            raw_flags = attrs.get("MtlFlags", "0").strip()
            flags = int(raw_flags, 16 if raw_flags.lower().startswith("0x") else 10)
            forward = bool(flags & 0x8000)
            forward_card = forward and material["shader_family"] in ("Illum", "HardSurface") and any(emission)
            strength = glow if glow > 0 else sum(emission)/3 if forward_card else 1 if emittance else 0
            actual = material["emissive"]
            assert all(abs(a-b) <= 1e-5 * max(1,abs(b)) for a,b in zip(actual["color"], expected_tint)), path
            assert abs(actual["strength"] - strength) <= 1e-5 * max(1, abs(strength)), path
            assert actual["uses_base_texture"] == (not emittance), path
            assert actual["forward_rendering"] == forward, path
            alpha = material["alpha_config"]
            assert alpha["forward_rendering"] == forward, path
            assert alpha["two_sided"] == bool(flags & 0x2), path
            assert abs(alpha["alpha_test"] - float(attrs.get("AlphaTest", "0"))) < 1e-6, path
            assert abs(alpha["opacity"] - float(attrs.get("Opacity", "1"))) < 1e-6, path
            black_with_glow += glow > 0 and not any(emission)
            materials.append(material)
    print("Glow-positive / black-Emissive submaterials:", black_with_glow)
    if not args.generic:
        root = json.loads((args.after / new["root_entity"]["material_sidecar"]).read_bytes())
        by_name = {m["submaterial_name"]: m for m in root["submaterials"]}
        # LIVE acceptance values supplied in the amended brief, not production rules.
        expected_strengths = {"emissives": 0.3, "Headlight_Glow": 1.0, "glows": 0.35}
        for name, expected_strength in expected_strengths.items():
            if name == "glows":
                m = next(m for m in materials if m["submaterial_name"] == name
                         and m["material_set_identity"]["source_stem"].casefold() == "klwe_las_rep_s1-3")
            else:
                m = by_name[name]
            assert abs(m["emissive"]["strength"] - expected_strength) < 1e-6, name
            attrs = {a["name"]: a["value"] for a in m["authored_attributes"]}
            diffuse = [float(v) for v in attrs["Diffuse"].split(",")]
            effective = [c*d*m["emissive"]["strength"] for c,d in zip(m["emissive"]["color"], diffuse)]
            print(name, json.dumps(m["emissive"], sort_keys=True), "surface RGB before texture:", effective)
        pipes = [m for m in materials if m["submaterial_name"] == "pipe mat 1"]
        assert pipes and all(m["emissive"]["strength"] == 0 for m in pipes)
        print("pipe mat 1", pipes[0]["emissive"])
        for name, kind in (("pom", "relief_only"), ("decals", "color_sheet"), ("stencil", "stencil_palette")):
            assert by_name[name]["decal"]["kind"] == kind
            print(name, json.dumps(by_name[name]["decal"], sort_keys=True))
    lights = all_lights(new)
    ids = [(light["source_container_index"], light["source_light_index"]) for light in lights]
    assert len(ids) == len(set(ids)), "duplicate lights across entity/container arrays"
    assert len(lights) == len(all_lights(old))
    print("Lights:", len(lights), "root:", len(new["root_entity"]["lights"]),
          "children:", sum(len(c["lights"]) for c in new["children"]),
          "interiors:", sum(len(c["lights"]) for c in new["interiors"]))
    print("Resolved helpers:", dict(collections.Counter(l.get("parent_node_name") for l in new["root_entity"]["lights"])))


if __name__ == "__main__":
    main()
