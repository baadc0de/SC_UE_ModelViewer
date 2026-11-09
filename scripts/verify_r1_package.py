#!/usr/bin/env python3
"""Verify the Gladius WP-R1 package, tar parity and material index contract.
Run with python3 -I: exported data is untrusted input.
"""
import json
import struct
import sys
import tarfile
from pathlib import Path


def glb(path):
    with path.open('rb') as source:
        magic, version, length = struct.unpack('<III', source.read(12))
        assert (magic, version, length) == (0x46546C67, 2, path.stat().st_size), path
        size, kind = struct.unpack('<II', source.read(8))
        assert kind == 0x4E4F534A, path
        return json.loads(source.read(size))


def main(root, archive_path):
    root = Path(root)
    files = {p.relative_to(root).as_posix(): p for p in root.rglob('*') if p.is_file()}
    meshes = [p for p in files.values() if p.suffix == '.glb']
    sidecars = [p for p in files.values() if p.name.endswith('.materials.json')]
    pngs = [p for p in files.values() if p.suffix == '.png']
    clips = [p for p in files.values() if 'animations' in p.parts and p.suffix == '.json']
    counts = (len(meshes), len(sidecars), len(pngs), len(clips))
    assert counts == (45, 23, 366, 63), counts
    assert all(p.stat().st_size for p in meshes)
    scene_path, = root.glob('Packages/*/scene.json')
    scene = json.loads(scene_path.read_bytes())
    assert scene['root_entity']['animations']
    root_mesh = root / scene['root_entity']['mesh_asset']
    doc = glb(root_mesh)
    root_counts = tuple(len(doc[k]) for k in ('nodes', 'meshes', 'materials'))
    assert root_counts == (274, 89, 27), root_counts
    sidecar = json.loads((root / scene['root_entity']['material_sidecar']).read_bytes())
    assert len(sidecar['submaterials']) == 28
    for index, material in enumerate(doc['materials']):
        name = sidecar['submaterials'][index]['submaterial_name']
        assert material['name'].endswith(f'_{name}_{index:02}'), (index, material['name'])
    highest = max(p['material'] for m in doc['meshes'] for p in m['primitives'] if 'material' in p)
    assert highest == 26, highest
    for mesh in meshes:
        doc = glb(mesh)
        for index, material in enumerate(doc.get('materials', [])):
            assert material['name'].endswith(f'_{index:02}'), (mesh, index)
        for item in doc.get('meshes', []):
            for primitive in item['primitives']:
                if 'material' in primitive:
                    assert primitive['material'] < len(doc['materials'])
    # Compare bytes without extracting untrusted tar paths onto disk.
    with tarfile.open(archive_path, 'r:') as archive:
        members = archive.getmembers()
        assert members[0].name.endswith('/scene.json')
        assert len(members) == len(files)
        assert {m.name for m in members} == set(files)
        for member in members:
            assert member.isfile(), member.name
            assert archive.extractfile(member).read() == files[member.name].read_bytes(), member.name
    print(f'GLBs/material sidecars/PNGs/clips: {counts}; no empty GLBs')
    print(f'Root nodes/meshes/materials: {root_counts}')
    print(f'Root animations: {len(scene["root_entity"]["animations"])}')
    print(f'scene.json first; all {len(files)} tar entries byte-identical to folder')
    print('GLB material slot indices preserved')


if __name__ == '__main__':
    main(*sys.argv[1:])
