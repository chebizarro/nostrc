/* SPDX-License-Identifier: MIT — plaintext object manifest chain (nostrc-v2zyh).
 *
 * A manifest segment is a canonical JSON Blossom blob:
 *   {"prev":<sha256 hex>|null,"repo":"<30618 d>","rows":[...],"v":1}
 * Each row is {"git_oid","sha256","size","type"}: the Blossom address of the
 * object's raw payload, keyed by Git OID (raw-payload addresses are not
 * unique; the empty blob and empty tree share one). Rows are strictly
 * ascending by git_oid. A segment with prev=null is a checkpoint holding
 * every row; others hold rows added since prev. The owner-signed 30618
 * carries ["hanami-manifest", <head sha256>].
 */
#ifndef HANAMI_MANIFEST_H
#define HANAMI_MANIFEST_H
#include "hanami/hanami-index.h"
#include "hanami/hanami-nostr.h"
#include <blossom/client.h>
#include <nip34.h>
#include <stdbool.h>

#define HANAMI_MANIFEST_VERSION 1
#define HANAMI_MANIFEST_TAG "hanami-manifest"
/* Shared with the sealed ref_state_node index bounds. */
#define HANAMI_INDEX_SNAPSHOT_MAX_BYTES (16u * 1024u * 1024u)
#define HANAMI_INDEX_SNAPSHOT_MAX_ROWS 65536u
/* Writers emit a checkpoint before a chain exceeds this many segments. */
#define HANAMI_MANIFEST_CHECKPOINT_LINKS 16u
/* Readers refuse chains deeper than this. */
#define HANAMI_MANIFEST_MAX_DEPTH 64u

/* Shared row validation (sealed and plaintext codecs). */
bool hanami_hex_valid(const char *s, size_t n);
/* Classic SHA-1 hex OID; a SHA-256 OID is never truncated. */
bool hanami_oid_hex_valid(const char *s);
bool hanami_object_type_valid(long long type);

typedef struct {
    char prev[65];          /* empty when checkpoint */
    hanami_index_entry_t *rows;
    size_t count;
} hanami_manifest;

void hanami_manifest_dispose(hanami_manifest *m);

/* Canonical encoding; rows need not be sorted (a sorted copy is encoded),
 * duplicates are rejected. Caller frees *out. */
hanami_error_t hanami_manifest_encode(const char *repo, const char *prev,
    const hanami_index_entry_t *rows, size_t count, char **out, size_t *len);

/* Strict decode: exact field sets, bounds, ascending unique OIDs and the
 * expected repo. HANAMI_ERR_INDEX on any violation. */
hanami_error_t hanami_manifest_decode(const uint8_t *data, size_t len,
    const char *repo, hanami_manifest *out);

/* Walk the chain from head back to a checkpoint or to the cache's recorded
 * head, importing rows into cache in one transaction and recording head.
 * Existing rows win (an imported segment cannot remap a trusted OID; reads
 * re-verify every object anyway). Chains deeper than
 * HANAMI_MANIFEST_MAX_DEPTH fail. *segments_read (nullable) counts GETs. */
hanami_error_t hanami_manifest_sync(blossom_client_t *client, hanami_index_t *cache,
    const char *repo, const char *head, size_t *segments_read);

/* Owner's newest kind-30618 for repo (EOSE snapshot; kind/author/d checked)
 * parsed into state, plus its manifest head (empty string when the event
 * has no hanami-manifest tag). HANAMI_ERR_NOT_FOUND when no state exists. */
hanami_error_t hanami_nostr_fetch_state_manifest(hanami_nostr_ctx_t *ctx,
    const char *repo, const char *owner, nip34_repo_state_t **state, char head[65]);

/* Every direct ref/HEAD target in state must be present in cache. */
hanami_error_t hanami_manifest_check_coverage(const nip34_repo_state_t *state,
    hanami_index_t *cache);
#endif
