/*
 * test_porthome_manifest.c — canonical CBOR round-trip + strict-parse
 * rejections + a small property-based fuzzer with a fixed seed.
 */

#include "nh_porthome_manifest.h"
#include "nh_porthome_crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define FAIL(...) do { fprintf(stderr, "FAIL: " __VA_ARGS__); fprintf(stderr, "\n"); return 1; } while (0)
#define ASSERT(cond) do { if (!(cond)) FAIL("%s:%d %s", __FILE__, __LINE__, #cond); } while (0)
#define ASSERT_EQ(a,b) do { long _a = (long)(a), _b = (long)(b); if (_a != _b) FAIL("%s:%d %s=%ld != %s=%ld", __FILE__, __LINE__, #a,_a,#b,_b); } while (0)

static uint64_t sm_state;
static uint64_t sm_next(void) {
    uint64_t z = (sm_state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}
static __attribute__((unused)) void fill_random(uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) b[i] = (uint8_t)(sm_next() & 0xFF);
}

/* Build a small fixture manifest with predictable content. */
static int build_fixture(nh_porthome_manifest *m, const uint8_t hk[32],
                        int n_entries) {
    uint8_t root[32]; for (int i = 0; i < 32; i++) root[i] = (uint8_t)i;
    if (nh_porthome_manifest_init(m, root) != 0) return -1;

    for (int i = 0; i < n_entries; i++) {
        char path[64];
        snprintf(path, sizeof(path), "dir%d/file_%d.bin", i / 10, i);
        char *penc = NULL;
        if (nh_porthome_encrypt_path(hk, path, &penc) != 0) return -1;
        nh_porthome_chunk chunks[2];
        memset(chunks, 0, sizeof(chunks));
        for (int j = 0; j < 32; j++) chunks[0].sha256[j] = (uint8_t)(i * 3 + j);
        chunks[0].size = 4096 + i;
        chunks[0].chunk_key_id = 0;
        for (int j = 0; j < 32; j++) chunks[1].sha256[j] = (uint8_t)(i * 5 + j);
        chunks[1].size = 512;
        chunks[1].chunk_key_id = 0;
        int rc = nh_porthome_manifest_add_file(m, penc,
            0644, 1000, 1000, 1700000000ULL * 1000000000ULL + (uint64_t)i,
            (uint64_t)(4096 + i + 512), chunks, 2);
        if (rc != 0) { free(penc); return -1; }
    }
    return 0;
}

static int test_encode_decode_roundtrip(int n) {
    uint8_t seed[32]; for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(i * 7);
    uint8_t hk[32]; if (nh_porthome_key_derive(seed, hk) != 0) return 1;
    nh_porthome_manifest m;
    if (build_fixture(&m, hk, n) != 0) FAIL("fixture build");

    uint8_t *cbor = NULL; size_t cbor_len = 0;
    ASSERT_EQ(nh_porthome_manifest_encode(&m, &cbor, &cbor_len), 0);
    ASSERT(cbor_len > 0);
    /* Canonical encoders are deterministic. Encode again -> same bytes. */
    uint8_t *cbor2 = NULL; size_t cbor2_len = 0;
    ASSERT_EQ(nh_porthome_manifest_encode(&m, &cbor2, &cbor2_len), 0);
    ASSERT_EQ(cbor_len, cbor2_len);
    ASSERT_EQ(memcmp(cbor, cbor2, cbor_len), 0);

    nh_porthome_manifest *m2 = NULL;
    ASSERT_EQ(nh_porthome_manifest_decode(cbor, cbor_len, &m2), 0);
    ASSERT(m2 != NULL);
    /* This fixture uses v1 add helpers (init defaults to V1 for
     * backward-compat with pre-q25o callers). Schema v2 exercises the
     * additive name_sealed / link_target_sealed slots and lives in
     * test_porthome_manifest_v2.c. */
    ASSERT_EQ(m2->version, NH_PORTHOME_MANIFEST_SCHEMA_VERSION_V1);
    ASSERT_EQ(memcmp(m2->home_root_id, m.home_root_id, 32), 0);
    ASSERT_EQ(m2->entries_len, m.entries_len);
    for (size_t i = 0; i < m.entries_len; i++) {
        ASSERT_EQ(strcmp(m2->entries[i].path_enc, m.entries[i].path_enc), 0);
        ASSERT_EQ(m2->entries[i].mode, m.entries[i].mode);
        ASSERT_EQ(m2->entries[i].mtime_ns, m.entries[i].mtime_ns);
        ASSERT_EQ(m2->entries[i].size, m.entries[i].size);
        ASSERT_EQ(m2->entries[i].kind, m.entries[i].kind);
        ASSERT_EQ(m2->entries[i].chunks_len, m.entries[i].chunks_len);
        for (size_t j = 0; j < m2->entries[i].chunks_len; j++) {
            ASSERT_EQ(memcmp(m2->entries[i].chunks[j].sha256,
                             m.entries[i].chunks[j].sha256, 32), 0);
            ASSERT_EQ(m2->entries[i].chunks[j].size, m.entries[i].chunks[j].size);
            ASSERT_EQ(m2->entries[i].chunks[j].chunk_key_id, 0);
        }
    }

    nh_porthome_manifest_dispose(m2); free(m2);
    nh_porthome_manifest_dispose(&m);
    free(cbor); free(cbor2);
    return 0;
}

