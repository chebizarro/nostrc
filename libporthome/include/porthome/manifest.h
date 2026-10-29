/*
 * porthome_manifest.h — portable-home Phase 1 manifest schema + codec.
 *
 * SPDX-License-Identifier: MIT
 *
 * WARNING: EXPERIMENTAL AND UNREVIEWED.
 * Experimental PCS v1 manifest primitives. Do not deploy without review.
 *
 * A "manifest" in the Phase-1 sense is the encrypted-pointer form of
 * docs/designs/home-from-relay.md §1: a versioned list of entries whose
 * paths are the encrypted-name form (see porthome_crypto.h,
 * porthome_encrypt_path). It is CBOR-serialised with canonical
 * ordering (RFC 8949 §4.2.1: definite-length, ascending integer keys,
 * no duplicates) and then AEAD-sealed via porthome_encrypt_manifest.
 *
 * The bytes stored on the relay as a kind-30078 event's `content` will,
 * in Phase 2, be the NIP-44 self-encryption of this AEAD-sealed CBOR;
 * Phase 1 exposes the AEAD-sealed CBOR as-is because it needs no
 * identity keypair to test.
 *
 * The schema version is 1. Decoders MUST refuse any other version.
 * Bead: nostrc-h10m.
 */

#ifndef PORTHOME_MANIFEST_H
#define PORTHOME_MANIFEST_H

#include <porthome/crypto.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Schema versions in flight.
 *   v1 (legacy): path_enc only. Emitted by pre-q25o pushes. Parse-forward
 *                only — the encoder never produces v1 again.
 *   v2 (current, nostrc-q25o): additive `name_sealed` + (for symlinks)
 *                `link_target_sealed` fields carrying the operator-visible
 *                plaintext under a home-key-derived AEAD. Enables the
 *                rename walk in nostr-home-fetch (nostrc-bms6). */
#define PORTHOME_MANIFEST_SCHEMA_VERSION_V1  1u
#define PORTHOME_MANIFEST_SCHEMA_VERSION_V2  2u
#define PORTHOME_MANIFEST_SCHEMA_VERSION     PORTHOME_MANIFEST_SCHEMA_VERSION_V2

/* Hard caps (mirror design §5.4). Any decoded value exceeding a cap is
 * a fatal parse error (PORTHOME_MANIFEST_ERR_TOO_LARGE). */
#define PORTHOME_MAX_ENTRIES        500000u
#define PORTHOME_MAX_CHUNKS_PER_ENTRY  64u  /* 64 * 4 MiB = 256 MiB per entry — Phase 1 is a fixture, not a real 2 GiB file */
#define PORTHOME_MAX_PATH_LEN        4096u
#define PORTHOME_MAX_SYMLINK_LEN     4096u
#define PORTHOME_MAX_CBOR_BYTES      (1u * 1024u * 1024u)  /* 1 MiB per encoded manifest — design §5.4 */

typedef enum {
    PORTHOME_KIND_FILE    = 1,
    PORTHOME_KIND_DIR     = 2,
    PORTHOME_KIND_SYMLINK = 3,
} porthome_entry_kind;

typedef struct {
    uint8_t  sha256[PORTHOME_SHA256_LEN];  /* Blossom address of the sealed chunk */
    uint32_t size;                            /* Sealed size on Blossom (plaintext_len + 29) */
    uint32_t chunk_key_id;                    /* MUST be 0 for the PCS v1 manifest codec only.
                                              * PCS v2 dir-node payloads carry the per-chunk
                                              * epoch here; see <porthome/dir-node.h>. */
} porthome_chunk;

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
    porthome_entry_kind kind;
    /* Optional on-wire directory marker. False is omitted on encode.
     * Marked directories and descendants remain in the manifest, but
     * provisioners skip eager content materialization. */
    bool lazy;
    /* Only present when kind == FILE. Owned array; length in chunks_len. */
    porthome_chunk *chunks;
    size_t            chunks_len;
    /* Only present when kind == SYMLINK. Owned. */
    char *symlink_target;

    /* ── Schema v2 (nostrc-q25o) — additive plaintext-name material ──
     *
     * The decoder stores the raw sealed bytes into `name_sealed` /
     * `link_target_sealed` (strict CBOR: opaque byte-strings, no
     * inner parsing). The plaintext members below are populated by
     * `porthome_manifest_open_names(m, home_key)` (called by the
     * `_sealed` wrapper on decode). Absent on v1 manifests. Owned. */
    uint8_t *name_sealed;
    size_t   name_sealed_len;
    /* NUL-terminated single-component basename (UTF-8, ≤ 255 bytes).
     * Rejected during open_names if it contains '/', NUL, '\', or
     * equals "." / ".." (path-smuggling defence). */
    char    *name_plain;

    /* Only meaningful when kind == SYMLINK. Owned. */
    uint8_t *link_target_sealed;
    size_t   link_target_sealed_len;
    char    *link_target_plain;
} porthome_entry;

