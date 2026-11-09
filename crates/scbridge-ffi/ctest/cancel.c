/* Cancel inside the exporter's first phase, on the second overall callback. */
#include "scbridge.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static atomic_uint calls;
static int cancel_second(const char *phase, uint64_t done, uint64_t total, void *user) {
    assert(user == &calls && phase && done <= total);
    unsigned call = atomic_fetch_add(&calls, 1) + 1;
    if (call == 2) printf("request cancellation on callback 2: %s\n", phase);
    return call >= 2;
}
int main(int argc, char **argv) {
    assert(argc == 3);
    sc_handle *h = NULL;
    assert(sc_open(argv[1], argv[2], &h) == SC_STATUS_OK);
    sc_cache_info before, after;
    assert(sc_cache_stats(h, &before) == SC_STATUS_OK);
    sc_blob blob = {0};
    sc_status status = sc_export_decomposed_mem(h, "AEGS_Gladius", NULL,
                                               cancel_second, &calls, &blob);
    assert(status == SC_STATUS_CANCELLED && atomic_load(&calls) == 2);
    assert(!blob.data && !blob.len);
    assert(strstr(sc_last_error(h), "cancelled"));
    assert(sc_cache_stats(h, &after) == SC_STATUS_OK);
    assert(before.bytes == after.bytes && before.entries == after.entries);
    char *json = NULL;
    assert(sc_search_entities(h, "gladius", 1, &json) == SC_STATUS_OK);
    assert(json && strstr(json, "AEGS_Gladius"));
    sc_free_string(json); sc_free_blob(&blob); sc_close(h);
    puts("C cancellation: CANCELLED (5), exactly 2 callbacks, no package cache entry; handle/process fine");
    return 0;
}
