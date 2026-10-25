/* SPDX-License-Identifier: MIT */
#ifndef VAULT_TREE_H
#define VAULT_TREE_H
#include <vault/vault.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Borrowed verified manifests, valid until record_free(). Only reachable nodes
 * are exposed; addresses/child IDs/epochs have been checked by record_open(). */
VAULT_API size_t vault_record_manifest_count(const vault_record *);
VAULT_API const porthome_dir_node *vault_record_manifest_at(const vault_record *, size_t,
                                                            const uint8_t **address);

typedef struct {
    const porthome_dir_node *node;
    porthome_ring_t ring;
} vault_tree_node;
typedef struct vault_tree vault_tree;
typedef int (*vault_tree_ring_fn)(void *, const uint8_t node_id[32], uint64_t epoch,
                                  porthome_ring_t *);
/* Fetch a signed head's complete manifest graph through addressed reads. Exact
 * held history is optional for new children: current overrides or derivation
 * supply their current key, never a decryption/key trial. Missing historical
 * chunk keys remain missing. Hashes precede decode; node IDs/epochs are bound.
 * Both outputs are owned. Aggregate manifest reception is capped at 64 MiB. */
VAULT_API int vault_record_fetch(const struct _NostrEvent *, const uint8_t vault_id[32],
                                 const uint8_t root_node_id[32], const porthome_ring_t *root_ring,
                                 uint64_t epoch, vault_tree_ring_fn held, void *,
                                 const vault_blob_io *, vault_record **, vault_tree **);

/* Copies a closed rooted graph (root is index 0) and exact per-node rings.
 * No I/O. Inputs remain caller-owned. Accessors borrow immutable views. */
VAULT_API int vault_tree_new(const vault_tree_node *, size_t, vault_tree **);
VAULT_API int vault_tree_from_record(const vault_record *, vault_tree_ring_fn, void *,
                                     vault_tree **);
VAULT_API size_t vault_tree_count(const vault_tree *);
VAULT_API const porthome_dir_node *vault_tree_node_at(const vault_tree *, size_t,
                                                      porthome_ring_t *);
VAULT_API void vault_tree_free(vault_tree *);
/* Owner assertion only; actual authority requires normal owner-signed publish.
 * Read membership is informational. Adding read never removes writer authority.
 * No change/revision bump if already present. */
VAULT_API int vault_tree_add_member(vault_tree *, const uint8_t actor[32], const uint8_t member[32],
                                    vault_cap);
/* Atomic recursive proposal. Uses vault_revoke for the root, rotates EVERY
 * descendant directory and rehashes the basenames inside each manifest. Chunks
 * keep their old epochs. Refuses nested share boundaries. Nothing is published
 * or deleted, and input tree/head stay unchanged on success/failure. */
VAULT_API int vault_tree_revoke(const vault_tree *, const porthome_head *, const uint8_t actor[32],
                                const uint8_t member[32], uint64_t written_at, vault_tree **);
/* Bottom-up durable blob puts, updating parent addresses only after child puts.
 * Transport callbacks must journal uncertain writes; no retries here. */
VAULT_API int vault_tree_seal(vault_tree *, const vault_blob_io *, uint8_t root_address[32]);
/* New unsigned owner head body after sealing. Previous must be owner's verified
 * head; NULL creates seq1. Caller signs/publishes the event via libporthome.
 * Root overrides remain sealed under the new key only, as in vault_revoke. */
VAULT_API int vault_tree_head_new(const vault_tree *, const porthome_head *previous,
                                  const uint8_t actor[32], const uint8_t root_address[32],
                                  uint64_t written_at, porthome_head **);

typedef struct {
    uint8_t before[32], after[32];
    uint64_t epoch;
    size_t size_plain;
} vault_tree_chunk_change;
typedef struct {
    uint64_t reserved;
    size_t completed, remaining;
    vault_tree_chunk_change *changes;
    size_t changes_len;
} vault_tree_reseal_result;
/* Runs the approved lazy/eager task over all old-epoch chunks. True lengths
 * come from authenticated chunk descriptors and are checked by canonical
 * old-key re-sealing, never guessed from padding. On BUDGET/I/O failure the
 * result reports acknowledged changes; persist the tree before resuming.
 * Does NOT drop historical keys or delete old objects. */
VAULT_API int vault_tree_reseal(vault_tree *, int eager, uint64_t budget, const vault_blob_io *,
                                vault_tree_reseal_result **);
VAULT_API void vault_tree_reseal_result_free(vault_tree_reseal_result *);

typedef struct {
    size_t nodes, chunks;
    uint64_t size_plain, size_sealed;
} vault_tree_check_result;
/* Checks all file chunks reachable in the closed graph: hash/size, exact epoch,
 * AEAD, canonical envelope/manifest true length, and per-file plaintext totals.
 * Missing epochs return EPOCH, not a healthy result. Manifests must
 * first be admitted by record_open; a caller-created tree is not that proof.
 * Optional head ref_state edges require their own codec (outside this API). */
VAULT_API int vault_tree_check(const vault_tree *, const vault_blob_io *,
                               vault_tree_check_result *);
#ifdef __cplusplus
}
#endif
#endif
