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
`sc_open(NULL, "./cache/", &handle)`, and call exports from the host's worker.
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
UNSUPPORTED. `collision_reserved` must be zero. Cache statistics expose an atomic
read-only manifest snapshot plus the core's latest Hit/Miss and automatic write
cap. Eviction is delegated to the core and is a one-shot cap, not a persistent
configuration change. Statistics report malformed/missing manifests as errors;
subsequent core cache operations own their recovery. Cache recovery and export
algorithms remain in the core.

The C integration shell has no build.rs. `cargo test` invokes it, builds the
cdylib and CLI sequentially, compiles the C caller with warnings as errors,
exports Gladius, extracts with `tar`, runs `scviewer validate`, compares tar bytes
with the CLI using the same cache, checks the ustar contract and live fixture
counts, and tests cache reopening and eviction. It requires the installed LIVE
4.10.2 game data. Artifacts remain under `out/`; temporary caches under `cache/`
are removed. To test already-built release artifacts:

```sh
SCBRIDGE_CTEST_LIB="$PWD/target/release/libscbridge.so" \
SCBRIDGE_CTEST_CLI="$PWD/target/release/scviewer" \
  crates/scbridge-ffi/ctest/run.sh
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
