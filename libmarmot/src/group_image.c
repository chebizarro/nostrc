/*
 * libmarmot - group image components
 *
 *   0x8002 marmot.group.blossom.image.v1 (app-components/group-blossom-image-v1.md)
 *   0x8007 marmot.group.avatar-url.v1    (app-components/group-avatar-url-v1.md)
 *
 * marmot-protocol/marmot 07da8ffb; checked against MDK v0.11.0 cgka-traits
 * codecs (tests/vectors/media/).  Both are QUIC-varint-prefixed opaque
 * vectors decoded exactly (shortest prefixes, no trailing bytes).
 *
 * SPDX-License-Identifier: MIT
 */

#include <marmot/marmot.h>
#include "media_v2.h"
#include "mls/mls-internal.h"

#include <arpa/inet.h>
#include <sodium.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define IMAGE_AAD_LABEL "marmot-group-image-v1"
#define AEAD_TAG_LEN crypto_aead_chacha20poly1305_ietf_ABYTES

/* ── Vector helpers ────────────────────────────────────────────────────── */

static int
write_vec(MlsTlsBuf *b, const void *data, size_t len)
{
    if (mls_tls_write_vli(b, len) != 0) return -1;
    return len ? mls_tls_buf_append(b, data, len) : 0;
}

/* Read opaque<0..max> into a malloc'd buffer (NULL when empty): 0, or
 * VEC_MALFORMED, or VEC_NOMEM (review N3: an allocation failure is not
 * malformed state). */
#define VEC_MALFORMED (-1)
#define VEC_NOMEM     (-2)
#define VEC_ERR(rc) ((rc) == VEC_NOMEM ? MARMOT_ERR_MEMORY : MARMOT_ERR_MEDIA_INVALID_REFERENCE)

static int
read_vec(MlsTlsReader *r, size_t max, uint8_t **out, size_t *out_len)
{
    size_t len;
    *out = NULL;
    *out_len = 0;
    if (mls_tls_read_vli(r, &len) != 0 || len > max || mls_tls_reader_remaining(r) < len)
        return -1;
    if (len == 0) return 0;
    uint8_t *buf = malloc(len + 1);   /* +1: callers may NUL-terminate */
    if (!buf) return VEC_NOMEM;
    if (mls_tls_read_fixed(r, buf, len) != 0) {
        free(buf);
        return -1;
    }
    buf[len] = 0;
    *out = buf;
    *out_len = len;
    return 0;
}

static MarmotError
finish_buf(MlsTlsBuf *b, uint8_t **out, size_t *out_len)
{
    *out = b->data;
    *out_len = b->len;
    b->data = NULL;
    return MARMOT_OK;
}

/* ── 0x8002 marmot.group.blossom.image.v1 ──────────────────────────────── */

/* The 0x8002 spec cites the frozen v1 media-type algorithm, under which
 * "image/" or "a/b/c" would be canonical; MDK v0.11.0's
 * canonicalize_marmot_media_type (documented as that frozen algorithm)
 * enforces the shared token and length rules instead.  libmarmot follows
 * MDK for interop (review N1; the discrepancy is to be raised upstream). */
static bool
image_media_type_valid(const char *mt)
{
    char *canon = NULL;
    if (!mt || !*mt || strlen(mt) > MARMOT_MEDIA_TYPE_MAX ||
        marmot_media_type_canonicalize(mt, &canon) != MARMOT_OK)
        return false;
    bool same = strcmp(canon, mt) == 0;
    free(canon);
    return same;
}

MarmotError
marmot_group_blossom_image_encode(const MarmotGroupBlossomImage *img, uint8_t **out,
                                  size_t *out_len)
{
    if (!img || !out || !out_len) return MARMOT_ERR_INVALID_ARG;
    *out = NULL;
    *out_len = 0;
    if (img->present && !image_media_type_valid(img->media_type))
        return MARMOT_ERR_MEDIA_INVALID_REFERENCE;
    MlsTlsBuf b;
    if (mls_tls_buf_init(&b, 160) != 0) return MARMOT_ERR_MEMORY;
    int rc;
    if (!img->present) {
        rc = write_vec(&b, NULL, 0) | write_vec(&b, NULL, 0) | write_vec(&b, NULL, 0) |
             write_vec(&b, NULL, 0) | write_vec(&b, NULL, 0);
    } else {
        rc = write_vec(&b, img->image_hash, 32);
        rc |= write_vec(&b, img->image_key, 32);
        rc |= write_vec(&b, img->image_nonce, 12);
        rc |= write_vec(&b, img->image_upload_key, 32);
        rc |= write_vec(&b, img->media_type, strlen(img->media_type));
    }
    if (rc != 0) {
        mls_tls_buf_free(&b);
        return MARMOT_ERR_MEMORY;
    }
    return finish_buf(&b, out, out_len);
}

MarmotError
marmot_group_blossom_image_decode(const uint8_t *data, size_t len,
                                  MarmotGroupBlossomImage *out)
{
    if (!data || !out) return MARMOT_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);
    MlsTlsReader r;
    mls_tls_reader_init(&r, data, len);
    uint8_t *f[5] = { 0 };
    size_t n[5] = { 0 };
    static const size_t max[5] = { 32, 32, 12, 32, MARMOT_MEDIA_TYPE_MAX };
    MarmotError err = MARMOT_OK;
    for (int i = 0; i < 5 && err == MARMOT_OK; i++) {
        int rv = read_vec(&r, max[i], &f[i], &n[i]);
        if (rv != 0) err = VEC_ERR(rv);
    }
    if (err == MARMOT_OK && !mls_tls_reader_done(&r)) err = MARMOT_ERR_MEDIA_INVALID_REFERENCE;
    bool any = n[0] || n[1] || n[2] || n[3] || n[4];
    if (err == MARMOT_OK && any) {
        /* Mixed partial state is invalid; the media type must be canonical. */
        if (n[0] != 32 || n[1] != 32 || n[2] != 12 || n[3] != 32 || n[4] == 0 ||
            memchr(f[4], 0, n[4]) || !image_media_type_valid((const char *)f[4])) {
            err = MARMOT_ERR_MEDIA_INVALID_REFERENCE;
        } else {
            out->present = true;
            memcpy(out->image_hash, f[0], 32);
            memcpy(out->image_key, f[1], 32);
            memcpy(out->image_nonce, f[2], 12);
            memcpy(out->image_upload_key, f[3], 32);
            out->media_type = (char *)f[4];
            f[4] = NULL;
        }
    }
    for (int i = 0; i < 5; i++) {
        if (f[i]) sodium_memzero(f[i], n[i]);
        free(f[i]);
    }
    if (err != MARMOT_OK) marmot_group_blossom_image_clear(out);
    return err;
}

