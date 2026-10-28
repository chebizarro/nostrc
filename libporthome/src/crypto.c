/*
 * porthome_crypto.c — Phase 1 crypto primitives.
 * SPDX-License-Identifier: MIT
 *
 * EXPERIMENTAL AND UNREVIEWED. See docs/designs/porthome-crypto-spec.md
 * for the byte layout, HKDF parameters, and threat-model deltas that
 * this file implements.
 */

#include <porthome/crypto.h>
#include "crypto-internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/bn.h>
#include <limits.h>
#include <nostr/nip44/padding.h>

#include <nostr/nip44/kdf.h>

/* Purpose-specific HKDF salt strings. Any change here bumps the on-wire
 * PORTHOME_WIRE_VERSION byte in lockstep — see spec §2. */
static const char SALT_HOME[]     = "porthome/v1/home";
static const char SALT_CHUNK[]    = "porthome/v1/chunk";
static const char SALT_MANIFEST[] = "porthome/v1/manifest";
static const char SALT_NAME[]     = "porthome/v1/name";

/* Cap the plaintext we'll seal in a single call. 2 GiB = design §5.4
 * per-file cap. Well below the AEAD's 2^38-64 byte hard limit. */
#define PORTHOME_MAX_SEAL_PT (2u * 1024u * 1024u * 1024u)

int porthome_sha256(const uint8_t *buf, size_t len, uint8_t out[32]) {
    if (!out) return PORTHOME_ERR_ARG;
    if (len && !buf) return PORTHOME_ERR_ARG;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) return PORTHOME_ERR_OOM;
    int rc = PORTHOME_ERR_CRYPTO;
    unsigned int mdlen = 0;
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) goto done;
    if (len && EVP_DigestUpdate(ctx, buf, len) != 1) goto done;
    if (EVP_DigestFinal_ex(ctx, out, &mdlen) != 1 || mdlen != 32) goto done;
    rc = PORTHOME_OK;
done:
    EVP_MD_CTX_free(ctx);
    if (rc != PORTHOME_OK) OPENSSL_cleanse(out, 32);
    return rc;
}