typedef struct {
    uint32_t version;                                  /* Schema version — 1 (parse-forward) or 2 (current). */
    uint8_t  home_root_id[PORTHOME_SHA256_LEN];     /* Opaque root identifier (per-home). */
    porthome_entry *entries;                        /* Owned. */
    size_t            entries_len;
} porthome_manifest;

/* Constructors and destructor.
 *
 * `porthome_manifest_init` produces a v1-shaped manifest for
 * backward compat with pre-q25o callers (syncd, existing tests).
 * `porthome_manifest_init_v2` produces a v2 manifest — the encoder
 * will then require the v2 add helpers (which populate `name_plain`)
 * and refuse to serialize entries that lack sealed-name material. */
PORTHOME_API int  porthome_manifest_init(porthome_manifest *out,
                               const uint8_t home_root_id[PORTHOME_SHA256_LEN]);
PORTHOME_API int  porthome_manifest_init_v2(porthome_manifest *out,
                                  const uint8_t home_root_id[PORTHOME_SHA256_LEN]);
PORTHOME_API void porthome_manifest_dispose(porthome_manifest *m);
PORTHOME_API void porthome_entry_dispose(porthome_entry *e);

/* Add-entry helpers. Each takes ownership of the strings — pass a
 * heap-allocated `path_enc` (typically from porthome_encrypt_path).
 * Returns 0 on success; the entry is copied into the manifest. */
PORTHOME_API int porthome_manifest_add_file(porthome_manifest *m,
                                  char *path_enc /* moved */,
                                  uint32_t mode,
                                  uint32_t uid_hint,
                                  uint32_t gid_hint,
                                  uint64_t mtime_ns,
                                  uint64_t size,
                                  const porthome_chunk *chunks,
                                  size_t chunks_len);
PORTHOME_API int porthome_manifest_add_dir(porthome_manifest *m,
                                 char *path_enc,
                                 uint32_t mode,
                                 uint32_t uid_hint,
                                 uint32_t gid_hint,
                                 uint64_t mtime_ns);
PORTHOME_API int porthome_manifest_add_symlink(porthome_manifest *m,
                                     char *path_enc,
                                     uint32_t mode,
                                     uint32_t uid_hint,
                                     uint32_t gid_hint,
                                     uint64_t mtime_ns,
                                     char *symlink_target /* moved */);

/* ────────────────────────────────────────────────────────────────────
 * Schema-v2 constructors (nostrc-q25o).
 *
 * Same shape as the v1 add helpers plus a plaintext-name parameter
 * (single basename, ≤ 255 bytes; must not be "." / "..", must not
 * contain '/', '\\' or NUL). The manifest encoder AEAD-seals it under
 * a home-key-derived key and emits K_E_NAME_SEALED. Ownership of the
 * plaintext `name` string moves into the manifest (the encoder copies
 * it into `name_plain` and free()s it in dispose).
 *
 * The v1 helpers above continue to compile against unchanged callers
 * (syncd, fuse) and leave `name_plain` NULL — the encoder will then
 * skip the name_sealed slot for those entries, which the design allows
 * during the migration. New code SHOULD prefer the v2 helpers. */
PORTHOME_API int porthome_manifest_add_file_v2(porthome_manifest *m,
                                     char *path_enc /* moved */,
                                     char *name /* moved: plaintext basename */,
                                     uint32_t mode,
                                     uint32_t uid_hint,
                                     uint32_t gid_hint,
                                     uint64_t mtime_ns,
                                     uint64_t size,
                                     const porthome_chunk *chunks,
                                     size_t chunks_len);
PORTHOME_API int porthome_manifest_add_dir_v2(porthome_manifest *m,
                                    char *path_enc,
                                    char *name,
                                    uint32_t mode,
                                    uint32_t uid_hint,
                                    uint32_t gid_hint,
                                    uint64_t mtime_ns);
