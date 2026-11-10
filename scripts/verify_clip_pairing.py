#!/usr/bin/env python3
"""Check that every exported clip holds the keys of the CAF its chrparams event names.

Two checks per package.

Structure (needs no game files): every clip lists its contributing animations in `sources` and every
channel names one of them by a whole, listed index; no source is unused; a channel entry is one
channel or a flat list of variants; the channels naming a source end on that source's `frame_count`;
the clip's own `fps` and start pose are its first source's; and the clip's `frame_count` is its
longest source's duration in clip frames, rounded up.

Pairing (against extracted game files): for each rig and each event of the rig's `.chrparams`, the
event's path, resolved under the `#filepath` in effect where it is declared, names an entry of the
rig's `$TracksDatabase`; metadata entry i of a `.dba` owns key block i. The exported clip's bones
that belong to that rig must be exactly that block's controllers and end on that block's last key,
and must all name one source whose rig, rate, length and start pose are that block's. A clip
exported under a database entry's own path is one animation of the rig its source names and may
move bones of several skeletons: it is compared as a whole with that rig's entry (`path=block`).
The `.dba` and CryXmlB readers here are written independently of the exporter's.

Inputs are extracted game files (never commit them), e.g.

    target/debug/starbreaker p4k extract --filter '**/*.chrparams' -o <extract>
    target/debug/starbreaker p4k extract --filter '**/*.dba' -o <extract>
    python3 -I scripts/verify_clip_pairing.py --extract <extract> out/<package> [...]

Run with python3 -I: packages and extracts are game-derived data. Exit status 1 when a clip's
structure is broken (`structure wrong`; a package exported before clips listed their `sources`
fails this way), an event's clip is missing or holds another block's keys (`wrong`), or times or
anchors them by another animation (`timing wrong`), or when the evidence to judge a clip by is
missing (`missing evidence`: a rig without `.chrparams` in the extract, a tracks database the
`.chrparams` names that is absent or unreadable, or a clip of a rig with a database that no entry
of it was compared with).

What is compared is which animation a clip holds and how it is timed and anchored: the set of
controllers, each controller's rotation and position key counts and last key time, the rate, the
length and the start pose. Key VALUES and the times of the keys before the last are not compared
with the database.

What is not judged is reported, not passed: `loose-caf events` (the event's CAF is no database
entry; a loose `.caf` is not compared here) and `events without clip` are counted, and the
`unjudged` part of the summary counts the rigs whose `.chrparams` names no tracks database (their
animations are loose `.caf` files) and how many of the package's (rig, clip) pairs no database
entry was compared with.
"""
import argparse
from fractions import Fraction
import json
import math
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
    """{CAF path lower: (controller keys, last key time, fps, start pose)} or None when the file has no metadata
    table. Controller keys are {hash: sorted [(rotation keys, position keys, last key time)]}, one per controller
    of that hash; the start pose is (rotation wxyz, position) in the export's axes."""
    data = path.read_bytes()
    at = data.find(b"#dba")
    blocks = []
    while at >= 0 and data[at:at + 4] == b"#dba":
        bone_count, _, _ = struct.unpack_from("<HHI", data, at + 4)
        hashes_at = at + 12
        controllers_at = hashes_at + bone_count * 4
        hashes = struct.unpack_from("<%dI" % bone_count, data, hashes_at)
        keys = {}
        for i in range(bone_count):
            c = controllers_at + i * 24
            rot_n, rot_flags, rot_times, _, pos_n, pos_flags, pos_times, _ = struct.unpack_from("<HHIIHHII", data, c)
            end = 0
            for n, flags, times_at in ((rot_n, rot_flags, rot_times), (pos_n, pos_flags, pos_times)):
                if n:
                    times = key_times(data, c + times_at, n, flags) if times_at else list(range(n))
                    if times:
                        end = max(end, max(times))
            keys.setdefault(hashes[i], []).append((rot_n, pos_n, end))
        blocks.append((bone_count, {h: sorted(k) for h, k in keys.items()}))
        at = controllers_at + bone_count * 24
    count = len(blocks)
    # Metadata table: u32 count, then `count` 48-byte entries: u16 at +8 the animation's rate, u16 at +10 the
    # block's controller count, four floats at +24 the start rotation (xyzw) and two at +40 the start position's
    # x and y (there is no z on disk).
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
    for i, (_, keys) in enumerate(blocks):
        end = data.find(b"\0", cursor)
        entry = table + 4 + 48 * i
        (fps,) = struct.unpack_from("<H", data, entry + 8)
        qx, qy, qz, qw, px, py = struct.unpack_from("<6f", data, entry + 24)
        # The export's axes are the engine's (x, y, z) as (x, -z, y); a quaternion is written w first.
        pose = ((qw, qx, -qz, qy), (px, -0.0, py))
        entries[data[cursor:end].decode("latin1").lower().replace("\\", "/")] = (keys, clip_end(keys), fps, pose)
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


