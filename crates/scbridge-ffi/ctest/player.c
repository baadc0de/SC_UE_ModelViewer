#include "scbridge.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static int32_t cancel(const char *phase, uint64_t done, uint64_t total, void *user) {
    (void)phase; (void)done; (void)total; (void)user; return 1;
}
int main(int argc, char **argv) {
    assert(argc == 3);
    struct sc_handle *h = NULL;
    assert(sc_open(NULL, argv[1], &h) == SC_STATUS_OK);
    struct sc_blob blob = {0};
    assert(scbridge_export_player(h, NULL, NULL, cancel, NULL, &blob) == SC_STATUS_CANCELLED);
    assert(blob.data == NULL && blob.len == 0);
    enum sc_status status = scbridge_export_player(h, NULL, NULL, NULL, NULL, &blob);
    if(status != SC_STATUS_OK) { fprintf(stderr,"%s\n",sc_last_error(h)); return 1; }
    FILE *f=fopen(argv[2],"wb"); assert(f);
    assert(fwrite(blob.data,1,blob.len,f)==blob.len); assert(fclose(f)==0);
    printf("PASS player FFI: cancellation cleared output; recovered handle exported %zu bytes\n",blob.len);
    sc_free_blob(&blob); assert(!blob.data && !blob.len); sc_close(h);
}
