/*
 * test_hanami_blossom_shim.c
 *
 * Unit tests for the PNG-shim wrapper (nostrc-bpum).
 *
 * The shim's job is to prepend a deterministic 41-byte "valid enough
 * for a body sniffer" PNG prefix to random-looking ciphertext so
 * community Blossom servers (blossom.band, blossom.primal.net) stop
 * 415-rejecting the upload. See docs/reviews/porthome-blossom-shim-
 * 2026-09-25.md.
 *
 * SPDX-License-Identifier: MIT
 */

#include "hanami/hanami-blossom-shim.h"
#include "hanami/hanami-types.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- tiny test harness --------------------------------------------- */

static int g_failed;

#define EXPECT(cond, msg)                                              \
    do {                                                               \
        if (!(cond)) {                                                 \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); \
            g_failed++;                                                \
        }                                                              \
    } while (0)

#define RUN(name) do {                                                 \
    int before = g_failed;                                             \
    printf("  %-56s", #name);                                          \
    fflush(stdout);                                                    \
    test_##name();                                                     \
    printf("%s\n", (g_failed == before) ? "OK" : "FAIL");              \
} while (0)

/* ---- reference SHA-256 (tiny inline impl to avoid OpenSSL link) ---- */

/* We only need SHA-256 to verify determinism (same input -> same hash).
 * A minimal from-scratch implementation is 40 lines. Not for production
 * use — but self-contained for a test. */

static uint32_t sha_rotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }

