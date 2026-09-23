/*
 * nh_porthome_crypto.c — Phase 1 crypto primitives.
 * SPDX-License-Identifier: MIT
 *
 * EXPERIMENTAL AND UNREVIEWED. See docs/designs/porthome-crypto-spec.md
 * for the byte layout, HKDF parameters, and threat-model deltas that
 * this file implements.
 */

#include "nh_porthome_crypto.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/sha.h>

/* Reuse the in-tree HKDF/HMAC-SHA256 implementation
 * (nips/nip44/src/core/nip44_hkdf_hmac.c). We link it into the target
 * from the CMake side; declaring the prototypes here keeps the header
 * out of the porthome/ include path. */
extern int nip44_hkdf_extract(const uint8_t *salt, size_t salt_len,
                              const uint8_t *ikm, size_t ikm_len,
                              uint8_t prk_out[32]);
extern int nip44_hkdf_expand(const uint8_t prk[32],
                             const uint8_t *info, size_t info_len,
                             uint8_t okm_out[], size_t okm_len);
extern int nip44_hmac_sha256(const uint8_t *key, size_t key_len,
                             const uint8_t *data1, size_t len1,
                             const uint8_t *data2, size_t len2,
                             uint8_t mac_out[32]);

/* Purpose-specific HKDF salt strings. Any change here bumps the on-wire
 * NH_PORTHOME_WIRE_VERSION byte in lockstep — see spec §2. */
static const char SALT_HOME[]     = "porthome/v1/home";
static const char SALT_CHUNK[]    = "porthome/v1/chunk";
static const char SALT_MANIFEST[] = "porthome/v1/manifest";
static const char SALT_NAME[]     = "porthome/v1/name";

/* Cap the plaintext we'll seal in a single call. 2 GiB = design §5.4
 * per-file cap. Well below the AEAD's 2^38-64 byte hard limit. */
#define NH_PORTHOME_MAX_SEAL_PT (2u * 1024u * 1024u * 1024u)

int nh_porthome_sha256(const uint8_t *buf, size_t len, uint8_t out[32]) {
    if (!out) return NH_PORTHOME_ERR_ARG;
    if (len && !buf) return NH_PORTHOME_ERR_ARG;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) return NH_PORTHOME_ERR_OOM;
    int rc = NH_PORTHOME_ERR_CRYPTO;
    unsigned int mdlen = 0;
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) goto done;
    if (len && EVP_DigestUpdate(ctx, buf, len) != 1) goto done;
    if (EVP_DigestFinal_ex(ctx, out, &mdlen) != 1 || mdlen != 32) goto done;
    rc = NH_PORTHOME_OK;
done:
    EVP_MD_CTX_free(ctx);
    if (rc != NH_PORTHOME_OK) OPENSSL_cleanse(out, 32);
    return rc;
}

