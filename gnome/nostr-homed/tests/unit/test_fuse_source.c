/*
 * test_fuse_source.c — nh_fuse_source tier ladder.
 *
 * Covers:
 *   - tier 2 hit: sealed blob preloaded into nh_syncd_cache decrypts +
 *     serves plaintext through _pread + _chunk;
 *   - tier 1 caching: second read of same chunk does NOT re-read the
 *     cache file (verified by stats.hits_chunk_lru);
 *   - EIO on missing chunk when no Blossom client wired;
 *   - cleanse-on-close (mount closes without crashing while LRU has
 *     live plaintext).
 *
 * We deliberately do NOT test Blossom fetch here — that lives in the
 * integration path with fake_porthome_blossom.py. Tier 3 with a NULL
 * blossom client is exactly the "offline" scenario.
 */

#define _GNU_SOURCE
#include "nh_fuse_source.h"
#include "nh_porthome_crypto.h"
#include "nh_syncd_cache.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static void hex_of(const uint8_t *b, size_t n, char *out) {
    static const char lc[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i*2] = lc[(b[i] >> 4) & 0xf];
        out[i*2 + 1] = lc[b[i] & 0xf];
    }
    out[n*2] = '\0';
}

int main(void) {
    char tmpl[] = "/tmp/nhfuse_src_XXXXXX";
    assert(mkdtemp(tmpl) != NULL);
    char cdir[300]; snprintf(cdir, sizeof cdir, "%s/cache", tmpl);
    nh_syncd_cache *cache = NULL;
    assert(nh_syncd_cache_open(cdir, 0, &cache) == 0);

    uint8_t seed[32];
    for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(i * 7 + 3);
    uint8_t hkey[32];
    assert(nh_porthome_key_derive(seed, hkey) == 0);

    /* One 6-byte plaintext chunk. */
    const char *pt = "hello!";
    uint8_t sha[32]; uint8_t *ct = NULL; size_t ct_len = 0;
    assert(nh_porthome_encrypt_chunk(hkey, (const uint8_t *)pt, strlen(pt),
                                     &ct, &ct_len, sha) == 0);
    char addr[65]; hex_of(sha, 32, addr);
    assert(nh_syncd_cache_put(cache, addr, ct, ct_len) == 0);
    free(ct);

    /* Source with no blossom (tier 3 will EIO). */
    nh_fuse_source_cfg cfg = {
        .home_dir = NULL,
        .tier0_enabled = false,
        .cache = cache,
        .blossom = NULL,
        .chunk_cache_bytes = 0,
        .offline_budget_ms = 0,
        .on_miss = NULL,
        .on_miss_ud = NULL,
    };
    memcpy(cfg.home_key, hkey, 32);
    nh_fuse_source *src = NULL;
    assert(nh_fuse_source_open(&cfg, &src) == 0);

    /* First read: tier 2 hit → decrypt → tier 1 populate. */
    const uint8_t *out_pt = NULL; size_t out_len = 0;
    int rc = nh_fuse_source_chunk(src, addr, &out_pt, &out_len);
    assert(rc == 0);
    assert(out_len == strlen(pt));
    assert(memcmp(out_pt, pt, out_len) == 0);

    nh_fuse_source_stats_t st = {0};
    nh_fuse_source_stats(src, &st);
    assert(st.hits_cache == 1);
    assert(st.hits_chunk_lru == 0);

    /* Second read: tier 1 (LRU) hit. */
    rc = nh_fuse_source_chunk(src, addr, &out_pt, &out_len);
    assert(rc == 0);
    assert(memcmp(out_pt, pt, out_len) == 0);
    nh_fuse_source_stats(src, &st);
    assert(st.hits_chunk_lru == 1);

    /* pread flow: assemble a 1-chunk file entry. */
    const char *chunks[] = { addr };
    char rbuf[64];
    ssize_t got = nh_fuse_source_pread(src, "notes.txt", "", strlen(pt),
                                       chunks, 1, 4u * 1024u * 1024u,
                                       rbuf, sizeof rbuf, 0);
    assert(got == (ssize_t)strlen(pt));
    assert(memcmp(rbuf, pt, (size_t)got) == 0);

    /* Missing chunk → EIO because no Blossom client. */
    const char *missing_addr = "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
    rc = nh_fuse_source_chunk(src, missing_addr, &out_pt, &out_len);
    assert(rc == -EIO);

    nh_fuse_source_close(src);
    nh_syncd_cache_close(cache);
    printf("test_fuse_source OK\n");
    return 0;
}
