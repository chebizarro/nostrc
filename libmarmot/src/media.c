/*
 * libmarmot - MIP-04 encrypted media v2
 *
 * features/encrypted-media.md at marmot-protocol/marmot 07da8ffb; MDK
 * v0.11.0 (946e0547) is the reference.  See marmot/marmot-media.h for the
 * construction and API contract.
 *
 * libmarmot before 0.12 derived an HMAC key labelled "marmot-media-key"
 * and authenticated only the MIME type: that format matches neither v2
 * nor the frozen v1, so marmot_encrypt_media() now refuses and the old
 * decryptor is kept read-only for references already stored locally.
 *
 * SPDX-License-Identifier: MIT
 */

#include <marmot/marmot.h>
#include "marmot-internal.h"
#include "commits.h"
#include "media_v2.h"
#include "mls/mls-internal.h"
#include "mls/mls_key_schedule.h"

#include <sodium.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define AEAD_TAG_LEN crypto_aead_chacha20poly1305_ietf_ABYTES

static const char MEDIA_V2_LABEL[] = MARMOT_MEDIA_V2_VERSION;

/* ── Small helpers ─────────────────────────────────────────────────────── */

static char *
dup_n(const char *s, size_t n)
{
    char *out = malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

static int
hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Exactly 2*n hex digits of either case (MDK accepts uppercase). */
static bool
hex_decode_exact(const char *s, size_t s_len, uint8_t *out, size_t n)
{
    if (s_len != 2 * n) return false;
    for (size_t i = 0; i < n; i++) {
        int hi = hex_nibble(s[2 * i]), lo = hex_nibble(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}

static void
hex_encode(const uint8_t *in, size_t n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 0x0f];
    }
    out[2 * n] = '\0';
}

bool
marmot_media_utf8_valid(const uint8_t *s, size_t len)
{
    size_t i = 0;
    while (i < len) {
        uint8_t c = s[i];
        size_t n;
        uint32_t cp, min;
        if (c < 0x80) { i++; continue; }
        else if ((c & 0xe0) == 0xc0) { n = 1; cp = c & 0x1f; min = 0x80; }
        else if ((c & 0xf0) == 0xe0) { n = 2; cp = c & 0x0f; min = 0x800; }
        else if ((c & 0xf8) == 0xf0) { n = 3; cp = c & 0x07; min = 0x10000; }
        else return false;
        if (len - i <= n) return false;
        for (size_t k = 1; k <= n; k++) {
            if ((s[i + k] & 0xc0) != 0x80) return false;
            cp = cp << 6 | (s[i + k] & 0x3f);
        }
        if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
        i += n + 1;
    }
    return true;
}

/* ── Media type canonicalization (foundation/canonical-encoding.md) ───── */

static bool
is_token_byte(uint8_t c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
        return true;
    return c != 0 && strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

static bool
is_mt_space(uint8_t c)
{
    return c == 0x09 || c == 0x0a || c == 0x0c || c == 0x0d || c == 0x20;
}

MarmotError
marmot_media_type_canonicalize(const char *input, char **out)
{
    if (!input || !out) return MARMOT_ERR_INVALID_ARG;
    *out = NULL;
    const uint8_t *b = (const uint8_t *)input;
    size_t end = strcspn(input, ";");
    size_t start = 0;
    while (start < end && is_mt_space(b[start])) start++;
    while (end > start && is_mt_space(b[end - 1])) end--;
    size_t len = end - start, slash = SIZE_MAX;
    for (size_t i = start; i < end; i++) {
        if (b[i] == '/') {
            if (slash != SIZE_MAX) return MARMOT_ERR_INVALID_INPUT;
            slash = i;
        } else if (!is_token_byte(b[i])) {
            return MARMOT_ERR_INVALID_INPUT;
        }
    }
    if (slash == SIZE_MAX) return MARMOT_ERR_INVALID_INPUT;
    size_t type_len = slash - start, sub_len = end - slash - 1;
    if (type_len == 0 || sub_len == 0 || type_len > 64 || sub_len > 64 ||
        len > MARMOT_MEDIA_TYPE_MAX)
        return MARMOT_ERR_INVALID_INPUT;
    char *mt = dup_n(input + start, len);
    if (!mt) return MARMOT_ERR_MEMORY;
    for (size_t i = 0; i < len; i++)
        if (mt[i] >= 'A' && mt[i] <= 'Z') mt[i] = (char)(mt[i] - 'A' + 'a');
    if (strcmp(mt, "image/jpg") == 0) {
        free(mt);
        mt = strdup("image/jpeg");
        if (!mt) return MARMOT_ERR_MEMORY;
    }
    *out = mt;
    return MARMOT_OK;
}

static bool
media_type_is_canonical(const char *mt)
{
    char *canon = NULL;
    if (marmot_media_type_canonicalize(mt, &canon) != MARMOT_OK) return false;
    bool same = strcmp(canon, mt) == 0;
    free(canon);
    return same;
}

static bool
filename_valid(const char *name)
{
    size_t n = name ? strlen(name) : 0;
    return n >= 1 && n <= MARMOT_MEDIA_FILENAME_MAX &&
           marmot_media_utf8_valid((const uint8_t *)name, n);
}

/* ── Derivation ────────────────────────────────────────────────────────── */

int
marmot_media_secret_from_exporter(const uint8_t exporter_secret[32], uint8_t media_secret[32])
{
    static const char context[] = "encrypted-media";
    return mls_exporter(exporter_secret, "marmot", (const uint8_t *)context,
                        sizeof context - 1, media_secret, 32);
}

/* label 0x00 hash 0x00 media_type 0x00 filename [0x00 "key"] */
static int
build_v2_bytes(const uint8_t hash[32], const char *media_type, const char *filename,
               bool key_suffix, uint8_t **out, size_t *out_len)
{
    size_t label_len = sizeof MEDIA_V2_LABEL - 1;
    size_t mt_len = strlen(media_type), fn_len = strlen(filename);
    size_t len = label_len + 1 + 32 + 1 + mt_len + 1 + fn_len + (key_suffix ? 4 : 0);
    uint8_t *buf = malloc(len), *p = buf;
    if (!buf) return -1;
    memcpy(p, MEDIA_V2_LABEL, label_len); p += label_len; *p++ = 0;
    memcpy(p, hash, 32); p += 32; *p++ = 0;
    memcpy(p, media_type, mt_len); p += mt_len; *p++ = 0;
    memcpy(p, filename, fn_len); p += fn_len;
    if (key_suffix) { *p++ = 0; memcpy(p, "key", 3); }
    *out = buf;
    *out_len = len;
    return 0;
}

int
marmot_media_v2_key_info(const uint8_t hash[32], const char *media_type,
                         const char *filename, uint8_t **out, size_t *out_len)
{
    return build_v2_bytes(hash, media_type, filename, true, out, out_len);
}

int
marmot_media_v2_aad(const uint8_t hash[32], const char *media_type,
                    const char *filename, uint8_t **out, size_t *out_len)
{
    return build_v2_bytes(hash, media_type, filename, false, out, out_len);
}

int
marmot_media_v2_file_key(const uint8_t media_secret[32], const uint8_t hash[32],
                         const char *media_type, const char *filename, uint8_t file_key[32])
{
    uint8_t *info = NULL;
    size_t info_len = 0;
    if (marmot_media_v2_key_info(hash, media_type, filename, &info, &info_len) != 0)
        return -1;
    /* media_secret is the PRK: HKDF-Expand only, HKDF-SHA256 whatever the
     * ciphersuite. */
    int rc = mls_crypto_hkdf_expand(file_key, 32, media_secret, info, info_len);
    free(info);
    return rc;
}

/* ── Seal / open with a given media secret ─────────────────────────────── */

MarmotError
marmot_media_v2_seal(const uint8_t media_secret[32], const uint8_t nonce[12],
                     const uint8_t *plaintext, size_t plaintext_len,
                     const char *media_type, const char *filename,
                     uint8_t **ciphertext, size_t *ciphertext_len,
                     MarmotMediaReference *ref)
{
    if (!media_secret || !nonce || !plaintext || !media_type || !filename ||
        !ciphertext || !ciphertext_len || !ref)
        return MARMOT_ERR_INVALID_ARG;
    *ciphertext = NULL;
    *ciphertext_len = 0;
    memset(ref, 0, sizeof *ref);
    if (plaintext_len == 0 || !media_type_is_canonical(media_type) || !filename_valid(filename))
        return MARMOT_ERR_INVALID_INPUT;
    if (plaintext_len > SIZE_MAX - AEAD_TAG_LEN) return MARMOT_ERR_INVALID_INPUT;

    uint8_t hash[32], key[32];
    SHA256(plaintext, plaintext_len, hash);
    uint8_t *aad = NULL;
    size_t aad_len = 0;
    if (marmot_media_v2_file_key(media_secret, hash, media_type, filename, key) != 0)
        return MARMOT_ERR_CRYPTO;
    if (marmot_media_v2_aad(hash, media_type, filename, &aad, &aad_len) != 0) {
        sodium_memzero(key, sizeof key);
        return MARMOT_ERR_MEMORY;
    }
    uint8_t *ct = malloc(plaintext_len + AEAD_TAG_LEN);
    if (!ct) {
        free(aad);
        sodium_memzero(key, sizeof key);
        return MARMOT_ERR_MEMORY;
    }
    unsigned long long ct_len = 0;
    int rc = crypto_aead_chacha20poly1305_ietf_encrypt(ct, &ct_len, plaintext, plaintext_len,
                                                       aad, aad_len, NULL, nonce, key);
    sodium_memzero(key, sizeof key);
    free(aad);
    if (rc != 0) {
        free(ct);
        return MARMOT_ERR_CRYPTO;
    }
    ref->media_type = strdup(media_type);
    ref->filename = strdup(filename);
    if (!ref->media_type || !ref->filename) {
        free(ct);
        marmot_media_reference_clear(ref);
        return MARMOT_ERR_MEMORY;
    }
    memcpy(ref->plaintext_sha256, hash, 32);
    memcpy(ref->nonce, nonce, 12);
    SHA256(ct, (size_t)ct_len, ref->ciphertext_sha256);
    *ciphertext = ct;
    *ciphertext_len = (size_t)ct_len;
    return MARMOT_OK;
}

/* The fields every v2 receiver needs, locators aside. */
static MarmotError
reference_validate_crypto_fields(const MarmotMediaReference *ref)
{
    if (!ref->media_type || !media_type_is_canonical(ref->media_type) ||
        !filename_valid(ref->filename))
        return MARMOT_ERR_MEDIA_INVALID_REFERENCE;
    return MARMOT_OK;
}

MarmotError
marmot_media_v2_open(const uint8_t media_secret[32], const MarmotMediaReference *ref,
                     const uint8_t *ciphertext, size_t ciphertext_len,
                     uint8_t **plaintext, size_t *plaintext_len)
{
    if (!media_secret || !ref || !ciphertext || !plaintext || !plaintext_len)
        return MARMOT_ERR_INVALID_ARG;
    *plaintext = NULL;
    *plaintext_len = 0;
    MarmotError err = marmot_media_reference_validate(ref);
    if (err != MARMOT_OK) return err;

    /* 1. The fetched bytes are the blob the reference names. */
    uint8_t got[32];
    SHA256(ciphertext, ciphertext_len, got);
    if (sodium_memcmp(got, ref->ciphertext_sha256, 32) != 0)
        return MARMOT_ERR_MEDIA_CIPHERTEXT_HASH;
    if (ciphertext_len < AEAD_TAG_LEN) return MARMOT_ERR_MEDIA_DECRYPT;

    /* 2. AEAD under the per-file key, AAD binding hash, type and name. */
    uint8_t key[32];
    uint8_t *aad = NULL;
    size_t aad_len = 0;
    if (marmot_media_v2_file_key(media_secret, ref->plaintext_sha256, ref->media_type,
                                 ref->filename, key) != 0)
        return MARMOT_ERR_CRYPTO;
    if (marmot_media_v2_aad(ref->plaintext_sha256, ref->media_type, ref->filename,
                            &aad, &aad_len) != 0) {
        sodium_memzero(key, sizeof key);
        return MARMOT_ERR_MEMORY;
    }
    size_t pt_cap = ciphertext_len - AEAD_TAG_LEN;
    uint8_t *pt = malloc(pt_cap ? pt_cap : 1);
    if (!pt) {
        free(aad);
        sodium_memzero(key, sizeof key);
        return MARMOT_ERR_MEMORY;
    }
    unsigned long long pt_len = 0;
    int rc = crypto_aead_chacha20poly1305_ietf_decrypt(pt, &pt_len, NULL, ciphertext,
                                                       ciphertext_len, aad, aad_len,
                                                       ref->nonce, key);
    sodium_memzero(key, sizeof key);
    free(aad);
    if (rc != 0) {
        free(pt);
        return MARMOT_ERR_MEDIA_DECRYPT;
    }

    /* 3. The plaintext is the file the reference claims. */
    SHA256(pt, (size_t)pt_len, got);
    if (sodium_memcmp(got, ref->plaintext_sha256, 32) != 0) {
        sodium_memzero(pt, (size_t)pt_len);
        free(pt);
        return MARMOT_ERR_MEDIA_HASH_MISMATCH;
    }
    *plaintext = pt;
    *plaintext_len = (size_t)pt_len;
    return MARMOT_OK;
}

/* ── Locators ──────────────────────────────────────────────────────────── */

static bool
is_blank(const char *s)
{
    for (; *s; s++)
        if (!(*s == ' ' || (*s >= 0x09 && *s <= 0x0d))) return false;
    return true;
}

/*
 * "Its value does not parse as a URL": an absolute URL with a valid scheme
 * (RFC 3986 / WHATWG scheme state).  For http and https -- the special
 * schemes a blossom-v1 locator may use -- the authority must also hold a
 * host (no forbidden host code point) and a port of at most 65535.
 */
static bool
locator_url_valid(const char *value, bool require_http)
{
    const char *p = value;
    while (*p && (unsigned char)*p <= 0x20) p++;   /* WHATWG trims C0 and space */
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z'))) return false;
    const char *s = p;
    while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
           (*p >= '0' && *p <= '9') || *p == '+' || *p == '-' || *p == '.')
        p++;
    if (*p != ':') return false;
    size_t scheme_len = (size_t)(p - s);
    bool http = (scheme_len == 4 && strncasecmp(s, "http", 4) == 0) ||
                (scheme_len == 5 && strncasecmp(s, "https", 5) == 0);
    if (require_http && !http) return false;
    p++;
    if (!http) return true;
    while (*p == '/' || *p == '\\') p++;
    size_t auth_len = strcspn(p, "/\\?#");
    const char *at = NULL;
    for (size_t i = 0; i < auth_len; i++)
        if (p[i] == '@') at = p + i;
    const char *host = at ? at + 1 : p;
    const char *end = p + auth_len;
    if (host >= end) return false;
    const char *port = NULL;
    if (*host == '[') {
        const char *close = memchr(host, ']', (size_t)(end - host));
        if (!close || close == host + 1) return false;
        if (close + 1 < end) {
            if (close[1] != ':') return false;
            port = close + 2;
        }
    } else {
        for (const char *q = host; q < end; q++) {
            if (*q == ':') { port = q + 1; end = q; break; }
            if ((unsigned char)*q <= 0x20 || strchr("#%/<>?@[\\]^|", *q)) {
                /* '%' would be decoded by WHATWG; keep the check strict. */
                return false;
            }
        }
        if (host == end) return false;
        if (port) {
            const char *pe = p + auth_len;
            unsigned long v = 0;
            for (const char *q = port; q < pe; q++) {
                if (*q < '0' || *q > '9') return false;
                v = v * 10 + (unsigned long)(*q - '0');
                if (v > 65535) return false;
            }
        }
        return true;
    }
    if (port) {
        unsigned long v = 0;
        for (const char *q = port; q < p + auth_len; q++) {
            if (*q < '0' || *q > '9') return false;
            v = v * 10 + (unsigned long)(*q - '0');
            if (v > 65535) return false;
        }
    }
    return true;
}

MarmotError
marmot_media_reference_validate(const MarmotMediaReference *ref)
{
    if (!ref) return MARMOT_ERR_INVALID_ARG;
    if (ref->locator_count == 0 || !ref->locators) return MARMOT_ERR_MEDIA_INVALID_REFERENCE;
    for (size_t i = 0; i < ref->locator_count; i++) {
        const MarmotMediaLocator *l = &ref->locators[i];
        if (!l->kind || !l->value || is_blank(l->kind) || is_blank(l->value) ||
            strchr(l->kind, ' '))
            return MARMOT_ERR_MEDIA_INVALID_REFERENCE;
        bool blossom = strcmp(l->kind, MARMOT_MEDIA_LOCATOR_BLOSSOM_V1) == 0;
        if (!locator_url_valid(l->value, blossom)) return MARMOT_ERR_MEDIA_INVALID_REFERENCE;
    }
    return reference_validate_crypto_fields(ref);
}

/* ── imeta build / parse ───────────────────────────────────────────────── */

static char *
field_printf2(const char *key, const char *value)
{
    size_t k = strlen(key), v = strlen(value);
    char *out = malloc(k + 1 + v + 1);
    if (!out) return NULL;
    memcpy(out, key, k);
    out[k] = ' ';
    memcpy(out + k + 1, value, v + 1);
    return out;
}

void
marmot_media_imeta_fields_free(char **fields, size_t count)
{
    if (!fields) return;
    for (size_t i = 0; i < count; i++) free(fields[i]);
    free(fields);
}

static bool
locator_kind_allowed(const char *kind, const char *const *allowed)
{
    if (!allowed || !allowed[0]) return strcmp(kind, MARMOT_MEDIA_LOCATOR_BLOSSOM_V1) == 0;
    for (; *allowed; allowed++)
        if (strcmp(kind, *allowed) == 0) return true;
    return false;
}

MarmotError
marmot_media_imeta_build(const MarmotMediaReference *ref, const char *const *allowed_kinds,
                         char ***out_fields, size_t *out_count)
{
    if (!ref || !out_fields || !out_count) return MARMOT_ERR_INVALID_ARG;
    *out_fields = NULL;
    *out_count = 0;
    MarmotError err = marmot_media_reference_validate(ref);
    if (err != MARMOT_OK) return err;
    /* A sender must not emit a locator kind its group policy forbids
     * (receivers would skip it); the default policy is blossom-v1. */
    for (size_t i = 0; i < ref->locator_count; i++)
        if (!locator_kind_allowed(ref->locators[i].kind, allowed_kinds))
            return MARMOT_ERR_MEDIA_INVALID_REFERENCE;

    size_t n = 2 + ref->locator_count + 5 + (ref->dim ? 1 : 0) + (ref->thumbhash ? 1 : 0);
    char **f = calloc(n + 1, sizeof *f);
    if (!f) return MARMOT_ERR_MEMORY;
    size_t i = 0;
    char hex[65];
    f[i++] = strdup("imeta");
    f[i++] = strdup("v " MARMOT_MEDIA_V2_VERSION);
    for (size_t l = 0; l < ref->locator_count; l++) {
        const MarmotMediaLocator *loc = &ref->locators[l];
        size_t kl = strlen(loc->kind), vl = strlen(loc->value);
        char *s = malloc(8 + kl + 1 + vl + 1);
        if (s) {
            memcpy(s, "locator ", 8);
            memcpy(s + 8, loc->kind, kl);
            s[8 + kl] = ' ';
            memcpy(s + 9 + kl, loc->value, vl + 1);
        }
        f[i++] = s;
    }
    hex_encode(ref->ciphertext_sha256, 32, hex);
    f[i++] = field_printf2("ciphertext_sha256", hex);
    hex_encode(ref->plaintext_sha256, 32, hex);
    f[i++] = field_printf2("plaintext_sha256", hex);
    hex_encode(ref->nonce, 12, hex);
    f[i++] = field_printf2("nonce", hex);
    f[i++] = field_printf2("m", ref->media_type);
    f[i++] = field_printf2("filename", ref->filename);
    if (ref->dim) f[i++] = field_printf2("dim", ref->dim);
    if (ref->thumbhash) f[i++] = field_printf2("thumbhash", ref->thumbhash);
    for (size_t k = 0; k < n; k++) {
        if (!f[k]) {
            marmot_media_imeta_fields_free(f, n);
            return MARMOT_ERR_MEMORY;
        }
    }
    *out_fields = f;
    *out_count = n;
    return MARMOT_OK;
}

static bool
has_prefix(const char *s, size_t len, const char *prefix)
{
    size_t p = strlen(prefix);
    return len >= p && memcmp(s, prefix, p) == 0;
}

/* A single-occurrence field: a second one is a duplicate, never first- or
 * last-wins (m, filename and plaintext_sha256 feed the key and AAD). */
static MarmotError
set_once(char **slot, const char *value, size_t len)
{
    if (*slot) return MARMOT_ERR_MEDIA_INVALID_REFERENCE;
    *slot = dup_n(value, len);
    return *slot ? MARMOT_OK : MARMOT_ERR_MEMORY;
}

MarmotError
marmot_media_imeta_parse(const char *const *fields, const size_t *field_lens,
                         size_t count, MarmotMediaReference *out)
{
    if (!fields || !out) return MARMOT_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);
    if (count == 0 || !fields[0]) return MARMOT_ERR_MEDIA_INVALID_REFERENCE;
    for (size_t i = 0; i < count; i++) {
        if (!fields[i]) return MARMOT_ERR_MEDIA_INVALID_REFERENCE;
        if (field_lens && memchr(fields[i], '\0', field_lens[i]))
            return MARMOT_ERR_MEDIA_INVALID_REFERENCE;
    }
    if (strcmp(fields[0], "imeta") != 0) return MARMOT_ERR_MEDIA_INVALID_REFERENCE;

    /* The version is judged first, so the verdict does not depend on field
     * order (as MDK's parse_media_attachment). */
    bool have_v = false;
    for (size_t i = 1; i < count; i++) {
        const char *f = fields[i];
        if (strcmp(f, "v") == 0) return MARMOT_ERR_MEDIA_INVALID_REFERENCE;
        if (strncmp(f, "v ", 2) == 0) {
            if (have_v) return MARMOT_ERR_MEDIA_INVALID_REFERENCE;
            if (strcmp(f + 2, MARMOT_MEDIA_V2_VERSION) != 0)
                return MARMOT_ERR_MEDIA_UNSUPPORTED_VERSION;
            have_v = true;
        }
    }
    if (!have_v) return MARMOT_ERR_MEDIA_UNSUPPORTED_VERSION;

    char *ct_hex = NULL, *pt_hex = NULL, *nonce_hex = NULL;
    MarmotError err = MARMOT_OK;
    for (size_t i = 1; i < count && err == MARMOT_OK; i++) {
        const char *f = fields[i];
        size_t len = strlen(f);
        if (strcmp(f, "blurhash") == 0 || has_prefix(f, len, "blurhash ")) {
            err = MARMOT_ERR_MEDIA_INVALID_REFERENCE;
            break;
        }
        if (has_prefix(f, len, "locator ")) {
            const char *rest = f + 8;
            const char *sp = strchr(rest, ' ');
            if (!sp) { err = MARMOT_ERR_MEDIA_INVALID_REFERENCE; break; }
            err = marmot_media_reference_add_locator(out, "", "");
            if (err != MARMOT_OK) break;
            MarmotMediaLocator *l = &out->locators[out->locator_count - 1];
            free(l->kind);
            free(l->value);
            l->kind = dup_n(rest, (size_t)(sp - rest));
            l->value = strdup(sp + 1);
            if (!l->kind || !l->value) err = MARMOT_ERR_MEMORY;
            continue;
        }
        const char *sp = strchr(f, ' ');
        if (!sp) {
            static const char *const keyed[] = {
                "locator", "ciphertext_sha256", "plaintext_sha256", "nonce",
                "m", "filename", "dim", "thumbhash", NULL };
            for (size_t k = 0; keyed[k]; k++)
                if (strcmp(f, keyed[k]) == 0) err = MARMOT_ERR_MEDIA_INVALID_REFERENCE;
            continue;   /* unknown keyless fields are ignored */
        }
        size_t klen = (size_t)(sp - f);
        const char *v = sp + 1;
        size_t vlen = len - klen - 1;
#define KEY_IS(lit) (klen == sizeof(lit) - 1 && memcmp(f, lit, klen) == 0)
        if (KEY_IS("v")) continue;
        else if (KEY_IS("ciphertext_sha256")) err = set_once(&ct_hex, v, vlen);
        else if (KEY_IS("plaintext_sha256")) err = set_once(&pt_hex, v, vlen);
        else if (KEY_IS("nonce")) err = set_once(&nonce_hex, v, vlen);
        else if (KEY_IS("m")) err = set_once(&out->media_type, v, vlen);
        else if (KEY_IS("filename")) err = set_once(&out->filename, v, vlen);
        else if (KEY_IS("dim")) err = set_once(&out->dim, v, vlen);
        else if (KEY_IS("thumbhash")) err = set_once(&out->thumbhash, v, vlen);
#undef KEY_IS
    }
    if (err == MARMOT_OK) {
        /* Required, non-empty. */
        if (!ct_hex || !*ct_hex || !pt_hex || !*pt_hex || !nonce_hex || !*nonce_hex ||
            !out->media_type || !*out->media_type || !out->filename || !*out->filename)
            err = MARMOT_ERR_MEDIA_INVALID_REFERENCE;
        else if (!hex_decode_exact(ct_hex, strlen(ct_hex), out->ciphertext_sha256, 32) ||
                 !hex_decode_exact(pt_hex, strlen(pt_hex), out->plaintext_sha256, 32) ||
                 !hex_decode_exact(nonce_hex, strlen(nonce_hex), out->nonce, 12))
            err = MARMOT_ERR_MEDIA_INVALID_REFERENCE;
        else
            err = marmot_media_reference_validate(out);
    }
    free(ct_hex);
    free(pt_hex);
    free(nonce_hex);
    if (err != MARMOT_OK) marmot_media_reference_clear(out);
    return err;
}

