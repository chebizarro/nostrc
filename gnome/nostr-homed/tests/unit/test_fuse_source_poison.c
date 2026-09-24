/*
 * test_fuse_source_poison.c — regression for bead nostrc-zw8v.
 *
 * Design: docs/designs/nostrfs-porthome-overlay.md §3.1 / §3.3.
 *
 * Mirrors live-smoke case (d) from
 * docs/reviews/porthome-fuse-live-2026-09-24.md but tighter: seeds a
 * wrong-bytes-for-hash entry in the local blob cache, wires a stub
 * Blossom (via nh_fuse_source_cfg.blossom_fetch) that serves the
 * correct sealed bytes, then invokes a read through the FUSE source
 * layer. Asserts:
 *   (i)   the read succeeds with correct plaintext,
 *   (ii)  the poisoned cache entry is gone or replaced with correct
 *         bytes,
 *   (iii) an audit log line was emitted to stderr.
 *
 * Corrupt-cache-never-serves-wrong-plaintext is proven by the fact
 * that decrypt rejects the flipped bytes (rc != 0); this test proves
 * the additional invariant that the mount self-heals to tier 3
 * instead of stranding the read.
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
        out[i*2]     = lc[(b[i] >> 4) & 0xf];
        out[i*2 + 1] = lc[b[i] & 0xf];
    }
    out[n*2] = '\0';
}

/* Stub Blossom that serves a pre-loaded (address → bytes) mapping.
 * Real Blossom fetches are content-verified upstream; per the seam's
 * contract, the stub returns bytes whose sha256 matches sha256_hex. */
typedef struct {
    char     addr[65];
    uint8_t *bytes;
    size_t   len;
    size_t   call_count;
} stub_blossom;

static int stub_blossom_fetch(void *ud, const char *sha256_hex,
                              uint8_t **out_data, size_t *out_len) {
    stub_blossom *s = (stub_blossom *)ud;
    s->call_count++;
    if (strcmp(sha256_hex, s->addr) != 0) return -105; /* NOT_FOUND */
    uint8_t *copy = malloc(s->len);
    if (!copy) return -108; /* OOM */
    memcpy(copy, s->bytes, s->len);
    *out_data = copy;
    *out_len = s->len;
    return 0;
}

/* Build the sharded cache path for an address. */
static void shard_path(const char *cache_dir, const char *addr,
                       char *out, size_t outn) {
    int n = snprintf(out, outn, "%s/%c%c/%c%c/%s",
                     cache_dir, addr[0], addr[1], addr[2], addr[3], addr);
    assert(n > 0 && (size_t)n < outn);
}

