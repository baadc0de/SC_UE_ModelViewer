#!/usr/bin/env python3
"""Check that every exported clip holds the keys of the CAF its chrparams event names.

For each rig of a decomposed package and each event of the rig's `.chrparams`: the event's path,
resolved under the `#filepath` in effect where it is declared, names an entry of the rig's
`$TracksDatabase`; metadata entry i of a `.dba` owns key block i. The exported clip's bones that
belong to that rig must be exactly that block's controllers and end on that block's last key.
The `.dba` and CryXmlB readers here are written independently of the exporter's.

Inputs are extracted game files (never commit them), e.g.

    target/debug/starbreaker p4k extract --filter '**/*.chrparams' -o <extract>
    target/debug/starbreaker p4k extract --filter '**/*.dba' -o <extract>
    python3 -I scripts/verify_clip_pairing.py --extract <extract> out/<package> [...]

Run with python3 -I: packages and extracts are game-derived data. Exit status 1 when an event's
clip is missing or holds another block's keys. Events whose CAF is no database entry are counted
(`loose-caf events` when the package has a clip for them, `events without clip` otherwise) and
not judged: a loose `.caf` is not compared here.
"""
import argparse
import json
from pathlib import Path
import re
import struct
import sys


def cryxml_rows(data):
    """(name, path) of every <Animation name= path=> element, in document order."""
    if data[:7] != b"CryXmlB":
        return None
    _, nodes_at, node_count, attrs_at, _, children_at, _, strings_at, _ = struct.unpack_from("<9I", data, 8)

    def text(offset):
        end = data.index(b"\0", strings_at + offset)
        return data[strings_at + offset:end].decode("utf-8", "replace")

    rows = []

    def walk(index):
        tag, _, attr_count, child_count, _, first_attr, first_child, _ = struct.unpack_from(
            "<IIHHiiii", data, nodes_at + index * 28)
        attrs = {}
        for k in range(attr_count):
            key, value = struct.unpack_from("<II", data, attrs_at + (first_attr + k) * 8)
            attrs[text(key)] = text(value)
        if text(tag).lower() == "animation" and "name" in attrs and "path" in attrs:
            rows.append((attrs["name"], attrs["path"]))
        for k in range(child_count):
            (child,) = struct.unpack_from("<I", data, children_at + (first_child + k) * 4)
            walk(child)

    if node_count:
        walk(0)
    return rows


def key_times(data, at, count, flags):
    kind = flags & 0x0F
    if kind == 0:
        return list(data[at:at + count])
    if kind == 1:
        return list(struct.unpack_from("<%dH" % count, data, at))
    if kind == 2:
        return list(struct.unpack_from("<HH", data, at))
    return None


def dba_entries(path):
    """{CAF path lower: (controller hashes, last key time)} or None when the file has no metadata table."""
    data = path.read_bytes()
    at = data.find(b"#dba")
    blocks = []
    while at >= 0 and data[at:at + 4] == b"#dba":
        bone_count, _, _ = struct.unpack_from("<HHI", data, at + 4)
        hashes_at = at + 12
        controllers_at = hashes_at + bone_count * 4
        hashes = struct.unpack_from("<%dI" % bone_count, data, hashes_at)
        last = 0
        for i in range(bone_count):
            c = controllers_at + i * 24
            rot_n, rot_flags, rot_times, _, pos_n, pos_flags, pos_times, _ = struct.unpack_from("<HHIIHHII", data, c)
            for n, flags, times_at in ((rot_n, rot_flags, rot_times), (pos_n, pos_flags, pos_times)):
                if n:
                    times = key_times(data, c + times_at, n, flags) if times_at else list(range(n))
                    if times:
                        last = max(last, max(times))
        blocks.append((bone_count, frozenset(hashes), last))
        at = controllers_at + bone_count * 24
    count = len(blocks)
    # Metadata table: u32 count, then `count` 48-byte entries whose u16 at +10 is the block's controller count.
    table = None
    probe = at
    while count:
        probe = data.find(struct.pack("<I", count), probe)
        if probe < 0 or probe + 4 + 48 * count > len(data):
            break
        if all(struct.unpack_from("<H", data, probe + 4 + 48 * i + 10)[0] == blocks[i][0] for i in range(count)):
            table = probe
            break
        probe += 1
    if table is None:
        return None
    cursor = table + (4 + 48 * count + 7) // 8 * 8
    while data[cursor] == 0:
        cursor += 1
    entries = {}
    for _, hashes, last in blocks:
        end = data.find(b"\0", cursor)
        entries[data[cursor:end].decode("latin1").lower().replace("\\", "/")] = (hashes, last)
        cursor = end + 1
    return entries