static void sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
    static const uint32_t K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
    };
    uint32_t h[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                      0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };

    size_t total = len;
    size_t padded_len = ((len + 9 + 63) / 64) * 64;
    uint8_t *msg = calloc(1, padded_len);
    assert(msg);
    memcpy(msg, data, len);
    msg[len] = 0x80;
    uint64_t bits = (uint64_t)total * 8u;
    for (int i = 0; i < 8; i++)
        msg[padded_len - 1 - i] = (uint8_t)(bits >> (8 * i));

    for (size_t off = 0; off < padded_len; off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = ((uint32_t)msg[off + i*4 + 0] << 24)
                 | ((uint32_t)msg[off + i*4 + 1] << 16)
                 | ((uint32_t)msg[off + i*4 + 2] <<  8)
                 |  (uint32_t)msg[off + i*4 + 3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = sha_rotr(w[i-15], 7) ^ sha_rotr(w[i-15], 18) ^ (w[i-15] >> 3);
            uint32_t s1 = sha_rotr(w[i-2], 17) ^ sha_rotr(w[i-2], 19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t S1 = sha_rotr(e,6) ^ sha_rotr(e,11) ^ sha_rotr(e,25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            uint32_t S0 = sha_rotr(a,2) ^ sha_rotr(a,13) ^ sha_rotr(a,22);
            uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + mj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }
    free(msg);
    for (int i = 0; i < 8; i++) {
        out[i*4+0] = (uint8_t)(h[i] >> 24);
        out[i*4+1] = (uint8_t)(h[i] >> 16);
        out[i*4+2] = (uint8_t)(h[i] >>  8);
        out[i*4+3] = (uint8_t)(h[i]      );
    }
}

/* ---- tests --------------------------------------------------------- */

static const uint8_t PNG_SIG[8] = {0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A};

static void test_encoded_len(void)
{
    EXPECT(hanami_blossom_shim_encoded_len(0)    == 41,   "len(0) = 41");
    EXPECT(hanami_blossom_shim_encoded_len(1024) == 1065, "len(1024) = 1065");
    EXPECT(hanami_blossom_shim_encoded_len(4*1024*1024)
            == (size_t)HANAMI_BLOSSOM_PNG_SHIM_LEN + 4*1024*1024,
           "len(4MiB) = 41+4MiB");
}

static void test_encode_rejects_bad_args(void)
{
    uint8_t out[128];
    /* NULL out. */
    EXPECT(hanami_blossom_shim_encode(NULL, 0, NULL, 128) == HANAMI_ERR_INVALID_ARG,
           "NULL out rejected");
    /* NULL ct with ct_len > 0. */
    EXPECT(hanami_blossom_shim_encode(NULL, 8, out, sizeof(out)) == HANAMI_ERR_INVALID_ARG,
           "NULL ct + ct_len>0 rejected");
    /* out_cap too small. */
    uint8_t ct[16] = {0};
    EXPECT(hanami_blossom_shim_encode(ct, sizeof(ct), out, 40) == HANAMI_ERR_INVALID_ARG,
           "small out_cap rejected");
    /* Legal empty encode. */
    EXPECT(hanami_blossom_shim_encode(NULL, 0, out, sizeof(out)) == HANAMI_OK,
           "NULL ct + ct_len=0 accepted");
}

static void test_encode_prefix_shape(void)
{
    uint8_t ct[123];
    for (size_t i = 0; i < sizeof(ct); i++) ct[i] = (uint8_t)(i * 31u + 7u);
    uint8_t out[512];
    hanami_error_t rc = hanami_blossom_shim_encode(ct, sizeof(ct), out, sizeof(out));
    EXPECT(rc == HANAMI_OK, "encode ok");

    /* PNG signature. */
    EXPECT(memcmp(out, PNG_SIG, 8) == 0, "PNG signature at [0..8)");

    /* IHDR length = 13. */
    EXPECT(out[8] == 0 && out[9] == 0 && out[10] == 0 && out[11] == 13,
           "IHDR length = 13 big-endian");
    /* IHDR type. */
    EXPECT(memcmp(out + 12, "IHDR", 4) == 0, "IHDR type");

    /* 1x1 grayscale non-interlaced IHDR body. */
    static const uint8_t ihdr_body[13] = {
        0,0,0,1, 0,0,0,1, 8, 0, 0, 0, 0
    };
    EXPECT(memcmp(out + 16, ihdr_body, 13) == 0, "IHDR body matches");

    /* IHDR CRC must be the standard PNG CRC (poly 0xEDB88320, init/xor
     * 0xFFFFFFFF) over "IHDR" + body. For our specific IHDR body
     * (1x1 grayscale 8-bit non-interlaced) the value is 0x3A7E9B55 —
     * verified against an independent CRC32 implementation. If the
     * shim's IHDR body ever changes, this constant must be updated. */
    static const uint8_t expected_crc[4] = { 0x3A, 0x7E, 0x9B, 0x55 };
    EXPECT(memcmp(out + 29, expected_crc, 4) == 0,
           "IHDR CRC == 0x3A7E9B55");

    /* IDAT length = 123 big-endian. */
    EXPECT(out[33] == 0 && out[34] == 0 && out[35] == 0 && out[36] == 123,
           "IDAT length = ct_len big-endian");
    /* IDAT type. */
    EXPECT(memcmp(out + 37, "IDAT", 4) == 0, "IDAT type");

    /* Ciphertext copied verbatim starting at offset 41. */
    EXPECT(memcmp(out + 41, ct, sizeof(ct)) == 0, "ciphertext at [41..)");
}

static void test_encode_deterministic(void)
{
    /* Same input twice → identical output → identical sha256 (D4
     * convergence preserved through the shim). */
    uint8_t ct[512];
    for (size_t i = 0; i < sizeof(ct); i++) ct[i] = (uint8_t)(i ^ 0x5A);
    uint8_t a[512 + 64], b[512 + 64];
    EXPECT(hanami_blossom_shim_encode(ct, sizeof(ct), a, sizeof(a)) == HANAMI_OK, "encode a");
    EXPECT(hanami_blossom_shim_encode(ct, sizeof(ct), b, sizeof(b)) == HANAMI_OK, "encode b");
    size_t enclen = hanami_blossom_shim_encoded_len(sizeof(ct));
    EXPECT(memcmp(a, b, enclen) == 0, "byte-identical shimmed output");

    uint8_t ha[32], hb[32];
    sha256(a, enclen, ha);
    sha256(b, enclen, hb);
    EXPECT(memcmp(ha, hb, 32) == 0, "sha256(shim||ct) is deterministic");

    /* And the sha256 differs from the plain-ciphertext sha256, as it
     * must — the shim changes the addressable blob identifier. */
    uint8_t hplain[32];
    sha256(ct, sizeof(ct), hplain);
    EXPECT(memcmp(ha, hplain, 32) != 0, "shimmed sha256 != plain sha256");
}

static void test_detect_and_strip_round_trip(void)
{
    /* Encode → detect → strip → recover original bytes exactly. */
    uint8_t ct[1024];
    for (size_t i = 0; i < sizeof(ct); i++) ct[i] = (uint8_t)((i * 13u + 3u) & 0xFF);
    uint8_t shimmed[sizeof(ct) + HANAMI_BLOSSOM_PNG_SHIM_LEN];
    EXPECT(hanami_blossom_shim_encode(ct, sizeof(ct), shimmed, sizeof(shimmed)) == HANAMI_OK,
           "encode ok");

    EXPECT(hanami_blossom_shim_detect(shimmed, sizeof(shimmed)) == true,
           "detect finds shim");

    const uint8_t *tail = NULL;
    size_t tail_len = 0;
    EXPECT(hanami_blossom_shim_strip(shimmed, sizeof(shimmed), &tail, &tail_len) == HANAMI_OK,
           "strip ok");
    EXPECT(tail_len == sizeof(ct), "strip tail len matches");
    EXPECT(memcmp(tail, ct, sizeof(ct)) == 0, "strip tail bytes match");
}

static void test_detect_rejects_non_shim(void)
{
    /* A real porthome D4 chunk always starts with 0x01. Detect must
     * return false and strip must refuse. */
    uint8_t d4[128];
    d4[0] = 0x01;
    for (size_t i = 1; i < sizeof(d4); i++) d4[i] = (uint8_t)i;

    EXPECT(hanami_blossom_shim_detect(d4, sizeof(d4)) == false,
           "D4 wire (starts 0x01) NOT a shim");

    const uint8_t *tail = NULL;
    size_t tail_len = 0;
    EXPECT(hanami_blossom_shim_strip(d4, sizeof(d4), &tail, &tail_len) == HANAMI_ERR_INVALID_ARG,
           "strip refuses non-shim");
    EXPECT(tail == NULL && tail_len == 0, "outs untouched on strip refuse");

    /* Too-short buffer. */
    EXPECT(hanami_blossom_shim_detect(d4, 8) == false, "8-byte buf too short");
    /* NULL. */
    EXPECT(hanami_blossom_shim_detect(NULL, 0) == false, "NULL buf rejected");

    /* Right-length buffer that starts with PNG signature but wrong
     * IHDR — must not match. Craft a bad IHDR (colour type nonsense). */
    uint8_t fake[HANAMI_BLOSSOM_PNG_SHIM_LEN + 4];
    memcpy(fake, PNG_SIG, 8);
    memset(fake + 8, 0xAA, sizeof(fake) - 8);
    EXPECT(hanami_blossom_shim_detect(fake, sizeof(fake)) == false,
           "PNG sig but bad IHDR NOT a shim");
}

static void test_detect_wrong_idat_length(void)
{
    /* If the IDAT length field disagrees with the actual tail length,
     * detect must refuse — the shim's contract is "len - 41 == IDAT
     * length". This guards against an adversary who wraps ciphertext
     * in a truncated/extended shim to trigger a mis-strip. */
    uint8_t ct[64];
    for (size_t i = 0; i < sizeof(ct); i++) ct[i] = (uint8_t)i;
    uint8_t shimmed[sizeof(ct) + HANAMI_BLOSSOM_PNG_SHIM_LEN];
    EXPECT(hanami_blossom_shim_encode(ct, sizeof(ct), shimmed, sizeof(shimmed)) == HANAMI_OK,
           "encode ok");
    /* Corrupt the IDAT length field to claim 999 bytes when the tail
     * is only 64. */
    shimmed[33] = 0; shimmed[34] = 0; shimmed[35] = 0x03; shimmed[36] = 0xE7;
    EXPECT(hanami_blossom_shim_detect(shimmed, sizeof(shimmed)) == false,
           "IDAT length mismatch → detect refuses");
}

int main(void)
{
    printf("libhanami Blossom PNG-shim tests\n");
    printf("================================\n");
    RUN(encoded_len);
    RUN(encode_rejects_bad_args);
    RUN(encode_prefix_shape);
    RUN(encode_deterministic);
    RUN(detect_and_strip_round_trip);
    RUN(detect_rejects_non_shim);
    RUN(detect_wrong_idat_length);
    printf("\n%s\n", g_failed == 0 ? "PASS" : "FAIL");
    return g_failed == 0 ? 0 : 1;
}
