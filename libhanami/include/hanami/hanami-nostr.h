/*
 * hanami-nostr.h - Nostr integration layer for NIP-34 events
 *
 * SPDX-License-Identifier: MIT
 *
 * Wraps libnostr to provide NIP-34-specific operations: publishing and
 * querying repo announcements (30617), state announcements (30618),
 * patches (1617), and status events (1630-1633).
 */

#ifndef HANAMI_NOSTR_H
#define HANAMI_NOSTR_H

#include "hanami-types.h"
#include <nip34.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
struct _NostrEvent;
typedef struct _NostrEvent NostrEvent;
struct NostrRelay;
typedef struct NostrFilter NostrFilter;

/* Opaque context handle */
typedef struct hanami_nostr_ctx hanami_nostr_ctx_t;

/* =========================================================================
 * Callback types
 * ========================================================================= */

/**
 * hanami_nostr_event_cb:
 * @event: received event (borrowed — do not free)
 * @user_data: opaque context
 *
 * Callback invoked when an event is received from a relay.
 */
typedef void (*hanami_nostr_event_cb)(const NostrEvent *event, void *user_data);

/* =========================================================================
 * Context lifecycle
 * ========================================================================= */

/**
 * hanami_nostr_ctx_new:
 * @relay_urls: NULL-terminated array of relay WebSocket URLs
 * @signer: (nullable): signer for publishing events
 * @out: (out): created context
 *
 * Create a Nostr context for NIP-34 operations.
 * The relay connections are established lazily on first use.
 *
 * Returns: HANAMI_OK on success
 */
hanami_error_t hanami_nostr_ctx_new(const char *const *relay_urls,
                                    const hanami_signer_t *signer,
                                    hanami_nostr_ctx_t **out);

/**
 * hanami_nostr_ctx_free:
 * @ctx: (transfer full) (nullable): context to free
 */
void hanami_nostr_ctx_free(hanami_nostr_ctx_t *ctx);

/* =========================================================================
 * Publishing
 * ========================================================================= */

/**
 * hanami_nostr_publish_repo:
 * @ctx: context
 * @repo_id: d-tag identifier
 * @name: repository name
 * @desc: (nullable): description
 * @clone_urls: (nullable): NULL-terminated clone URLs
 * @web_urls: (nullable): NULL-terminated web URLs
 *
 * Create and publish a kind 30617 repository announcement.
 * The event is signed via the context's signer and published to all relays.
 *
 * Returns: HANAMI_OK on success
 */
hanami_error_t hanami_nostr_publish_repo(hanami_nostr_ctx_t *ctx,
                                         const char *repo_id,
                                         const char *name,
                                         const char *desc,
                                         const char *const *clone_urls,
                                         const char *const *web_urls);

/**
 * hanami_nostr_publish_state:
 * @ctx: context
 * @repo_id: d-tag identifier
 * @refs: array of ref entries
 * @ref_count: number of refs
 * @head: (nullable): HEAD value (e.g. "ref: refs/heads/main")
 *
 * Create and publish a kind 30618 repository state event.
 *
 * Returns: HANAMI_OK on success
 */
hanami_error_t hanami_nostr_publish_state(hanami_nostr_ctx_t *ctx,
                                          const char *repo_id,
                                          const nip34_ref_t *refs,
                                          size_t ref_count,
                                          const char *head);

/**
 * hanami_nostr_publish_event:
 * @ctx: context
 * @event: signed event to publish (borrowed)
 *
 * Publish a pre-built, pre-signed event to all connected relays.
 *
 * Returns: HANAMI_OK on success
 */
hanami_error_t hanami_nostr_publish_event(hanami_nostr_ctx_t *ctx,
                                          NostrEvent *event);

/* =========================================================================
 * Querying
 * ========================================================================= */

/**
 * hanami_nostr_fetch_repo:
 * @ctx: context
 * @repo_id: d-tag identifier
 * @owner_pubkey: hex pubkey of the repo owner
 * @out: (out): parsed repository (caller frees with nip34_repository_free)
 *
 * Query relays for the latest kind 30617 event for this repo.
 *
 * Returns: HANAMI_OK on success, HANAMI_ERR_NOT_FOUND if no event found
 */
hanami_error_t hanami_nostr_fetch_repo(hanami_nostr_ctx_t *ctx,
                                       const char *repo_id,
                                       const char *owner_pubkey,
                                       nip34_repository_t **out);

/**
 * hanami_nostr_fetch_state:
 * @ctx: context
 * @repo_id: d-tag identifier
 * @owner_pubkey: hex pubkey of the repo owner
 * @out: (out): parsed repo state (caller frees with nip34_repo_state_free)
 *
 * Query relays for the latest kind 30618 event for this repo.
 *
 * Returns: HANAMI_OK on success, HANAMI_ERR_NOT_FOUND if no event found
 */
hanami_error_t hanami_nostr_fetch_state(hanami_nostr_ctx_t *ctx,
                                        const char *repo_id,
                                        const char *owner_pubkey,
                                        nip34_repo_state_t **out);

/* =========================================================================
 * Filter construction helpers
 * ========================================================================= */

/**
 * hanami_nostr_build_repo_filter:
 * @repo_id: d-tag value to match
 * @owner_pubkey: (nullable): restrict to this author
 *
 * Build a NostrFilter for kind 30617 repo announcements.
 * Caller must free with nostr_filter_free().
 *
 * Returns: newly allocated filter, or NULL on error
 */
NostrFilter *hanami_nostr_build_repo_filter(const char *repo_id,
                                            const char *owner_pubkey);

/**
 * hanami_nostr_build_state_filter:
 * @repo_id: d-tag value to match
 * @owner_pubkey: (nullable): restrict to this author
 *
 * Build a NostrFilter for kind 30618 repo state events.
 * Caller must free with nostr_filter_free().
 *
 * Returns: newly allocated filter, or NULL on error
 */
NostrFilter *hanami_nostr_build_state_filter(const char *repo_id,
                                             const char *owner_pubkey);

/**
 * hanami_nostr_build_patches_filter:
 * @repo_addr: "a" tag value: "30617:<pubkey>:<repo_id>"
 *
 * Build a NostrFilter for kind 1617 patches targeting a repo.
 * Caller must free with nostr_filter_free().
 *
 * Returns: newly allocated filter, or NULL on error
 */
NostrFilter *hanami_nostr_build_patches_filter(const char *repo_addr);

#ifdef __cplusplus
}
#endif

#endif /* HANAMI_NOSTR_H */
