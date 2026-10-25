/* SPDX-License-Identifier: MIT */
#include "vc.h"
#include <porthome/crypto.h>
#include <openssl/crypto.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static blossom_error_t sign_blob(const char *json, char **out, void *ctx)
{
    vc_app *a = ctx;
    *out = NULL;
    NostrEvent *e = nostr_event_new();
    if (!e)
        return BLOSSOM_ERR_NOMEM;
    int rc = nostr_event_deserialize(e, json);
    if (!rc && nostr_event_get_kind(e) != 24242)
        rc = 1;
    if (!rc)
        rc = nostr_event_sign(e, a->secret);
    if (!rc)
        *out = nostr_event_serialize(e);
    nostr_event_free(e);
    return rc || !*out ? BLOSSOM_ERR_AUTH : BLOSSOM_OK;
}
int vc_blobs_open(vc_app *a)
{
    a->signer = (blossom_signer_t){a->public_key, sign_blob, a};
    for (size_t i = 0; i < a->state.blossom_len; i++) {
        blossom_client_opts_t opts = {.endpoint = a->state.blossom[i],
                                      .timeout_seconds =
                                          (long)((a->options.deadline_ms + 999) / 1000),
                                      .max_blob_bytes = VC_MAX_STATE,
                                      .allow_insecure = a->options.allow_local};
        if (blossom_client_new(&opts, &a->signer, &a->clients[i]) != BLOSSOM_OK)
            return VAULT_ERR_IO;
        blossom_capability_state_t private_state;
        if (vault_vsp_probe_blossom(a->clients[i], &private_state) != BLOSSOM_OK)
            return VAULT_ERR_IO;
        fprintf(stderr, "Blossom[%zu] advertised-private=%d (advertisement, not access proof)\n", i,
                private_state);
    }
    return 0;
}
void vc_blobs_close(vc_app *a)
{
    for (size_t i = 0; i < VC_MAX_ENDPOINTS; i++)
        blossom_client_free(a->clients[i]);
    free(a->verified);
}
static int verified(vc_app *a, const uint8_t hash[32])
{
    for (size_t i = 0; i < a->verified_len; i++)
        if (!memcmp(a->verified[i], hash, 32))
            return 1;
    return 0;
}
static int remember(vc_app *a, const uint8_t hash[32])
{
    if (verified(a, hash))
        return 0;
    if (a->verified_len == a->verified_cap) {
        size_t n = a->verified_cap ? a->verified_cap * 2 : 32;
        void *p = realloc(a->verified, n * 32);
        if (!p)
            return VAULT_ERR_OOM;
        a->verified = p;
        a->verified_cap = n;
    }
    memcpy(a->verified[a->verified_len++], hash, 32);
    return 0;
}
int vc_blob_read(void *ctx, const uint8_t hash[32], uint8_t **b, size_t *n)
{
    vc_app *a = ctx;
    *b = NULL;
    *n = 0;
    if (a->local_put || verified(a, hash))
        return vc_cache_get(a, hash, b, n);
    char h[65];
    porthome_hex64(hash, h);
    if (blossom_client_get(a->clients[0], h, b, n) != BLOSSOM_OK) {
        vc_error(a, "Blossom GET failed for %s (status %ld)", h,
                 blossom_client_http_status(a->clients[0]));
        return VAULT_ERR_IO;
    }
    int rc = vc_cache_put(a, hash, *b, *n);
    if (!rc)
        rc = remember(a, hash);
    if (rc) {
        free(*b);
        *b = NULL;
        *n = 0;
    }
    return rc;
}
int vc_blob_put(void *ctx, const uint8_t *b, size_t n, const uint8_t hash[32])
{
    vc_app *a = ctx;
    int rc = vc_cache_put(a, hash, b, n);
    if (!rc && a->chunk_io) {
        a->state.needs_publication = 1;
    }
    if (rc || a->local_put)
        return rc;
    vc_state *s = &a->state;
    if (s->pending_blob)
        return VAULT_ERR_AGAIN;
    uint64_t mask = 0;
    rc = vc_ack_get(a, hash, &mask);
    if (rc)
        return rc;
    char h[65];
    porthome_hex64(hash, h);
    for (size_t i = 0; i < s->blossom_len; i++) {
        if (mask & (UINT64_C(1) << i))
            continue;
        char address[65];
        if (blossom_client_address(a->clients[i], b, n, address) != BLOSSOM_OK ||
            strcmp(address, h)) {
            vc_error(
                a,
                "server requires a different wire representation; refusing address substitution");
            return VAULT_ERR_IO;
        }
        s->pending_blob = 1;
        memcpy(s->blob_hash, hash, 32);
        s->blob_server = i;
        if ((rc = vc_state_save(a)))
            return rc;
        blossom_blob_descriptor_t *d = NULL;
        blossom_error_t br = blossom_client_put(a->clients[i], b, n, h, &d);
        if (br != BLOSSOM_OK || !d || strcmp(d->sha256, h) || d->size != n) {
            blossom_blob_descriptor_free(d);
            vc_error(a, "Blossom PUT uncertain; use --resume --reconcile (no automatic retry)");
            return VAULT_ERR_AGAIN;
        }
        blossom_blob_descriptor_free(d);
        mask |= UINT64_C(1) << i;
        if ((rc = vc_ack_put(a, hash, mask)))
            return rc;
        s->pending_blob = 0;
        if ((rc = vc_state_save(a)))
            return rc;
    }
    return 0;
}
int vc_reconcile_blob(vc_app *a)
{
    vc_state *s = &a->state;
    if (!s->pending_blob)
        return 0;
    if (!a->options.reconcile)
        return VAULT_ERR_AGAIN;
    uint8_t *expected = NULL, *actual = NULL;
    size_t n = 0, z = 0;
    int rc = vc_cache_get(a, s->blob_hash, &expected, &n);
    if (rc)
        return rc;
    char h[65];
    porthome_hex64(s->blob_hash, h);
    if (blossom_client_get(a->clients[s->blob_server], h, &actual, &z) != BLOSSOM_OK || n != z ||
        CRYPTO_memcmp(expected, actual, n))
        rc = VAULT_ERR_AGAIN;
    free(expected);
    free(actual);
    if (rc) {
        vc_error(a, "cannot reconcile uncertain PUT: expected bytes not observed; no retry");
        return rc;
    }
    uint64_t mask;
    if ((rc = vc_ack_get(a, s->blob_hash, &mask)))
        return rc;
    if ((rc = vc_ack_put(a, s->blob_hash, mask | (UINT64_C(1) << s->blob_server))))
        return rc;
    s->pending_blob = 0;
    return vc_state_save(a);
}