int main(void) {
    /* Fresh tmp workspace. */
    char tmpl[] = "/tmp/nhfuse_poison_XXXXXX";
    assert(mkdtemp(tmpl) != NULL);
    char cdir[300];  snprintf(cdir, sizeof cdir, "%s/cache", tmpl);
    char stderr_path[300]; snprintf(stderr_path, sizeof stderr_path, "%s/err.log", tmpl);

    nh_syncd_cache *cache = NULL;
    assert(nh_syncd_cache_open(cdir, 0, &cache) == 0);

    /* Derive home key from a deterministic seed. */
    uint8_t seed[32];
    for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(0x11 * (i + 1));
    uint8_t hkey[32];
    assert(nh_porthome_key_derive(seed, hkey) == 0);

    /* Seal one chunk of plaintext → correct sealed bytes + address. */
    const char *pt = "The quick brown fox jumps over the lazy dog.";
    size_t pt_len = strlen(pt);
    uint8_t sha[32]; uint8_t *ct_good = NULL; size_t ct_good_len = 0;
    assert(nh_porthome_encrypt_chunk(hkey, (const uint8_t *)pt, pt_len,
                                     &ct_good, &ct_good_len, sha) == 0);
    char addr[65]; hex_of(sha, 32, addr);

    /* Seed the cache with a POISONED entry: same file name (== the
     * declared address), but bytes replaced by urandom of the same
     * length. Use nh_syncd_cache_put with the correct bytes first to
     * make sure the sharded dir exists, then overwrite. Bypasses the
     * cache's own hash check on the overwrite — this is exactly the
     * on-disk corruption the design describes. */
    assert(nh_syncd_cache_put(cache, addr, ct_good, ct_good_len) == 0);
    char blob_path[512]; shard_path(cdir, addr, blob_path, sizeof blob_path);
    {
        struct stat st;
        assert(stat(blob_path, &st) == 0);
        assert((size_t)st.st_size == ct_good_len);
        uint8_t *garbage = malloc(ct_good_len);
        assert(garbage);
        int urnd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        assert(urnd >= 0);
        size_t off = 0;
        while (off < ct_good_len) {
            ssize_t r = read(urnd, garbage + off, ct_good_len - off);
            assert(r > 0);
            off += (size_t)r;
        }
        close(urnd);
        int fd = open(blob_path, O_WRONLY | O_TRUNC | O_CLOEXEC);
        assert(fd >= 0);
        off = 0;
        while (off < ct_good_len) {
            ssize_t w = write(fd, garbage + off, ct_good_len - off);
            assert(w > 0);
            off += (size_t)w;
        }
        close(fd);
        free(garbage);
    }

    /* Stub Blossom serves the CORRECT sealed bytes. */
    stub_blossom stub = { .len = ct_good_len };
    memcpy(stub.addr, addr, 65);
    stub.bytes = malloc(ct_good_len);
    assert(stub.bytes);
    memcpy(stub.bytes, ct_good, ct_good_len);

    /* Redirect stderr into a file so we can assert the NOTICE line. */
    fflush(stderr);
    int saved_stderr = dup(fileno(stderr));
    assert(saved_stderr >= 0);
    int errfd = open(stderr_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    assert(errfd >= 0);
    assert(dup2(errfd, fileno(stderr)) >= 0);
    close(errfd);

    /* Open the source with the stub wired in. blossom (real client) is
     * NULL — the seam is the exclusive tier-3 path. */
    nh_fuse_source_cfg cfg = {
        .home_dir = NULL,
        .tier0_enabled = false,
        .cache = cache,
        .blossom = NULL,
        .chunk_cache_bytes = 0,
        .offline_budget_ms = 0,
        .on_miss = NULL,
        .on_miss_ud = NULL,
        .blossom_fetch = stub_blossom_fetch,
        .blossom_fetch_ud = &stub,
    };
    memcpy(cfg.home_key, hkey, 32);
    nh_fuse_source *src = NULL;
    assert(nh_fuse_source_open(&cfg, &src) == 0);

    /* Read. Tier 2 hits the poisoned file → decrypt fails → fallthrough
     * → tier 3 (stub) → tier 2 repopulated with the correct sealed
     * bytes → decrypt succeeds → correct plaintext. */
    const uint8_t *out = NULL; size_t out_len = 0;
    int rc = nh_fuse_source_chunk(src, addr, &out, &out_len);

    /* Restore stderr so subsequent asserts print on failure. */
    fflush(stderr);
    assert(dup2(saved_stderr, fileno(stderr)) >= 0);
    close(saved_stderr);

    /* (i) correct plaintext served. */
    assert(rc == 0);
    assert(out_len == pt_len);
    assert(memcmp(out, pt, pt_len) == 0);

    /* Stub was actually consulted — proves fallthrough happened. */
    assert(stub.call_count == 1);

    /* (ii) cache entry is either gone or now holds the CORRECT sealed
     *      bytes. Both are acceptable under the design: unlink is the
     *      poisoning-eviction guarantee, and tier-3 populate immediately
     *      after is the self-heal. */
    struct stat st;
    if (stat(blob_path, &st) == 0) {
        assert((size_t)st.st_size == ct_good_len);
        uint8_t *reread = malloc(ct_good_len);
        assert(reread);
        int fd = open(blob_path, O_RDONLY | O_CLOEXEC);
        assert(fd >= 0);
        size_t off = 0;
        while (off < ct_good_len) {
            ssize_t r = read(fd, reread + off, ct_good_len - off);
            assert(r > 0);
            off += (size_t)r;
        }
        close(fd);
        assert(memcmp(reread, ct_good, ct_good_len) == 0);
        free(reread);
    } /* else: unlinked — acceptable. */

    /* (iii) NOTICE line landed on stderr; must NOT contain any hex
     *       from the home_key. */
    {
        FILE *f = fopen(stderr_path, "rb");
        assert(f);
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        assert(sz > 0);
        rewind(f);
        char *buf = malloc((size_t)sz + 1);
        assert(buf);
        assert(fread(buf, 1, (size_t)sz, f) == (size_t)sz);
        buf[sz] = '\0';
        fclose(f);
        assert(strstr(buf, "NOTICE") != NULL);
        assert(strstr(buf, "tier-2 cache entry") != NULL);
        assert(strstr(buf, "falling through to tier-3") != NULL);
        assert(strstr(buf, addr) != NULL);
        /* Key material MUST NOT appear. */
        char keyhex[65]; hex_of(hkey, 32, keyhex);
        /* Match on the first 16 hex chars — long enough to be unique. */
        char probe[17]; memcpy(probe, keyhex, 16); probe[16] = '\0';
        assert(strstr(buf, probe) == NULL);
        free(buf);
    }

    /* Second read: served straight from tier 1 (LRU) — proves the
     * repopulated / re-decrypted plaintext is now cached. */
    stub.call_count = 0;
    out = NULL; out_len = 0;
    rc = nh_fuse_source_chunk(src, addr, &out, &out_len);
    assert(rc == 0);
    assert(out_len == pt_len);
    assert(memcmp(out, pt, pt_len) == 0);
    assert(stub.call_count == 0);

    /* Stats: exactly one tier-3 fetch, no tier-2 hits (tier 2 was
     * poisoned on first read, absent-or-good on second which hit
     * LRU first anyway). */
    nh_fuse_source_stats_t stats = {0};
    nh_fuse_source_stats(src, &stats);
    assert(stats.fetches == 1);
    assert(stats.hits_chunk_lru == 1);

    nh_fuse_source_close(src);
    nh_syncd_cache_close(cache);
    free(stub.bytes);
    free(ct_good);
    printf("test_fuse_source_poison OK\n");
    return 0;
}
