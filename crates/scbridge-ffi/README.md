# SCBridge C ABI

Linux `cdylib` over `scviewer-core`, with a cbindgen-generated C/C++ header.
Build from the workspace root (one Cargo build at a time):

```sh
scripts/build_ffi.sh
scripts/build_ffi.sh --check-header
CARGO_TARGET_DIR=./target cargo test -p scbridge-ffi -- --nocapture
```

The build script uses the workspace release profile (thin LTO, stripping,
unwinding panics) and stages the library and header under the Unreal external
module `Source/ThirdParty/SCBridgeLib`. Only the header, module rules and ignore
rule belong in git; the staged `.so` is ignored. Add `SCBridgeLib` to the consuming
Runtime module's dependencies. No editor dependency is introduced here.

`include/scbridge.h` is the complete ownership, threading and option contract.
Initialize options with `sc_default_options`, open with
`sc_open(NULL, absolute_cache_path, &handle)`, and call exports from the host's worker.
Resolve that absolute path from the launch directory before UE changes cwd.
An explicit p4k path must also be UTF-8.
NULL cache disables persistence. Entity search returns a JSON array; clip and
light queries return the core catalogue objects unchanged. Exact entity names
are required for catalogues and exports. `sc_blob` owns an uncompressed tar.

Serialize all access to each handle, including error retrieval and close. A
handle may move between threads. Progress may run on worker/rayon threads and
must not reenter the handle; callbacks and their borrowed context finish before
export returns. Free owned JSON and blobs using the matching functions. Read
`sc_last_error(NULL)` for open failures on the same thread. A panic returns
`SC_STATUS_PANIC` and poisons the handle; close it and reopen.
The host serialization lock must block close, error retrieval and search while
export is busy, until it returns or is cancelled; the library does not supply
this host lock. Do not call back into the busy handle.

ABI v2 changes `sc_progress_cb` from void to int: zero continues, nonzero cancels.
A cancelled export returns `SC_STATUS_CANCELLED` (5), an empty blob and no new
package cache entry; the handle remains usable. Checked exporter phase boundaries
observe cancellation, as do core checkpoints before tar build/cache publication.
Long individual decoding operations are not interrupted. No callback occurs
past the final publication checkpoint; cancellation after that commit point is
ignored while cache publication/folder unpack completes. Rebuild callback
consumers against the regenerated header.

Status values: OK=0, INVALID_ARGUMENT=1, ERROR=2, PANIC=3, UNSUPPORTED=4, CANCELLED=5. Boolean
options are uint32 values restricted to 0/1, and material modes are integer
constants so malformed C enum input cannot create an invalid Rust enum.

`animations=0` disables clip output and animation sidecars, matching CLI
`--no-animations`. Cache format v10 includes the option in the package key and invalidates timestamped packages. `collision_reserved` must be zero. Statistics use `Session::cache_stats()`
under the core cache lock: missing cache/manifest reports zero, malformed manifests
recover using the existing core policy, and the latest Hit/Miss is unchanged.
Eviction is a one-shot cap, not a persistent configuration change.

The C integration test is an explicit script step, separate from `cargo test`.
It never invokes Cargo. The release script builds the library, CLI and discovery
probe together so they share the same exporter/cache identity:

```sh
scripts/build_ffi.sh
crates/scbridge-ffi/ctest/run.sh
```

The probe uses the same `find_p4k()` as the core tests; no archive means a clean
skip. The script compiles the C caller, extracts its tar, runs `scviewer validate`,
compares independent uncached FFI/CLI exports and checks structural invariants.
A separate cache test requires CLI defaults to hit the entry written by FFI defaults.
Every step has an explicit status check, even if Bash errexit is suppressed by a
conditional invocation. A final PASS/FAIL marker survives output filtering; callers
that pipe the script must still enable pipefail in their own shell.
It also cancels on callback two, verifies the same handle still works and no
package entry was added, then compares/validates animation-disabled C/CLI tars.
Gladius counts are printed rather than pinned to one game build. Both the work
folder under `out/` and temporary cache are removed on success or failure;
`SCBRIDGE_CTEST_KEEP=1` retains only the work folder. Prebuilt artifacts can be
selected with `SCBRIDGE_CTEST_LIB`, `SCBRIDGE_CTEST_CLI` and
`SCBRIDGE_CTEST_DISCOVER`. The default paths are `target/release/libscbridge.so`,
`target/release/scviewer` and `target/debug/examples/discover`.

Synthetic verifier regressions need no game files:

```sh
python3 -I crates/scbridge-ffi/ctest/test_verify.py
```

Independent uncached exports are byte-identical, including generated UI
provenance. Upstream schema-v2 `.export_stamp.json` derives from archive build id,
exporter source identity/version, entity, options and generated file hashes. The
C test also loads the extracted package with Blender's actual `PackageBundle`
reader. No byte-comparison exception or stamp rewrite is used.
The benchmark runs independent processes with caching disabled and compares
median wall times over five alternating pairs after a warmup pair:

```sh
CARGO_TARGET_DIR=./target cargo build --release -p scviewer-cli
uv run python crates/scbridge-ffi/ctest/benchmark.py
```

A feature-gated `sc_test_panic` hook is available only with `test-hooks` (or Rust
unit tests); it is absent from the deployment `.so`. Compile C with
`-DSCBRIDGE_TEST_HOOKS` and run the smoke program's `panic` mode against that test
library. The normal `cargo test` also forces the same boundary panic. Expected
panic-hook text goes to stderr; execution survives and returns status 3.