/* ── Reference helpers ─────────────────────────────────────────────────── */

MarmotError
marmot_media_reference_add_locator(MarmotMediaReference *ref, const char *kind,
                                   const char *value)
{
    if (!ref || !kind || !value) return MARMOT_ERR_INVALID_ARG;
    if (ref->locator_count >= SIZE_MAX / sizeof *ref->locators - 1) return MARMOT_ERR_MEMORY;
    MarmotMediaLocator *grown = realloc(ref->locators,
                                        (ref->locator_count + 1) * sizeof *grown);
    if (!grown) return MARMOT_ERR_MEMORY;
    ref->locators = grown;
    MarmotMediaLocator *l = &grown[ref->locator_count];
    l->kind = strdup(kind);
    l->value = strdup(value);
    if (!l->kind || !l->value) {
        free(l->kind);
        free(l->value);
        return MARMOT_ERR_MEMORY;
    }
    ref->locator_count++;
    return MARMOT_OK;
}

MarmotError
marmot_media_reference_set_hints(MarmotMediaReference *ref, const char *dim,
                                 const char *thumbhash)
{
    if (!ref) return MARMOT_ERR_INVALID_ARG;
    char *d = dim ? strdup(dim) : NULL, *t = thumbhash ? strdup(thumbhash) : NULL;
    if ((dim && !d) || (thumbhash && !t)) {
        free(d);
        free(t);
        return MARMOT_ERR_MEMORY;
    }
    free(ref->dim);
    free(ref->thumbhash);
    ref->dim = d;
    ref->thumbhash = t;
    return MARMOT_OK;
}

