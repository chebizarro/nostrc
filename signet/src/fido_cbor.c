/* SPDX-License-Identifier: MIT */
/* signet FIDO CBOR/COSE — minimal deterministic encoder (Phase 0 spike). */

#include "signet/fido_cbor.h"

#include <stdlib.h>
#include <string.h>

#include <openssl/sha.h>

/* ---- tiny growable byte buffer ---- */
typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
    int err;
} buf_t;

static void buf_init(buf_t *b) { memset(b, 0, sizeof(*b)); }

static int buf_reserve(buf_t *b, size_t extra)
{
    if (b->err) return -1;
    if (b->len + extra <= b->cap) return 0;
    size_t ncap = b->cap ? b->cap * 2 : 64;
    while (ncap < b->len + extra) ncap *= 2;
    uint8_t *nd = realloc(b->data, ncap);
    if (!nd) { b->err = 1; return -1; }
    b->data = nd;
    b->cap = ncap;
    return 0;
}

static void buf_put(buf_t *b, const uint8_t *p, size_t n)
{
    if (buf_reserve(b, n)) return;
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

static void buf_u8(buf_t *b, uint8_t v) { buf_put(b, &v, 1); }

/* CBOR head: major type (top 3 bits) + argument. */
static void cbor_head(buf_t *b, int major, uint64_t arg)
{
    uint8_t m = (uint8_t)(major << 5);
    if (arg < 24) {
        buf_u8(b, m | (uint8_t)arg);
    } else if (arg < 256) {
        buf_u8(b, m | 24); buf_u8(b, (uint8_t)arg);
    } else if (arg < 65536) {
        buf_u8(b, m | 25);
        buf_u8(b, (uint8_t)(arg >> 8)); buf_u8(b, (uint8_t)arg);
    } else if (arg < 0x100000000ULL) {
        buf_u8(b, m | 26);
        buf_u8(b, (uint8_t)(arg >> 24)); buf_u8(b, (uint8_t)(arg >> 16));
        buf_u8(b, (uint8_t)(arg >> 8));  buf_u8(b, (uint8_t)arg);
    } else {
        buf_u8(b, m | 27);
        for (int i = 56; i >= 0; i -= 8) buf_u8(b, (uint8_t)(arg >> i));
    }
}

static void cbor_uint(buf_t *b, uint64_t v)  { cbor_head(b, 0, v); }
static void cbor_nint(buf_t *b, int64_t v)   { cbor_head(b, 1, (uint64_t)(-1 - v)); }
static void cbor_bstr(buf_t *b, const uint8_t *p, size_t n) { cbor_head(b, 2, n); buf_put(b, p, n); }
static void cbor_tstr(buf_t *b, const char *s) { size_t n = strlen(s); cbor_head(b, 3, n); buf_put(b, (const uint8_t *)s, n); }
static void cbor_map(buf_t *b, size_t n)     { cbor_head(b, 5, n); }

static int buf_finish(buf_t *b, uint8_t **out, size_t *out_len)
{
    if (b->err) { free(b->data); return -1; }
    *out = b->data;
    *out_len = b->len;
    return 0;
}

/* ---- COSE_Key EC2 / P-256 / ES256 ---- */
int signet_cose_ec2_p256(const uint8_t x[32], const uint8_t y[32],
                         uint8_t **out, size_t *out_len)
{
    if (!x || !y || !out || !out_len) return -1;
    buf_t b; buf_init(&b);
    cbor_map(&b, 5);
    cbor_uint(&b, 1);  cbor_uint(&b, 2);      /* kty : EC2 */
    cbor_uint(&b, 3);  cbor_nint(&b, -7);     /* alg : ES256 */
    cbor_nint(&b, -1); cbor_uint(&b, 1);      /* crv : P-256 */
    cbor_nint(&b, -2); cbor_bstr(&b, x, 32);  /* x */
    cbor_nint(&b, -3); cbor_bstr(&b, y, 32);  /* y */
    return buf_finish(&b, out, out_len);
}

/* ---- authenticatorData ---- */
int signet_fido_auth_data(const char *rp_id, uint8_t flags, uint32_t sign_count,
                          const uint8_t aaguid[16],
                          const uint8_t *cred_id, size_t cred_id_len,
                          const uint8_t *cose_key, size_t cose_key_len,
                          uint8_t **out, size_t *out_len)
{
    if (!rp_id || !out || !out_len) return -1;

    buf_t b; buf_init(&b);

    uint8_t rp_hash[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char *)rp_id, strlen(rp_id), rp_hash);
    buf_put(&b, rp_hash, sizeof(rp_hash));           /* rpIdHash (32) */

    buf_u8(&b, flags);                               /* flags (1) */

    uint8_t sc[4] = {                                /* signCount (4, big-endian) */
        (uint8_t)(sign_count >> 24), (uint8_t)(sign_count >> 16),
        (uint8_t)(sign_count >> 8),  (uint8_t)sign_count };
    buf_put(&b, sc, 4);

    if (flags & SIGNET_FIDO_FLAG_AT) {
        if (!aaguid || !cred_id || !cose_key || cred_id_len > 0xFFFF) { free(b.data); return -1; }
        buf_put(&b, aaguid, 16);                     /* AAGUID (16) */
        uint8_t cl[2] = { (uint8_t)(cred_id_len >> 8), (uint8_t)cred_id_len };
        buf_put(&b, cl, 2);                          /* credIdLen (2, big-endian) */
        buf_put(&b, cred_id, cred_id_len);           /* credId */
        buf_put(&b, cose_key, cose_key_len);         /* COSE public key */
    }

    return buf_finish(&b, out, out_len);
}

/* ---- "none" attestation object ---- */
int signet_fido_attestation_none(const uint8_t *auth_data, size_t auth_data_len,
                                 uint8_t **out, size_t *out_len)
{
    if (!auth_data || !out || !out_len) return -1;
    buf_t b; buf_init(&b);
    cbor_map(&b, 3);
    cbor_tstr(&b, "fmt");      cbor_tstr(&b, "none");
    cbor_tstr(&b, "attStmt");  cbor_map(&b, 0);
    cbor_tstr(&b, "authData"); cbor_bstr(&b, auth_data, auth_data_len);
    return buf_finish(&b, out, out_len);
}
