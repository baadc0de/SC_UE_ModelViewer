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


LIGHT_FIELDS = (
    "bulb_radius_m", "plane_width_m", "plane_height_m", "shadow_casting",
    "max_shadow_cast_distance_m", "affects_this_area_only", "affect_gi",
    "attenuation_tweak", "plane_softness", "fade_min_distance_m",
    "fade_max_distance_m", "importance", "glow_multiplier",
    "intensity_candela_proxy_model", "importance_name", "affect_gi_mode",
)


def check_light_fields(lights):
    for light in lights:
        assert all(field in light for field in LIGHT_FIELDS), light["name"]
        assert light["intensity_candela_proxy_model"] == "raw_x1500"
        assert light["intensity_unit"] == "cryengine_authored_intensity"
        for state in light["states"].values():
            assert state["intensity_candela_proxy_model"] == "raw_x1500"
            assert state["intensity_cd"] == state["intensity_candela_proxy"]
        if light["importance_name"] is not None:
            assert isinstance(light["importance"], int), light["importance_name"]
        if light["affect_gi_mode"] in ("On", "Off"):
            assert light["affect_gi"] == (light["affect_gi_mode"] == "On")
        default = light["states"].get("defaultState")
        if default is not None:
            assert light["active_state"] == "defaultState", light["name"]
            assert light["intensity_raw"] == default["intensity_raw"], light["name"]
            if default["intensity_raw"] == 0:
                assert light["intensity_candela_proxy"] == light["intensity"] == 0, light["name"]
    return sum(light["states"].get("defaultState", {}).get("intensity_raw") == 0
               for light in lights)