void
marmot_group_blossom_image_clear(MarmotGroupBlossomImage *img)
{
    if (!img) return;
    free(img->media_type);
    sodium_memzero(img, sizeof *img);
}

static int
image_aad(const char *media_type, uint8_t **out, size_t *out_len)
{
    size_t l = sizeof IMAGE_AAD_LABEL - 1, m = strlen(media_type);
    uint8_t *a = malloc(l + 1 + m);
    if (!a) return -1;
    memcpy(a, IMAGE_AAD_LABEL, l);
    a[l] = 0;
    memcpy(a + l + 1, media_type, m);
    *out = a;
    *out_len = l + 1 + m;
    return 0;
}

MarmotError
marmot_group_image_seal(const uint8_t key[32], const uint8_t nonce[12],
                        const uint8_t *plaintext, size_t plaintext_len,
                        const char *media_type, uint8_t **ciphertext, size_t *ciphertext_len)
{
    if (!key || !nonce || !plaintext || !media_type || !ciphertext || !ciphertext_len)
        return MARMOT_ERR_INVALID_ARG;
    *ciphertext = NULL;
    *ciphertext_len = 0;
    if (plaintext_len == 0 || plaintext_len > SIZE_MAX - AEAD_TAG_LEN ||
        !image_media_type_valid(media_type))
        return MARMOT_ERR_INVALID_INPUT;
    uint8_t *aad = NULL;
    size_t aad_len = 0;
    if (image_aad(media_type, &aad, &aad_len) != 0) return MARMOT_ERR_MEMORY;
    uint8_t *ct = malloc(plaintext_len + AEAD_TAG_LEN);
    if (!ct) {
        free(aad);
        return MARMOT_ERR_MEMORY;
    }
    unsigned long long ct_len = 0;
    int rc = crypto_aead_chacha20poly1305_ietf_encrypt(ct, &ct_len, plaintext, plaintext_len,
                                                       aad, aad_len, NULL, nonce, key);
    free(aad);
    if (rc != 0) {
        free(ct);
        return MARMOT_ERR_CRYPTO;
    }
    *ciphertext = ct;
    *ciphertext_len = (size_t)ct_len;
    return MARMOT_OK;
}

/* A secp256k1 secret key: 0 < k < n, big-endian. */
static bool
secp256k1_scalar_valid(const uint8_t k[32])
{
    static const uint8_t order[32] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xfe, 0xba, 0xae, 0xdc, 0xe6, 0xaf, 0x48,
        0xa0, 0x3b, 0xbf, 0xd2, 0x5e, 0x8c, 0xd0, 0x36, 0x41, 0x41 };
    static const uint8_t zero[32] = { 0 };
    return memcmp(k, zero, 32) != 0 && memcmp(k, order, 32) < 0;
}

MarmotError
marmot_group_image_encrypt(const uint8_t *plaintext, size_t plaintext_len,
                           const char *media_type, MarmotGroupBlossomImage *out,
                           uint8_t **ciphertext, size_t *ciphertext_len)
{
    if (!plaintext || !media_type || !out || !ciphertext || !ciphertext_len)
        return MARMOT_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);
    *ciphertext = NULL;
    *ciphertext_len = 0;
    char *canon = NULL;
    MarmotError err = marmot_media_type_canonicalize(media_type, &canon);
    if (err != MARMOT_OK) return err;
    /* Fresh key, nonce and Blossom write key for every image. */
    randombytes_buf(out->image_key, sizeof out->image_key);
    randombytes_buf(out->image_nonce, sizeof out->image_nonce);
    do {
        randombytes_buf(out->image_upload_key, sizeof out->image_upload_key);
    } while (!secp256k1_scalar_valid(out->image_upload_key));
    err = marmot_group_image_seal(out->image_key, out->image_nonce, plaintext, plaintext_len,
                                  canon, ciphertext, ciphertext_len);
    if (err != MARMOT_OK) {
        free(canon);
        marmot_group_blossom_image_clear(out);
        return err;
    }
    SHA256(*ciphertext, *ciphertext_len, out->image_hash);
    out->media_type = canon;
    out->present = true;
    return MARMOT_OK;
}

MarmotError
marmot_group_image_decrypt(const MarmotGroupBlossomImage *img, const uint8_t *ciphertext,
                           size_t ciphertext_len, uint8_t **plaintext, size_t *plaintext_len)
{
    if (!img || !ciphertext || !plaintext || !plaintext_len) return MARMOT_ERR_INVALID_ARG;
    *plaintext = NULL;
    *plaintext_len = 0;
    if (!img->present || !image_media_type_valid(img->media_type))
        return MARMOT_ERR_MEDIA_INVALID_REFERENCE;
    uint8_t got[32];
    SHA256(ciphertext, ciphertext_len, got);
    if (sodium_memcmp(got, img->image_hash, 32) != 0) return MARMOT_ERR_MEDIA_CIPHERTEXT_HASH;
    if (ciphertext_len < AEAD_TAG_LEN) return MARMOT_ERR_MEDIA_DECRYPT;
    uint8_t *aad = NULL;
    size_t aad_len = 0;
    if (image_aad(img->media_type, &aad, &aad_len) != 0) return MARMOT_ERR_MEMORY;
    uint8_t *pt = malloc(ciphertext_len - AEAD_TAG_LEN + 1);
    if (!pt) {
        free(aad);
        return MARMOT_ERR_MEMORY;
    }
    unsigned long long pt_len = 0;
    int rc = crypto_aead_chacha20poly1305_ietf_decrypt(pt, &pt_len, NULL, ciphertext,
                                                       ciphertext_len, aad, aad_len,
                                                       img->image_nonce, img->image_key);
    free(aad);
    if (rc != 0) {
        free(pt);
        return MARMOT_ERR_MEDIA_DECRYPT;
    }
    *plaintext = pt;
    *plaintext_len = (size_t)pt_len;
    return MARMOT_OK;
}

/* ── 0x8007 avatar URL: a strict WHATWG-serializer subset ──────────────── */

typedef struct {
    char  *s;
    size_t len, cap;
    bool   oom;
} Str;

static void
str_putn(Str *b, const char *p, size_t n)
{
    if (b->oom) return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 64;
        while (cap < b->len + n + 1) cap *= 2;
        char *g = realloc(b->s, cap);
        if (!g) { b->oom = true; return; }
        b->s = g;
        b->cap = cap;
    }
    memcpy(b->s + b->len, p, n);
    b->len += n;
    b->s[b->len] = 0;
}

static void str_put(Str *b, const char *p) { str_putn(b, p, strlen(p)); }
static void str_putc(Str *b, char c) { str_putn(b, &c, 1); }

