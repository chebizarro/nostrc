/* Compatibility adapter; all HTTP, guard and representation policy is libblossom. */
#include "nh_porthome_blossom.h"
#include <stdlib.h>
#include <string.h>
int nh_porthome_blossom_url_ok(const char *s) { return blossom_pool_url_ok(s); }
int nh_porthome_blossom_new(const nh_porthome_blossom_opts_t *o,
    const blossom_signer_t *s, nh_porthome_blossom_t **out) {
    if (!o) return BLOSSOM_POOL_ERR_ARG;
    blossom_pool_opts_t opts = *o;
    /* Legacy local integration harness transport opt-in, not shim policy. */
    const char *test = getenv("NH_PORTHOME_ALLOW_INSECURE");
    opts.allow_insecure = test && !strcmp(test, "1");
    return blossom_pool_new(&opts, s, out);
}
void nh_porthome_blossom_free(nh_porthome_blossom_t *c) { blossom_pool_free(c); }
int nh_porthome_blossom_upload(nh_porthome_blossom_t *c, const uint8_t *b,
    size_t n, const char *h, char out[65]) { return blossom_pool_upload(c, b, n, h, out); }
int nh_porthome_blossom_fetch(nh_porthome_blossom_t *c, const char *h,
    uint8_t **b, size_t *n) { return blossom_pool_fetch(c, h, b, n); }
int nh_porthome_blossom_has(nh_porthome_blossom_t *c, const char *h, bool *yes) {
    return blossom_pool_has(c, h, yes);
}
int nh_porthome_blossom_client_delete(nh_porthome_blossom_t *c, const char *h) {
    return blossom_pool_delete(c, h);
}
int nh_porthome_blossom_upload_batch(nh_porthome_blossom_t *c,
    nh_porthome_blossom_batch_blob_t *b, size_t n,
    nh_porthome_blossom_batch_per_server_t *out) { return blossom_pool_upload_batch(c, b, n, out); }