void porthome_hex64(const uint8_t in[32], char out_hex[65]) {
    static const char HX[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out_hex[2*i]   = HX[(in[i] >> 4) & 0xF];
        out_hex[2*i+1] = HX[in[i] & 0xF];
    }
    out_hex[64] = '\0';
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

int porthome_from_hex64(const char *in_hex, uint8_t out[32]) {
    if (!in_hex || !out) return PORTHOME_ERR_ARG;
    if (strnlen(in_hex, 65) != 64) return PORTHOME_ERR_ARG;
    for (int i = 0; i < 32; i++) {
        int hi = hex_nibble(in_hex[2*i]);
        int lo = hex_nibble(in_hex[2*i+1]);
        if (hi < 0 || lo < 0) return PORTHOME_ERR_ARG;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return PORTHOME_OK;
}

/* HKDF-SHA256: extract + expand. */
static int hkdf_sha256(const char *salt, size_t salt_len,
                       const uint8_t *ikm, size_t ikm_len,
                       const uint8_t *info, size_t info_len,
                       uint8_t *okm_out, size_t okm_len) {
    uint8_t prk[32];
    if (nostr_nip44_hkdf_extract((const uint8_t *)salt, salt_len,
                           ikm, ikm_len, prk) != 0)
        return PORTHOME_ERR_CRYPTO;
    int rc = nostr_nip44_hkdf_expand(prk, info, info_len, okm_out, okm_len);
    OPENSSL_cleanse(prk, sizeof(prk));
    return rc == 0 ? PORTHOME_OK : PORTHOME_ERR_CRYPTO;
}

int porthome_fscrypt_master_derive(const uint8_t home_key[32],
    const char *account_id, const uint8_t device_salt[32], uint32_t generation,
    uint8_t out_master[64]) {
    if (!out_master) return PORTHOME_ERR_ARG;
    if (!home_key || !account_id || !device_salt || !generation ||
        strnlen(account_id, 37) != 36) {
        OPENSSL_cleanse(out_master, 64);
        return PORTHOME_ERR_ARG;
    }
    for (size_t i = 0; i < 36; i++) {
        int dash = i == 8 || i == 13 || i == 18 || i == 23;
        char c = account_id[i];
        if ((dash && c != '-') || (!dash && !((c >= '0' && c <= '9') ||
                                               (c >= 'a' && c <= 'f')))) {
            OPENSSL_cleanse(out_master, 64);
            return PORTHOME_ERR_ARG;
        }
    }
    uint8_t info[5 + 36 + 1 + 32 + 4];
    memcpy(info, "home", 5);
    memcpy(info + 5, account_id, 36);
    info[41] = 0;
    memcpy(info + 42, device_salt, 32);
    for (unsigned i = 0; i < 4; i++) info[74 + i] = (uint8_t)(generation >> (24 - i * 8));
    static const char salt[] = "porthome/local/fscrypt/v1";
    int rc = hkdf_sha256(salt, sizeof salt - 1, home_key, 32,
                         info, sizeof info, out_master, 64);
    OPENSSL_cleanse(info, sizeof info);
    if (rc != PORTHOME_OK) OPENSSL_cleanse(out_master, 64);
    return rc;
}

int porthome_key_derive(const uint8_t seed[32], uint8_t out_home_key[32]) {
    if (!seed || !out_home_key) return PORTHOME_ERR_ARG;
    int rc = hkdf_sha256(SALT_HOME, sizeof(SALT_HOME) - 1,
                         seed, 32, NULL, 0, out_home_key, 32);
    if (rc != PORTHOME_OK) OPENSSL_cleanse(out_home_key, 32);
    return rc;
}

/* ────────────────────────────────────────────────────────────────────
 * Convergent AEAD sealing (chunks and manifest nodes).
 *
 * The receiver only sees `blob = version || nonce || ct || tag`. It
 * must be able to derive the AEAD key from those wire bytes and
 * `home_key` — SHA256(plaintext) is not available yet. So the sender
 * derives (nonce, key) in two HKDF steps rather than a single 44-byte
 * expansion:
 *
 *   nonce = HKDF(salt, home_key, info=SHA256(plaintext), L=12)   [sender only]
 *   key   = HKDF(salt, home_key, info=nonce,             L=32)   [both]
 *
 * The receiver reads `nonce` from the wire, derives `key` the same
 * way, and validates the AEAD tag. Convergence is preserved (same
 * plaintext → same nonce → same key → same ciphertext), and the
 * nonce-reuse hazard reduces to a SHA-256 collision on any two
 * plaintexts a single home has ever stored.
 * ──────────────────────────────────────────────────────────────────── */

static int seal(const uint8_t home_key[32],
                   const char *salt, size_t salt_len,
                   uint8_t version, const uint8_t *plain_hash,
                   const uint8_t *pt, size_t pt_len,
                   uint8_t **out_blob, size_t *out_blob_len,
                   uint8_t out_blob_sha256[32] /* nullable */) {
    if (!home_key || !out_blob || !out_blob_len) return PORTHOME_ERR_ARG;
    if (pt_len && !pt) return PORTHOME_ERR_ARG;
    if (pt_len > PORTHOME_MAX_SEAL_PT || pt_len > INT_MAX) return PORTHOME_ERR_TOO_LARGE;
    *out_blob = NULL; *out_blob_len = 0;

    uint8_t pt_hash[32];
    int rc = PORTHOME_OK;
    if (plain_hash) memcpy(pt_hash, plain_hash, sizeof(pt_hash));
    else rc = porthome_sha256(pt, pt_len, pt_hash);
    if (rc != PORTHOME_OK) return rc;

    uint8_t nonce[PORTHOME_NONCE_LEN];
    rc = hkdf_sha256(salt, salt_len, home_key, 32,
                     pt_hash, sizeof(pt_hash), nonce, sizeof(nonce));
    OPENSSL_cleanse(pt_hash, sizeof(pt_hash));
    if (rc != PORTHOME_OK) { OPENSSL_cleanse(nonce, sizeof(nonce)); return rc; }

    uint8_t key[PORTHOME_KEY_LEN];
    rc = hkdf_sha256(salt, salt_len, home_key, 32,
                     nonce, sizeof(nonce), key, sizeof(key));
    if (rc != PORTHOME_OK) {
        OPENSSL_cleanse(key, sizeof(key));
        OPENSSL_cleanse(nonce, sizeof(nonce));
        return rc;
    }

    size_t blob_len = 1 + PORTHOME_NONCE_LEN + pt_len + PORTHOME_TAG_LEN;
    uint8_t *blob = (uint8_t *)malloc(blob_len);
    if (!blob) {
        OPENSSL_cleanse(key, sizeof(key));
        OPENSSL_cleanse(nonce, sizeof(nonce));
        return PORTHOME_ERR_OOM;
    }
    blob[0] = version;
    memcpy(blob + 1, nonce, PORTHOME_NONCE_LEN);
    uint8_t *ct  = blob + 1 + PORTHOME_NONCE_LEN;
    uint8_t *tag = ct + pt_len;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int ok = 0;
    int outl = 0;
    if (!ctx) goto seal_done;
    if (EVP_EncryptInit_ex(ctx, EVP_chacha20_poly1305(), NULL, NULL, NULL) != 1) goto seal_done;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN,
                            PORTHOME_NONCE_LEN, NULL) != 1) goto seal_done;
    if (EVP_EncryptInit_ex(ctx, NULL, NULL, key, nonce) != 1) goto seal_done;
    if (pt_len) {
        if (EVP_EncryptUpdate(ctx, ct, &outl, pt, (int)pt_len) != 1) goto seal_done;
        if ((size_t)outl != pt_len) goto seal_done;
    }
    if (EVP_EncryptFinal_ex(ctx, ct + outl, &outl) != 1) goto seal_done;
    if (outl != 0) goto seal_done;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG,
                            PORTHOME_TAG_LEN, tag) != 1) goto seal_done;
    ok = 1;

