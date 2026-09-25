/*
 * nh_porthome_crypto.h — portable-home Phase 1 crypto primitives.
 *
 * SPDX-License-Identifier: MIT
 *
 * WARNING: EXPERIMENTAL AND UNREVIEWED.
 * Gated behind NOSTR_HOMED_ENABLE_PORTHOME_EXPERIMENTAL. Do not deploy.
 *
 * This module realises the D4 convergent-encryption construction from
 * docs/designs/home-from-relay.md, in the exact byte layout documented
 * in docs/designs/porthome-crypto-spec.md. That spec is the single review
 * target for D4; if you are reading this header without reading the spec,
 * stop and read the spec.
 *
 * Bead: nostrc-h10m (E-portable-home Phase 1).
 */

#ifndef NH_PORTHOME_CRYPTO_H
#define NH_PORTHOME_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ────────────────────────────────────────────────────────────────────
 * Sizes and error codes
 * ──────────────────────────────────────────────────────────────────── */

/* Home key, chunk key, manifest key — all 32 bytes. */
#define NH_PORTHOME_KEY_LEN        32
/* ChaCha20-Poly1305 nonce. */
#define NH_PORTHOME_NONCE_LEN      12
/* Poly1305 tag. */
#define NH_PORTHOME_TAG_LEN        16
/* SHA-256 output. */
#define NH_PORTHOME_SHA256_LEN     32
/* Version byte prefix on every sealed blob. Bumped in lockstep with the
 * per-purpose HKDF salt strings if the layout ever changes. */
#define NH_PORTHOME_WIRE_VERSION   0x01
/* Overhead of the sealed-blob wire layout: 1 (version) + 12 (nonce) + 16 (tag). */
#define NH_PORTHOME_SEAL_OVERHEAD  29

/* Encrypted-name form: HMAC-SHA256(name_key, component)[0..24]
 * rendered as 48 lowercase hex chars + NUL. */
#define NH_PORTHOME_NAME_TAG_LEN   24
#define NH_PORTHOME_NAME_HEX_LEN   (NH_PORTHOME_NAME_TAG_LEN * 2 + 1)

/* Error codes. All success paths return 0; failures return < 0. */
typedef enum {
    NH_PORTHOME_OK                 =  0,
    NH_PORTHOME_ERR_ARG            = -1,
    NH_PORTHOME_ERR_CRYPTO         = -2,
    NH_PORTHOME_ERR_HASH_MISMATCH  = -3,
    NH_PORTHOME_ERR_VERSION        = -4,
    NH_PORTHOME_ERR_TOO_LARGE      = -5,
    NH_PORTHOME_ERR_PATH           = -6,
    NH_PORTHOME_ERR_OOM            = -7,
} nh_porthome_status;

/* ────────────────────────────────────────────────────────────────────
 * Home key derivation
 *
 * Phase 1: seed[32] is a caller-provided fixture. In production (Phase 2)
 * the broker will source the seed from the kind-30078 pointer's
 * `wrapped_home_key` via NIP-46 nip44_decrypt — see design §4.3. That
 * production plumbing is NOT implemented here.
 *
 * home_key = HKDF-SHA256(salt="porthome/v1/home", ikm=seed, info="", L=32).
 * ──────────────────────────────────────────────────────────────────── */

int nh_porthome_key_derive(const uint8_t seed[NH_PORTHOME_KEY_LEN],
                           uint8_t out_home_key[NH_PORTHOME_KEY_LEN]);

/* ────────────────────────────────────────────────────────────────────
 * Chunk sealing (convergent)
 *
 * Wire layout of the sealed chunk (also of the sealed manifest node):
 *   version[1] || nonce[12] || ciphertext[N] || tag[16]      (N == pt_len)
 *
 * key   = HKDF(salt="porthome/v1/chunk",    ikm=home_key, info=SHA256(pt), L=44)[0..32]
 * nonce = HKDF(salt="porthome/v1/chunk",    ikm=home_key, info=SHA256(pt), L=44)[32..44]
 * ct||tag = ChaCha20-Poly1305(key, nonce, aad=NULL, pt)
 *
 * The Blossom address of the sealed chunk is SHA256(sealed_bytes).
 * `out_sha256` (optional; may be NULL) is that address, computed for the
 * caller's convenience.
 *
 * Success writes a heap buffer to `*out_ct` (caller frees with free())
 * of length `*out_ct_len == pt_len + NH_PORTHOME_SEAL_OVERHEAD`.
 * On failure the outputs are NULL/0 and every derived key byte is wiped.
 * ──────────────────────────────────────────────────────────────────── */