static void
str_pct(Str *b, uint8_t c)
{
    char e[4];
    snprintf(e, sizeof e, "%%%02X", c);
    str_putn(b, e, 3);
}

/* WHATWG IPv4 number: decimal, 0x hex, or leading-0 octal. */
static bool
ipv4_number(const char *p, size_t n, uint64_t *out)
{
    unsigned base = 10;
    if (n >= 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; n -= 2; }
    else if (n >= 2 && p[0] == '0') { base = 8; p++; n--; }
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        int d;
        char c = p[i];
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        if ((unsigned)d >= base) return false;
        v = v * base + (unsigned)d;
        if (v > 0xffffffffull) return false;
    }
    *out = v;
    return true;
}

/* host is lowercased ASCII.  Returns 1 IPv4 written, 0 not IPv4, -1 invalid. */
static int
host_ipv4(const char *host, Str *b)
{
    size_t n = strlen(host);
    size_t end = n;
    if (end > 0 && host[end - 1] == '.') end--;   /* one trailing dot is ignored */
    size_t last = end;
    while (last > 0 && host[last - 1] != '.') last--;
    const char *lp = host + last;
    size_t ll = end - last;
    if (ll == 0) return 0;
    bool digits = true;
    for (size_t i = 0; i < ll; i++) if (lp[i] < '0' || lp[i] > '9') digits = false;
    uint64_t probe;
    if (!digits && !(ll >= 2 && lp[0] == '0' && lp[1] == 'x' && ipv4_number(lp, ll, &probe)))
        return 0;   /* does not end in a number: a domain */
    uint64_t parts[4];
    size_t np = 0, s = 0;
    for (size_t i = 0; i <= end; i++) {
        if (i == end || host[i] == '.') {
            if (np == 4 || i == s || !ipv4_number(host + s, i - s, &parts[np])) return -1;
            np++;
            s = i + 1;
        }
    }
    uint64_t v = 0;
    for (size_t i = 0; i + 1 < np; i++) {
        if (parts[i] > 255) return -1;
        v = v << 8 | parts[i];
    }
    if (parts[np - 1] >= (1ull << (8 * (5 - np)))) return -1;
    v = (v << (8 * (5 - np))) | parts[np - 1];
    char out[16];
    snprintf(out, sizeof out, "%u.%u.%u.%u", (unsigned)(v >> 24) & 255,
             (unsigned)(v >> 16) & 255, (unsigned)(v >> 8) & 255, (unsigned)v & 255);
    str_put(b, out);
    return 1;
}

/* RFC 5952-style serialization, as the WHATWG IPv6 serializer. */
static bool
host_ipv6(const char *p, size_t n, Str *b)
{
    char tmp[64];
    if (n == 0 || n >= sizeof tmp || memchr(p, '%', n)) return false;
    memcpy(tmp, p, n);
    tmp[n] = 0;
    /* An embedded IPv4 tail is checked here, as the WHATWG IPv6 parser
     * does it (four decimal parts 0..255, no leading zero): inet_pton
     * differs by platform (macOS accepts "::1.2.3.04"). */
    const char *dot = memchr(tmp, '.', n);
    if (dot) {
        const char *v4 = dot;
        while (v4 > tmp && v4[-1] != ':') v4--;
        int parts = 0;
        for (const char *q = v4; *q;) {
            const char *s = q;
            unsigned v = 0;
            while (*q >= '0' && *q <= '9' && q - s < 4) v = v * 10 + (unsigned)(*q++ - '0');
            if (q == s || q - s > 3 || v > 255 || (q - s > 1 && *s == '0')) return false;
            parts++;
            if (*q == '.') q++;
            else if (*q) return false;
        }
        if (parts != 4 || tmp[n - 1] == '.') return false;
    }
    uint8_t a[16];
    if (inet_pton(AF_INET6, tmp, a) != 1) return false;
    uint16_t w[8];
    for (int i = 0; i < 8; i++) w[i] = (uint16_t)(a[2 * i] << 8 | a[2 * i + 1]);
    int best = -1, best_len = 1;
    for (int i = 0; i < 8;) {
        if (w[i] != 0) { i++; continue; }
        int j = i;
        while (j < 8 && w[j] == 0) j++;
        if (j - i > best_len) { best = i; best_len = j - i; }
        i = j;
    }
    str_putc(b, '[');
    bool ignore0 = false;
    for (int i = 0; i < 8; i++) {
        if (ignore0 && w[i] == 0) continue;
        ignore0 = false;
        if (i == best) {
            str_put(b, i == 0 ? "::" : ":");
            ignore0 = true;
            continue;
        }
        char h[8];
        snprintf(h, sizeof h, "%x", w[i]);
        str_put(b, h);
        if (i != 7) str_putc(b, ':');
    }
    str_putc(b, ']');
    return true;
}

static bool
is_single_dot(const char *s, size_t n)
{
    return (n == 1 && s[0] == '.') || (n == 3 && strncasecmp(s, "%2e", 3) == 0);
}

static bool
is_double_dot(const char *s, size_t n)
{
    return (n == 2 && memcmp(s, "..", 2) == 0) ||
           (n == 4 && (strncasecmp(s, ".%2e", 4) == 0 || strncasecmp(s, "%2e.", 4) == 0)) ||
           (n == 6 && strncasecmp(s, "%2e%2e", 6) == 0);
}

/*
 * The two URL profiles of the group components, both strict WHATWG
 * serializer subsets (nostrc-u7cb review M3):
 *   URL_AVATAR    0x8007 avatar URL: https, query allowed;
 *   URL_ENDPOINT  0x800b default blob endpoint (MDK
 *                 validate_and_normalize_blob_endpoint_url_v2): http or
 *                 https, no query.
 * Neither allows userinfo or a fragment; both are at most 2048 bytes.
 */
typedef enum { URL_AVATAR, URL_ENDPOINT } UrlProfile;

_Static_assert(MARMOT_GROUP_AVATAR_URL_MAX == MARMOT_MEDIA_POLICY_ENDPOINT_URL_MAX,
               "one URL length bound for both profiles");

/*
 * Normalize @raw under @profile.  On failure, *whatwg_invalid (if given)
 * tells "no WHATWG parser accepts this host" (review L2: an IPv4 host that
 * ends in a number but fails the IPv4 parser -- an octet over 255, more
 * than four parts, a non-numeric part --, or a bracketed host that is no
 * IPv6 address or carries a zone id) from "outside libmarmot's verifiable
 * subset" (IDNA, '_' hosts, ...).  Only the first makes a stored URL
 * invalid; the second leaves it unverified.
 */