static int test_property(void) {
    /* 100+ random-sized manifests, fixed seed. */
    sm_state = 0xB0A710ADULL;
    for (int trial = 0; trial < 128; trial++) {
        int n = (int)(sm_next() % 25) + 1;
        if (test_encode_decode_roundtrip(n)) return 1;
    }
    return 0;
}

static int test_seal_roundtrip(void) {
    uint8_t seed[32]; for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(0x11 * i);
    uint8_t hk[32]; if (nh_porthome_key_derive(seed, hk) != 0) return 1;
    nh_porthome_manifest m;
    if (build_fixture(&m, hk, 32) != 0) return 1;

    uint8_t *sealed = NULL; size_t sealed_len = 0;
    ASSERT_EQ(nh_porthome_manifest_encode_sealed(&m, hk, &sealed, &sealed_len), 0);
    /* Sealed layout: 1 + 12 + N + 16. Decrypt back and re-decode. */
    nh_porthome_manifest *back = NULL;
    ASSERT_EQ(nh_porthome_manifest_decode_sealed(sealed, sealed_len, hk, &back), 0);
    ASSERT_EQ(back->entries_len, m.entries_len);
    /* Wrong key -> AEAD failure. */
    uint8_t other[32]; other[0] = hk[0] ^ 1; memcpy(other + 1, hk + 1, 31);
    nh_porthome_manifest *bad = NULL;
    ASSERT(nh_porthome_manifest_decode_sealed(sealed, sealed_len, other, &bad) != 0);
    ASSERT(bad == NULL);

    nh_porthome_manifest_dispose(back); free(back);
    nh_porthome_manifest_dispose(&m);
    free(sealed);
    return 0;
}

/* ────────────── strict-parse rejections ────────────── */

static uint8_t *encode_and_take(nh_porthome_manifest *m, size_t *out_len) {
    uint8_t *b = NULL;
    if (nh_porthome_manifest_encode(m, &b, out_len) != 0) return NULL;
    return b;
}

static int test_reject_trailing_bytes(void) {
    uint8_t seed[32]; memset(seed, 0x7, 32);
    uint8_t hk[32]; nh_porthome_key_derive(seed, hk);
    nh_porthome_manifest m;
    if (build_fixture(&m, hk, 4) != 0) return 1;
    size_t n; uint8_t *b = encode_and_take(&m, &n);
    ASSERT(b != NULL);
    uint8_t *b2 = (uint8_t *)malloc(n + 4);
    memcpy(b2, b, n);
    memcpy(b2 + n, "junk", 4);
    nh_porthome_manifest *pm = NULL;
    ASSERT(nh_porthome_manifest_decode(b2, n + 4, &pm) != 0);
    ASSERT(pm == NULL);
    free(b); free(b2); nh_porthome_manifest_dispose(&m);
    return 0;
}

static int test_reject_oversize(void) {
    nh_porthome_manifest *pm = NULL;
    uint8_t big[16]; memset(big, 0xA0, sizeof(big));
    /* We can't easily construct a valid CBOR > 1 MiB inline; instead
     * assert the guard triggers on a claimed length above the cap. */
    ASSERT(nh_porthome_manifest_decode(big, (size_t)NH_PORTHOME_MAX_CBOR_BYTES + 1, &pm) != 0);
    ASSERT(pm == NULL);
    return 0;
}