PORTHOME_API int porthome_manifest_add_symlink_v2(porthome_manifest *m,
                                        char *path_enc,
                                        char *name,
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
PORTHOME_API int porthome_manifest_encode(const porthome_manifest *m,
                                uint8_t **out, size_t *out_len);

/* Decode CBOR into a fresh porthome_manifest (caller must dispose
 * with porthome_manifest_dispose + free). Strict:
 *   - refuses trailing bytes,
 *   - refuses duplicate map keys,
 *   - refuses non-canonical (indefinite-length, non-ascending keys),
 *   - enforces every cap listed in this header,
 *   - refuses unknown schema versions,
 *   - the PCS v1 manifest codec alone refuses non-zero `chunk_key_id`
 *     with PORTHOME_ERR_EPOCH; PCS v2 dir-node payloads carry the per-chunk
 *     epoch in that field (see <porthome/dir-node.h>),
 *   - refuses paths that are absolute, ".", "..", contain "/./", "/../",
 *     start with "/", end with "/", or contain NUL / '\\',
 *   - refuses a lazy marker on non-directory entries or any marker value
 *     other than the canonical unsigned integer 1,
 *   - refuses non-file entries carrying `chunks`,
 *   - refuses non-symlink entries carrying `symlink_target`,
 *   - refuses file entries without `chunks` when size > 0.
 * A valid PCS v2 dir-node also returns PORTHOME_ERR_EPOCH, never a flat
 * manifest with zeroed epochs. Epoch refusals leave `*out` NULL. */
PORTHOME_API int porthome_manifest_decode(const uint8_t *cbor, size_t cbor_len,
                                porthome_manifest **out);

/* Encrypted round-trip (convenience): encode + AEAD-seal. Caller frees.
 * `encode_sealed` seals every entry's plaintext name material FIRST
 * (in-place assignment of e->name_sealed etc.), then AEAD-seals the
 * canonical CBOR under the manifest key. `decode_sealed` reverses:
 * strict CBOR parse THEN `porthome_manifest_open_names` on the
 * result so downstream code sees `name_plain` / `symlink_target`
 * populated for v2 pointers (and NULL for v1). Both flat name schemas
 * use PCS v1 sealing, not the PCS v2 dir-node codec in <porthome/dir-node.h>.
 * PCS v2 sealed envelopes return PORTHOME_ERR_EPOCH; CBOR epoch errors
 * propagate unchanged. Epoch refusals leave `*out` NULL. */
PORTHOME_API int porthome_manifest_encode_sealed(const porthome_manifest *m,
                                       const uint8_t home_key[PORTHOME_KEY_LEN],
                                       uint8_t **out, size_t *out_len);
PORTHOME_API int porthome_manifest_decode_sealed(const uint8_t *sealed, size_t sealed_len,
                                       const uint8_t home_key[PORTHOME_KEY_LEN],
                                       porthome_manifest **out);

/* ────────────────────────────────────────────────────────────────────
 * Name-material seal / open (schema v2, nostrc-q25o)
 * ──────────────────────────────────────────────────────────────────── */

/* Populate `name_sealed` (and `link_target_sealed` for symlinks) from
 * `name_plain` (and `symlink_target`) on every entry that has plaintext
 * set. Existing sealed bytes are freed and rewritten. Called by
 * `encode_sealed`; exported so tests can drive it. */
PORTHOME_API int porthome_manifest_seal_names(porthome_manifest *m,
                                    const uint8_t home_key[PORTHOME_KEY_LEN]);

/* Populate `name_plain` (and `symlink_target`) from `name_sealed`
 * (and `link_target_sealed`) on every entry that has sealed bytes.
 * REJECTS the whole manifest (returns PORTHOME_ERR_PATH) if any
 * decrypted basename is "." / ".." / contains '/' or NUL, or if a
 * symlink target is absolute or exceeds PORTHOME_MAX_SYMLINK_LEN.
 * v1 manifests (no sealed bytes) succeed with plaintext members left
 * NULL — the fetch materialiser detects that and skips its rename
 * walk. Called by `decode_sealed`; exported so tests can drive it. */
PORTHOME_API int porthome_manifest_open_names(porthome_manifest *m,
                                    const uint8_t home_key[PORTHOME_KEY_LEN]);

#ifdef __cplusplus
}
#endif

#endif /* PORTHOME_MANIFEST_H */
