/* SPDX-License-Identifier: MIT
 * Shared-vault dir_node v2. Distinct from legacy manifest.h's flat schemas.
 * Experimental; see shared-vault-composition.md §3.4. No authorization policy.
 */
#ifndef PORTHOME_DIR_NODE_H
#define PORTHOME_DIR_NODE_H
#include <porthome/manifest.h>
#ifdef __cplusplus
extern "C" {
#endif
#define PORTHOME_DIR_NODE_VERSION 2u
#define PORTHOME_VAULT_MAX_MEMBERS 4096u
/* Up to 2 GiB at the chunker's minimum size; the encoded-CBOR cap also applies. */
#define PORTHOME_NODE_MAX_CHUNKS 32768u

typedef struct {
    uint8_t vault_id[32], owner[32];
    uint8_t (*writers)[32], (*readers)[32];
    size_t writers_len, readers_len;
    uint64_t grants_rev;
} porthome_vault_info;

typedef struct {
    uint8_t sha256[32];
    uint64_t size_sealed, chunk_key_id, size_plain;
} porthome_node_chunk;

typedef struct {
    char name_enc[PORTHOME_NAME_HEX_LEN]; /* Exactly 48 lowercase hex chars; keyed index. */
    char name_plain[256]; /* Required UTF-8 basename, 1..255 bytes, inside sealed body. */
    porthome_entry_kind kind;
    uint32_t mode;
    uint64_t mtime_ns, size_plain;
    /* Directory-only fields. has_key_override is 0 or 1. */
    uint8_t node_id[32], manifest_sha256[32], key_override[32];
    uint64_t child_epoch;
    int has_key_override;
    /* File-only; decoded arrays are owned. */
    porthome_node_chunk *chunks;
    size_t chunks_len;
    /* Symlink-only; decoded string is owned. Safe relative path, no .. . */
    char *symlink_target;
} porthome_node_entry;

typedef struct {
    uint8_t node_id[32];
    uint64_t epoch; /* Nonzero; identifies the key that seals this node. */
    porthome_vault_info *vault; /* Optional; decoded block/arrays are owned. */
    porthome_node_entry *entries;
    size_t entries_len;
} porthome_dir_node;

/* Definite-length canonical CBOR, ascending integer keys, no unknown,
 * duplicate, missing or non-shortest keys/values, no trailing bytes.
 * Node map: 0=version(2), 1=node_id, 2=epoch, 3=entries, 4=vault(optional).
 * Vault: 0=vault_id, 1=owner, 2=writers, 3=readers, 4=grants_rev.
 * Entry: 0=name_enc, 1=kind, 2=mode, 3=mtime_ns, 4=size_plain, 5=payload,
 *        6=name_plain (required safe single-component UTF-8 basename).
 * Dir payload: 0=node_id, 1=child_epoch, 2=manifest_sha256, 3=override(optional).
 * File payload: array of {0=sha256, 1=size_sealed, 2=chunk_key_id, 3=size_plain}.
 * chunk_key_id is the nonzero epoch of the node key that sealed that chunk;
 * it need not equal the node's current epoch. The zero-only restriction in
 * manifest.h applies only to the PCS v1 manifest codec, not to these payloads.
 * Chunk plaintext lengths sum exactly to the file length without overflow.
 * This completes the unreleased v2 format in place; incomplete v2 maps fail.
 * Symlink payload: safe relative target text. Entry order is preserved; duplicate
 * names are refused. Epochs are nonzero, names are single tags (not paths).
 * Entry/text/CBOR caps reuse manifest.h; chunk/member resource caps are above.
 * Referenced sealed sizes may exceed the v2 chunker's maximum for v1 migration.
 * Encoder borrows input; decoder allocates all arrays/strings. On success
 * caller disposes decoded node then free()s it; raw CBOR may contain secrets.
 * On failure outputs are NULL/0. ERR_ARG includes malformed CBOR/schema. */
PORTHOME_API int porthome_dir_node_encode(const porthome_dir_node *node,
    uint8_t **out, size_t *out_len);
PORTHOME_API int porthome_dir_node_decode(const uint8_t *cbor, size_t len,
    porthome_dir_node **out);
PORTHOME_API void porthome_dir_node_dispose(porthome_dir_node *node);
PORTHOME_API int porthome_dir_node_seal(const porthome_dir_node *node,
    const uint8_t node_key[32], uint8_t **out, size_t *out_len, uint8_t address[32]);
PORTHOME_API int porthome_dir_node_open(const porthome_ring_t *ring,
    uint64_t epoch, const uint8_t *blob, size_t len, porthome_dir_node **out);
#ifdef __cplusplus
}
#endif
#endif
