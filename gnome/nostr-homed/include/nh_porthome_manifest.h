/*
 * nh_porthome_manifest.h — portable-home Phase 1 manifest schema + codec.
 *
 * SPDX-License-Identifier: MIT
 *
 * WARNING: EXPERIMENTAL AND UNREVIEWED.
 * Gated behind NOSTR_HOMED_ENABLE_PORTHOME_EXPERIMENTAL. Do not deploy.
 *
 * A "manifest" in the Phase-1 sense is the encrypted-pointer form of
 * docs/designs/home-from-relay.md §1: a versioned list of entries whose
 * paths are the encrypted-name form (see nh_porthome_crypto.h,
 * nh_porthome_encrypt_path). It is CBOR-serialised with canonical
 * ordering (RFC 8949 §4.2.1: definite-length, ascending integer keys,
 * no duplicates) and then AEAD-sealed via nh_porthome_encrypt_manifest.
 *
 * The bytes stored on the relay as a kind-30078 event's `content` will,
 * in Phase 2, be the NIP-44 self-encryption of this AEAD-sealed CBOR;
 * Phase 1 exposes the AEAD-sealed CBOR as-is because it needs no
 * identity keypair to test.
 *
 * The schema version is 1. Decoders MUST refuse any other version.
 * Bead: nostrc-h10m.
 */

#ifndef NH_PORTHOME_MANIFEST_H
#define NH_PORTHOME_MANIFEST_H

#include "nh_porthome_crypto.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NH_PORTHOME_MANIFEST_SCHEMA_VERSION  1u

/* Hard caps (mirror design §5.4). Any decoded value exceeding a cap is
 * a fatal parse error (NH_PORTHOME_MANIFEST_ERR_TOO_LARGE). */
#define NH_PORTHOME_MAX_ENTRIES        500000u
#define NH_PORTHOME_MAX_CHUNKS_PER_ENTRY  64u  /* 64 * 4 MiB = 256 MiB per entry — Phase 1 is a fixture, not a real 2 GiB file */
#define NH_PORTHOME_MAX_PATH_LEN        4096u
#define NH_PORTHOME_MAX_SYMLINK_LEN     4096u
#define NH_PORTHOME_MAX_CBOR_BYTES      (1u * 1024u * 1024u)  /* 1 MiB per encoded manifest — design §5.4 */

typedef enum {
    NH_PORTHOME_KIND_FILE    = 1,
    NH_PORTHOME_KIND_DIR     = 2,
    NH_PORTHOME_KIND_SYMLINK = 3,
} nh_porthome_entry_kind;

typedef struct {
    uint8_t  sha256[NH_PORTHOME_SHA256_LEN];  /* Blossom address of the sealed chunk */
    uint32_t size;                            /* Sealed size on Blossom (plaintext_len + 29) */
    uint32_t chunk_key_id;                    /* Reserved; MUST be 0 in Phase 1. */
} nh_porthome_chunk;

typedef struct {
    /* Encrypted-path form (slash-joined 48-hex components). Owned. */
    char    *path_enc;
    /* File mode (masked, sticky/setuid stripped at apply-time by the caller). */
    uint32_t mode;
    /* Local uid/gid hints. Not authoritative — see design §2.3 (uid/gid
     * are machine-local; the applier chown()s to the local account). */
    uint32_t uid_hint;
    uint32_t gid_hint;
    uint64_t mtime_ns;
    /* Original file size (bytes). For dirs/symlinks: 0. */
    uint64_t size;
    nh_porthome_entry_kind kind;
    /* Only present when kind == FILE. Owned array; length in chunks_len. */
    nh_porthome_chunk *chunks;
    size_t            chunks_len;
    /* Only present when kind == SYMLINK. Owned. */
    char *symlink_target;
} nh_porthome_entry;

typedef struct {
    uint32_t version;                                  /* Schema version — MUST equal 1. */
    uint8_t  home_root_id[NH_PORTHOME_SHA256_LEN];     /* Opaque root identifier (per-home). */
    nh_porthome_entry *entries;                        /* Owned. */
    size_t            entries_len;
} nh_porthome_manifest;

/* Constructors and destructor. */
int  nh_porthome_manifest_init(nh_porthome_manifest *out,
                               const uint8_t home_root_id[NH_PORTHOME_SHA256_LEN]);
void nh_porthome_manifest_dispose(nh_porthome_manifest *m);
void nh_porthome_entry_dispose(nh_porthome_entry *e);

/* Add-entry helpers. Each takes ownership of the strings — pass a
 * heap-allocated `path_enc` (typically from nh_porthome_encrypt_path).
 * Returns 0 on success; the entry is copied into the manifest. */
int nh_porthome_manifest_add_file(nh_porthome_manifest *m,
                                  char *path_enc /* moved */,
                                  uint32_t mode,
                                  uint32_t uid_hint,
                                  uint32_t gid_hint,
                                  uint64_t mtime_ns,
                                  uint64_t size,
                                  const nh_porthome_chunk *chunks,
                                  size_t chunks_len);
int nh_porthome_manifest_add_dir(nh_porthome_manifest *m,
                                 char *path_enc,
                                 uint32_t mode,
                                 uint32_t uid_hint,
                                 uint32_t gid_hint,
                                 uint64_t mtime_ns);
int nh_porthome_manifest_add_symlink(nh_porthome_manifest *m,
                                     char *path_enc,
                                     uint32_t mode,
                                     uint32_t uid_hint,
                                     uint32_t gid_hint,
                                     uint64_t mtime_ns,
                                     char *symlink_target /* moved */);

/* ────────────────────────────────────────────────────────────────────
 * Codec (canonical CBOR + strict decoding)
 * ──────────────────────────────────────────────────────────────────── */

/* Encode a manifest to canonical CBOR. Success: `*out` is a heap
 * buffer (caller frees with free()) of length `*out_len`. */
int nh_porthome_manifest_encode(const nh_porthome_manifest *m,
                                uint8_t **out, size_t *out_len);

/* Decode CBOR into a fresh nh_porthome_manifest (caller must dispose
 * with nh_porthome_manifest_dispose + free). Strict:
 *   - refuses trailing bytes,
 *   - refuses duplicate map keys,
 *   - refuses non-canonical (indefinite-length, non-ascending keys),
 *   - enforces every cap listed in this header,
 *   - refuses unknown schema versions,
 *   - refuses non-zero `chunk_key_id` (Phase 1 has no other keys),
 *   - refuses paths that are absolute, ".", "..", contain "/./", "/../",
 *     start with "/", end with "/", or contain NUL / '\\',
 *   - refuses non-file entries carrying `chunks`,
 *   - refuses non-symlink entries carrying `symlink_target`,
 *   - refuses file entries without `chunks` when size > 0. */
int nh_porthome_manifest_decode(const uint8_t *cbor, size_t cbor_len,
                                nh_porthome_manifest **out);

/* Encrypted round-trip (convenience): encode + AEAD-seal. Caller frees. */
int nh_porthome_manifest_encode_sealed(const nh_porthome_manifest *m,
                                       const uint8_t home_key[NH_PORTHOME_KEY_LEN],
                                       uint8_t **out, size_t *out_len);
int nh_porthome_manifest_decode_sealed(const uint8_t *sealed, size_t sealed_len,
                                       const uint8_t home_key[NH_PORTHOME_KEY_LEN],
                                       nh_porthome_manifest **out);

#ifdef __cplusplus
}
#endif

#endif /* NH_PORTHOME_MANIFEST_H */