def rig_key(path):
    return "data/" + re.sub("^data/", "", (path or "").lower().replace("\\", "/"))


def whole_number(value):
    return isinstance(value, int) and not isinstance(value, bool)


def number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def last_key(track):
    """Time of a channel's last key in frames; an omitted time array means the sample index is the time."""
    ends = [0]
    for part in ("rotation", "position"):
        times, values = track.get(part + "_time"), track.get(part)
        if times:
            ends.append(max(times))
        elif values:
            ends.append(len(values) - 1)
    return max(ends)


def channel_keys(track):
    """(rotation keys, position keys, last key time) of one exported channel, as `dba_entries` has them."""
    return (len(track.get("rotation") or ()), len(track.get("position") or ()), last_key(track))


def clip_end(keys):
    return max(k[2] for of_hash in keys.values() for k in of_hash)


def structure(body):
    """Problems of one clip that need no game files to see; empty when the clip is well formed."""
    sources = body.get("sources")
    if not isinstance(sources, list) or not sources:
        return ["no source list"]
    problems = []
    for at, record in enumerate(sources):
        if not (isinstance(record, dict) and whole_number(record.get("fps")) and record["fps"] >= 1
                and whole_number(record.get("frame_count")) and record["frame_count"] >= 0):
            return ["source %d has no usable rate and length" % at]
    ends = {}
    for bone, entry in (body.get("bones") or {}).items():
        for track in entry if isinstance(entry, list) else [entry]:
            if not isinstance(track, dict):
                problems.append("%s: a variant list inside a variant list" % bone)
                continue
            at = track.get("source")
            if not whole_number(at) or not 0 <= at < len(sources):
                problems.append("%s: source %r is no listed animation" % (bone, at))
                continue
            for part in ("rotation", "position"):
                times, values = track.get(part + "_time"), track.get(part)
                if times is not None and (not isinstance(values, list) or len(times) != len(values)):
                    problems.append("%s: %d %s times for %d keys" % (bone, len(times), part, len(values or ())))
            ends[at] = max(ends.get(at, 0), last_key(track))
    for at, record in enumerate(sources):
        if at not in ends:
            problems.append("source %d is named by no channel" % at)
        elif math.ceil(ends[at]) != record["frame_count"]:
            problems.append("source %d is %d frames long, its channels end on %s" % (at, record["frame_count"], ends[at]))
    first = sources[0]
    for key in ("fps", "start_rotation", "start_position"):
        if body.get(key) != first.get(key):
            problems.append("clip %s %r is not its first source's %r" % (key, body.get(key), first.get(key)))
    if number(body.get("fps")) and body["fps"] > 0:
        longest = max(math.ceil(Fraction(r["frame_count"]) * Fraction(body["fps"]) / Fraction(r["fps"])) for r in sources)
        if body.get("frame_count") != longest:
            problems.append("clip is %r frames long, its longest source %d" % (body.get("frame_count"), longest))
    return problems


def same(a, b):
    return (isinstance(a, list) and len(a) == len(b)
            and all(number(x) and math.isclose(x, y, rel_tol=1e-6, abs_tol=1e-6) for x, y in zip(a, b)))


