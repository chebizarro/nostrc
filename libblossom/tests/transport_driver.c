/* Exercises public client/pool entrypoints against the HTTP fixture or live store. */
#include <blossom/client.h>
#include <blossom/pool.h>
#include <blossom/shim.h>
#include <nostr-event.h>
#include <nostr-keys.h>
#include <nostr/bud02/auth.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static size_t sign_count;

static blossom_error_t sign_json(const char *json, char **out, void *key) {
    sign_count++;
    *out = NULL;
    NostrEvent *ev = nostr_event_new();
    if (!ev) return BLOSSOM_ERR_NOMEM;
    if (nostr_event_deserialize_compact(ev, json, NULL) == 1 && nostr_event_sign(ev, key) == 0)
        *out = nostr_event_serialize_compact(ev);
    nostr_event_free(ev);
    return *out ? BLOSSOM_OK : BLOSSOM_ERR_AUTH;
}
static blossom_error_t wrong_hash_sign(const char *json, char **out, void *key) {
    (void)json; *out = NULL;
    NostrEvent *ev = nostr_bud02_create_auth_event(NOSTR_BUD02_ACTION_UPLOAD,
        "0000000000000000000000000000000000000000000000000000000000000000", 0, NULL);
    if (ev && nostr_event_sign(ev, key) == 0) *out = nostr_event_serialize_compact(ev);
    nostr_event_free(ev);
    return *out ? BLOSSOM_OK : BLOSSOM_ERR_AUTH;
}
static blossom_client_t *client(const char *url, const blossom_signer_t *signer, size_t cap) {
    blossom_client_opts_t opts = { .endpoint = url, .timeout_seconds = 10,
        .max_blob_bytes = cap, .allow_insecure = !strncmp(url, "http://", 7) };
    blossom_client_t *c = NULL;
    assert(blossom_client_new(&opts, signer, &c) == BLOSSOM_OK);
    return c;
}
static void roundtrip(blossom_client_t *c, size_t n) {
    uint8_t *data = malloc(n ? n : 1);
    assert(data && RAND_bytes(data, (int)n) == 1);
    char hash[65];
    assert(blossom_client_address(c, data, n, hash) == BLOSSOM_OK);
    blossom_blob_descriptor_t *d = NULL;
    blossom_error_t rc = blossom_client_put(c, data, n, hash, &d);
    printf("put bytes=%zu rc=%d hash=%s\n", n, rc, hash); fflush(stdout);
    assert(rc == BLOSSOM_OK && d && !strcmp(d->sha256, hash));
    uint8_t *wire = NULL, *content = NULL; size_t wn = 0, cn = 0;
    assert(blossom_client_get(c, hash, &wire, &wn) == BLOSSOM_OK);
    assert(wn == d->size);
    assert(blossom_client_get_content(c, hash, &content, &cn) == BLOSSOM_OK);
    assert(cn == n && (n == 0 || !memcmp(content, data, n)));
    bool exists = false;
    assert(blossom_client_head(c, hash, &exists) == BLOSSOM_OK && exists);
    assert(blossom_client_delete(c, hash) == BLOSSOM_OK);
    assert(blossom_client_head(c, hash, &exists) == BLOSSOM_OK && !exists);
    free(wire); free(content); free(data); blossom_blob_descriptor_free(d);
}
int main(int argc, char **argv) {
    assert(argc >= 3);
    char *key = nostr_key_generate_private(); assert(key);
    char *pubkey = nostr_key_get_public(key); assert(pubkey);
    blossom_signer_t signer = { .pubkey = pubkey, .sign = sign_json, .user_data = key };
    blossom_client_t *c = client(argv[2], &signer, 0);
    if (!strcmp(argv[1], "live")) {
        const size_t sizes[] = {1024, 1024*1024, 2*1024*1024, 4*1024*1024, 8*1024*1024};
        int failed = 0;
        for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
            size_t n = sizes[i];
            uint8_t *data = malloc(n); assert(data && RAND_bytes(data, (int)n) == 1);
            char hash[65];
            blossom_error_t e = blossom_client_address(c, data, n, hash);
            blossom_blob_t blob = {.sha256_hex = hash, .bytes = data, .len = n};
            blossom_batch_result_t result = {0};
            if (e == BLOSSOM_OK) e = blossom_client_put_batch(c, &blob, 1, &result);
            printf("live put plaintext_bytes=%zu http=%ld rc=%d\n", n, result.http_status, e); fflush(stdout);
            if (e == BLOSSOM_OK) {
                uint8_t *got = NULL; size_t gn = 0;
                e = blossom_client_get_content(c, hash, &got, &gn);
                bool same = e == BLOSSOM_OK && gn == n && !memcmp(got, data, n);
                blossom_error_t cleanup = blossom_client_delete(c, hash);
                printf("live get rc=%d same=%d delete_rc=%d\n", e, same, cleanup); fflush(stdout);
                if (!same || cleanup != BLOSSOM_OK) failed = 1;
                free(got);
            } else failed = 1;
            free(data);
            if (i + 1 < sizeof sizes / sizeof sizes[0]) {
                struct timespec delay = {.tv_sec = 10}; nanosleep(&delay, NULL);
            }
        }
        blossom_client_free(c); blossom_shim_cache_reset();
        OPENSSL_cleanse(key, strlen(key)); free(key); free(pubkey);
        return failed;
    } else if (!strcmp(argv[1], "roundtrip")) {
        const size_t sizes[] = {0, 1, 1024, 65536};
        for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) roundtrip(c, sizes[i]);
        const uint8_t a[] = "batch-a", b[] = "batch-b";
        blossom_blob_t blobs[] = {{NULL, a, sizeof a}, {NULL, b, sizeof b}};
        blossom_batch_result_t r[2];
        assert(blossom_client_put_batch(c, blobs, 2, r) == BLOSSOM_OK);
        char ha[65], hb[65];
        assert(blossom_client_address(c, a, sizeof a, ha) == BLOSSOM_OK);
        assert(blossom_client_address(c, b, sizeof b, hb) == BLOSSOM_OK);
        char origin[1024]; snprintf(origin, sizeof origin, "%s/%s", argv[2], ha);
        blossom_blob_descriptor_t *mirrored = NULL;
        assert(blossom_client_mirror(c, origin, &mirrored) == BLOSSOM_OK);
        assert(mirrored && !strcmp(mirrored->sha256, ha));
        blossom_blob_descriptor_free(mirrored);
        assert(blossom_client_delete(c, ha) == BLOSSOM_OK && blossom_client_delete(c, hb) == BLOSSOM_OK);
        char *list = NULL; size_t len = 0;
        assert(blossom_client_list(c, pubkey, &list, &len) == BLOSSOM_OK); free(list);
    } else if (!strcmp(argv[1], "pool")) {
        assert(argc == 4);
        const char *urls[] = { argv[2], argv[3] };
        /* Observe with explicit local-test transport before pool preparation. */
        assert(blossom_probe_capabilities(c) == BLOSSOM_OK);
        blossom_client_t *other = client(urls[1], &signer, 0);
        assert(blossom_probe_capabilities(other) == BLOSSOM_OK);
        blossom_pool_opts_t opts = { .servers = urls, .n_servers = 2, .allow_insecure = true };
        blossom_pool_t *pool = NULL;
        assert(blossom_pool_new(&opts, &signer, &pool) == 0);
        uint8_t a[] = "pool-a", b[] = "pool-b";
        blossom_pool_batch_blob_t blobs[] = {{.bytes=a,.len=sizeof a}, {.bytes=b,.len=sizeof b}};
        blossom_pool_batch_per_server_t stats[2];
        assert(blossom_pool_upload_batch(pool, blobs, 2, stats) == 0);
        assert(blobs[0].bytes == a && blobs[0].len == sizeof a);
        assert(blobs[1].bytes == b && blobs[1].len == sizeof b);
        assert(stats[0].chunks_uploaded == 2 && stats[1].chunks_uploaded == 2);
        for (size_t i = 0; i < 2; i++) {
            uint8_t *wire = NULL; size_t len = 0;
            assert(blossom_pool_fetch(pool, blobs[i].sha256_hex, &wire, &len) == 0);
            assert(blossom_shim_detect(wire, len)); free(wire);
            assert(blossom_pool_delete(pool, blobs[i].sha256_hex) == 0);
        }
        blossom_pool_free(pool); blossom_client_free(other);
    } else if (!strcmp(argv[1], "auth-get")) {
        assert(argc == 4);
        assert(blossom_probe_capabilities(c) == BLOSSOM_OK);
        assert(blossom_client_get_capabilities(c)->private_server == BLOSSOM_CAP_YES);
        const char expected[] = "authenticated GET ownership regression";
        for (size_t i = 0; i < 3; i++) {
            uint8_t *data = NULL; size_t n = 0;
            assert(blossom_client_get(c, argv[3], &data, &n) == BLOSSOM_OK);
            assert(n == sizeof expected - 1 && !memcmp(data, expected, n));
            free(data);
            assert(sign_count == i + 1);
        }
    } else if (!strcmp(argv[1], "private")) {
        assert(blossom_probe_capabilities(c) == BLOSSOM_OK);
        const blossom_server_capabilities_t *caps = blossom_client_get_capabilities(c);
        assert(caps->private_server == BLOSSOM_CAP_YES && caps->max_size == 2048);
        assert(caps->batch_ok == BLOSSOM_CAP_UNKNOWN && caps->raw_random_ok == BLOSSOM_CAP_UNKNOWN);
        uint8_t large[2049] = {0};
        assert(blossom_client_put(c, large, sizeof large, NULL, NULL) == BLOSSOM_ERR_TOO_LARGE);
        blossom_client_t *anon = client(argv[2], NULL, 0);
        char hash[65]; memset(hash, '0', 64); hash[64] = 0;
        bool exists; uint8_t *data = NULL; size_t n = 0;
        assert(blossom_client_head(anon, hash, &exists) == BLOSSOM_ERR_AUTH);
        assert(blossom_client_get(anon, hash, &data, &n) == BLOSSOM_ERR_AUTH);
        blossom_client_free(anon);
        roundtrip(c, 1024);
    } else if (!strcmp(argv[1], "bad-signer")) {
        assert(blossom_probe_capabilities(c) == BLOSSOM_OK);
        blossom_client_free(c);
        signer.sign = wrong_hash_sign;
        c = client(argv[2], &signer, 0);
        const uint8_t body[] = "must not upload under another hash";
        assert(blossom_client_put(c, body, sizeof body, NULL, NULL) == BLOSSOM_ERR_AUTH);
        blossom_blob_t blob = {.bytes=body, .len=sizeof body};
        blossom_batch_result_t result;
        assert(blossom_client_put_batch(c, &blob, 1, &result) == BLOSSOM_ERR_AUTH);
        assert(result.error == BLOSSOM_ERR_AUTH);
        blossom_server_capabilities_t observed = *blossom_client_get_capabilities(c);
        observed.batch_ok = BLOSSOM_CAP_NO;
        blossom_capability_cache_set(argv[2], &observed);
        assert(blossom_client_put_batch(c, &blob, 1, &result) == BLOSSOM_ERR_AUTH);
        assert(result.error == BLOSSOM_ERR_AUTH);
    } else if (!strcmp(argv[1], "guards")) {
        blossom_client_free(c); c = client(argv[2], NULL, 128);
        for (int i = 0; i < 4; i++) {
            char hash[65]; memset(hash, '0' + i, 64); hash[64] = 0;
            uint8_t *data = NULL; size_t n = 0;
            blossom_error_t rc = blossom_client_get(c, hash, &data, &n);
            assert(rc == (i == 0 || i == 2 ? BLOSSOM_ERR_TOO_LARGE :
                          i == 1 ? BLOSSOM_ERR_HASH_MISMATCH : BLOSSOM_ERR_NETWORK));
            assert(!data && !n);
        }
    } else abort();
    blossom_client_free(c); blossom_shim_cache_reset();
    OPENSSL_cleanse(key, strlen(key)); free(key); free(pubkey);
    puts("OK transport entrypoints"); return 0;
}
