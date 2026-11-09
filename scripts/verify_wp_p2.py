#!/usr/bin/env python3
"""Validate untrusted WP-P2 player tars without extracting or executing content.
Run with python3 -I. Optional --require-entity checks a requested loadout fixture.
"""
import argparse
import hashlib
import json
import struct
import tarfile
from pathlib import Path, PurePosixPath


def required_state(state, weapons):
    weapon = weapons.get(state['weapon_child_id'])
    return state['stance'] == 'stand' and (
        state['weapon_child_id'] is None or
        {'stocked', 'rifle'} <= set(weapon['mannequin_tags']))


def verify(path, required):
    with tarfile.open(path, 'r:') as tar:
        members = tar.getmembers()
        assert members and members[0].name.endswith('/scene.json'), 'scene must be first'
        names = {m.name for m in members}
        assert len(names) == len(members), 'duplicate tar paths'
        for m in members:
            p = PurePosixPath(m.name)
            assert m.isfile() and not p.is_absolute() and '..' not in p.parts
        def read(name):
            m = tar.getmember(name)
            assert m.size <= 512 * 1024 * 1024, 'oversized entry'
            return tar.extractfile(m).read()
        scene = json.loads(read(members[0].name))
        package = str(PurePosixPath(members[0].name).parent)
        assert scene['version'] == 2 and scene['assembly_kind'] == 'character'
        player = scene['player']
        root = scene['root_entity']
        glb = read(root['mesh_asset'])
        assert glb[:4] == b'glTF'
        size, kind = struct.unpack_from('<II', glb, 12)
        assert kind == 0x4E4F534A
        doc = json.loads(glb[20:20 + size])
        assert doc.get('skins'), 'root must carry a skin'
        joints = {n.get('name') for n in doc['nodes']}
        assert player['camera_bone'] in joints and player['root_motion_bone'] in joints
        assert player['hand_ports']
        for p in player['hand_ports']:
            assert p['attach_bone'] in joints and 'select' in p['flags']
        def assets(node, package_dir):
            for field in ('mesh_asset', 'material_sidecar'):
                if node.get(field):
                    assert node[field] in names, node[field]
            for clip in node.get('animations', []):
                assert package_dir + '/' + clip['sidecar'] in names
            for child in node.get('children', []):
                assets(child, child.get('package_dir', package_dir))
        assets(root, package)
        for child in scene['children']:
            assets(child, child.get('package_dir', package))
            if not child.get('id', '').startswith('player_item'):
                assert 'port' in child and 'flags' in child['port'], child['entity_name']
        assert 'port' in root and 'flags' in root['port']
        for diagnostic in player.get('diagnostics', []):
            print('DIAGNOSTIC ' + json.dumps(diagnostic, sort_keys=True))
        missing = {d['clip'] for d in player.get('diagnostics', []) if d['kind'] == 'missing_clip'}
        weapons = {w['child_id']: w for w in player['weapons']}
        children = {c['id']: c for c in scene['children'] if 'id' in c}
        for entity in required:
            assert any(c['entity_name'] == entity for c in children.values()), entity
        assert player['weapons'], 'no stowed weapons'
        for w in player['weapons']:
            c = children[w['child_id']]
            assert c['assembly_kind'] == 'fps_weapon' and c['weapon_assembly']['anchors']
            assert c['port'] == w['stow_port'] and c['port']['attach_bone'] in joints
            assert w['mannequin_tags'] and w['grip_data']
            fragments = [f['fragment'].lower() for s in player['motion']['states']
                         if s['weapon_child_id'] == w['child_id'] for f in s['fragments']]
            assert 'select' in fragments and 'deselect' in fragments, w['entity']
        clips = {}
        for index in root['animations']:
            clip = json.loads(read(package + '/' + index['sidecar']))
            assert clip['name'] == index['name']
            assert all(k in clip for k in ['additive', 'cyclic', 'duration', 'root_motion', 'bones'])
            assert clip['duration'] >= 0 and clip['root_motion']['bone'] == player['root_motion_bone']
            clips[clip['name']] = clip
        assert any(c['additive'] for c in clips.values()), 'no additive body clip'
        spaces = {s['id']: s for s in player['blend_spaces']}
        assert spaces, 'no blend spaces'
        for s in spaces.values():
            for e in s['examples']:
                if e.get('missing'):
                    assert e['clip'] in missing and e['clip'] not in clips
                    continue
                assert e['clip'] in clips, e['clip']
                assert len(e['params']) == len(s['dimensions'])
                for v, d in zip(e['params'], s['dimensions']):
                    assert d['min'] <= v <= d['max'], (s['id'], v, d)
            for face in s['faces']:
                assert all(0 <= i < len(s['examples']) for i in face)
        for state in player['motion']['states']:
            for f in state['fragments']:
                for layer in f['layers']:
                    if layer.get('missing'):
                        if 'clip' in layer:
                            assert layer['clip'] in missing and layer['clip'] not in clips
                        else:
                            assert any(e.get('missing') for e in spaces[layer['blend_space']]['examples'])
                        assert not (required_state(state, weapons) and f['fragment'].lower() in
                                    {'motionidle', 'motionmovement', 'select', 'deselect'}), (state, f)
                        continue
                    if 'clip' in layer:
                        assert layer['clip'] in clips, layer['clip']
                        assert layer['additive'] == clips[layer['clip']]['additive']
                    else:
                        assert layer['blend_space'] in spaces
        for weapon_id in [None] + [w['child_id'] for w in player['weapons']
                                    if {'stocked', 'rifle'} <= set(w['mannequin_tags'])]:
            for fp in (False, True):
                resolved = {f['fragment'].lower() for state in player['motion']['states']
                            if state['weapon_child_id'] == weapon_id and state['stance'] == 'stand'
                            and state['first_person'] == fp for f in state['fragments']}
                required_fragments = {'motionidle', 'motionmovement'}
                if weapon_id is not None:
                    required_fragments |= {'select', 'deselect'}
                assert required_fragments <= resolved, (weapon_id, fp, resolved)
        print(f"PASS {path}: {player['gender']}, {len(children)} attached items, "
              f"{len(player['weapons'])} weapons, {len(player['motion']['states'])} states, "
              f"{len(spaces)} blend spaces, {len(clips)} body clips; scene first")


