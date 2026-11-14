// The scbridge C API for platforms without the Rust library (SC_WITH_FFI=0, e.g. Wasm): every call reports
// SC_STATUS_UNSUPPORTED and honours the header's output contract (outputs NULL / zero on failure, frees are no-ops),
// so the callers (FSCBridgeClient, the texture importer) compile unchanged and fail cleanly at run time. Those builds
// show baked content instead of reading Data.p4k.
#include "CoreMinimal.h"
#include "scbridge.h"

#if !SC_WITH_FFI

namespace
{
const char* const NoFFIError = "scbridge is not available in this build (no Data.p4k access; baked content only)";
}

extern "C"
{
enum sc_status sc_open(const char*, const char*, struct sc_handle** out)
{
    if (out) *out = nullptr;
    return SC_STATUS_UNSUPPORTED;
}
enum sc_status sc_default_options(struct sc_options* out)
{
    if (out) FMemory::Memzero(*out);
    return SC_STATUS_UNSUPPORTED;
}
enum sc_status sc_search_entities(struct sc_handle*, const char*, uint32_t, char** out)
{
    if (out) *out = nullptr;
    return SC_STATUS_UNSUPPORTED;
}
enum sc_status sc_clip_catalogue(struct sc_handle*, const char*, char** out)
{
    if (out) *out = nullptr;
    return SC_STATUS_UNSUPPORTED;
}
enum sc_status sc_lights(struct sc_handle*, const char*, char** out)
{
    if (out) *out = nullptr;
    return SC_STATUS_UNSUPPORTED;
}
enum sc_status sc_cache_stats(struct sc_handle*, struct sc_cache_info* out)
{
    if (out) FMemory::Memzero(*out);
    return SC_STATUS_UNSUPPORTED;
}
enum sc_status sc_cache_evict(struct sc_handle*, uint64_t, uint64_t* removed)
{
    if (removed) *removed = 0;
    return SC_STATUS_UNSUPPORTED;
}
enum sc_status sc_export_decomposed_mem(struct sc_handle*, const char*, const struct sc_options*, sc_progress_cb, void*, struct sc_blob* out_tar)
{
    if (out_tar) *out_tar = sc_blob{nullptr, 0};
    return SC_STATUS_UNSUPPORTED;
}
enum sc_status sc_search_environments(struct sc_handle*, const char*, uint32_t, char** out)
{
    if (out) *out = nullptr;
    return SC_STATUS_UNSUPPORTED;
}
enum sc_status sc_export_environment_mem(struct sc_handle*, const char*, const struct sc_options*, sc_progress_cb, void*, struct sc_blob* out_tar)
{
    if (out_tar) *out_tar = sc_blob{nullptr, 0};
    return SC_STATUS_UNSUPPORTED;
}
const char* sc_last_error(struct sc_handle*)
{
    return NoFFIError;
}
void sc_free_string(char*) {}
void sc_free_blob(struct sc_blob* blob)
{
    if (blob) *blob = sc_blob{nullptr, 0};
}
void sc_close(struct sc_handle*) {}
enum sc_status sc_dds_prepare(uint8_t*, size_t, bool, struct sc_dds_info* out)
{
    if (out) FMemory::Memzero(*out);
    return SC_STATUS_UNSUPPORTED;
}
enum sc_status sc_dds_mean_rgb(const uint8_t*, size_t, float* out_rgb)
{
    if (out_rgb) out_rgb[0] = out_rgb[1] = out_rgb[2] = 0.f;
    return SC_STATUS_UNSUPPORTED;
}
enum sc_status sc_texels_decode_bgra8(const uint8_t*, size_t, uint32_t, uint32_t, uint32_t, uint8_t*, size_t)
{
    return SC_STATUS_UNSUPPORTED;
}
enum sc_status scbridge_export_player(struct sc_handle*, const char*, const char*, sc_progress_cb, void*, struct sc_blob* out_tar)
{
    if (out_tar) *out_tar = sc_blob{nullptr, 0};
    return SC_STATUS_UNSUPPORTED;
}
}

#endif // !SC_WITH_FFI
