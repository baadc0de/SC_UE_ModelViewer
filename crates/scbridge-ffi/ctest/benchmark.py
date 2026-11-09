"""Alternate release C/CLI processes with persistence off; compare median wall times."""
from pathlib import Path
import json
import statistics
import subprocess
import tarfile
import time

root = Path(__file__).resolve().parents[3]
out = root / "out/wp-r4-timing"
out.mkdir(parents=True, exist_ok=True)
lib = root / "target/release/libscbridge.so"
exe = out / "smoke"
subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I",
                str(root / "crates/scbridge-ffi/include"),
                str(root / "crates/scbridge-ffi/ctest/smoke.c"), str(lib),
                f"-Wl,-rpath,{lib.parent}", "-o", str(exe)], check=True)
commands = {
    "FFI": [str(exe), str(out), "-", "bench"],
    "CLI": [str(root / "target/release/scviewer"), "--no-cache", "decomposed-tar",
            "AEGS_Gladius", str(out / "cli.tar")],
}
results = {key: [] for key in commands}
print("Persistence disabled; five alternating pairs; OS file cache warm; full process wall time", flush=True)
for key, command in commands.items():
    print(key, command, flush=True)
for trial in range(6):
    for key in (["CLI", "FFI"] if trial % 2 == 0 else ["FFI", "CLI"]):
        start = time.perf_counter()
        with (out / f"{trial}-{key}.log").open("wb") as log:
            subprocess.run(commands[key], cwd=root, stdout=log, stderr=subprocess.STDOUT, check=True)
        elapsed = time.perf_counter() - start
        print(f"{'warmup' if trial == 0 else trial} {key} {elapsed:.6f} s", flush=True)
        if trial:
            results[key].append(elapsed)
ffi, cli = (statistics.median(results[key]) for key in ["FFI", "CLI"])
ratio = ffi / cli
print(f"median FFI={ffi:.6f} s CLI={cli:.6f} s ratio={ratio:.4f} overhead={(ratio - 1) * 100:.2f}%", flush=True)
# An independent export legitimately has different upstream provenance timestamps.
# Keep all 501 other files byte-exact and permit only those two metadata values.
with tarfile.open(out / "ffi.tar") as a, tarfile.open(out / "cli.tar") as b:
    assert a.getnames() == b.getnames()
    differing = []
    for name in a.getnames():
        left, right = a.extractfile(name).read(), b.extractfile(name).read()
        if left != right:
            differing.append(name)
            assert name == "Data/UI/Generated/.export_stamp.json", name
            left, right = json.loads(left), json.loads(right)
            for key in ["binary_built_at_epoch_s", "written_at_epoch_s"]:
                assert key in left and key in right
                del left[key], right[key]
            assert left == right
    print(f"Independent export differences (timestamp metadata only): {differing}", flush=True)
assert ratio <= 1.10, "FFI exceeds 10% wall time overhead budget"
print("Timing budget: PASS (exact tar parity is checked separately with a shared cache)", flush=True)