seal_done:
    EVP_CIPHER_CTX_free(ctx);
    OPENSSL_cleanse(key, sizeof(key));
    OPENSSL_cleanse(nonce, sizeof(nonce));
    if (!ok) {
        OPENSSL_cleanse(blob, blob_len);
        free(blob);
        return PORTHOME_ERR_CRYPTO;
    }
    if (out_blob_sha256) {
        int hrc = porthome_sha256(blob, blob_len, out_blob_sha256);
        if (hrc != PORTHOME_OK) { free(blob); return hrc; }
    }
    *out_blob = blob;
    *out_blob_len = blob_len;
    return PORTHOME_OK;
}

static int open_blob(const uint8_t home_key[32],
                   const char *salt, size_t salt_len, uint8_t version,
                   const uint8_t *blob, size_t blob_len,
                   uint8_t **out_pt, size_t *out_pt_len) {
    if (!home_key || !out_pt || !out_pt_len || !blob) return PORTHOME_ERR_ARG;
    *out_pt = NULL; *out_pt_len = 0;
    if (blob_len < (size_t)PORTHOME_SEAL_OVERHEAD) return PORTHOME_ERR_VERSION;
    if (blob[0] != version) return PORTHOME_ERR_VERSION;

    size_t pt_len = blob_len - PORTHOME_SEAL_OVERHEAD;
    if (pt_len > PORTHOME_MAX_SEAL_PT || pt_len > INT_MAX) return PORTHOME_ERR_TOO_LARGE;

    const uint8_t *nonce = blob + 1;
    const uint8_t *ct    = blob + 1 + PORTHOME_NONCE_LEN;
    const uint8_t *tag   = ct + pt_len;

    uint8_t key[PORTHOME_KEY_LEN];
    int rc = hkdf_sha256(salt, salt_len, home_key, 32,
                         nonce, PORTHOME_NONCE_LEN,
                         key, sizeof(key));
    if (rc != PORTHOME_OK) { OPENSSL_cleanse(key, sizeof(key)); return rc; }

    uint8_t *pt = (uint8_t *)malloc(pt_len ? pt_len : 1);
    if (!pt) { OPENSSL_cleanse(key, sizeof(key)); return PORTHOME_ERR_OOM; }

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int ok = 0;
    int outl = 0;
    if (!ctx) goto open_done;
    if (EVP_DecryptInit_ex(ctx, EVP_chacha20_poly1305(), NULL, NULL, NULL) != 1) goto open_done;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN,
                            PORTHOME_NONCE_LEN, NULL) != 1) goto open_done;
    if (EVP_DecryptInit_ex(ctx, NULL, NULL, key, nonce) != 1) goto open_done;
    if (pt_len) {
        if (EVP_DecryptUpdate(ctx, pt, &outl, ct, (int)pt_len) != 1) goto open_done;
        if ((size_t)outl != pt_len) goto open_done;
    }
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG,
                            PORTHOME_TAG_LEN, (void *)tag) != 1) goto open_done;
    if (EVP_DecryptFinal_ex(ctx, pt + outl, &outl) != 1) goto open_done;
    if (outl != 0) goto open_done;
    ok = 1;

