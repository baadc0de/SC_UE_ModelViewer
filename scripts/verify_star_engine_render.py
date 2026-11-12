#!/usr/bin/env python3
"""Re-derive the Star Engine render constants in StarEngineRender.json from the installed game.

The fixture (ue/Plugins/SCBridge/Resources/StarEngineRender.json) holds values the Viewer cannot read at
run time: renderer cvar defaults compiled into StarCitizen.exe and constants / functions of the tokenised
shader sources in ShaderCache_Vulkan.pak. This script extracts them again and fails on any difference.

  python3 -I scripts/verify_star_engine_render.py [--exe PATH] [--p4k PATH] [--shaders DIR|PAK] [--dump]

By default both inputs are the installed game's own files: StarCitizen.exe, and ShaderCache_Vulkan.pak read
in place from Data.p4k (it is a stored member, so no extraction can go stale). --shaders checks an extracted
copy instead; the verdict then says so, because such a copy is not proven to be the installed build's.
Run with -I: the executable and the shader cache are untrusted input. Standard library plus binutils
objdump. --dump prints the extracted values as JSON instead of comparing (used to author the fixture).
Provenance and method: ue/Plugins/SCBridge/Resources/StarEngineRender.notes.md.
"""
import argparse
import hashlib
import io
import json
import mmap
import os
import re
import struct
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FIXTURE = ROOT / "ue/Plugins/SCBridge/Resources/StarEngineRender.json"
LIVE = Path(os.environ.get("HOME", "")) / "Games/star-citizen/drive_c/Program Files/Roberts Space Industries/StarCitizen/LIVE"
PAK = "Engine\\ShaderCache_Vulkan.pak"  # member of Data.p4k holding the tokenised shader sources
# Open-ended cvar families: the fixture must list every member the executable registers (the loader builds
# the exposure-compensation curve from whatever anchors the fixture carries).
FAMILIES = ("r_ExposureCompensationEV",)

# --- FXB1 tokenised shader source (cfib / cfxb) -------------------------------------------------------------
# Header: u32 at 12 = offset of the name table; u32 tokens from 28 up to it; the table is (u32 hash, cstring)*.
# Tokens below 4096 are built-ins; only the ones needed to read constants and function bodies are named here.
PUNCT = dict(zip(range(25, 46), "( ) [ ] { } < > , . : ; ! \" ' ? = + - / *".split()))
PRE = {1: "#include", 2: "#define", 3: "#define", 4: "#undef", 6: "#if", 7: "#ifdef", 8: "#ifndef", 9: "#if",
       10: "#ifdef", 11: "#ifndef", 12: "#elif", 13: "#endif", 14: "#else", 15: "||", 16: "&&"}
KEYWORDS = {65: "int", 69: "uint", 64: "bool", 74: "struct", 76: "inout", 77: "const", 85: "return"}
for _i, _n in enumerate("float float2 float3 float4 float4x4 float3x4 float2x4 float3x3 half half2 half3 half4".split()):
    KEYWORDS[46 + _i] = _n
for _i, _n in enumerate("x y z w r g b a".split()):
    KEYWORDS[424 + _i] = _n
for _i in range(25):
    KEYWORDS[432 + _i] = str(_i)


def shader_tokens(blob):
    assert blob[:4] == b"FXB1", "not an FXB1 shader source"
    table = struct.unpack_from("<I", blob, 12)[0]
    names, at = {}, table
    while at < len(blob):
        end = blob.index(b"\0", at + 4)
        names[struct.unpack_from("<I", blob, at)[0]] = blob[at + 4:end].decode("latin1")
        at = end + 1
    out = []
    for tok in struct.unpack_from(f"<{(table - 28) // 4}I", blob, 28):
        out.append(names.get(tok) or PUNCT.get(tok) or PRE.get(tok) or KEYWORDS.get(tok) or f"<k{tok}>")
    return out


def shader_text(tokens):
    """Space-joined tokens with split numeric literals rejoined: `1000 . 0f` -> `1000.0f`."""
    return re.sub(r"(?<![\w.])(\d+) \. (\d+f?)(?![\w.])", r"\1.\2", " ".join(tokens))


