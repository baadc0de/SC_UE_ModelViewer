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

Status values: OK=0, INVALID_ARGUMENT=1, ERROR=2, PANIC=3, UNSUPPORTED=4. Boolean
options are uint32 values restricted to 0/1, and material modes are integer
constants so malformed C enum input cannot create an invalid Rust enum.

The core currently always emits animation sidecars; `animations=0` returns
UNSUPPORTED. `collision_reserved` must be zero. Statistics use `Session::cache_stats()`
under the core cache lock: missing cache/manifest reports zero, malformed manifests
recover using the existing core policy, and the latest Hit/Miss is unchanged.
Eviction is a one-shot cap, not a persistent configuration change.

The C integration test is an explicit script step, separate from `cargo test`.
It never invokes Cargo. Build the library and discovery probe with the release
script, and build the CLI before running it:

```sh
scripts/build_ffi.sh
CARGO_TARGET_DIR=./target cargo build --release -p scviewer-cli
crates/scbridge-ffi/ctest/run.sh
```

The probe uses the same `find_p4k()` as the core tests; no archive means a clean
skip. The script compiles the C caller, extracts its tar, runs `scviewer validate`,
compares with the CLI using the same cache and checks structural invariants.
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

Independent uncached exports contain different upstream
`Data/UI/Generated/.export_stamp.json` timestamps. This wrapper does not rewrite
or reorder tar bytes: exact parity is verified using the shared core package
cache. The remaining 501 files were byte-identical in the independent comparison.
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