void nh_porthome_hex64(const uint8_t in[32], char out_hex[65]) {
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

int nh_porthome_from_hex64(const char *in_hex, uint8_t out[32]) {
    if (!in_hex || !out) return NH_PORTHOME_ERR_ARG;
    for (int i = 0; i < 32; i++) {
        int hi = hex_nibble(in_hex[2*i]);
        int lo = hex_nibble(in_hex[2*i+1]);
        if (hi < 0 || lo < 0) return NH_PORTHOME_ERR_ARG;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    if (in_hex[64] != '\0') return NH_PORTHOME_ERR_ARG;
    return NH_PORTHOME_OK;
}

/* HKDF-SHA256: extract + expand. */
static int hkdf_sha256(const char *salt, size_t salt_len,
                       const uint8_t *ikm, size_t ikm_len,
                       const uint8_t *info, size_t info_len,
                       uint8_t *okm_out, size_t okm_len) {
    uint8_t prk[32];
    if (nip44_hkdf_extract((const uint8_t *)salt, salt_len,
                           ikm, ikm_len, prk) != 0)
        return NH_PORTHOME_ERR_CRYPTO;
    int rc = nip44_hkdf_expand(prk, info, info_len, okm_out, okm_len);
    OPENSSL_cleanse(prk, sizeof(prk));
    return rc == 0 ? NH_PORTHOME_OK : NH_PORTHOME_ERR_CRYPTO;
}

int nh_porthome_key_derive(const uint8_t seed[32], uint8_t out_home_key[32]) {
    if (!seed || !out_home_key) return NH_PORTHOME_ERR_ARG;
    int rc = hkdf_sha256(SALT_HOME, sizeof(SALT_HOME) - 1,
                         seed, 32, NULL, 0, out_home_key, 32);
    if (rc != NH_PORTHOME_OK) OPENSSL_cleanse(out_home_key, 32);
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

static int seal_v1(const uint8_t home_key[32],
                   const char *salt, size_t salt_len,
                   const uint8_t *pt, size_t pt_len,
                   uint8_t **out_blob, size_t *out_blob_len,
                   uint8_t out_blob_sha256[32] /* nullable */) {
    if (!home_key || !out_blob || !out_blob_len) return NH_PORTHOME_ERR_ARG;
    if (pt_len && !pt) return NH_PORTHOME_ERR_ARG;
    if (pt_len > NH_PORTHOME_MAX_SEAL_PT) return NH_PORTHOME_ERR_TOO_LARGE;
    *out_blob = NULL; *out_blob_len = 0;

    uint8_t pt_hash[32];
    int rc = nh_porthome_sha256(pt, pt_len, pt_hash);
    if (rc != NH_PORTHOME_OK) return rc;

    uint8_t nonce[NH_PORTHOME_NONCE_LEN];
    rc = hkdf_sha256(salt, salt_len, home_key, 32,
                     pt_hash, sizeof(pt_hash), nonce, sizeof(nonce));
    OPENSSL_cleanse(pt_hash, sizeof(pt_hash));
    if (rc != NH_PORTHOME_OK) { OPENSSL_cleanse(nonce, sizeof(nonce)); return rc; }

    uint8_t key[NH_PORTHOME_KEY_LEN];
    rc = hkdf_sha256(salt, salt_len, home_key, 32,
                     nonce, sizeof(nonce), key, sizeof(key));
    if (rc != NH_PORTHOME_OK) {
        OPENSSL_cleanse(key, sizeof(key));
        OPENSSL_cleanse(nonce, sizeof(nonce));
        return rc;
    }

    size_t blob_len = 1 + NH_PORTHOME_NONCE_LEN + pt_len + NH_PORTHOME_TAG_LEN;
    uint8_t *blob = (uint8_t *)malloc(blob_len);
    if (!blob) {
        OPENSSL_cleanse(key, sizeof(key));
        OPENSSL_cleanse(nonce, sizeof(nonce));
        return NH_PORTHOME_ERR_OOM;
    }
    blob[0] = NH_PORTHOME_WIRE_VERSION;
    memcpy(blob + 1, nonce, NH_PORTHOME_NONCE_LEN);
    uint8_t *ct  = blob + 1 + NH_PORTHOME_NONCE_LEN;
    uint8_t *tag = ct + pt_len;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int ok = 0;
    int outl = 0;
    if (!ctx) goto seal_done;
    if (EVP_EncryptInit_ex(ctx, EVP_chacha20_poly1305(), NULL, NULL, NULL) != 1) goto seal_done;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN,
                            NH_PORTHOME_NONCE_LEN, NULL) != 1) goto seal_done;
    if (EVP_EncryptInit_ex(ctx, NULL, NULL, key, nonce) != 1) goto seal_done;
    if (pt_len) {
        if (EVP_EncryptUpdate(ctx, ct, &outl, pt, (int)pt_len) != 1) goto seal_done;
        if ((size_t)outl != pt_len) goto seal_done;
    }
    if (EVP_EncryptFinal_ex(ctx, ct + outl, &outl) != 1) goto seal_done;
    if (outl != 0) goto seal_done;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG,
                            NH_PORTHOME_TAG_LEN, tag) != 1) goto seal_done;
    ok = 1;

seal_done:
    EVP_CIPHER_CTX_free(ctx);
    OPENSSL_cleanse(key, sizeof(key));
    OPENSSL_cleanse(nonce, sizeof(nonce));
    if (!ok) {
        OPENSSL_cleanse(blob, blob_len);
        free(blob);
        return NH_PORTHOME_ERR_CRYPTO;
    }
    if (out_blob_sha256) {
        int hrc = nh_porthome_sha256(blob, blob_len, out_blob_sha256);
        if (hrc != NH_PORTHOME_OK) { free(blob); return hrc; }
    }
    *out_blob = blob;
    *out_blob_len = blob_len;
    return NH_PORTHOME_OK;
}

