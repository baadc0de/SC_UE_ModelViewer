#!/usr/bin/env python3
"""Compare native local GLB bone transforms or animation sidecar bytes.

Run with python3 -I when inspecting game-derived exports. No archive extraction
or third-party modules are needed. Bone names and bind data come from arguments.
"""
import argparse
import json
import math
from pathlib import Path
import struct
import tarfile


def glb_nodes(path):
    with path.open("rb") as stream:
        magic, version, _ = struct.unpack("<III", stream.read(12))
        assert (magic, version) == (0x46546C67, 2), f"Invalid GLB: {path}"
        size, kind = struct.unpack("<II", stream.read(8))
        assert kind == 0x4E4F534A, f"Missing JSON chunk: {path}"
        return json.loads(stream.read(size))["nodes"]


def local_transform(node):
    if "matrix" not in node:
        x, y, z, w = node.get("rotation", [0, 0, 0, 1])
        return node.get("translation", [0, 0, 0]), [w, x, y, z]
    m = node["matrix"]
    # glTF column-major matrix, removing any scale before quaternion extraction.
    scales = [math.sqrt(sum(m[c * 4 + r] ** 2 for r in range(3))) for c in range(3)]
    r = [[m[c * 4 + row] / scales[c] for c in range(3)] for row in range(3)]
    trace = sum(r[i][i] for i in range(3))
    if trace > 0:
        s = 2 * math.sqrt(trace + 1)
        q = [s / 4, (r[2][1] - r[1][2]) / s,
             (r[0][2] - r[2][0]) / s, (r[1][0] - r[0][1]) / s]
    else:
        i = max(range(3), key=lambda axis: r[axis][axis])
        j, k = (i + 1) % 3, (i + 2) % 3
        s = 2 * math.sqrt(1 + r[i][i] - r[j][j] - r[k][k])
        q = [(r[k][j] - r[j][k]) / s, 0, 0, 0]
        q[i + 1] = s / 4
        q[j + 1] = (r[j][i] + r[i][j]) / s
        q[k + 1] = (r[k][i] + r[i][k]) / s
    norm = math.sqrt(sum(v * v for v in q))
    return m[12:15], [v / norm for v in q]


def pose(args):
    reference = glb_nodes(args.reference)
    bind = glb_nodes(args.bind)
    for name, component in [(args.position_bone, "position"), (args.rotation_bone, "rotation")]:
        matches = [n for n in reference if n.get("name") == name]
        originals = [n for n in bind if n.get("name") == name]
        assert len(matches) == len(originals) == 1, (name, len(matches), len(originals))
        position, rotation = local_transform(matches[0])
        bind_position, bind_rotation = local_transform(originals[0])
        distance = math.dist(position, bind_position)
        dot = abs(sum(a * b for a, b in zip(rotation, bind_rotation)))
        angle = math.degrees(2 * math.acos(min(1, dot)))
        print(f"{name}: native Y-up position={position}; rotation wxyz={rotation}")
        if component == "position":
            print(f"  bind position={bind_position}; error={distance:.9g} m")
            assert distance <= args.position_tolerance
        else:
            print(f"  bind rotation wxyz={bind_rotation}; error={angle:.9g} degrees")
            assert angle <= args.rotation_tolerance


def sidecars(args):
    before = {str(p.relative_to(args.baseline)): p.read_bytes()
              for p in args.baseline.glob("Packages/*/animations/*.json")}
    assert before, f"No baseline animation sidecars: {args.baseline}"
    with tarfile.open(args.export, "r:") as archive:
        after = {}
        for member in archive:
            if member.isfile() and "/animations/" in member.name and member.name.endswith(".json"):
                assert member.name not in after, f"Duplicate tar member: {member.name}"
                after[member.name] = archive.extractfile(member).read()
    assert before.keys() == after.keys(), f"Sidecar paths differ: {before.keys() ^ after.keys()}"
    changed = [name for name in before if before[name] != after[name]]
    assert not changed, f"Sidecar bytes differ: {changed}"
    print(f"{args.baseline}: {len(before)} animation sidecars byte-identical; 0 changed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    bones = commands.add_parser("pose", help="Check selected bone components against bind GLB")
    bones.add_argument("reference", type=Path)
    bones.add_argument("bind", type=Path)
    bones.add_argument("--position-bone", required=True)
    bones.add_argument("--rotation-bone", required=True)
    bones.add_argument("--position-tolerance", type=float, default=1e-4)
    bones.add_argument("--rotation-tolerance", type=float, default=1e-3)
    bones.set_defaults(run=pose)
    clips = commands.add_parser("sidecars", help="Compare existing sidecars to a fresh package tar")
    clips.add_argument("baseline", type=Path)
    clips.add_argument("export", type=Path)
    clips.set_defaults(run=sidecars)
    args = parser.parse_args()
    args.run(args)


if __name__ == "__main__":
    main()