def function_tokens(tokens, name):
    """Tokens of every definition `name ( ... ) { ... }` (signature and body), overloads in file order."""
    found = []
    for at, tok in enumerate(tokens):
        if tok != name or tokens[at + 1] != "(":
            continue
        close = at + 1
        depth = 0
        for close in range(at + 1, len(tokens)):
            depth += (tokens[close] == "(") - (tokens[close] == ")")
            if depth == 0:
                break
        if tokens[close + 1] != "{":
            continue  # a call or a prototype
        depth = 0
        for end in range(close + 1, len(tokens)):
            depth += (tokens[end] == "{") - (tokens[end] == "}")
            if depth == 0:
                found += tokens[at:end + 1]
                break
    assert found, f"shader function {name} not found"
    return found


# --- a stored member of Data.p4k, read in place --------------------------------------------------------------
# Data.p4k is a ZIP64 archive. The end-of-central-directory locator in the file tail gives the ZIP64 record,
# that gives the central directory, and the member's entry gives its local header; a stored (method 0),
# unencrypted member is then a byte window of the archive, which zipfile can open as the nested zip it is.
class Window(io.RawIOBase):
    def __init__(self, file, start, size):
        self.file, self.start, self.size, self.pos = file, start, size, 0

    def seekable(self):
        return True

    def readable(self):
        return True

    def tell(self):
        return self.pos

    def seek(self, offset, whence=0):
        self.pos = (offset, self.pos + offset, self.size + offset)[whence]
        return self.pos

    def readinto(self, buffer):
        count = max(0, min(len(buffer), self.size - self.pos))
        self.file.seek(self.start + self.pos)
        got = self.file.readinto(memoryview(buffer)[:count])
        self.pos += got
        return got


def stored_member(p4k, name):
    """(file object, data offset, size, crc32) of the stored member `name` of the ZIP64 archive `p4k`."""
    file = open(p4k, "rb")
    view = mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ)
    locator = view.rfind(b"PK\x06\x07", max(0, len(view) - (1 << 16)))
    assert locator >= 0, f"{p4k}: no ZIP64 end-of-central-directory locator"
    record = struct.unpack_from("<Q", view, locator + 8)[0]
    assert view[record:record + 4] == b"PK\x06\x06", f"{p4k}: bad ZIP64 end-of-central-directory record"
    size, start = struct.unpack_from("<QQ", view, record + 40)
    raw, at = name.encode(), start
    while True:
        at = view.find(raw, at, start + size)
        assert at >= 0, f"{p4k}: no member {name}"
        entry = at - 46
        if view[entry:entry + 4] == b"PK\x01\x02" and struct.unpack_from("<H", view, entry + 28)[0] == len(raw):
            break
        at += 1
    flags, method = struct.unpack_from("<HH", view, entry + 8)
    crc, packed, unpacked = struct.unpack_from("<III", view, entry + 16)
    extra_size = struct.unpack_from("<H", view, entry + 30)[0]
    header = struct.unpack_from("<I", view, entry + 42)[0]
    extra, end = entry + 46 + len(raw), entry + 46 + len(raw) + extra_size
    while extra + 4 <= end:  # ZIP64 extra (id 1): the 64-bit values of the fields stored as 0xFFFFFFFF, in this order
        ident, length = struct.unpack_from("<HH", view, extra)
        if ident == 1:
            wide = iter(struct.unpack_from(f"<{length // 8}Q", view, extra + 4))
            unpacked = next(wide) if unpacked == 0xFFFFFFFF else unpacked
            packed = next(wide) if packed == 0xFFFFFFFF else packed
            header = next(wide) if header == 0xFFFFFFFF else header
        extra += 4 + length
    assert method == 0 and not flags & 1 and packed == unpacked, f"{name}: not a stored plain member (method {method}, flags {flags:#x})"
    assert view[header:header + 4] == b"PK\x03\x04", f"{name}: bad local header"
    name_size, local_extra = struct.unpack_from("<HH", view, header + 26)
    return file, header + 30 + name_size + local_extra, unpacked, crc