static int open_v1(const uint8_t home_key[32],
                   const char *salt, size_t salt_len,
                   const uint8_t *blob, size_t blob_len,
                   uint8_t **out_pt, size_t *out_pt_len) {
    if (!home_key || !out_pt || !out_pt_len || !blob) return NH_PORTHOME_ERR_ARG;
    *out_pt = NULL; *out_pt_len = 0;
    if (blob_len < (size_t)NH_PORTHOME_SEAL_OVERHEAD) return NH_PORTHOME_ERR_VERSION;
    if (blob[0] != NH_PORTHOME_WIRE_VERSION) return NH_PORTHOME_ERR_VERSION;

    size_t pt_len = blob_len - NH_PORTHOME_SEAL_OVERHEAD;
    if (pt_len > NH_PORTHOME_MAX_SEAL_PT) return NH_PORTHOME_ERR_TOO_LARGE;

    const uint8_t *nonce = blob + 1;
    const uint8_t *ct    = blob + 1 + NH_PORTHOME_NONCE_LEN;
    const uint8_t *tag   = ct + pt_len;

    uint8_t key[NH_PORTHOME_KEY_LEN];
    int rc = hkdf_sha256(salt, salt_len, home_key, 32,
                         nonce, NH_PORTHOME_NONCE_LEN,
                         key, sizeof(key));
    if (rc != NH_PORTHOME_OK) { OPENSSL_cleanse(key, sizeof(key)); return rc; }

    uint8_t *pt = (uint8_t *)malloc(pt_len ? pt_len : 1);
    if (!pt) { OPENSSL_cleanse(key, sizeof(key)); return NH_PORTHOME_ERR_OOM; }

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int ok = 0;
    int outl = 0;
    if (!ctx) goto open_done;
    if (EVP_DecryptInit_ex(ctx, EVP_chacha20_poly1305(), NULL, NULL, NULL) != 1) goto open_done;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN,
                            NH_PORTHOME_NONCE_LEN, NULL) != 1) goto open_done;
    if (EVP_DecryptInit_ex(ctx, NULL, NULL, key, nonce) != 1) goto open_done;
    if (pt_len) {
        if (EVP_DecryptUpdate(ctx, pt, &outl, ct, (int)pt_len) != 1) goto open_done;
        if ((size_t)outl != pt_len) goto open_done;
    }
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG,
                            NH_PORTHOME_TAG_LEN, (void *)tag) != 1) goto open_done;
    if (EVP_DecryptFinal_ex(ctx, pt + outl, &outl) != 1) goto open_done;
    if (outl != 0) goto open_done;
    ok = 1;

open_done:
    EVP_CIPHER_CTX_free(ctx);
    OPENSSL_cleanse(key, sizeof(key));
    if (!ok) {
        OPENSSL_cleanse(pt, pt_len ? pt_len : 1);
        free(pt);
        return NH_PORTHOME_ERR_CRYPTO;
    }
    *out_pt = pt;
    *out_pt_len = pt_len;
    return NH_PORTHOME_OK;
}

int nh_porthome_encrypt_chunk(const uint8_t home_key[32],
                              const uint8_t *pt, size_t pt_len,
                              uint8_t **out_ct, size_t *out_ct_len,
                              uint8_t out_sha256[32]) {
    return seal_v1(home_key, SALT_CHUNK, sizeof(SALT_CHUNK) - 1,
                   pt, pt_len, out_ct, out_ct_len, out_sha256);
}

int nh_porthome_decrypt_chunk(const uint8_t home_key[32],
                              const uint8_t *ct, size_t ct_len,
                              uint8_t **out_pt, size_t *out_pt_len) {
    return open_v1(home_key, SALT_CHUNK, sizeof(SALT_CHUNK) - 1,
                   ct, ct_len, out_pt, out_pt_len);
}

int nh_porthome_encrypt_manifest(const uint8_t home_key[32],
                                 const uint8_t *pt, size_t pt_len,
                                 uint8_t **out_ct, size_t *out_ct_len) {
    return seal_v1(home_key, SALT_MANIFEST, sizeof(SALT_MANIFEST) - 1,
                   pt, pt_len, out_ct, out_ct_len, NULL);
}