open_done:
    EVP_CIPHER_CTX_free(ctx);
    OPENSSL_cleanse(key, sizeof(key));
    if (!ok) {
        OPENSSL_cleanse(pt, pt_len ? pt_len : 1);
        free(pt);
        return PORTHOME_ERR_CRYPTO;
    }
    *out_pt = pt;
    *out_pt_len = pt_len;
    return PORTHOME_OK;
}

int porthome_encrypt_chunk(const uint8_t home_key[32],
                              const uint8_t *pt, size_t pt_len,
                              uint8_t **out_ct, size_t *out_ct_len,
                              uint8_t out_sha256[32]) {
    return seal(home_key, SALT_CHUNK, sizeof(SALT_CHUNK) - 1, PORTHOME_WIRE_VERSION, NULL,
                   pt, pt_len, out_ct, out_ct_len, out_sha256);
}

int porthome_decrypt_chunk(const uint8_t home_key[32],
                              const uint8_t *ct, size_t ct_len,
                              uint8_t **out_pt, size_t *out_pt_len) {
    return open_blob(home_key, SALT_CHUNK, sizeof(SALT_CHUNK) - 1, PORTHOME_WIRE_VERSION,
                   ct, ct_len, out_pt, out_pt_len);
}

int porthome_encrypt_manifest(const uint8_t home_key[32],
                                 const uint8_t *pt, size_t pt_len,
                                 uint8_t **out_ct, size_t *out_ct_len) {
    return seal(home_key, SALT_MANIFEST, sizeof(SALT_MANIFEST) - 1, PORTHOME_WIRE_VERSION, NULL,
                   pt, pt_len, out_ct, out_ct_len, NULL);
}

int porthome_decrypt_manifest(const uint8_t home_key[32],
                                 const uint8_t *ct, size_t ct_len,
                                 uint8_t **out_pt, size_t *out_pt_len) {
    return open_blob(home_key, SALT_MANIFEST, sizeof(SALT_MANIFEST) - 1, PORTHOME_WIRE_VERSION,
                   ct, ct_len, out_pt, out_pt_len);
}

/* ────────────────────────────────────────────────────────────────────
 * Name-field AEAD (schema-v2, nostrc-q25o).
 * Same seal/open_blob machinery as chunks/manifests but with the
 * name-purpose salt so a ciphertext produced under one purpose cannot
 * be redirected to another. Convergent (same plaintext → same bytes)
 * per D4. Callers use this to seal per-entry basenames and symlink
 * targets alongside the one-way `path_enc` HMAC form.
 * ──────────────────────────────────────────────────────────────────── */

int porthome_encrypt_name_field(const uint8_t home_key[32],
                                   const uint8_t *pt, size_t pt_len,
                                   uint8_t **out_ct, size_t *out_ct_len) {
    return seal(home_key, SALT_NAME, sizeof(SALT_NAME) - 1, PORTHOME_WIRE_VERSION, NULL,
                   pt, pt_len, out_ct, out_ct_len, NULL);
}

int porthome_decrypt_name_field(const uint8_t home_key[32],
                                   const uint8_t *ct, size_t ct_len,
                                   uint8_t **out_pt, size_t *out_pt_len) {
    return open_blob(home_key, SALT_NAME, sizeof(SALT_NAME) - 1, PORTHOME_WIRE_VERSION,
                   ct, ct_len, out_pt, out_pt_len);
}