class Shaders:
    """Tokenised shader sources: a directory of .cfib/.cfxb files, a ShaderCache pak, or (p4k=) the pak inside Data.p4k."""

    def __init__(self, path=None, p4k=None):
        self.zip, self.cache, self.installed = None, {}, p4k is not None
        if p4k is not None:
            file, start, size, crc = stored_member(p4k, PAK)
            self.zip = zipfile.ZipFile(io.BufferedReader(Window(file, start, size)))
            self.source = f"{PAK} ({size} bytes, crc32 {crc:08x}) read in place from {p4k}"
        else:
            self.path = Path(path)
            if self.path.is_file():
                self.zip = zipfile.ZipFile(self.path)
            self.source = f"{self.path} (an extracted copy, NOT read from Data.p4k)"
        if self.zip:
            self.members = {Path(n).name.lower(): n for n in self.zip.namelist()}

    def tokens(self, name):
        if name not in self.cache:
            if self.zip:
                blob = self.zip.read(self.members[name.lower()])
            else:
                blob = (self.path / name).read_bytes()
            self.cache[name] = shader_tokens(blob)
        return self.cache[name]

    def constant(self, entry):
        found = re.findall(entry["pattern"], shader_text(self.tokens(entry["file"])))
        assert len(set(found)) == 1, f"{entry['file']}: /{entry['pattern']}/ matched {found}"
        return float(found[0].rstrip("f").replace(" ", ""))  # `- 14`: the sign is its own token

    def texture(self, entry):
        """A texture's default path in its declaration; the decoder spaces every token (`Engine / Textures / a . dds`)."""
        found = re.findall(entry["pattern"], shader_text(self.tokens(entry["file"])))
        assert len(set(found)) == 1, f"{entry['file']}: /{entry['pattern']}/ matched {found}"
        return found[0].replace(" ", "")

    def function_hash(self, entry, name):
        return hashlib.sha256(" ".join(function_tokens(self.tokens(entry["file"]), name)).encode()).hexdigest()