void
marmot_media_reference_clear(MarmotMediaReference *ref)
{
    if (!ref) return;
    for (size_t i = 0; i < ref->locator_count; i++) {
        free(ref->locators[i].kind);
        free(ref->locators[i].value);
    }
    free(ref->locators);
    free(ref->media_type);
    free(ref->filename);
    free(ref->dim);
    free(ref->thumbhash);
    memset(ref, 0, sizeof *ref);
}

void
marmot_media_upload_clear(MarmotMediaUpload *upload)
{
    if (!upload) return;
    free(upload->ciphertext);
    marmot_media_reference_clear(&upload->reference);
    memset(upload, 0, sizeof *upload);
}

MarmotError
marmot_media_blossom_fallback_url(const char *base_url, const uint8_t hash[32], char **out)
{
    if (!base_url || !hash || !out) return MARMOT_ERR_INVALID_ARG;
    *out = NULL;
    size_t n = strlen(base_url);
    while (n > 0 && base_url[n - 1] == '/') n--;
    if (n == 0) return MARMOT_ERR_INVALID_INPUT;
    char *url = malloc(n + 1 + 64 + 1);
    if (!url) return MARMOT_ERR_MEMORY;
    memcpy(url, base_url, n);
    url[n] = '/';
    hex_encode(hash, 32, url + n + 1);
    *out = url;
    return MARMOT_OK;
}