static MarmotError
url_normalize_ex(const char *raw, UrlProfile profile, char **out, bool *whatwg_invalid)
{
    if (whatwg_invalid) *whatwg_invalid = false;
    if (!raw || !out) return MARMOT_ERR_INVALID_ARG;
    *out = NULL;
    size_t raw_len = strlen(raw);
    if (raw_len == 0 || raw_len > MARMOT_GROUP_AVATAR_URL_MAX ||
        !marmot_media_utf8_valid((const uint8_t *)raw, raw_len))
        return MARMOT_ERR_INVALID_INPUT;

    /* WHATWG: strip leading/trailing C0 and space, drop every tab/LF/CR. */
    size_t a = 0, z = raw_len;
    while (a < z && (uint8_t)raw[a] <= 0x20) a++;
    while (z > a && (uint8_t)raw[z - 1] <= 0x20) z--;
    char *in = malloc(z - a + 1);
    if (!in) return MARMOT_ERR_MEMORY;
    size_t n = 0;
    for (size_t i = a; i < z; i++)
        if (raw[i] != '\t' && raw[i] != '\n' && raw[i] != '\r') in[n++] = raw[i];
    in[n] = 0;

    MarmotError err = MARMOT_ERR_INVALID_INPUT;
    Str b = { 0 };
    char *host = NULL;
    const char *p = in;
    bool https;
    if (n >= 6 && strncasecmp(p, "https:", 6) == 0) {
        https = true;
        p += 6;
    } else if (profile == URL_ENDPOINT && n >= 5 && strncasecmp(p, "http:", 5) == 0) {
        https = false;
        p += 5;
    } else {
        goto done;
    }
    while (*p == '/' || *p == '\\') p++;

    /* Authority: no userinfo, a host, an optional port. */
    size_t auth = strcspn(p, "/\\?#");
    if (memchr(p, '@', auth)) goto done;
    const char *ae = p + auth, *port = NULL;
    str_put(&b, https ? "https://" : "http://");
    if (*p == '[') {
        const char *close = memchr(p, ']', auth);
        if (!close) goto done;
        if (!host_ipv6(p + 1, (size_t)(close - p - 1), &b)) {
            /* Every WHATWG IPv6 serialization is an inet_pton address;
             * anything else in brackets (a zone id, a malformed literal)
             * is no valid host at all. */
            if (whatwg_invalid) *whatwg_invalid = true;
            goto done;
        }
        if (close + 1 < ae) {
            if (close[1] != ':') goto done;
            port = close + 2;
        }
    } else {
        const char *colon = memchr(p, ':', auth);
        const char *he = colon ? colon : ae;
        if (he == p) goto done;
        host = malloc((size_t)(he - p) + 1);
        if (!host) { err = MARMOT_ERR_MEMORY; goto done; }
        size_t hn = 0;
        for (const char *q = p; q < he; q++) {
            uint8_t c = (uint8_t)*q;
            if (c >= 'A' && c <= 'Z') c = (uint8_t)(c - 'A' + 'a');
            /* ASCII letters, digits, '-' and '.' only: anything else needs
             * IDNA or percent-decoding, which this subset does not do. */
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.'))
                goto done;
            host[hn++] = (char)c;
        }
        host[hn] = 0;
        int v4 = host_ipv4(host, &b);
        if (v4 < 0) {
            /* Ends in a number, yet the WHATWG IPv4 parser fails. */
            if (whatwg_invalid) *whatwg_invalid = true;
            goto done;
        }
        if (v4 == 0) {
            /* A domain: non-empty labels (one trailing dot allowed), no
             * punycode labels (they need UTS #46 validation). */
            size_t s = 0;
            for (size_t i = 0; i <= hn; i++) {
                if (i == hn || host[i] == '.') {
                    if (i == s && !(i == hn && hn > 0 && host[hn - 1] == '.')) goto done;
                    if (i - s >= 4 && strncmp(host + s, "xn--", 4) == 0) goto done;
                    s = i + 1;
                }
            }
            str_put(&b, host);
        }
        if (colon) port = colon + 1;
    }
    if (port) {
        unsigned long v = 0;
        if (port == ae) goto port_done;   /* "host:" is no port */
        for (const char *q = port; q < ae; q++) {
            if (*q < '0' || *q > '9') goto done;
            v = v * 10 + (unsigned long)(*q - '0');
            if (v > 65535) goto done;
        }
        if (v != (https ? 443u : 80u)) {   /* the scheme's default port is dropped */
            char ps[8];
            snprintf(ps, sizeof ps, ":%lu", v);
            str_put(&b, ps);
        }
    }
port_done:
    p = ae;

    /* Path (WHATWG path state): '\' is '/', dot segments pop or are
     * dropped, and the path percent-encode set is encoded.  Each pushed
     * segment is "/" + bytes; `starts` remembers where each one begins. */
    {
        size_t pe = strcspn(p, "?#");
        const char *pend = p + pe;
        size_t *starts = malloc((pe + 2) * sizeof *starts);
        size_t depth = 0;
        if (!starts) { err = MARMOT_ERR_MEMORY; goto done; }
        const char *seg = p == pend ? pend : p + 1;   /* skip the leading '/' */
        for (;;) {
            const char *se = seg;
            while (se < pend && *se != '/' && *se != '\\') se++;
            size_t sl = (size_t)(se - seg);
            bool last = se == pend;
            bool dd = is_double_dot(seg, sl), sd = !dd && is_single_dot(seg, sl);
            if (dd && depth > 0) {
                /* ".." over a drive-letter-like segment ("b:"): url 2.5.8
                 * keeps it, ada-url pops it -- no single canonical form. */
                const char *popped = b.s + starts[depth - 1] + 1;
                size_t pl = b.len - starts[depth - 1] - 1;
                if (pl == 2 && popped[1] == ':' &&
                    ((popped[0] >= 'a' && popped[0] <= 'z') || (popped[0] >= 'A' && popped[0] <= 'Z'))) {
                    free(starts);
                    goto done;
                }
                b.len = starts[--depth];
            }
            if ((dd || sd) && last) {
                starts[depth++] = b.len;
                str_putc(&b, '/');
            } else if (!dd && !sd) {
                starts[depth++] = b.len;
                str_putc(&b, '/');
                for (const char *q = seg; q < se; q++) {
                    uint8_t c = (uint8_t)*q;
                    if (c == '^' || c == '|' || c == '[' || c == ']') {
                        free(starts);
                        goto done;   /* serializer behaviour differs by version */
                    }
                    if (c < 0x20 || c > 0x7e || c == ' ' || c == '"' || c == '<' ||
                        c == '>' || c == '`' || c == '{' || c == '}')
                        str_pct(&b, c);
                    else
                        str_putc(&b, (char)c);
                }
            }
            if (last) break;
            seg = se + 1;
        }
        free(starts);
        if (b.oom) { err = MARMOT_ERR_MEMORY; goto done; }
        if (b.s) b.s[b.len] = 0;
        p = pend;
    }
    if (*p == '?' && profile == URL_ENDPOINT) goto done;   /* endpoints have no query */
    if (*p == '?') {
        str_putc(&b, '?');
        for (p++; *p && *p != '#'; p++) {
            uint8_t c = (uint8_t)*p;
            if (c == '^' || c == '|' || c == '[' || c == ']' || c == '\\' || c == '{' ||
                c == '}' || c == '`')
                goto done;
            if (c < 0x20 || c > 0x7e || c == ' ' || c == '"' || c == '<' || c == '>' ||
                c == '\'')
                str_pct(&b, c);
            else
                str_putc(&b, (char)c);
        }
    }
    if (*p == '#') goto done;   /* fragments are invalid */
    if (b.oom) { err = MARMOT_ERR_MEMORY; goto done; }
    if (b.len > MARMOT_GROUP_AVATAR_URL_MAX) goto done;
    *out = b.s;
    b.s = NULL;
    err = MARMOT_OK;
done:
    free(b.s);
    free(host);
    free(in);
    return err;
}