static int test_reject_unknown_version(void) {
    /* Hand-craft a canonical CBOR manifest header with version=3 —
     * nostrc-q25o added v2 as the second accepted version, so we use
     * v3 here as the "unknown" version the strict decoder must
     * refuse. */
    /* map(3): 0xA3
     *   uint(1): 0x01
     *   uint(3): 0x03    ← version, expected to be refused
     *   uint(2): 0x02
     *   bstr(32): 0x58 0x20 <32 bytes>
     *   uint(3): 0x03
     *   array(0): 0x80
     */
    uint8_t buf[64];
    size_t p = 0;
    buf[p++] = 0xA3;
    buf[p++] = 0x01; buf[p++] = 0x03;       /* version=3 (unknown) */
    buf[p++] = 0x02; buf[p++] = 0x58; buf[p++] = 0x20;
    for (int i = 0; i < 32; i++) buf[p++] = (uint8_t)i;
    buf[p++] = 0x03; buf[p++] = 0x80;
    nh_porthome_manifest *pm = NULL;
    ASSERT(nh_porthome_manifest_decode(buf, p, &pm) != 0);
    ASSERT(pm == NULL);
    return 0;
}

static int test_reject_duplicate_keys(void) {
    /* Same as above but with version=1 appearing twice — a canonical
     * decoder must reject that as strictly-ascending violation. */
    /* map(4)? — we duplicate the "1"-keyed value; the decoder sees
     * map(3) declared, but if we bump to map(4) that's invalid too. Use
     * a hand-crafted entry map with duplicate key K_E_MODE (2). */
    /* Build a full-shape manifest by hand:
     *   map(3): version=1, root=zeros, entries=array(1) of entry
     *   entry: map(8): keys 1,2,2,3,4,5,6,7  (duplicate 2)
     */
    uint8_t buf[256]; size_t p = 0;
    buf[p++] = 0xA3;                       /* map(3) */
    buf[p++] = 0x01; buf[p++] = 0x01;      /* v=1 */
    buf[p++] = 0x02; buf[p++] = 0x58; buf[p++] = 0x20;
    for (int i = 0; i < 32; i++) buf[p++] = 0;
    buf[p++] = 0x03; buf[p++] = 0x81;       /* entries=array(1) */
    /* entry: map(8), keys 1,2,2,3,4,5,6,7 (dup on 2) */
    buf[p++] = 0xA8;
    buf[p++] = 0x01; buf[p++] = 0x62; buf[p++] = 'a'; buf[p++] = 'a'; /* path="aa" (bad, but we're testing dup rejection first) */
    buf[p++] = 0x02; buf[p++] = 0x18; buf[p++] = 0x81; /* mode=0x81 */
    buf[p++] = 0x02; buf[p++] = 0x18; buf[p++] = 0x81; /* mode again (duplicate key) */
    buf[p++] = 0x03; buf[p++] = 0x00;
    buf[p++] = 0x04; buf[p++] = 0x00;
    buf[p++] = 0x05; buf[p++] = 0x00;
    buf[p++] = 0x06; buf[p++] = 0x00;
    buf[p++] = 0x07; buf[p++] = 0x02; /* kind=DIR */
    nh_porthome_manifest *pm = NULL;
    int rc = nh_porthome_manifest_decode(buf, p, &pm);
    ASSERT(rc != 0);
    ASSERT(pm == NULL);
    return 0;
}