def same_authored_value(a, b):
    """Direct f32 JSON and scene Value JSON can use different decimal spellings."""
    if isinstance(a, dict) and isinstance(b, dict):
        return a.keys() == b.keys() and all(same_authored_value(a[k], b[k]) for k in a)
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(same_authored_value(x, y) for x, y in zip(a, b))
    if isinstance(a, float) and isinstance(b, float):
        return struct.pack("<f", a) == struct.pack("<f", b)
    return a == b


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
        material = sidecar["submaterials"][slot]
        tokens = set(material["decoded_feature_flags"]["tokens"])
        assert ((material["shader_family"] in ("Illum", "HardSurface")
                 and material["alpha_config"]["forward_rendering"])
                or tokens & {"EMITTANCE_MAP", "MACRO_EMISSIVE_MAP"}), material["submaterial_name"]
        def selector_matches(link, node):
            return link["subMeshName"] in ("", "*") or node.get("name", "").casefold() == link["subMeshName"].casefold()
        authored = [(kind, i, j, link) for kind, i, j, link in links
                    if any(selector_matches(link, node) and "mesh" in node
                           and any(p.get("material") == slot for p in gltf["meshes"][node["mesh"]]["primitives"])
                           for node in gltf["nodes"])]
        assert len(target["property_links"]) == len(authored)
        for exported, (kind, i, j, link) in zip(target["property_links"], authored):
            assert (exported["effect_kind"], exported["effect_index"], exported["property_link_index"]) == (kind, i, j)
            assert exported["submesh_name"] == link["subMeshName"]
            assert exported["parameter_record_id"] == link["parameterName"]["_RecordId_"]
            assert exported["tag_record_id"] == category[kind][i]["tag"]["_RecordId_"]
            expected_modifiers = []
            for modifier in link["modifiers"]:
                typ = modifier["_Type_"]
                if typ == "SEntityEffectSystem_MultiplierModifier":
                    expected_modifiers.append({"kind": "multiplier", "multiplier": modifier["multiplier"]})
                elif typ == "SEntityEffectSystem_MinMaxModifier":
                    expected_modifiers.append({"kind": "min_max", "minimum": modifier["minimum"], "maximum": modifier["maximum"]})
                else:
                    raise AssertionError(f"unverified modifier type {typ}")
            assert exported["modifiers"] == expected_modifiers
        for key in ("multiplier", "minimum", "maximum"):
            assert target[key] == [link[key] for link in target["property_links"]]
        print("Glow modifiers:", {key: target[key] for key in ("multiplier", "minimum", "maximum")})
        print("Glow material:", target["submaterial_name"], "static strength", material["emissive"]["strength"],
              "tint", material["emissive"]["color"], "selectors", [link[3]["subMeshName"] for link in authored])
        matches = [(node, primitive, kind, i, j) for node in gltf["nodes"] if "mesh" in node
                   for primitive in gltf["meshes"][node["mesh"]]["primitives"] if primitive.get("material") == slot
                   for kind, i, j, link in links if selector_matches(link, node)]
        assert matches, f"target lacks enabled Glow link on its selected geometry: {target}"
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
    parser.add_argument("--glow-record", type=Path, nargs="+", default=[], help="raw DataCore entity JSON for live Glow-link join checks")
    parser.add_argument("--catalogue", type=Path, help="scviewer lights JSON for authored-field parity")
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
    # Named LIVE acceptance lists supplied by the review brief, never production rules.
    expected_targets = {"AEGS_Gladius": ["engine_glow"],
                        "RSI_Scorpius": ["Glow_Thrusters"] * 4,
                        "DRAK_Cutlass_Black": ["thruster_glow"] * 2}
    entity = new["root_entity"]["entity_name"].removeprefix("EntityClassDefinition.")
    targets = control.get("targets", [])
    if entity in expected_targets:
        assert sorted(t["submaterial_name"] for t in targets) == sorted(expected_targets[entity])
    assert all(t["property_links"] for t in targets), "engine targets must have authored Glow links"
    print("Engine target list:", [t["submaterial_name"] for t in targets])
    for record_path in args.glow_record:
        check_glow_join(args.after, new, record_path)
    if args.glow_record:
        checked_entities = {json.loads(p.read_bytes())["_RecordName_"].removeprefix("EntityClassDefinition.")
                            for p in args.glow_record}
        assert {t["entity_name"] for t in targets} <= checked_entities, "supply raw records for every target owner"
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
            strength = glow if glow > 0 else 1 if forward_card or emittance else 0
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
        targets = control["targets"]
        assert [t["submaterial_name"] for t in targets] == ["engine_glow"]
        # LIVE evidence from the focused Glow-link brief and thruster-record.json.
        assert targets[0]["multiplier"] == [50, 100]
        assert targets[0]["minimum"] == [10, 20]
        assert targets[0]["maximum"] == [10000000, 10000000]
        assert (control["units"], control["min_strength"], control["max_strength"], control["default_strength"]) == ("material_property_glow", 10, 10000000, 10)
        print("Glow control:", {k: control[k] for k in ("units", "min_strength", "max_strength", "default_strength")})
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
    off_defaults = check_light_fields(lights)
    identity = lambda light: (light["source_container_index"], light["source_light_index"])
    previous = {identity(light): light for light in all_lights(old)}
    changed = sum(light["active_state"] != previous[identity(light)]["active_state"] for light in lights)
    print("WP-R5b:", entity, "active state changed:", changed, "authored-off defaults:", off_defaults)
    if args.catalogue:
        catalogue = json.loads(args.catalogue.read_bytes())["lights"]
        check_light_fields(catalogue)
        indexed = {identity(light): light for light in catalogue}
        assert len(indexed) == len(lights)
        for light in lights:
            other = indexed[identity(light)]
            for field in (*LIGHT_FIELDS, "active_state", "intensity_raw", "states"):
                assert same_authored_value(light[field], other[field]), (light["name"], field)
        print("Catalogue/scene authored-field parity:", len(lights), "lights")
    if lights:
        print("Authored field sample:", json.dumps({field: lights[0][field] for field in LIGHT_FIELDS}, sort_keys=True))
    ids = [(light["source_container_index"], light["source_light_index"]) for light in lights]
    assert len(ids) == len(set(ids)), "duplicate lights across entity/container arrays"
    assert len(lights) == len(all_lights(old))
    print("Lights:", len(lights), "root:", len(new["root_entity"]["lights"]),
          "children:", sum(len(c["lights"]) for c in new["children"]),
          "interiors:", sum(len(c["lights"]) for c in new["interiors"]))
    print("Resolved helpers:", dict(collections.Counter(l.get("parent_node_name") for l in new["root_entity"]["lights"])))


if __name__ == "__main__":
    main()