def differs(record, rig, fps, last, pose):
    """What of a source record is not the block's: empty when the record is that block's animation."""
    out = []
    if rig_key(record.get("skeleton_path")) != rig:
        out.append("rig %r" % record.get("skeleton_path"))
    if record.get("fps") != fps:
        out.append("rate %r, block %d" % (record.get("fps"), fps))
    if record.get("frame_count") != last:
        out.append("length %r, block %d" % (record.get("frame_count"), last))
    if not same(record.get("start_rotation"), pose[0]):
        out.append("start rotation %r, block %r" % (record.get("start_rotation"), pose[0]))
    if not same(record.get("start_position"), pose[1]):
        out.append("start position %r, block %r" % (record.get("start_position"), pose[1]))
    return out


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
        clips = {}  # (rig path, event name) -> [(clip name, {controller hash: [channel keys]}, source records named)]
        whole = {}  # clip name -> the same, over every rig the clip moves
        notes = []
        malformed = 0
        for clip in scene["root_entity"].get("animations") or []:
            body = json.loads((scenes[0].parent / clip["sidecar"]).read_bytes()) if clip.get("sidecar") else clip
            problems = structure(body)
            if problems:
                malformed += 1
                notes.append(("STRUCTURE", clip["name"], problems[:4]))
            per_rig, named = {}, {}
            sources = body.get("sources") if isinstance(body.get("sources"), list) else []
            for bone_hash, tracks in (body.get("bones") or {}).items():
                for track in tracks if isinstance(tracks, list) else [tracks]:
                    if not isinstance(track, dict):
                        continue
                    rig = rig_key(track.get("source_skeleton_path"))
                    per_rig.setdefault(rig, {}).setdefault(int(bone_hash, 16), []).append(channel_keys(track))
                    at = track.get("source")
                    record = sources[at] if whole_number(at) and 0 <= at < len(sources) and isinstance(sources[at], dict) else None
                    named.setdefault(rig, {})[at if record is not None else None] = record
            for rig, keyed in per_rig.items():
                clips.setdefault((rig, clip["name"].split("__")[-1]), []).append((clip["name"], keyed, named[rig]))
            every = {}
            for keyed in per_rig.values():
                for h, of_hash in keyed.items():
                    every.setdefault(h, []).extend(of_hash)
            whole[clip["name"]] = (every,
                                   {at: record for of_rig in named.values() for at, record in of_rig.items()}, sorted(per_rig))
        rigs = sorted({rig for rig, _ in clips})
        loose = absent = by_path = 0
        tally = {"right": 0, "wrong": 0, "mistimed": 0}
        judged = set()
        missing = {}  # rig -> why nothing of it can be compared
        databases = {}

        def database(rig):
            """(events, {CAF path: entry}) of a rig's chrparams, or None with the reason recorded."""
            if rig not in databases:
                databases[rig] = None
                chrparams = chr_files.get(str(Path(rig).with_suffix(".chrparams")))
                rows = cryxml_rows(chrparams.read_bytes()) if chrparams else None
                if rows is None:
                    missing[rig] = "no chrparams"
                else:
                    path, rig_events = events(rows)
                    dba_path = lookup(dba_files, path) if path else None
                    if dba_path and dba_path not in dba_cache:
                        dba_cache[dba_path] = dba_entries(dba_path)
                    if dba_path and dba_cache[dba_path]:
                        databases[rig] = (rig_events, dba_cache[dba_path])
                    else:
                        missing[rig] = "tracks database absent or unreadable" if path else "no tracks database"
            return databases[rig]

        def judge(label, exported, entry, owner):
            """right / mistimed / wrong for clips that should hold one database entry's keys, timed and anchored by it."""
            keys, last, fps, pose = entry
            held = [e for e in exported or [] if {h: sorted(k) for h, k in e[1].items()} == keys]
            faults = [(n, "names %d sources" % len(named)) if len(named) != 1 or None in named
                      else (n, differs(next(iter(named.values())), owner, fps, last, pose)) for n, _, named in held]
            if any(not fault for _, fault in faults):
                return "right"
            if held:
                notes.append(("TIMING", label, "block: %d fps, %d frames" % (fps, last), faults))
                return "mistimed"
            notes.append(("WRONG" if exported else "NO CLIP", label, "block: %d bones, last key %d" % (len(keys), last),
                          [(n, len(k), clip_end(k), "controllers differ" if set(k) != set(keys) else
                            "keys differ on %d controllers" % sum(sorted(k[h]) != keys[h] for h in k)) for n, k, _ in exported or []]))
            return "wrong"

        for rig in rigs:
            rig_events, entries = database(rig) or ((), {})
            for name, caf in rig_events:
                entry = entries.get(caf.lower().replace("\\", "/"))
                exported = clips.get((rig, name))
                if entry is None:
                    loose, absent = loose + bool(exported), absent + (not exported)
                    continue
                judged.add((rig, name))
                tally[judge((Path(rig).name, name), exported, entry, rig)] += 1
        # A database entry exported under its own path is one animation of the rig whose database lists it, and
        # may move bones of several skeletons: compared as a whole, against the entry of the rig it names.
        for name, (keyed, named, moved) in sorted(whole.items()):
            owners = {rig_key(record.get("skeleton_path")) for record in named.values() if record}
            entry = None
            if len(owners) == 1:
                (owner,) = owners
                entry = (database(owner) or ((), {}))[1].get(name.lower().replace("\\", "/"))
            if entry is not None:
                judged.update((rig, name) for rig in moved)
                verdict = judge((Path(owner).name, name), [(name, keyed, named)], entry, owner)
                by_path += verdict == "right"
                tally[verdict] += verdict != "right"
        right, wrong, mistimed = tally["right"], tally["wrong"], tally["mistimed"]
        notes += sorted((why, rig) for rig, why in missing.items())
        no_database = sum(why == "no tracks database" for why in missing.values())
        # Evidence that should exist and does not: a rig's files, or an entry for a clip of a rig that has a database.
        unjudged = sorted(set(clips) - judged)
        evidence = len(missing) - no_database + sum(missing.get(rig) != "no tracks database" for rig, _ in unjudged)
        print(package, "rigs", len(rigs), "event=block", right, "path=block", by_path, "wrong", wrong, "timing wrong", mistimed,
              "structure wrong", malformed, "loose-caf events", loose, "events without clip", absent,
              "missing evidence", evidence, "unjudged: rigs without database", no_database,
              "rig-clip pairs", len(unjudged), "of", len(clips))
        for rig, name in unjudged:
            notes.append(("unjudged" if missing.get(rig) == "no tracks database" else "MISSING EVIDENCE", Path(rig).name, name))
        for note in notes:
            print("   ", note)
        failed |= wrong > 0 or mistimed > 0 or malformed > 0 or evidence > 0
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