static int test_reject_absolute_and_dotdot(void) {
    /* Craft a well-formed manifest with a path field that is "/abs" or "..". */
    /* Build a valid CBOR by encoding once, then patching the path field
     * — simpler: encode-with-empty-path (which will already be rejected)
     * is not enough. So we directly hand-craft a manifest with 1 dir
     * entry whose path is "..". */
    uint8_t buf[128]; size_t p = 0;
    buf[p++] = 0xA3;
    buf[p++] = 0x01; buf[p++] = 0x01;
    buf[p++] = 0x02; buf[p++] = 0x58; buf[p++] = 0x20;
    for (int i = 0; i < 32; i++) buf[p++] = 0;
    buf[p++] = 0x03; buf[p++] = 0x81;
    buf[p++] = 0xA7;                       /* map(7) */
    buf[p++] = 0x01; buf[p++] = 0x62; buf[p++] = '.'; buf[p++] = '.';
    buf[p++] = 0x02; buf[p++] = 0x00;
    buf[p++] = 0x03; buf[p++] = 0x00;
    buf[p++] = 0x04; buf[p++] = 0x00;
    buf[p++] = 0x05; buf[p++] = 0x00;
    buf[p++] = 0x06; buf[p++] = 0x00;
    buf[p++] = 0x07; buf[p++] = 0x02; /* kind=DIR */
    nh_porthome_manifest *pm = NULL;
    ASSERT(nh_porthome_manifest_decode(buf, p, &pm) != 0);
    ASSERT(pm == NULL);

    /* Now try absolute /a — string "/a" length 2. */
    p = 0;
    buf[p++] = 0xA3;
    buf[p++] = 0x01; buf[p++] = 0x01;
    buf[p++] = 0x02; buf[p++] = 0x58; buf[p++] = 0x20;
    for (int i = 0; i < 32; i++) buf[p++] = 0;
    buf[p++] = 0x03; buf[p++] = 0x81;
    buf[p++] = 0xA7;
    buf[p++] = 0x01; buf[p++] = 0x62; buf[p++] = '/'; buf[p++] = 'a';
    buf[p++] = 0x02; buf[p++] = 0x00;
    buf[p++] = 0x03; buf[p++] = 0x00;
    buf[p++] = 0x04; buf[p++] = 0x00;
    buf[p++] = 0x05; buf[p++] = 0x00;
    buf[p++] = 0x06; buf[p++] = 0x00;
    buf[p++] = 0x07; buf[p++] = 0x02;
    ASSERT(nh_porthome_manifest_decode(buf, p, &pm) != 0);
    ASSERT(pm == NULL);
    return 0;
}

static int test_reject_nonzero_chunk_key_id(void) {
    /* Build a file entry whose only chunk has chunk_key_id = 1. */
    uint8_t buf[256]; size_t p = 0;
    buf[p++] = 0xA3;
    buf[p++] = 0x01; buf[p++] = 0x01;
    buf[p++] = 0x02; buf[p++] = 0x58; buf[p++] = 0x20;
    for (int i = 0; i < 32; i++) buf[p++] = 0;
    buf[p++] = 0x03; buf[p++] = 0x81;
    buf[p++] = 0xA8; /* map(8): 1..7 + 8=chunks */
    buf[p++] = 0x01; buf[p++] = 0x62; buf[p++] = 'a'; buf[p++] = 'a';
    buf[p++] = 0x02; buf[p++] = 0x18; buf[p++] = 0x80; /* mode */
    buf[p++] = 0x03; buf[p++] = 0x00;
    buf[p++] = 0x04; buf[p++] = 0x00;
    buf[p++] = 0x05; buf[p++] = 0x00;
    buf[p++] = 0x06; buf[p++] = 0x0A; /* size=10 */
    buf[p++] = 0x07; buf[p++] = 0x01; /* kind=FILE */
    buf[p++] = 0x08; buf[p++] = 0x81; /* chunks array(1) */
    buf[p++] = 0xA3; /* chunk map(3) */
    buf[p++] = 0x01; buf[p++] = 0x58; buf[p++] = 0x20;
    for (int i = 0; i < 32; i++) buf[p++] = (uint8_t)i;
    buf[p++] = 0x02; buf[p++] = 0x0A;  /* size=10 */
    buf[p++] = 0x03; buf[p++] = 0x01;  /* chunk_key_id=1 → must be refused */
    nh_porthome_manifest *pm = NULL;
    ASSERT(nh_porthome_manifest_decode(buf, p, &pm) != 0);
    ASSERT(pm == NULL);
    return 0;
}

int main(void) {
    if (test_encode_decode_roundtrip(1))  return 1;
    if (test_encode_decode_roundtrip(50)) return 1;
    if (test_property())                  return 1;
    if (test_seal_roundtrip())            return 1;
    if (test_reject_trailing_bytes())     return 1;
    if (test_reject_oversize())           return 1;
    if (test_reject_unknown_version())    return 1;
    if (test_reject_duplicate_keys())     return 1;
    if (test_reject_absolute_and_dotdot()) return 1;
    if (test_reject_nonzero_chunk_key_id()) return 1;
    printf("OK porthome_manifest\n");
    return 0;
}