def compare(before, after):
    """Check every payload; report the source-provenance exception explicitly."""
    def bundle(path):
        p = Path(path)
        if p.is_dir():
            files = {}
            for f in p.rglob('*'):
                assert not f.is_symlink(), f
                if f.is_file():
                    files[f.relative_to(p).as_posix()] = f.read_bytes()
            return files, None
        with tarfile.open(path, 'r:') as tar:
            members = tar.getmembers()
            assert all(m.isfile() and m.size <= 512 * 1024 * 1024 for m in members)
            assert len({m.name for m in members}) == len(members)
            files = {m.name: tar.extractfile(m).read() for m in members}
        raw = p.read_bytes()
        print(f"SHA256 {path}: {hashlib.sha256(raw).hexdigest()}")
        return files, raw
    a, raw_a = bundle(before)
    b, raw_b = bundle(after)
    assert a.keys() == b.keys(), 'package member paths changed'
    changed = [name for name in a if a[name] != b[name]]
    for name in changed:
        assert name.endswith('/.export_stamp.json'), f'payload changed: {name}'
        old, new = json.loads(a[name]), json.loads(b[name])
        previous = old['exporter']['source_sha256']
        current = new['exporter']['source_sha256']
        new['exporter']['source_sha256'] = previous
        assert old == new, f'provenance changed beyond source identity: {name}'
        print(f"PROVENANCE {name}: source_sha256 {previous} -> {current}")
    if raw_a is not None and raw_b is not None:
        # Compare the original bytes with only stamp payloads replaced in memory.
        # No tar or provenance file is written or rewritten.
        normalized = bytearray(raw_b)
        with tarfile.open(after, 'r:') as tar:
            for name in changed:
                m = tar.getmember(name)
                assert len(a[name]) == m.size
                normalized[m.offset_data:m.offset_data + m.size] = a[name]
        assert raw_a == normalized, 'tar framing/order/metadata changed'
        print('Archive bytes: ' + ('IDENTICAL' if raw_a == raw_b else
                                  'DIFFERENT; only exporter source provenance differs'))
    print(f"PAYLOAD PASS: {len(a) - len(changed)}/{len(a)} members byte-identical; "
          f"{len(changed)} source-provenance differences")


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('packages', nargs='*')
    parser.add_argument('--compare', nargs=2, metavar=('BEFORE', 'AFTER'))
    parser.add_argument('--require-entity', action='append', default=[])
    args = parser.parse_args()
    if not args.packages and not args.compare:
        parser.error('provide packages or --compare BEFORE AFTER')
    if args.compare:
        compare(*args.compare)
    for package in args.packages:
        verify(package, args.require_entity)
