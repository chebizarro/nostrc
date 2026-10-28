/*
 * porthome_crypto.h — portable-home Phase 1 crypto primitives.
 *
 * SPDX-License-Identifier: MIT
 *
 * WARNING: EXPERIMENTAL AND UNREVIEWED.
 * Experimental PCS v1/v2 primitives. Do not deploy without review.
 *
 * This module realises the D4 convergent-encryption construction from
 * docs/designs/home-from-relay.md, in the exact byte layout documented
 * in docs/designs/porthome-crypto-spec.md. That spec is the single review
 * target for D4; if you are reading this header without reading the spec,
 * stop and read the spec.
 *
 * Bead: nostrc-h10m (E-portable-home Phase 1).
 */

#ifndef PORTHOME_CRYPTO_H
#define PORTHOME_CRYPTO_H

#include <porthome/export.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ────────────────────────────────────────────────────────────────────
 * Sizes and error codes
 * ──────────────────────────────────────────────────────────────────── */

/* Home key, chunk key, manifest key — all 32 bytes. */
#define PORTHOME_KEY_LEN        32
/* ChaCha20-Poly1305 nonce. */
#define PORTHOME_NONCE_LEN      12
/* Poly1305 tag. */
#define PORTHOME_TAG_LEN        16
/* SHA-256 output. */
#define PORTHOME_SHA256_LEN     32
/* Version byte prefix on every sealed blob. Bumped in lockstep with the
 * per-purpose HKDF salt strings if the layout ever changes. */
#define PORTHOME_WIRE_VERSION   0x01
#define PORTHOME_WIRE_VERSION_V2 0x02
/* Overhead of the sealed-blob wire layout: 1 (version) + 12 (nonce) + 16 (tag). */
#define PORTHOME_SEAL_OVERHEAD  29

/* Encrypted-name form: HMAC-SHA256(name_key, component)[0..24]
 * rendered as 48 lowercase hex chars + NUL. */
#define PORTHOME_NAME_TAG_LEN   24
#define PORTHOME_NAME_HEX_LEN   (PORTHOME_NAME_TAG_LEN * 2 + 1)

/* Error codes. All success paths return 0; failures return < 0. */
typedef enum {
    PORTHOME_OK                 =  0,
    PORTHOME_ERR_ARG            = -1,
    PORTHOME_ERR_CRYPTO         = -2,
    PORTHOME_ERR_HASH_MISMATCH  = -3,
    PORTHOME_ERR_VERSION        = -4,
    PORTHOME_ERR_TOO_LARGE      = -5,
    PORTHOME_ERR_PATH           = -6,
    PORTHOME_ERR_OOM            = -7,
    PORTHOME_ERR_EPOCH          = -8, /* requested epoch absent; no fallback */
} porthome_status;

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

PORTHOME_API int porthome_key_derive(const uint8_t seed[PORTHOME_KEY_LEN],
                           uint8_t out_home_key[PORTHOME_KEY_LEN]);

/* Local-only fscrypt master; never serialize it in a PCS manifest or /run.
* account_id is the canonical lowercase hyphenated UUID (36 ASCII bytes).
* device_salt is 32 random, persistent per-device bytes. generation starts at 1.
* HKDF-SHA256 salt="porthome/local/fscrypt/v1", IKM=home_key,
* info="home\0" || account_id || "\0" || device_salt || be32(generation).
* Caller must mlock the 64-byte output and wipe it after the add-key ioctl. */
PORTHOME_API int porthome_fscrypt_master_derive(
  const uint8_t home_key[PORTHOME_KEY_LEN], const char *account_id,
  const uint8_t device_salt[32], uint32_t generation, uint8_t out_master[64]);

/* ────────────────────────────────────────────────────────────────────
 * Chunk sealing (convergent)
 *
 * Wire layout of the sealed chunk (also of the sealed manifest node):
 *   version[1] || nonce[12] || ciphertext[N] || tag[16]      (N == pt_len)
 *
 * nonce = HKDF(salt="porthome/v1/chunk", ikm=home_key, info=SHA256(pt), L=12)
 * key   = HKDF(salt="porthome/v1/chunk", ikm=home_key, info=nonce, L=32)
 * ct||tag = ChaCha20-Poly1305(key, nonce, aad=NULL, pt)
 *
 * The Blossom address of the sealed chunk is SHA256(sealed_bytes).
 * `out_sha256` (optional; may be NULL) is that address, computed for the
 * caller's convenience.
 *
 * Success writes a heap buffer to `*out_ct` (caller frees with free())
 * of length `*out_ct_len == pt_len + PORTHOME_SEAL_OVERHEAD`.
 * On failure the outputs are NULL/0 and every derived key byte is wiped.
 * ──────────────────────────────────────────────────────────────────── */