int nh_porthome_encrypt_chunk(const uint8_t home_key[NH_PORTHOME_KEY_LEN],
                              const uint8_t *pt, size_t pt_len,
                              uint8_t **out_ct, size_t *out_ct_len,
                              uint8_t out_sha256[NH_PORTHOME_SHA256_LEN]);

int nh_porthome_decrypt_chunk(const uint8_t home_key[NH_PORTHOME_KEY_LEN],
                              const uint8_t *ct, size_t ct_len,
                              uint8_t **out_pt, size_t *out_pt_len);

/* Same construction as chunk sealing but with domain-separated salt
 * "porthome/v1/manifest". Independent key derivation from the chunk
 * path — a ciphertext produced under one purpose cannot be
 * redirected to the other. */

int nh_porthome_encrypt_manifest(const uint8_t home_key[NH_PORTHOME_KEY_LEN],
                                 const uint8_t *pt, size_t pt_len,
                                 uint8_t **out_ct, size_t *out_ct_len);

int nh_porthome_decrypt_manifest(const uint8_t home_key[NH_PORTHOME_KEY_LEN],
                                 const uint8_t *ct, size_t ct_len,
                                 uint8_t **out_pt, size_t *out_pt_len);

/* ────────────────────────────────────────────────────────────────────
 * Name encryption (keyed hash; see spec §7)
 *
 * Deterministic per (home_key, component). 48 lowercase hex chars + NUL.
 *
 * `component` must be a single path component (no '/'), not empty,
 * not "." and not "..", not longer than 255 bytes. Otherwise:
 * NH_PORTHOME_ERR_PATH.
 *
 * `nh_porthome_encrypt_path` splits `path` on '/', validates each
 * component, encrypts, and joins the results with '/'. A leading '/'
 * (absolute path) is stripped; a trailing '/' is refused. Returns
 * a heap C-string in `*out_joined` (caller frees).
 * ──────────────────────────────────────────────────────────────────── */

int nh_porthome_encrypt_name(const uint8_t home_key[NH_PORTHOME_KEY_LEN],
                             const char *component,
                             char out_hex[NH_PORTHOME_NAME_HEX_LEN]);

int nh_porthome_encrypt_path(const uint8_t home_key[NH_PORTHOME_KEY_LEN],
                             const char *path,
                             char **out_joined);

/* ────────────────────────────────────────────────────────────────────
 * Name-field AEAD sealing (schema-v2, bead nostrc-q25o).
 *
 * Unlike `nh_porthome_encrypt_name` (one-way keyed HMAC → 48-hex, used
 * for the on-relay `path_enc`), this pair AEAD-seals arbitrary UTF-8
 * bytes (typically a single basename or a symlink target) under
 * salt="porthome/v1/name", so the pulling party can DECRYPT them back
 * to plaintext and rename the materialised tree from path_enc form to
 * the operator-visible layout.
 *
 * Same convergent-AEAD construction as chunks/manifests (D4): same
 * (home_key, plaintext) → same sealed bytes. That gives the schema-v2
 * manifest deterministic name_sealed rows for a fixed home_key. */

int nh_porthome_encrypt_name_field(const uint8_t home_key[NH_PORTHOME_KEY_LEN],
                                   const uint8_t *pt, size_t pt_len,
                                   uint8_t **out_ct, size_t *out_ct_len);

int nh_porthome_decrypt_name_field(const uint8_t home_key[NH_PORTHOME_KEY_LEN],
                                   const uint8_t *ct, size_t ct_len,
                                   uint8_t **out_pt, size_t *out_pt_len);

/* ────────────────────────────────────────────────────────────────────
 * Utility: SHA-256 of an arbitrary buffer (thin wrapper).
 * Provided so callers of the wrapper (Blossom) do not need a second
 * SHA-256 implementation for content-addressing.
 * ──────────────────────────────────────────────────────────────────── */

int nh_porthome_sha256(const uint8_t *buf, size_t len,
                       uint8_t out[NH_PORTHOME_SHA256_LEN]);

/* Render a 32-byte hash as 64 lowercase hex chars + NUL. */
void nh_porthome_hex64(const uint8_t in[NH_PORTHOME_SHA256_LEN],
                       char out_hex[65]);

/* Parse 64 hex chars into a 32-byte buffer. Returns 0 on success. */
int nh_porthome_from_hex64(const char *in_hex,
                           uint8_t out[NH_PORTHOME_SHA256_LEN]);

#ifdef __cplusplus
}
#endif

#endif /* NH_PORTHOME_CRYPTO_H */
