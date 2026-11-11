# SCBridge C ABI

Linux `cdylib` over `scviewer-core`, with a cbindgen-generated C/C++ header.
Build from the workspace root (one Cargo build at a time):

```sh
scripts/build_ffi.sh
scripts/build_ffi.sh --check-header
CARGO_TARGET_DIR=./target cargo test -p scbridge-ffi -- --nocapture
```

The build script defaults to the workspace debug profile for development.
`--release` or `SC_FFI_PROFILE=release` selects the shipping profile (thin LTO,
stripping, unwinding panics). It stages the library and header under the Unreal external
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
It never invokes Cargo. The build script builds the library, CLI and discovery
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
`SCBRIDGE_CTEST_DISCOVER`. The default paths are `target/debug/libscbridge.so`,
`target/debug/scviewer` and `target/debug/examples/discover`. Set
`SC_FFI_PROFILE=release` for all three release artifacts (or override each path).

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

### Authored light fields (WP-R5b)

`sc_lights` still returns `{entity, ports, lights}`; each light now also carries
`bulb_radius_m`, optional `plane_width_m` / `plane_height_m`, `shadow_casting`,
optional `max_shadow_cast_distance_m`, `affects_this_area_only`, `affect_gi`,
`attenuation_tweak`, optional `plane_softness`, optional `fade_min_distance_m` /
`fade_max_distance_m`, optional signed `importance`, and `glow_multiplier`.
Distances are metres; unknown optional values are null and authored zeroes are
preserved. Sparse components inherit every missing attribute (including nested
states) from the entity class's DataCore `LightComponentParams` at export time.
Explicit zeroes and false values remain overrides. Only if neither source has
the attribute do bulb radius and attenuation use CryEngine's 0.05 m and 1.0
respectively. Unresolved radius, glow multiplier, flags, state temperature and
intensity (and their proxy aliases) are null. Unknown `affect_gi` remains null
with `affect_gi_mode` preserved and one warning per export. Missing importance
enum metadata also warns once per export, retaining the name and null ordinal.

Resolved light type, colour, style, state temperature-use flag and preset tag
also remain null if absent from both the component and class record. Consumers
must distinguish unknown values from explicit zero/false values.

An authored `defaultState` is always active, including intensity zero. Only
when absent does the first positive auxiliary/emergency/cinematic state win.
`intensity_unit` stays `cryengine_authored_intensity`; the flat record and each
state add `intensity_candela_proxy_model: "raw_x1500"`. Existing `intensity`,
`intensity_cd` and `intensity_candela_proxy` compatibility values retain their
maths (including ambient glow multiplier ×0.25); they are not calibrated cd.
No C ABI signature changes. The same fields appear in decomposed scene v2 and
bundled GLB light extras. Layer-1 `CACHE_FORMAT_VERSION` is 12 (previously 11);
UE `SCPackageReader` continues accepting scene v1/v2.

Since format 14 the package's texture PNGs state their colour encoding (`sRGB`
chunk or `gAMA` 1.0) and `palettes.json` `finish.*.glossiness` is a 0..1
fraction; no C ABI change.

WP-R5b enum detail: LIVE `importance` is often a name. `importance_name` preserves
it and `importance` exports the DataCore property enum's zero-based option index,
resolved from the current database rather than a copied table. `affect_gi_mode`
preserves `On` / `Off` / `InheritAffectsObjects`; `affect_gi` resolves the latter
from the component's `affectsObjects` flag. Numeric importance and boolean GI
spellings remain supported. Missing importance is null; standalone SOC parsing
without DataCore keeps the name with a null index until interior assembly.

WP-P2a adds `scbridge_export_player(handle, gender_or_null, loadout_or_null,
progress, user, out_tar)`. NULL arguments choose male and the first armed default
loadout in name order. It returns the same owned, scene-first tar as
`scviewer-cli player export`; free it with `sc_free_blob`. Invalid genders return
INVALID_ARGUMENT with an empty blob. Cancellation checkpoints bracket assembly,
tar creation and cache publication. Existing entry points and structs are unchanged.

`scripts/build_ffi.sh` explicitly defaults to the debug profile for development
(as required by AGENTS.md), and stages `target/debug/libscbridge.so`. For shipping,
use `scripts/build_ffi.sh --release` or `SC_FFI_PROFILE=release scripts/build_ffi.sh`;
this builds and stages `target/release/libscbridge.so`. `--check-header` checks the
C ABI header without staging a library. `SC_FFI_PROFILE` accepts only `debug` or
`release`; an explicit `--release` overrides it.


`sc_dds_prepare(data, len, normal_map, out)` and `sc_dds_mean_rgb(data, len, out_rgb)`
are handle-free DDS helpers returning `sc_status`; failures use
`sc_last_error(NULL)` on the calling thread. Preparation validates before mutation,
rebiases BC4/BC5 SNORM endpoints after clamping -128 to -127, and optionally negates
BC5 green (normal-map Y). The DX10 format and `sc_dds_info` reflect the prepared
UNORM payload. Call preparation once per imported texture. Mean RGB covers the
top mip in linear light, linearizing sRGB before averaging. Output storage is
zeroed on failure and must not overlap input bytes.