/* ────────────────────────────────────────────────────────────────────
 * Name encryption (keyed hash; spec §7)
 * ──────────────────────────────────────────────────────────────────── */

static int component_is_ok(const char *c) {
    if (!c) return 0;
    size_t n = strlen(c);
    if (n == 0 || n > 255) return 0;
    if (strcmp(c, ".") == 0 || strcmp(c, "..") == 0) return 0;
    for (size_t i = 0; i < n; i++) {
        if (c[i] == '/' || c[i] == '\0' || c[i] == '\\') return 0;
    }
    return 1;
}

static int encrypt_name(const uint8_t home_key[32], const char *salt,
                             const char *component,
                             char out_hex[PORTHOME_NAME_HEX_LEN]) {
    if (!home_key || !component || !out_hex) return PORTHOME_ERR_ARG;
    if (!component_is_ok(component)) return PORTHOME_ERR_PATH;

    uint8_t name_key[32];
    int rc = hkdf_sha256(salt, strlen(salt), home_key, 32, NULL, 0, name_key, 32);
    if (rc != PORTHOME_OK) {
        OPENSSL_cleanse(name_key, sizeof(name_key));
        return rc;
    }

    uint8_t mac[32];
    rc = nostr_nip44_hmac_sha256(name_key, sizeof(name_key),
                           (const uint8_t *)component, strlen(component),
                           NULL, 0, mac);
    OPENSSL_cleanse(name_key, sizeof(name_key));
    if (rc != 0) {
        OPENSSL_cleanse(mac, sizeof(mac));
        return PORTHOME_ERR_CRYPTO;
    }

    static const char HX[] = "0123456789abcdef";
    for (size_t i = 0; i < PORTHOME_NAME_TAG_LEN; i++) {
        out_hex[2*i]   = HX[(mac[i] >> 4) & 0xF];
        out_hex[2*i+1] = HX[mac[i] & 0xF];
    }
    out_hex[PORTHOME_NAME_HEX_LEN - 1] = '\0';
    OPENSSL_cleanse(mac, sizeof(mac));
    return PORTHOME_OK;
}

int porthome_encrypt_name(const uint8_t home_key[32], const char *component,
                          char out_hex[PORTHOME_NAME_HEX_LEN]) {
    return encrypt_name(home_key, SALT_NAME, component, out_hex);
}

int porthome_encrypt_path(const uint8_t home_key[32],
                             const char *path,
                             char **out_joined) {
    if (!home_key || !path || !out_joined) return PORTHOME_ERR_ARG;
    *out_joined = NULL;

    while (*path == '/') path++; /* strip leading '/' */
    size_t plen = strlen(path);
    if (plen == 0) return PORTHOME_ERR_PATH;
    if (path[plen - 1] == '/') return PORTHOME_ERR_PATH;

    size_t ncomp = 1;
    for (size_t i = 0; i < plen; i++) if (path[i] == '/') ncomp++;

    /* Upper bound: (48 hex per component) + (ncomp - 1) separators + NUL. */
    size_t olen = ((size_t)(PORTHOME_NAME_HEX_LEN - 1)) * ncomp + ncomp;
    char *out = (char *)malloc(olen);
    if (!out) return PORTHOME_ERR_OOM;
    size_t opos = 0;

    size_t i = 0;
    int rc = PORTHOME_OK;
    while (i < plen) {
        size_t j = i;
        while (j < plen && path[j] != '/') j++;
        if (j == i) { rc = PORTHOME_ERR_PATH; goto path_done; }
        size_t clen = j - i;
        if (clen > 255) { rc = PORTHOME_ERR_PATH; goto path_done; }
        char comp[256];
        memcpy(comp, path + i, clen);
        comp[clen] = '\0';
        char enc[PORTHOME_NAME_HEX_LEN];
        rc = porthome_encrypt_name(home_key, comp, enc);
        if (rc != PORTHOME_OK) goto path_done;
        if (opos > 0) out[opos++] = '/';
        memcpy(out + opos, enc, PORTHOME_NAME_HEX_LEN - 1);
        opos += PORTHOME_NAME_HEX_LEN - 1;
        i = (j < plen) ? j + 1 : j;
    }
    out[opos] = '\0';
    *out_joined = out;
    return PORTHOME_OK;

path_done:
    OPENSSL_cleanse(out, olen);
    free(out);
    *out_joined = NULL;
    return rc;
}