/* ── Group-bound encrypt / decrypt ─────────────────────────────────────── */

static MarmotError
load_media_secret(Marmot *m, const MarmotGroupId *gid, uint64_t epoch, uint8_t secret[32])
{
    uint8_t exporter[32];
    MarmotError err = m->storage->get_exporter_secret(m->storage->ctx, gid, epoch, exporter);
    if (err != MARMOT_OK) return err;
    int rc = marmot_media_secret_from_exporter(exporter, secret);
    sodium_memzero(exporter, sizeof exporter);
    return rc == 0 ? MARMOT_OK : MARMOT_ERR_CRYPTO;
}

static MarmotError
encrypt_impl(Marmot *m, const MarmotGroupId *gid, const uint8_t *plaintext, size_t len,
             const char *media_type, const char *filename, MarmotMediaUpload *out)
{
    MarmotGroup *group = NULL;
    MarmotError err = m->storage->find_group_by_mls_id(m->storage->ctx, gid, &group);
    if (err != MARMOT_OK) return err;
    if (!group) return MARMOT_ERR_GROUP_NOT_FOUND;
    err = marmot_group_reconcile(m, group);   /* after an interrupted transition */
    uint64_t epoch = group->epoch;
    marmot_group_free(group);
    if (err != MARMOT_OK) return err;

    char *canonical = NULL;
    err = marmot_media_type_canonicalize(media_type, &canonical);
    if (err != MARMOT_OK) return err;
    uint8_t secret[32], nonce[12];
    err = load_media_secret(m, gid, epoch, secret);
    if (err == MARMOT_OK) {
        randombytes_buf(nonce, sizeof nonce);   /* fresh for every encryption */
        err = marmot_media_v2_seal(secret, nonce, plaintext, len, canonical, filename,
                                   &out->ciphertext, &out->ciphertext_len, &out->reference);
        out->source_epoch = epoch;
    }
    sodium_memzero(secret, sizeof secret);
    free(canonical);
    return err;
}