static MarmotError
url_normalize(const char *raw, UrlProfile profile, char **out)
{
    return url_normalize_ex(raw, profile, out, NULL);
}

MarmotError
marmot_group_avatar_url_normalize(const char *raw, char **out)
{
    return url_normalize(raw, URL_AVATAR, out);
}

MarmotError
marmot_group_avatar_url_encode(const MarmotGroupAvatarUrl *av, uint8_t **out, size_t *out_len)
{
    if (!av || !out || !out_len) return MARMOT_ERR_INVALID_ARG;
    *out = NULL;
    *out_len = 0;
    bool has_url = av->url && *av->url;
    if (!has_url && (av->dim_len || av->thumbhash_len)) return MARMOT_ERR_MEDIA_INVALID_REFERENCE;
    if (av->dim_len > MARMOT_GROUP_AVATAR_HINT_MAX ||
        av->thumbhash_len > MARMOT_GROUP_AVATAR_HINT_MAX ||
        (av->dim_len && !av->dim) || (av->thumbhash_len && !av->thumbhash))
        return MARMOT_ERR_MEDIA_INVALID_REFERENCE;
    char *url = NULL;
    if (has_url && av->url_unverified) return MARMOT_ERR_MEDIA_INVALID_REFERENCE;
    if (has_url) {
        MarmotError err = marmot_group_avatar_url_normalize(av->url, &url);
        if (err != MARMOT_OK)
            return err == MARMOT_ERR_MEMORY ? err : MARMOT_ERR_MEDIA_INVALID_REFERENCE;
    }
    MlsTlsBuf b;
    if (mls_tls_buf_init(&b, 64) != 0) {
        free(url);
        return MARMOT_ERR_MEMORY;
    }
    int rc = write_vec(&b, url, url ? strlen(url) : 0);
    rc |= write_vec(&b, av->dim, av->dim_len);
    rc |= write_vec(&b, av->thumbhash, av->thumbhash_len);
    free(url);
    if (rc != 0) {
        mls_tls_buf_free(&b);
        return MARMOT_ERR_MEMORY;
    }
    return finish_buf(&b, out, out_len);
}

typedef enum { AVATAR_VALID, AVATAR_INVALID, AVATAR_UNVERIFIED } AvatarClass;

/*
 * Stored URL bytes (0x8007 avatar, 0x800b endpoint), judged three ways
 * (nostrc-u7cb review M3).  WHATWG is a living standard and its
 * implementations already differ (url 2.5.8 keeps '^' raw in a path,
 * ada-url encodes it; they treat "b:" segments apart), so libmarmot only
 * calls a value invalid when no serializer version can have produced it,
 * and accepts the rest unverified instead of forking the group.
 */
static AvatarClass
url_classify(const char *url, size_t len, UrlProfile profile)
{
    for (size_t i = 0; i < len; i++)
        if ((uint8_t)url[i] <= 0x20 || (uint8_t)url[i] >= 0x7f)
            return AVATAR_INVALID;   /* a serialization is printable ASCII */
    size_t scheme_len;
    unsigned long default_port;
    if (len >= 8 && memcmp(url, "https://", 8) == 0) {
        scheme_len = 8;
        default_port = 443;
    } else if (profile == URL_ENDPOINT && len >= 7 && memcmp(url, "http://", 7) == 0) {
        scheme_len = 7;
        default_port = 80;
    } else {
        return AVATAR_INVALID;
    }
    if (memchr(url, '#', len)) return AVATAR_INVALID;
    /* Any '?' starts a query (it ends the authority and the path). */
    if (profile == URL_ENDPOINT && memchr(url, '?', len)) return AVATAR_INVALID;
    const char *a = url + scheme_len, *end = url + len;
    const char *ae = a;
    while (ae < end && *ae != '/' && *ae != '?') ae++;
    if (ae == a || ae == end || *ae != '/') return AVATAR_INVALID;   /* host; path "/" */
    const char *host_end = ae;
    if (*a == '[') {
        const char *close = memchr(a, ']', (size_t)(ae - a));
        if (!close) return AVATAR_INVALID;
        host_end = close + 1;
    } else {
        const char *q = a;
        /* Serialized hosts are lowercase and percent-decoded; "[]<>\\^" are
         * forbidden host code points in every WHATWG version. */
        for (; q < ae && *q != ':'; q++)
            if (*q == '@' || *q == '%' || (*q >= 'A' && *q <= 'Z') || strchr("[]<>\\^", *q))
                return AVATAR_INVALID;
        host_end = q;
        if (host_end == a) return AVATAR_INVALID;
    }
    if (memchr(a, '@', (size_t)(ae - a))) return AVATAR_INVALID;
    if (host_end < ae) {   /* ":port": shortest decimal, not the default */
        if (*host_end != ':' || host_end + 1 == ae) return AVATAR_INVALID;
        const char *pd = host_end + 1;
        if (*pd == '0' && ae - pd > 1) return AVATAR_INVALID;
        unsigned long v = 0;
        for (const char *q = pd; q < ae; q++) {
            if (*q < '0' || *q > '9') return AVATAR_INVALID;
            v = v * 10 + (unsigned long)(*q - '0');
            if (v > 65535) return AVATAR_INVALID;
        }
        if (v == default_port) return AVATAR_INVALID;
    }
    const char *qs = memchr(ae, '?', (size_t)(end - ae));
    const char *pe = qs ? qs : end;
    for (const char *seg = ae + 1;;) {   /* path */
        const char *se = seg;
        while (se < pe && *se != '/') se++;
        if (is_single_dot(seg, (size_t)(se - seg)) || is_double_dot(seg, (size_t)(se - seg)))
            return AVATAR_INVALID;
        if (se >= pe) break;
        seg = se + 1;
    }
    for (const char *q = ae; q < pe; q++)
        if (strchr("\\\"<>`{}", *q)) return AVATAR_INVALID;
    for (const char *q = pe; q < end; q++)
        if (strchr("\"<>'", *q)) return AVATAR_INVALID;

    /* Inside libmarmot's subset its normalization is exact; outside it,
     * this decoder cannot tell. */
    char *norm = NULL;
    bool whatwg_invalid = false;
    MarmotError nerr = url_normalize_ex(url, profile, &norm, &whatwg_invalid);
    if (nerr != MARMOT_OK) return whatwg_invalid ? AVATAR_INVALID : AVATAR_UNVERIFIED;
    bool same = strcmp(norm, url) == 0;
    free(norm);
    return same ? AVATAR_VALID : AVATAR_INVALID;
}