PORTHOME_API int porthome_encrypt_chunk(const uint8_t home_key[PORTHOME_KEY_LEN],
                              const uint8_t *pt, size_t pt_len,
                              uint8_t **out_ct, size_t *out_ct_len,
                              uint8_t out_sha256[PORTHOME_SHA256_LEN]);

PORTHOME_API int porthome_decrypt_chunk(const uint8_t home_key[PORTHOME_KEY_LEN],
                              const uint8_t *ct, size_t ct_len,
                              uint8_t **out_pt, size_t *out_pt_len);

/* Same construction as chunk sealing but with domain-separated salt
 * "porthome/v1/manifest". Independent key derivation from the chunk
 * path — a ciphertext produced under one purpose cannot be
 * redirected to the other. */

PORTHOME_API int porthome_encrypt_manifest(const uint8_t home_key[PORTHOME_KEY_LEN],
                                 const uint8_t *pt, size_t pt_len,
                                 uint8_t **out_ct, size_t *out_ct_len);

PORTHOME_API int porthome_decrypt_manifest(const uint8_t home_key[PORTHOME_KEY_LEN],
                                 const uint8_t *ct, size_t ct_len,
                                 uint8_t **out_pt, size_t *out_pt_len);

/* ────────────────────────────────────────────────────────────────────
 * Name encryption (keyed hash; see spec §7)
 *
 * Deterministic per (home_key, component). 48 lowercase hex chars + NUL.
 *
 * `component` must be a single path component (no '/'), not empty,
 * not "." and not "..", not longer than 255 bytes. Otherwise:
 * PORTHOME_ERR_PATH.
 *
 * `porthome_encrypt_path` splits `path` on '/', validates each
 * component, encrypts, and joins the results with '/'. A leading '/'
 * (absolute path) is stripped; a trailing '/' is refused. Returns
 * a heap C-string in `*out_joined` (caller frees).
 * ──────────────────────────────────────────────────────────────────── */

PORTHOME_API int porthome_encrypt_name(const uint8_t home_key[PORTHOME_KEY_LEN],
                             const char *component,
                             char out_hex[PORTHOME_NAME_HEX_LEN]);

PORTHOME_API int porthome_encrypt_path(const uint8_t home_key[PORTHOME_KEY_LEN],
                             const char *path,
                             char **out_joined);

/* ────────────────────────────────────────────────────────────────────
 * Name-field AEAD sealing (schema-v2, bead nostrc-q25o).
 *
 * Unlike `porthome_encrypt_name` (one-way keyed HMAC → 48-hex, used
 * for the on-relay `path_enc`), this pair AEAD-seals arbitrary UTF-8
 * bytes (typically a single basename or a symlink target) under
 * salt="porthome/v1/name", so the pulling party can DECRYPT them back
 * to plaintext and rename the materialised tree from path_enc form to
 * the operator-visible layout.
 *
 * Same convergent-AEAD construction as chunks/manifests (D4): same
 * (home_key, plaintext) → same sealed bytes. That gives the schema-v2
 * manifest deterministic name_sealed rows for a fixed home_key. */

PORTHOME_API int porthome_encrypt_name_field(const uint8_t home_key[PORTHOME_KEY_LEN],
                                   const uint8_t *pt, size_t pt_len,
                                   uint8_t **out_ct, size_t *out_ct_len);

PORTHOME_API int porthome_decrypt_name_field(const uint8_t home_key[PORTHOME_KEY_LEN],
                                   const uint8_t *ct, size_t ct_len,
                                   uint8_t **out_pt, size_t *out_pt_len);

/* ────────────────────────────────────────────────────────────────────
 * Utility: SHA-256 of an arbitrary buffer (thin wrapper).
 * Provided so callers of the wrapper (Blossom) do not need a second
 * SHA-256 implementation for content-addressing.
 * ──────────────────────────────────────────────────────────────────── */