MarmotError
marmot_media_encrypt(Marmot *m, const MarmotGroupId *gid, const uint8_t *plaintext,
                     size_t plaintext_len, const char *media_type, const char *filename,
                     MarmotMediaUpload *out)
{
    if (!m || !gid || !plaintext || !media_type || !filename || !out)
        return MARMOT_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);
    if (!m->storage || !m->storage->find_group_by_mls_id || !m->storage->get_exporter_secret)
        return MARMOT_ERR_STORAGE;
    /* A reconcile of an interrupted transition may write: one transaction. */
    MarmotError err = marmot_txn_begin(m);
    if (err != MARMOT_OK) return err;
    err = encrypt_impl(m, gid, plaintext, plaintext_len, media_type, filename, out);
    MarmotError end = marmot_txn_end(m, err);
    if (end != MARMOT_OK) marmot_media_upload_clear(out);
    return end;
}

static MarmotError
check_epoch_impl(Marmot *m, const MarmotGroupId *gid, uint64_t source_epoch)
{
    MarmotGroup *group = NULL;
    MarmotError err = m->storage->find_group_by_mls_id(m->storage->ctx, gid, &group);
    if (err != MARMOT_OK) return err;
    if (!group) return MARMOT_ERR_GROUP_NOT_FOUND;
    err = marmot_group_reconcile(m, group);   /* as marmot_create_message() */
    uint64_t epoch = group->epoch;
    marmot_group_free(group);
    if (err != MARMOT_OK) return err;
    return epoch == source_epoch ? MARMOT_OK : MARMOT_ERR_MEDIA_EPOCH_CHANGED;
}