MarmotError
marmot_group_avatar_url_decode(const uint8_t *data, size_t len, MarmotGroupAvatarUrl *out)
{
    if (!data || !out) return MARMOT_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);
    MlsTlsReader r;
    mls_tls_reader_init(&r, data, len);
    uint8_t *url = NULL;
    size_t url_len = 0;
    MarmotError err = MARMOT_ERR_MEDIA_INVALID_REFERENCE;
    int rv;
    if ((rv = read_vec(&r, MARMOT_GROUP_AVATAR_URL_MAX, &url, &url_len)) != 0 ||
        (rv = read_vec(&r, MARMOT_GROUP_AVATAR_HINT_MAX, &out->dim, &out->dim_len)) != 0 ||
        (rv = read_vec(&r, MARMOT_GROUP_AVATAR_HINT_MAX, &out->thumbhash,
                       &out->thumbhash_len)) != 0) {
        err = VEC_ERR(rv);
        goto fail;
    }
    if (!mls_tls_reader_done(&r)) goto fail;
    if (url_len == 0) {
        if (out->dim_len || out->thumbhash_len) goto fail;
        return MARMOT_OK;
    }
    if (memchr(url, 0, url_len) || !marmot_media_utf8_valid(url, url_len)) goto fail;
    /* A decoder never repairs: the stored bytes are kept as they are. */
    switch (url_classify((const char *)url, url_len, URL_AVATAR)) {
    case AVATAR_INVALID:
        goto fail;
    case AVATAR_UNVERIFIED:
        out->url_unverified = true;
        break;
    case AVATAR_VALID:
        break;
    }
    out->url = (char *)url;
    return MARMOT_OK;
fail:
    free(url);
    marmot_group_avatar_url_clear(out);
    return err;
}

void
marmot_group_avatar_url_clear(MarmotGroupAvatarUrl *av)
{
    if (!av) return;
    free(av->url);
    free(av->dim);
    free(av->thumbhash);
    memset(av, 0, sizeof *av);
}

MarmotGroupAvatarSource
marmot_group_avatar_select(const MarmotGroupAvatarUrl *avatar_url,
                           const MarmotGroupBlossomImage *blossom_image)
{
    if (avatar_url && avatar_url->url && *avatar_url->url)
        return avatar_url->url_unverified ? MARMOT_GROUP_AVATAR_URL_PLACEHOLDER
                                          : MARMOT_GROUP_AVATAR_URL;
    if (blossom_image && blossom_image->present) return MARMOT_GROUP_AVATAR_BLOSSOM;
    return MARMOT_GROUP_AVATAR_NONE;
}

/* ── 0x8006 marmot.group.agent-text-stream.quic.v1 (nostrc-qp24.5.2) ──── */

static bool
agent_policy_valid(const MarmotAgentTextStreamPolicy *p)
{
    /* MDK AgentTextStreamQuicPolicyV1::validate, in its order. */
    return p->required_member_roles != 0 &&
           (p->required_member_roles & ~MARMOT_AGENT_STREAM_ROLE_MASK) == 0 &&
           (p->allowed_member_roles & ~MARMOT_AGENT_STREAM_ROLE_MASK) == 0 &&
           (p->required_member_roles & ~p->allowed_member_roles) == 0 &&
           p->max_plaintext_frame_len != 0 &&
           p->max_plaintext_frame_len <= MARMOT_AGENT_STREAM_MAX_FRAME_LEN &&
           p->replay_ttl_secs <= MARMOT_AGENT_STREAM_MAX_REPLAY_TTL &&
           p->padding_bucket_bytes <= MARMOT_AGENT_STREAM_MAX_PADDING;
}

MarmotError
marmot_agent_text_stream_policy_decode(const uint8_t *data, size_t len,
                                       MarmotAgentTextStreamPolicy *out)
{
    if (!out || (len > 0 && !data)) return MARMOT_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);
    if (len != MARMOT_AGENT_STREAM_STATE_LEN) return MARMOT_ERR_EXTENSION_FORMAT;
    MarmotAgentTextStreamPolicy p = {
        .required_member_roles = data[0],
        .allowed_member_roles = data[1],
        .max_plaintext_frame_len = (uint32_t)data[2] << 24 | (uint32_t)data[3] << 16 |
                                   (uint32_t)data[4] << 8 | data[5],
        .replay_ttl_secs = (uint32_t)data[6] << 24 | (uint32_t)data[7] << 16 |
                           (uint32_t)data[8] << 8 | data[9],
        .padding_bucket_bytes = (uint16_t)(data[10] << 8 | data[11]),
    };
    if (!agent_policy_valid(&p)) return MARMOT_ERR_EXTENSION_FORMAT;
    *out = p;
    return MARMOT_OK;
}

MarmotError
marmot_agent_text_stream_policy_encode(const MarmotAgentTextStreamPolicy *p,
                                       uint8_t out[MARMOT_AGENT_STREAM_STATE_LEN])
{
    if (!p || !out) return MARMOT_ERR_INVALID_ARG;
    if (!agent_policy_valid(p)) return MARMOT_ERR_INVALID_INPUT;
    out[0] = p->required_member_roles;
    out[1] = p->allowed_member_roles;
    for (int i = 0; i < 4; i++) {
        out[2 + i] = (uint8_t)(p->max_plaintext_frame_len >> (24 - 8 * i));
        out[6 + i] = (uint8_t)(p->replay_ttl_secs >> (24 - 8 * i));
    }
    out[10] = (uint8_t)(p->padding_bucket_bytes >> 8);
    out[11] = (uint8_t)p->padding_bucket_bytes;
    return MARMOT_OK;
}

