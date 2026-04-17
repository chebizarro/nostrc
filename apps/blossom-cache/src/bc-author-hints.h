/*
 * bc-author-hints.h - Resolve ?as=<pubkey> author hints via kind:10063
 *
 * SPDX-License-Identifier: MIT
 *
 * Queries a Nostr relay for kind:10063 (Blossom Server List) events
 * to resolve which Blossom servers a given author publishes to.
 * Results are cached in memory with a TTL.
 */

#ifndef BC_AUTHOR_HINTS_H
#define BC_AUTHOR_HINTS_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct _BcAuthorHintCache BcAuthorHintCache;

/**
 * bc_author_hint_cache_new:
 * @relay_url: (nullable): Nostr relay WebSocket URL (e.g. "wss://relay.damus.io").
 *   If NULL or empty, lookups are disabled and always return NULL.
 * @ttl_seconds: how long to cache results (0 = no caching)
 *
 * Returns: (transfer full): a new hint cache
 */
BcAuthorHintCache *bc_author_hint_cache_new(const gchar *relay_url, guint ttl_seconds);

/**
 * bc_author_hint_cache_free:
 * @cache: (transfer full) (nullable): cache to free
 */
void bc_author_hint_cache_free(BcAuthorHintCache *cache);

/**
 * bc_author_hint_cache_lookup:
 * @cache: the hint cache
 * @pubkey_hex: 64-char hex public key of the author
 *
 * Look up Blossom server URLs for the given author. May make a blocking
 * relay query on cache miss (with a short timeout).
 *
 * Returns: (transfer full) (array zero-terminated=1) (nullable):
 *   NULL-terminated array of server URL strings, or NULL. Free with g_strfreev().
 */
gchar **bc_author_hint_cache_lookup(BcAuthorHintCache *cache,
                                     const gchar *pubkey_hex);

G_END_DECLS

#endif /* BC_AUTHOR_HINTS_H */