MarmotError
marmot_media_check_epoch(Marmot *m, const MarmotGroupId *gid, uint64_t source_epoch)
{
    if (!m || !gid) return MARMOT_ERR_INVALID_ARG;
    if (!m->storage || !m->storage->find_group_by_mls_id) return MARMOT_ERR_STORAGE;
    MarmotError err = marmot_txn_begin(m);
    if (err != MARMOT_OK) return err;
    err = check_epoch_impl(m, gid, source_epoch);
    /* EPOCH_CHANGED is an answer, not a failure: keep a repair it made. */
    if (err == MARMOT_ERR_MEDIA_EPOCH_CHANGED) marmot_txn_keep(m);
    return marmot_txn_end(m, err);
}

MarmotError
marmot_media_decrypt(Marmot *m, const MarmotGroupId *gid, uint64_t source_epoch,
                     const MarmotMediaReference *ref, const uint8_t *ciphertext,
                     size_t ciphertext_len, uint8_t **plaintext_out, size_t *plaintext_len)
{
    if (!m || !gid || !ref || !ciphertext || !plaintext_out || !plaintext_len)
        return MARMOT_ERR_INVALID_ARG;
    *plaintext_out = NULL;
    *plaintext_len = 0;
    if (!m->storage || !m->storage->get_exporter_secret) return MARMOT_ERR_STORAGE;
    MarmotError err = marmot_media_reference_validate(ref);
    if (err != MARMOT_OK) return err;
    uint8_t secret[32];
    err = load_media_secret(m, gid, source_epoch, secret);
    if (err != MARMOT_OK) return err;
    err = marmot_media_v2_open(secret, ref, ciphertext, ciphertext_len,
                               plaintext_out, plaintext_len);
    sodium_memzero(secret, sizeof secret);
    return err;
}