int nh_porthome_decrypt_manifest(const uint8_t home_key[32],
                                 const uint8_t *ct, size_t ct_len,
                                 uint8_t **out_pt, size_t *out_pt_len) {
    return open_v1(home_key, SALT_MANIFEST, sizeof(SALT_MANIFEST) - 1,
                   ct, ct_len, out_pt, out_pt_len);
}

/* ────────────────────────────────────────────────────────────────────
 * Name encryption (keyed hash; spec §7)
 * ──────────────────────────────────────────────────────────────────── */

static int derive_name_key(const uint8_t home_key[32], uint8_t out[32]) {
    return hkdf_sha256(SALT_NAME, sizeof(SALT_NAME) - 1,
                       home_key, 32, NULL, 0, out, 32);
}

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

int nh_porthome_encrypt_name(const uint8_t home_key[32],
                             const char *component,
                             char out_hex[NH_PORTHOME_NAME_HEX_LEN]) {
    if (!home_key || !component || !out_hex) return NH_PORTHOME_ERR_ARG;
    if (!component_is_ok(component)) return NH_PORTHOME_ERR_PATH;

    uint8_t name_key[32];
    int rc = derive_name_key(home_key, name_key);
    if (rc != NH_PORTHOME_OK) {
        OPENSSL_cleanse(name_key, sizeof(name_key));
        return rc;
    }

    uint8_t mac[32];
    rc = nip44_hmac_sha256(name_key, sizeof(name_key),
                           (const uint8_t *)component, strlen(component),
                           NULL, 0, mac);
    OPENSSL_cleanse(name_key, sizeof(name_key));
    if (rc != 0) {
        OPENSSL_cleanse(mac, sizeof(mac));
        return NH_PORTHOME_ERR_CRYPTO;
    }

    static const char HX[] = "0123456789abcdef";
    for (size_t i = 0; i < NH_PORTHOME_NAME_TAG_LEN; i++) {
        out_hex[2*i]   = HX[(mac[i] >> 4) & 0xF];
        out_hex[2*i+1] = HX[mac[i] & 0xF];
    }
    out_hex[NH_PORTHOME_NAME_HEX_LEN - 1] = '\0';
    OPENSSL_cleanse(mac, sizeof(mac));
    return NH_PORTHOME_OK;
}

int nh_porthome_encrypt_path(const uint8_t home_key[32],
                             const char *path,
                             char **out_joined) {
    if (!home_key || !path || !out_joined) return NH_PORTHOME_ERR_ARG;
    *out_joined = NULL;

    while (*path == '/') path++; /* strip leading '/' */
    size_t plen = strlen(path);
    if (plen == 0) return NH_PORTHOME_ERR_PATH;
    if (path[plen - 1] == '/') return NH_PORTHOME_ERR_PATH;

    size_t ncomp = 1;
    for (size_t i = 0; i < plen; i++) if (path[i] == '/') ncomp++;

    /* Upper bound: (48 hex per component) + (ncomp - 1) separators + NUL. */
    size_t olen = ((size_t)(NH_PORTHOME_NAME_HEX_LEN - 1)) * ncomp + ncomp;
    char *out = (char *)malloc(olen);
    if (!out) return NH_PORTHOME_ERR_OOM;
    size_t opos = 0;

    size_t i = 0;
    int rc = NH_PORTHOME_OK;
    while (i < plen) {
        size_t j = i;
        while (j < plen && path[j] != '/') j++;
        if (j == i) { rc = NH_PORTHOME_ERR_PATH; goto path_done; }
        size_t clen = j - i;
        if (clen > 255) { rc = NH_PORTHOME_ERR_PATH; goto path_done; }
        char comp[256];
        memcpy(comp, path + i, clen);
        comp[clen] = '\0';
        char enc[NH_PORTHOME_NAME_HEX_LEN];
        rc = nh_porthome_encrypt_name(home_key, comp, enc);
        if (rc != NH_PORTHOME_OK) goto path_done;
        if (opos > 0) out[opos++] = '/';
        memcpy(out + opos, enc, NH_PORTHOME_NAME_HEX_LEN - 1);
        opos += NH_PORTHOME_NAME_HEX_LEN - 1;
        i = (j < plen) ? j + 1 : j;
    }
    out[opos] = '\0';
    *out_joined = out;
    return NH_PORTHOME_OK;

path_done:
    OPENSSL_cleanse(out, olen);
    free(out);
    *out_joined = NULL;
    return rc;
}