MarmotAgentTextStreamPolicy
marmot_agent_text_stream_policy_user_to_agent_default(void)
{
    return (MarmotAgentTextStreamPolicy){
        .required_member_roles = MARMOT_AGENT_STREAM_ROLE_RECEIVE,
        .allowed_member_roles = MARMOT_AGENT_STREAM_ROLE_RECEIVE | MARMOT_AGENT_STREAM_ROLE_SEND,
        .max_plaintext_frame_len = 4096,
        .replay_ttl_secs = 0,
        .padding_bucket_bytes = 0,
    };
}

/* ── 0x800b marmot.group.encrypted-media.v2 (nostrc-m6tp) ──────────────── */

/* MDK ENCRYPTED_MEDIA_*_VECTOR_MAX_LEN: the outer vectors' bounds. */
#define POLICY_FORMAT_MAX     64u
#define POLICY_KINDS_VEC_MAX  (MARMOT_MEDIA_POLICY_MAX_LOCATOR_KINDS * \
                               (MARMOT_MEDIA_POLICY_LOCATOR_KIND_MAX + 2))
#define POLICY_ENDPOINTS_VEC_MAX \
    (MARMOT_MEDIA_POLICY_MAX_ENDPOINTS * \
     ((MARMOT_MEDIA_POLICY_LOCATOR_KIND_MAX + 2) + (MARMOT_MEDIA_POLICY_ENDPOINT_URL_MAX + 2) + 2))

/* MDK validate_locator_kind: 1..64 bytes of [a-z0-9-]. */
static bool
locator_kind_valid(const uint8_t *s, size_t n)
{
    if (n == 0 || n > MARMOT_MEDIA_POLICY_LOCATOR_KIND_MAX) return false;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '-'))
            return false;
    return true;
}

static bool
strv_contains(char *const *v, size_t n, const char *s)
{
    for (size_t i = 0; i < n; i++)
        if (strcmp(v[i], s) == 0) return true;
    return false;
}

void
marmot_group_media_policy_clear(MarmotGroupMediaPolicy *p)
{
    if (!p) return;
    for (size_t i = 0; p->allowed_locator_kinds && i < p->allowed_locator_kind_count; i++)
        free(p->allowed_locator_kinds[i]);
    free(p->allowed_locator_kinds);
    for (size_t i = 0; p->default_blob_endpoints && i < p->default_blob_endpoint_count; i++) {
        free(p->default_blob_endpoints[i].locator_kind);
        free(p->default_blob_endpoints[i].base_url);
    }
    free(p->default_blob_endpoints);
    memset(p, 0, sizeof *p);
}

MarmotError
marmot_group_media_policy_decode(const uint8_t *data, size_t len, MarmotGroupMediaPolicy *out)
{
    if (!out || (len > 0 && !data)) return MARMOT_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);
    const MarmotError bad = MARMOT_ERR_MEDIA_INVALID_REFERENCE;
    MarmotError err = bad;
    uint8_t *format = NULL, *kinds = NULL, *endpoints = NULL;
    size_t format_len = 0, kinds_len = 0, endpoints_len = 0;
    MlsTlsReader r;
    mls_tls_reader_init(&r, data, len);
    int rv;
    if ((rv = read_vec(&r, POLICY_FORMAT_MAX, &format, &format_len)) != 0 ||
        (rv = read_vec(&r, POLICY_KINDS_VEC_MAX, &kinds, &kinds_len)) != 0 ||
        (rv = read_vec(&r, POLICY_ENDPOINTS_VEC_MAX, &endpoints, &endpoints_len)) != 0) {
        err = VEC_ERR(rv);
        goto out;
    }
    if (!mls_tls_reader_done(&r)) goto out;
    if (format_len != strlen(MARMOT_MEDIA_V2_VERSION) ||
        memcmp(format, MARMOT_MEDIA_V2_VERSION, format_len) != 0)
        goto out;

    /* allowed_locator_kinds: concatenated vectors, unique, 1..16. */
    out->allowed_locator_kinds = calloc(MARMOT_MEDIA_POLICY_MAX_LOCATOR_KINDS + 1, sizeof(char *));
    out->default_blob_endpoints = calloc(MARMOT_MEDIA_POLICY_MAX_ENDPOINTS,
                                         sizeof(MarmotMediaBlobEndpoint));
    if (!out->allowed_locator_kinds || !out->default_blob_endpoints) {
        err = MARMOT_ERR_MEMORY;
        goto out;
    }
    MlsTlsReader kr;
    mls_tls_reader_init(&kr, kinds, kinds_len);
    while (!mls_tls_reader_done(&kr)) {
        uint8_t *k = NULL;
        size_t kl = 0;
        if (out->allowed_locator_kind_count == MARMOT_MEDIA_POLICY_MAX_LOCATOR_KINDS) goto out;
        if ((rv = read_vec(&kr, MARMOT_MEDIA_POLICY_LOCATOR_KIND_MAX, &k, &kl)) != 0) {
            err = VEC_ERR(rv);
            goto out;
        }
        if (!k || !locator_kind_valid(k, kl) ||
            strv_contains(out->allowed_locator_kinds, out->allowed_locator_kind_count,
                          (const char *)k)) {
            free(k);
            goto out;
        }
        out->allowed_locator_kinds[out->allowed_locator_kind_count++] = (char *)k;
    }
    if (out->allowed_locator_kind_count == 0) goto out;

    /* default_blob_endpoints: (kind, base_url) pairs, unique, 1..16. */
    MlsTlsReader er;
    mls_tls_reader_init(&er, endpoints, endpoints_len);
    while (!mls_tls_reader_done(&er)) {
        if (out->default_blob_endpoint_count == MARMOT_MEDIA_POLICY_MAX_ENDPOINTS) goto out;
        MarmotMediaBlobEndpoint *ep = &out->default_blob_endpoints[out->default_blob_endpoint_count];
        uint8_t *k = NULL, *u = NULL;
        size_t kl = 0, ul = 0;
        if ((rv = read_vec(&er, MARMOT_MEDIA_POLICY_LOCATOR_KIND_MAX, &k, &kl)) != 0) {
            err = VEC_ERR(rv);
            goto out;
        }
        if ((rv = read_vec(&er, MARMOT_MEDIA_POLICY_ENDPOINT_URL_MAX, &u, &ul)) != 0) {
            err = VEC_ERR(rv);
            free(k);
            goto out;
        }
        /* The kind is checked before the URL, as MDK does. */
        bool ok = k && locator_kind_valid(k, kl) &&
                  strv_contains(out->allowed_locator_kinds, out->allowed_locator_kind_count,
                                (const char *)k) &&
                  u && !memchr(u, 0, ul) && marmot_media_utf8_valid(u, ul);
        AvatarClass cls = ok ? url_classify((const char *)u, ul, URL_ENDPOINT) : AVATAR_INVALID;
        if (cls == AVATAR_INVALID) {
            free(k);
            free(u);
            goto out;
        }
        for (size_t i = 0; i < out->default_blob_endpoint_count; i++)
            if (strcmp(out->default_blob_endpoints[i].locator_kind, (const char *)k) == 0 &&
                strcmp(out->default_blob_endpoints[i].base_url, (const char *)u) == 0) {
                free(k);
                free(u);
                goto out;
            }
        ep->locator_kind = (char *)k;
        ep->base_url = (char *)u;
        ep->base_url_unverified = cls == AVATAR_UNVERIFIED;
        out->default_blob_endpoint_count++;
    }
    if (out->default_blob_endpoint_count == 0) goto out;
    err = MARMOT_OK;