/* ── Legacy (libmarmot < 0.12) ─────────────────────────────────────────── */

MarmotError
marmot_encrypt_media(Marmot *m, const MarmotGroupId *mls_group_id,
                     const uint8_t *file_data, size_t file_len,
                     const char *mime_type, const char *filename,
                     MarmotEncryptedMedia *result)
{
    (void)m; (void)mls_group_id; (void)file_data; (void)file_len;
    (void)mime_type; (void)filename;
    if (result) memset(result, 0, sizeof *result);
    return MARMOT_ERR_MEDIA_LEGACY_FORMAT;
}

/* HMAC-SHA256(exporter_secret, "marmot-media-key" || 0x01): the old key. */
static int
legacy_media_key(const uint8_t exporter_secret[32], uint8_t out_key[32])
{
    static const uint8_t input[] = "marmot-media-key\x01";
    unsigned int len = 32;
    return HMAC(EVP_sha256(), exporter_secret, 32, input, sizeof input - 1,
                out_key, &len) ? 0 : -1;
}

MarmotError
marmot_decrypt_media(Marmot *m, const MarmotGroupId *mls_group_id,
                     const uint8_t *encrypted_data, size_t enc_len,
                     const MarmotImetaInfo *imeta, uint8_t **out_data, size_t *out_len)
{
    if (!m || !mls_group_id || !encrypted_data || !imeta || !out_data || !out_len)
        return MARMOT_ERR_INVALID_ARG;
    if (!m->storage || !m->storage->get_exporter_secret) return MARMOT_ERR_STORAGE;
    *out_data = NULL;
    *out_len = 0;
    if (enc_len < AEAD_TAG_LEN) return MARMOT_ERR_INVALID_INPUT;
    /* The plaintext hash is mandatory: the old format let a sender omit it
     * (nostrc-u7cb review L5). */
    static const uint8_t zero[32] = { 0 };
    if (sodium_memcmp(imeta->file_hash, zero, 32) == 0) return MARMOT_ERR_MEDIA_INVALID_REFERENCE;

    uint8_t exporter[32], key[32];
    MarmotError err = m->storage->get_exporter_secret(m->storage->ctx, mls_group_id,
                                                      imeta->epoch, exporter);
    if (err != MARMOT_OK) return err;
    int krc = legacy_media_key(exporter, key);
    sodium_memzero(exporter, sizeof exporter);
    if (krc != 0) return MARMOT_ERR_CRYPTO;

    uint8_t *pt = malloc(enc_len - AEAD_TAG_LEN + 1);
    if (!pt) {
        sodium_memzero(key, sizeof key);
        return MARMOT_ERR_MEMORY;
    }
    const uint8_t *aad = (const uint8_t *)(imeta->mime_type ? imeta->mime_type : "");
    size_t aad_len = imeta->mime_type ? strlen(imeta->mime_type) : 0;
    unsigned long long pt_len = 0;
    int rc = crypto_aead_chacha20poly1305_ietf_decrypt(pt, &pt_len, NULL, encrypted_data,
                                                       enc_len, aad, aad_len, imeta->nonce,
                                                       key);
    sodium_memzero(key, sizeof key);
    if (rc != 0) {
        free(pt);
        return MARMOT_ERR_MEDIA_DECRYPT;
    }
    uint8_t got[32];
    SHA256(pt, (size_t)pt_len, got);
    if (sodium_memcmp(got, imeta->file_hash, 32) != 0) {
        sodium_memzero(pt, (size_t)pt_len);
        free(pt);
        return MARMOT_ERR_MEDIA_HASH_MISMATCH;
    }
    *out_data = pt;
    *out_len = (size_t)pt_len;
    return MARMOT_OK;
}

void
marmot_encrypted_media_clear(MarmotEncryptedMedia *result)
{
    if (!result) return;
    free(result->encrypted_data);
    free(result->imeta.mime_type);
    free(result->imeta.filename);
    free(result->imeta.url);
    memset(result, 0, sizeof(*result));
}
