/* Live C consumer: outputs owned tar and catalogues; shell verifies/extracts them. */
#define _POSIX_C_SOURCE 200809L
#include "scbridge.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static sc_handle *h;
static atomic_uint callbacks;
static double now(void) {
    struct timespec t;
    assert(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return t.tv_sec + t.tv_nsec / 1e9;
}
static void check(sc_status s) {
    if (s != SC_STATUS_OK) {
        fprintf(stderr, "status=%d: %s\n", s, sc_last_error(h)); exit(1);
    }
}
static void progress(const char *phase, uint64_t done, uint64_t total, void *user) {
    assert(phase && done <= total && user == &callbacks);
    atomic_fetch_add(&callbacks, 1);
}
static void write_file(const char *root, const char *name, const void *data, size_t len) {
    char path[4096];
    assert(snprintf(path, sizeof(path), "%s/%s", root, name) < (int)sizeof(path));
    FILE *f = fopen(path, "wb"); assert(f);
    assert(fwrite(data, 1, len, f) == len); assert(fclose(f) == 0);
}
int main(int argc, char **argv) {
    assert(argc == 4); /* output directory, cache directory or '-', smoke/bench/panic */
    double start = now();
    check(sc_open(NULL, strcmp(argv[2], "-") == 0 ? NULL : argv[2], &h));
    if (strcmp(argv[3], "evict") == 0) {
        uint64_t removed; sc_cache_info info;
        check(sc_cache_evict(h, 0, &removed)); check(sc_cache_stats(h, &info));
        assert(info.bytes == 0 && info.entries == 0);
        printf("cache eviction: %llu bytes removed\n", (unsigned long long)removed);
        sc_close(h); return 0;
    }
    if (strcmp(argv[3], "panic") == 0) {
#ifdef SCBRIDGE_TEST_HOOKS
        assert(sc_test_panic(h) == SC_STATUS_PANIC);
        assert(strstr(sc_last_error(h), "forced WP-R4 panic"));
        printf("C panic: status=3 message=%s; process survived\n", sc_last_error(h));
        char *out = NULL;
        assert(sc_search_entities(h, "gladius", 1, &out) == SC_STATUS_PANIC);
        assert(!out); sc_close(h); return 0;
#else
        fprintf(stderr, "panic hook not compiled\n"); return 1;
#endif
    }
    char *json = NULL;
    check(sc_search_entities(h, "gladius", 20, &json));
    assert(strstr(json, "AEGS_Gladius")); sc_free_string(json);
    sc_options o; check(sc_default_options(&o));
    sc_blob tar = {0};
    double export_start = now();
    check(sc_export_decomposed_mem(h, "AEGS_Gladius", &o, progress, &callbacks, &tar));
    double export_time = now() - export_start;
    assert(tar.data && tar.len > 1024 && atomic_load(&callbacks) > 0);
    write_file(argv[1], "ffi.tar", tar.data, tar.len);
    printf("FFI export=%.6f s total=%.6f s bytes=%zu callbacks=%u\n",
           export_time, now() - start, tar.len, atomic_load(&callbacks));
    sc_free_blob(&tar); assert(!tar.data && !tar.len); sc_free_blob(&tar);
    if (strcmp(argv[3], "bench") == 0) { sc_close(h); return 0; }
    check(sc_clip_catalogue(h, "AEGS_Gladius", &json));
    write_file(argv[1], "clips.json", json, strlen(json)); sc_free_string(json);
    check(sc_lights(h, "AEGS_Gladius", &json));
    write_file(argv[1], "lights.json", json, strlen(json)); sc_free_string(json);
    json = NULL;
    assert(sc_clip_catalogue(h, "not_an_entity", &json) == SC_STATUS_ERROR);
    assert(!json && strlen(sc_last_error(h)));
    const char invalid_utf8[] = {(char)0xff, 0};
    assert(sc_search_entities(h, invalid_utf8, 1, &json) == SC_STATUS_INVALID_ARGUMENT);
    assert(!json);
    o.animations = 0;
    assert(sc_export_decomposed_mem(h, "AEGS_Gladius", &o, NULL, NULL, &tar) == SC_STATUS_UNSUPPORTED);
    assert(!tar.data && !tar.len);
    sc_cache_info info; check(sc_cache_stats(h, &info));
    printf("cache enabled=%u bytes=%llu entries=%llu last_hit=%u\n", info.enabled,
           (unsigned long long)info.bytes, (unsigned long long)info.entries, info.last_hit);
    uint64_t removed; check(sc_cache_evict(h, UINT64_MAX, &removed)); assert(removed == 0);
    sc_close(h); h = NULL;
    check(sc_open(NULL, strcmp(argv[2], "-") == 0 ? NULL : argv[2], &h));
    check(sc_export_decomposed_mem(h, "AEGS_Gladius", NULL, NULL, NULL, &tar));
    check(sc_cache_stats(h, &info)); assert(!info.enabled || info.last_hit);
    sc_free_blob(&tar);
    puts("cache reopen hit: PASS");
    sc_close(h); sc_close(NULL); sc_free_string(NULL); sc_free_blob(NULL);
    puts("C ABI smoke: PASS"); return 0;
}