/* PCS v2: salts are literal protocol constants; never reuse a v1 domain. */
static const char V2_HOME[] = "porthome/v2/home";
static const char V2_NODE[] = "porthome/v2/node";
static const char V2_CHUNK[] = "porthome/v2/chunk";
static const char V2_MANIFEST[] = "porthome/v2/manifest";
static const char V2_NAME[] = "porthome/v2/name";
static const char V2_HEAD[] = "porthome/v2/head";

static int derive_v2(const char *salt, const uint8_t key[32],
                     const uint8_t *info, size_t info_len, uint8_t out[32]) {
    if (!key || !out) return PORTHOME_ERR_ARG;
    int rc = hkdf_sha256(salt, strlen(salt), key, 32, info, info_len, out, 32);
    if (rc != PORTHOME_OK) OPENSSL_cleanse(out, 32);
    return rc;
}

int porthome_root_node_key(const uint8_t seed[32], uint8_t out[32]) {
    return derive_v2(V2_HOME, seed, NULL, 0, out);
}

int porthome_child_node_key(const uint8_t parent[32], const uint8_t node_id[32],
                            const uint8_t *key_override, uint8_t out[32]) {
    if (!out) return PORTHOME_ERR_ARG;
    if (key_override) { memmove(out, key_override, 32); return PORTHOME_OK; }
    if (!node_id) return PORTHOME_ERR_ARG;
    return derive_v2(V2_NODE, parent, node_id, 32, out);
}

int porthome_name_key_v2(const uint8_t node_key[32], uint8_t out[32]) {
    return derive_v2(V2_NAME, node_key, NULL, 0, out);
}

int porthome_head_key_v2(const uint8_t node_key[32], uint8_t out[32]) {
    if (!node_key || !out) return PORTHOME_ERR_ARG;
    uint8_t raw[32];
    int rc = derive_v2(V2_HEAD, node_key, NULL, 0, raw);
    BIGNUM *scalar = NULL, *order = NULL;
    BN_CTX *ctx = NULL;
    if (rc != PORTHOME_OK) goto done;
    rc = PORTHOME_ERR_CRYPTO;
    scalar = BN_bin2bn(raw, 32, NULL);
    ctx = BN_CTX_new();
    if (!scalar || !ctx || !BN_hex2bn(&order,
        "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141")) goto done;
    BN_set_flags(scalar, BN_FLG_CONSTTIME);
    if (!BN_nnmod(scalar, scalar, order, ctx) || BN_is_zero(scalar) ||
        BN_bn2binpad(scalar, out, 32) != 32) goto done;
    rc = PORTHOME_OK;
done:
    OPENSSL_cleanse(raw, sizeof(raw));
    BN_clear_free(scalar);
    BN_free(order);
    BN_CTX_free(ctx);
    if (rc != PORTHOME_OK) OPENSSL_cleanse(out, 32);
    return rc;
}

int porthome_encrypt_name_v2(const uint8_t node_key[32], const char *component,
                             char out_hex[PORTHOME_NAME_HEX_LEN]) {
    return encrypt_name(node_key, V2_NAME, component, out_hex);
}

int porthome_ring_key(const porthome_ring_t *ring, uint64_t epoch,
                    const uint8_t **key) {
    if (!ring || (ring->count && !ring->entries) || !epoch) return PORTHOME_ERR_ARG;
    const uint8_t *match = NULL;
    uint64_t previous = 0;
    for (size_t i = 0; i < ring->count; ++i) {
        const porthome_ring_entry_t *entry = &ring->entries[i];
        if (entry->epoch <= previous) return PORTHOME_ERR_ARG;
        previous = entry->epoch;
        if (entry->epoch == epoch) match = entry->node_key;
    }
    if (!match) return PORTHOME_ERR_EPOCH;
    *key = match;
    return PORTHOME_OK;
}