PORTHOME_API int porthome_sha256(const uint8_t *buf, size_t len,
                       uint8_t out[PORTHOME_SHA256_LEN]);

/* Render a 32-byte hash as 64 lowercase hex chars + NUL. */
PORTHOME_API void porthome_hex64(const uint8_t in[PORTHOME_SHA256_LEN],
                       char out_hex[65]);

/* Parse 64 hex chars into a 32-byte buffer. Returns 0 on success. */
PORTHOME_API int porthome_from_hex64(const char *in_hex,
                           uint8_t out[PORTHOME_SHA256_LEN]);

/* PCS v2 cryptree. All keys and node IDs are 32 bytes. A non-NULL
 * key_override is copied verbatim instead of deriving from parent/node_id.
 * Input/output key buffers may alias. Head derivation reduces modulo the
 * secp256k1 order; the negligible zero result fails with ERR_CRYPTO. */
PORTHOME_API int porthome_root_node_key(const uint8_t seed[32], uint8_t out[32]);
PORTHOME_API int porthome_child_node_key(const uint8_t parent[32],
    const uint8_t node_id[32], const uint8_t *key_override, uint8_t out[32]);
PORTHOME_API int porthome_name_key_v2(const uint8_t node_key[32], uint8_t out[32]);
PORTHOME_API int porthome_head_key_v2(const uint8_t node_key[32], uint8_t out[32]);
PORTHOME_API int porthome_encrypt_name_v2(const uint8_t node_key[32],
    const char *component, char out_hex[PORTHOME_NAME_HEX_LEN]);

/* Borrowed, immutable view. Entries must have strictly increasing, nonzero
 * epochs. Epochs are metadata, not bytes in the sealed envelope. Select exactly
 * chunk_key_id (or manifest epoch); never attempt another key after failure.
 * Callers verify the requested Blossom address BEFORE invoking decode. */
typedef struct {
    uint64_t epoch;
    uint8_t node_key[32];
} porthome_ring_entry_t;
typedef struct {
    const porthome_ring_entry_t *entries;
    size_t count;
} porthome_ring_t;

/* Explicit v2 paths; v1 APIs above never accept v2 and vice versa.
 * Chunks: nonce binds SHA256(original plaintext), then deterministic zero
 * padding to the NIP-44 bucket length (no NIP-44 u16 length prefix).
 * size_plain comes from the authenticated manifest. Decode authenticates all
 * padded bytes, checks the bucket length and zero tail, then truncates.
 * Manifests: same v2 layout/domain separation, WITHOUT chunk padding; the
 * manifest codec/schema is separate. Both return malloc buffers (free()).
 * Valid output pointers are reset to NULL/0 on all failures. */
PORTHOME_API int porthome_encrypt_chunk_v2(const uint8_t node_key[32],
    const uint8_t *pt, size_t pt_len, uint8_t **out, size_t *out_len,
    uint8_t out_sha256[32]);
PORTHOME_API int porthome_decrypt_chunk_v2(const porthome_ring_t *ring,
    uint64_t chunk_key_id, const uint8_t *blob, size_t blob_len,
    size_t size_plain, uint8_t **out, size_t *out_len);
PORTHOME_API int porthome_encrypt_manifest_v2(const uint8_t node_key[32],
    const uint8_t *pt, size_t pt_len, uint8_t **out, size_t *out_len,
    uint8_t out_sha256[32]);
PORTHOME_API int porthome_decrypt_manifest_v2(const porthome_ring_t *ring,
    uint64_t epoch, const uint8_t *blob, size_t blob_len,
    uint8_t **out, size_t *out_len);

/* Pure buzhash content-defined chunker. Repeatedly call on the remaining
 * suffix. Returns the first cut, or len at EOF (the final chunk may be below
 * MIN); returns 0 for empty input or NULL. Rolling window: 64 bytes; nominal
 * target 256 KiB, minimum 64 KiB, hard maximum 1 MiB. */
#define PORTHOME_CHUNK_MIN    (64u * 1024u)
#define PORTHOME_CHUNK_TARGET (256u * 1024u)
#define PORTHOME_CHUNK_MAX    (1024u * 1024u)
PORTHOME_API size_t porthome_chunk_next(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* PORTHOME_CRYPTO_H */