# --- cvar defaults in the executable ------------------------------------------------------------------------
# A cvar registration is `mov rax,[console]; lea rdx,[name]; <default>; ...; call [rax+slot]` with the default
# in r9d (integer, slot 0x40) or xmm3 (float, slot 0x48). The name string is located in the image, its
# RIP-relative `lea rdx` references are found by scanning the executable sections, and objdump disassembles
# the enclosing function (bounds from the PE exception directory) up to the call. Registers are tracked
# because the compiler shares one constant between registrations (`vmovss xmm6,[c]` ... `vmovaps xmm3,xmm6`).
WIDE = {"r" + n: "e" + n for n in ("ax", "bx", "cx", "dx", "si", "di", "bp")} | {f"r{i}": f"r{i}d" for i in range(8, 16)}
VOLATILE = {"eax", "ecx", "edx", "r8d", "r9d", "r10d", "r11d", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5"}


class Exe:
    def __init__(self, path):
        self.path = str(path)
        self.file = open(path, "rb")
        self.map = mmap.mmap(self.file.fileno(), 0, access=mmap.ACCESS_READ)
        pe = struct.unpack_from("<I", self.map, 0x3C)[0]
        count = struct.unpack_from("<H", self.map, pe + 6)[0]
        assert struct.unpack_from("<H", self.map, pe + 24)[0] == 0x20B, "not a PE32+ image"
        self.base = struct.unpack_from("<Q", self.map, pe + 48)[0]
        at = pe + 24 + struct.unpack_from("<H", self.map, pe + 20)[0]
        self.sections = []
        for _ in range(count):
            _vsize, rva, rsize, raw = struct.unpack_from("<IIII", self.map, at + 8)
            self.sections.append((rva, raw, rsize, struct.unpack_from("<I", self.map, at + 36)[0] & 0x20000000))
            at += 40
        self.pdata = struct.unpack_from("<II", self.map, pe + 24 + 112 + 3 * 8)  # exception directory (rva, size)

    def function_start(self, va):
        """Start of the function containing `va`: binary search of the sorted RUNTIME_FUNCTION table."""
        table, rva = self.offset(self.base + self.pdata[0]), va - self.base
        lo, hi = 0, self.pdata[1] // 12
        while lo < hi:
            mid = (lo + hi) // 2
            begin, end = struct.unpack_from("<II", self.map, table + mid * 12)
            if rva < begin:
                hi = mid
            elif rva >= end:
                lo = mid + 1
            else:
                return self.base + begin
        raise AssertionError(f"no function contains {va:#x}")

    def offset(self, va):
        for rva, raw, rsize, _ in self.sections:
            if rva <= va - self.base < rva + rsize:
                return raw + va - self.base - rva
        raise AssertionError(f"address {va:#x} outside the image")

    def name_addresses(self, name):
        out, needle, at = [], b"\0" + name.encode() + b"\0", 0
        while (at := self.map.find(needle, at)) >= 0:
            for rva, raw, rsize, _ in self.sections:
                if raw <= at + 1 < raw + rsize:
                    out.append(self.base + rva + at + 1 - raw)
            at += 1
        return out

    def references(self, wanted):
        """{string address: [address of each `lea rdx,[rip+d]` that loads it]}."""
        refs = {va: [] for va in wanted}
        for rva, raw, rsize, executable in self.sections:
            if not executable:
                continue
            data, at = self.map[raw:raw + rsize], 0
            while (at := data.find(b"\x48\x8d\x15", at)) >= 0:
                if at + 7 <= len(data):
                    target = self.base + rva + at + 7 + struct.unpack_from("<i", data, at + 3)[0]
                    if target in refs:
                        refs[target].append(self.base + rva + at)
                at += 1
        return refs

    def registration(self, ref):
        """(kind, default) of the registration whose name is loaded at `ref`; None when the reference is
        another use of the name (a lookup: the next call is not a console registration slot)."""
        asm = subprocess.run(["objdump", "-d", "-M", "intel", "--no-show-raw-insn", f"--start-address={self.function_start(ref):#x}",
                              f"--stop-address={ref + 0x80:#x}", self.path], check=True, capture_output=True, text=True).stdout
        regs, slots, named = {}, {}, False
        for line in asm.splitlines():
            m = re.match(r"\s*([0-9a-f]+):\s+(\S+)\s*(.*)", line)
            if not m:
                continue
            addr, op, args = int(m.group(1), 16), m.group(2), m.group(3)
            comment = re.search(r"# 0x([0-9a-f]+)", args)
            ops = [o.strip() for o in args.split("#")[0].split(",")]
            named = named or addr == ref
            slot = re.fullmatch(r"QWORD PTR \[rax\+(0x4[08])\]", ops[-1])
            if op == "call":
                target = slot.group(1) if slot else slots.get(ops[0])
                if named and not target:
                    return None
                if named:
                    kind, reg = ("int", "r9d") if target == "0x40" else ("float", "xmm3")
                    assert reg in regs, f"untracked {reg} at the registration call {addr:#x}"
                    return kind, regs[reg]
                slots = {}
                for reg in VOLATILE:
                    regs.pop(reg, None)
                continue
            dest = WIDE.get(ops[0], ops[0])
            slots.pop(ops[0], None)
            if op == "mov" and slot:
                slots[ops[0]] = slot.group(1)  # `mov r10,[rax+slot]` ... `call r10`
                regs.pop(dest, None)
            elif op in ("vmovss", "movss") and dest.startswith("xmm") and comment and "[rip" in args:
                regs[dest] = struct.unpack_from("<f", self.map, self.offset(int(comment.group(1), 16)))[0]
            elif op in ("vmovaps", "movaps", "vmovss", "movss") and dest.startswith("xmm") and ops[-1] in regs:
                regs[dest] = regs[ops[-1]]
            elif op in ("vxorps", "xorps", "xor") and len(set(ops)) == 1:
                regs[dest] = 0.0 if dest.startswith("xmm") else 0
            elif op == "mov" and ops[0] in WIDE.values() and re.fullmatch(r"0x[0-9a-f]+", ops[1]):
                regs[dest] = int(ops[1], 16)
            elif op == "mov" and ops[0] in WIDE.values() and ops[1] in regs:
                regs[dest] = regs[ops[1]]
            elif op not in ("cmp", "test", "push") and not ops[0].startswith(("QWORD", "DWORD", "WORD", "BYTE", "XMMWORD")):
                regs.pop(dest, None)  # any other write to a tracked register: value unknown
        return None

    def family(self, prefix):
        """Every cvar name `<prefix><digits>` in the image."""
        return sorted({m.group(1).decode() for m in re.finditer(rb"\0(" + re.escape(prefix.encode()) + rb"\d+)\0", self.map)})

    def cvars(self, names):
        addresses = {name: self.name_addresses(name) for name in names}
        refs = self.references({va for vas in addresses.values() for va in vas})
        out = {}
        for name, vas in addresses.items():
            found = {self.registration(ref) for va in vas for ref in refs[va]} - {None}
            assert len(found) == 1, f"{name}: expected one registration, found {sorted(found)}"
            kind, value = found.pop()
            out[name] = {"kind": kind, "value": value if kind == "int" else round(value, 6)}
        return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--fixture", type=Path, default=FIXTURE)
    ap.add_argument("--exe", type=Path, default=LIVE / "Bin64/StarCitizen.exe")
    ap.add_argument("--p4k", type=Path, default=LIVE / "Data.p4k", help="archive whose ShaderCache_Vulkan.pak is read in place")
    ap.add_argument("--shaders", type=Path, help="check an extracted copy instead: directory of .cfib/.cfxb files or a ShaderCache pak")
    ap.add_argument("--dump", action="store_true")
    args = ap.parse_args()
    fixture = json.loads(args.fixture.read_text())
    assert fixture["schema"] == "star_engine_render_v1", fixture["schema"]
    shaders = Shaders(args.shaders) if args.shaders else Shaders(p4k=args.p4k)
    exe = Exe(args.exe)
    families = {prefix: exe.family(prefix) for prefix in FAMILIES}
    names = list(fixture["exe_cvars"]) + [n for members in families.values() for n in members if n not in fixture["exe_cvars"]]
    live = {
        "exe_cvars": exe.cvars(names),
        "shader_constants": {k: shaders.constant(e) for k, e in fixture["shader_constants"].items()},
        "shader_functions": {k: shaders.function_hash(e, k) for k, e in fixture["shader_functions"].items()},
        "shader_textures": {k: shaders.texture(e) for k, e in fixture.get("shader_textures", {}).items()},
    }
    if args.dump:
        print(json.dumps(live, indent=2))
        return 0
    print(f"SOURCE exe: {args.exe}")
    print(f"SOURCE shaders: {shaders.source}")
    bad = []
    for prefix, members in families.items():
        listed = sorted(n for n in fixture["exe_cvars"] if re.fullmatch(re.escape(prefix) + r"\d+", n))
        if listed != members:
            bad.append(f"exe_cvar_families.{prefix}: fixture lists {listed}, the executable registers {members}")
    for name, entry in fixture["exe_cvars"].items():
        got = live["exe_cvars"][name]
        if got["kind"] != entry["kind"] or abs(got["value"] - entry["value"]) > 1e-6 * max(1.0, abs(entry["value"])):
            bad.append(f"exe_cvars.{name}: fixture {entry['kind']} {entry['value']}, game {got['kind']} {got['value']}")
    for name, entry in fixture["shader_constants"].items():
        if live["shader_constants"][name] != entry["value"]:
            bad.append(f"shader_constants.{name}: fixture {entry['value']}, game {live['shader_constants'][name]}")
    for name, entry in fixture.get("shader_textures", {}).items():
        if live["shader_textures"][name] != entry["value"]:
            bad.append(f"shader_textures.{name}: fixture {entry['value']}, game {live['shader_textures'][name]}")
    for name, entry in fixture["shader_functions"].items():
        if live["shader_functions"][name] != entry["sha256"]:
            bad.append(f"shader_functions.{name}: token hash changed ({live['shader_functions'][name]})")
    for line in bad:
        print("MISMATCH", line)
    checked = len(families) + sum(len(fixture.get(k, {})) for k in ("exe_cvars", "shader_constants", "shader_functions", "shader_textures"))
    against = "the installed game" if shaders.installed else "the executable and an extracted shader copy (see SOURCE; not proven to be the installed build)"
    print(f"STAR_ENGINE_RENDER {'FAILED' if bad else 'OK'}: {checked - len(bad)}/{checked} checks match {against}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