int porthome_encrypt_chunk_v2(const uint8_t node_key[32],
    const uint8_t *pt, size_t pt_len, uint8_t **out, size_t *out_len,
    uint8_t out_sha256[32]) {
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (!node_key || !out || !out_len || (pt_len && !pt)) return PORTHOME_ERR_ARG;
    if (pt_len > INT_MAX) return PORTHOME_ERR_TOO_LARGE;
    size_t padded_len = nip44_calc_padded_len(pt_len);
    if (padded_len > PORTHOME_MAX_SEAL_PT || padded_len > INT_MAX)
        return PORTHOME_ERR_TOO_LARGE;
    uint8_t hash[32];
    int rc = porthome_sha256(pt, pt_len, hash);
    if (rc != PORTHOME_OK) return rc;
    uint8_t *padded = calloc(1, padded_len);
    if (!padded) { OPENSSL_cleanse(hash, sizeof(hash)); return PORTHOME_ERR_OOM; }
    if (pt_len) memcpy(padded, pt, pt_len);
    rc = seal(node_key, V2_CHUNK, sizeof(V2_CHUNK) - 1, PORTHOME_WIRE_VERSION_V2,
              hash, padded, padded_len, out, out_len, out_sha256);
    OPENSSL_cleanse(hash, sizeof(hash));
    OPENSSL_cleanse(padded, padded_len);
    free(padded);
    return rc;
}

int porthome_decrypt_chunk_v2(const porthome_ring_t *ring,
    uint64_t chunk_key_id, const uint8_t *blob, size_t blob_len,
    size_t size_plain, uint8_t **out, size_t *out_len) {
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (!out || !out_len || !blob) return PORTHOME_ERR_ARG;
    if (blob_len < PORTHOME_SEAL_OVERHEAD || blob[0] != PORTHOME_WIRE_VERSION_V2)
        return PORTHOME_ERR_VERSION;
    const uint8_t *key;
    int rc = porthome_ring_key(ring, chunk_key_id, &key);
    if (rc != PORTHOME_OK) return rc;
    if (size_plain > INT_MAX) return PORTHOME_ERR_TOO_LARGE;
    if (blob_len - PORTHOME_SEAL_OVERHEAD != nip44_calc_padded_len(size_plain))
        return PORTHOME_ERR_ARG;
    uint8_t *pt = NULL;
    size_t padded_len = 0;
    rc = open_blob(key, V2_CHUNK, sizeof(V2_CHUNK) - 1, PORTHOME_WIRE_VERSION_V2,
                   blob, blob_len, &pt, &padded_len);
    if (rc != PORTHOME_OK) return rc;
    uint8_t tail = 0;
    for (size_t i = size_plain; i < padded_len; ++i) tail |= pt[i];
    if (tail) {
        OPENSSL_cleanse(pt, padded_len);
        free(pt);
        return PORTHOME_ERR_CRYPTO;
    }
    *out = pt;
    *out_len = size_plain;
    return PORTHOME_OK;
}

int porthome_encrypt_manifest_v2(const uint8_t node_key[32],
    const uint8_t *pt, size_t pt_len, uint8_t **out, size_t *out_len,
    uint8_t out_sha256[32]) {
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    return seal(node_key, V2_MANIFEST, sizeof(V2_MANIFEST) - 1,
                PORTHOME_WIRE_VERSION_V2, NULL, pt, pt_len, out, out_len, out_sha256);
}

int porthome_decrypt_manifest_v2(const porthome_ring_t *ring,
    uint64_t epoch, const uint8_t *blob, size_t blob_len,
    uint8_t **out, size_t *out_len) {
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (!out || !out_len || !blob) return PORTHOME_ERR_ARG;
    if (blob_len < PORTHOME_SEAL_OVERHEAD || blob[0] != PORTHOME_WIRE_VERSION_V2)
        return PORTHOME_ERR_VERSION;
    const uint8_t *key;
    int rc = porthome_ring_key(ring, epoch, &key);
    if (rc != PORTHOME_OK) return rc;
    return open_blob(key, V2_MANIFEST, sizeof(V2_MANIFEST) - 1, PORTHOME_WIRE_VERSION_V2,
                      blob, blob_len, out, out_len);
}