def events(rows):
    """(tracks database, [(event name, resolved CAF path)]) of one chrparams, `#filepath` as a running directive."""
    prefix = database = None
    out = []
    for name, path in rows:
        lower = name.lower()
        if lower == "#filepath":
            prefix = path
        elif lower == "$tracksdatabase":
            database = path
        elif not name.startswith(("$", "#")) and "*" not in name:
            directory = prefix.rstrip("/\\") if prefix else ""
            out.append((name, directory + "/" + path.lstrip("/\\") if directory else path))
    return database, out


def index(root, suffix):
    return {p.relative_to(root).as_posix().lower(): p for p in root.rglob("*") if p.suffix.lower() == suffix}


def lookup(files, path):
    key = path.lower().replace("\\", "/")
    return files.get(key) or files.get("data/" + key)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("packages", type=Path, nargs="+")
    parser.add_argument("--extract", type=Path, help="directory holding the extracted .chrparams and .dba files")
    parser.add_argument("--chrparams-root", type=Path)
    parser.add_argument("--dba-root", type=Path)
    args = parser.parse_args()
    chr_root, dba_root = args.chrparams_root or args.extract, args.dba_root or args.extract
    assert chr_root and dba_root, "pass --extract, or --chrparams-root and --dba-root"
    chr_files, dba_files, dba_cache = index(chr_root, ".chrparams"), index(dba_root, ".dba"), {}
    failed = False
    for package in args.packages:
        scenes = list(package.glob("Packages/*/scene.json"))
        assert len(scenes) == 1, (package, "expected one scene")
        scene = json.loads(scenes[0].read_bytes())
        clips = {}  # (rig path, event name) -> [(clip name, {controller hash: last key time})]
        for clip in scene["root_entity"].get("animations") or []:
            body = json.loads((scenes[0].parent / clip["sidecar"]).read_bytes()) if clip.get("sidecar") else clip
            per_rig = {}
            for bone_hash, tracks in (body.get("bones") or {}).items():
                for track in tracks if isinstance(tracks, list) else [tracks]:
                    rig = "data/" + re.sub("^data/", "", (track.get("source_skeleton_path") or "").lower().replace("\\", "/"))
                    last = max(list(track.get("rotation_time") or [0]) + list(track.get("position_time") or [0]))
                    per_rig.setdefault(rig, {})[int(bone_hash, 16)] = last
            for rig, keyed in per_rig.items():
                clips.setdefault((rig, clip["name"].split("__")[-1]), []).append((clip["name"], keyed))
        rigs = sorted({rig for rig, _ in clips})
        right = wrong = loose = absent = 0
        notes = []
        for rig in rigs:
            chrparams = chr_files.get(str(Path(rig).with_suffix(".chrparams")))
            rows = cryxml_rows(chrparams.read_bytes()) if chrparams else None
            if rows is None:
                notes.append(("no chrparams", rig))
                continue
            database, rig_events = events(rows)
            dba_path = lookup(dba_files, database) if database else None
            if dba_path and dba_path not in dba_cache:
                dba_cache[dba_path] = dba_entries(dba_path)
            entries = dba_cache.get(dba_path) if dba_path else None
            for name, caf in rig_events:
                exported = clips.get((rig, name))
                entry = entries.get(caf.lower().replace("\\", "/")) if entries else None
                if entry is None:
                    loose, absent = loose + bool(exported), absent + (not exported)
                    continue
                hashes, last = entry
                if exported and any(set(keyed) == hashes and max(keyed.values()) == last for _, keyed in exported):
                    right += 1
                else:
                    wrong += 1
                    notes.append(("WRONG" if exported else "NO CLIP", Path(rig).name, name, "block: %d bones, last key %d" % (len(hashes), last),
                                  [(n, len(k), max(k.values())) for n, k in exported or []]))
        print(package, "rigs", len(rigs), "event=block", right, "wrong", wrong, "loose-caf events", loose, "events without clip", absent)
        for note in notes:
            print("   ", note)
        failed |= wrong > 0
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