out:
    free(format);
    free(kinds);
    free(endpoints);
    if (err != MARMOT_OK) marmot_group_media_policy_clear(out);
    return err;
}

/* MDK normalize_locator_kind: trimmed (Unicode whitespace in MDK; ASCII
 * here, since anything else then fails the charset), ASCII-lowercased. */
static char *
locator_kind_normalize(const char *s)
{
    if (!s) return NULL;
    size_t a = 0, z = strlen(s);
    while (a < z && strchr(" \t\n\v\f\r", s[a])) a++;
    while (z > a && strchr(" \t\n\v\f\r", s[z - 1])) z--;
    char *k = malloc(z - a + 1);
    if (!k) return NULL;
    for (size_t i = a; i < z; i++)
        k[i - a] = (s[i] >= 'A' && s[i] <= 'Z') ? (char)(s[i] - 'A' + 'a') : s[i];
    k[z - a] = 0;
    return k;
}

MarmotError
marmot_group_media_policy_encode(const MarmotGroupMediaPolicy *p, uint8_t **out, size_t *out_len)
{
    if (!p || !out || !out_len || (p->allowed_locator_kind_count && !p->allowed_locator_kinds) ||
        (p->default_blob_endpoint_count && !p->default_blob_endpoints))
        return MARMOT_ERR_INVALID_ARG;
    *out = NULL;
    *out_len = 0;
    MarmotError err = MARMOT_ERR_INVALID_INPUT;
    MarmotGroupMediaPolicy n = { 0 };
    MlsTlsBuf kinds = { 0 }, endpoints = { 0 }, b = { 0 };
    n.allowed_locator_kinds = calloc(p->allowed_locator_kind_count + 1, sizeof(char *));
    n.default_blob_endpoints = calloc(p->default_blob_endpoint_count + 1,
                                      sizeof(MarmotMediaBlobEndpoint));
    if (!n.allowed_locator_kinds || !n.default_blob_endpoints) {
        err = MARMOT_ERR_MEMORY;
        goto out;
    }
    for (size_t i = 0; i < p->allowed_locator_kind_count; i++) {
        char *k = locator_kind_normalize(p->allowed_locator_kinds[i]);
        if (!k) {
            err = p->allowed_locator_kinds[i] ? MARMOT_ERR_MEMORY : MARMOT_ERR_INVALID_INPUT;
            goto out;
        }
        if (!locator_kind_valid((const uint8_t *)k, strlen(k)) ||
            strv_contains(n.allowed_locator_kinds, n.allowed_locator_kind_count, k)) {
            bool dup = locator_kind_valid((const uint8_t *)k, strlen(k));
            free(k);
            if (!dup) goto out;
            continue;   /* MDK dedups */
        }
        n.allowed_locator_kinds[n.allowed_locator_kind_count++] = k;
    }
    if (n.allowed_locator_kind_count == 0 ||
        n.allowed_locator_kind_count > MARMOT_MEDIA_POLICY_MAX_LOCATOR_KINDS)
        goto out;
    for (size_t i = 0; i < p->default_blob_endpoint_count; i++) {
        const MarmotMediaBlobEndpoint *src = &p->default_blob_endpoints[i];
        if (src->base_url_unverified) goto out;
        char *k = locator_kind_normalize(src->locator_kind);
        char *u = NULL;
        if (!k) {
            err = src->locator_kind ? MARMOT_ERR_MEMORY : MARMOT_ERR_INVALID_INPUT;
            goto out;
        }
        MarmotError uerr = src->base_url ? url_normalize(src->base_url, URL_ENDPOINT, &u)
                                         : MARMOT_ERR_INVALID_INPUT;
        if (!locator_kind_valid((const uint8_t *)k, strlen(k)) ||
            !strv_contains(n.allowed_locator_kinds, n.allowed_locator_kind_count, k) ||
            uerr != MARMOT_OK) {
            free(k);
            free(u);
            if (uerr == MARMOT_ERR_MEMORY) err = uerr;
            goto out;
        }
        bool dup = false;
        for (size_t j = 0; j < n.default_blob_endpoint_count && !dup; j++)
            dup = strcmp(n.default_blob_endpoints[j].locator_kind, k) == 0 &&
                  strcmp(n.default_blob_endpoints[j].base_url, u) == 0;
        if (dup) {
            free(k);
            free(u);
            continue;
        }
        n.default_blob_endpoints[n.default_blob_endpoint_count].locator_kind = k;
        n.default_blob_endpoints[n.default_blob_endpoint_count].base_url = u;
        n.default_blob_endpoint_count++;
    }
    if (n.default_blob_endpoint_count == 0 ||
        n.default_blob_endpoint_count > MARMOT_MEDIA_POLICY_MAX_ENDPOINTS)
        goto out;

    err = MARMOT_ERR_MEMORY;
    if (mls_tls_buf_init(&kinds, 32) != 0 || mls_tls_buf_init(&endpoints, 128) != 0 ||
        mls_tls_buf_init(&b, 160) != 0)
        goto out;
    int rc = 0;
    for (size_t i = 0; i < n.allowed_locator_kind_count; i++)
        rc |= write_vec(&kinds, n.allowed_locator_kinds[i], strlen(n.allowed_locator_kinds[i]));
    for (size_t i = 0; i < n.default_blob_endpoint_count; i++) {
        const MarmotMediaBlobEndpoint *ep = &n.default_blob_endpoints[i];
        rc |= write_vec(&endpoints, ep->locator_kind, strlen(ep->locator_kind));
        rc |= write_vec(&endpoints, ep->base_url, strlen(ep->base_url));
    }
    rc |= write_vec(&b, MARMOT_MEDIA_V2_VERSION, strlen(MARMOT_MEDIA_V2_VERSION));
    rc |= write_vec(&b, kinds.data, kinds.len);
    rc |= write_vec(&b, endpoints.data, endpoints.len);
    if (rc != 0) goto out;
    err = finish_buf(&b, out, out_len);
out:
    marmot_group_media_policy_clear(&n);
    mls_tls_buf_free(&kinds);
    mls_tls_buf_free(&endpoints);
    mls_tls_buf_free(&b);
    return err;
}
