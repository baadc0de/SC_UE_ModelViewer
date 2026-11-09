"""Alternate release C/CLI processes with persistence off; compare median wall times."""
from pathlib import Path
import statistics
import subprocess
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
assert (out / "ffi.tar").read_bytes() == (out / "cli.tar").read_bytes(), "independent tar byte mismatch"
print("Independent uncached tar byte comparison: PASS", flush=True)
assert ratio <= 1.10, "FFI exceeds 10% wall time overhead budget"
print("Timing budget: PASS", flush=True)
