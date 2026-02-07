/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 gnostr contributors
 *
 * gnostr-nip10-thread-manager.h - Unified NIP-10 thread parsing with cache
 *
 * nostrc-pp64 (Epic 4.3): Consolidates all NIP-10 thread parsing into a
 * single canonical API with an LRU cache. All code should use this instead
 * of implementing custom e-tag parsing.
 *
 * Supports:
 * - NIP-10 explicit markers (root/reply at tag index 3)
 * - Positional fallback for legacy events (1 e-tag = root, 2+ = first/last)
 * - NIP-22 uppercase "E" tags for comment threading
 * - NIP-22 "A"/"a" tags for addressable event references (articles, etc.)
 * - NIP-22 "k" tag for root event kind
 * - Relay hints at tag index 2
 * - Thread-safe singleton cache
 */

#ifndef GNOSTR_NIP10_THREAD_MANAGER_H
#define GNOSTR_NIP10_THREAD_MANAGER_H

#include <glib.h>
#include <stdbool.h>

G_BEGIN_DECLS

/**
 * GnostrNip10ThreadInfo:
 * @root_id: (nullable): hex string of root event ID. Owned by cache.
 * @reply_id: (nullable): hex string of immediate reply parent ID. Owned by cache.
 * @root_relay_hint: (nullable): relay URL hint for root event. Owned by cache.
 * @reply_relay_hint: (nullable): relay URL hint for reply parent. Owned by cache.
 * @root_addr: (nullable): NIP-22 addressable event ref ("kind:pubkey:d-tag"). Owned by cache.
 * @root_addr_relay: (nullable): relay hint for addressable event. Owned by cache.
 * @root_kind: kind of the root event from "k" tag, or -1 if absent.
 * @has_explicit_markers: TRUE if the event uses NIP-10 explicit markers
 *
 * Thread context parsed from NIP-10 e-tags. Strings are owned by the
 * cache and valid until gnostr_nip10_cache_clear() is called.
 * For long-lived use, copy the strings.
 */
typedef struct {
    const char *root_id;
    const char *reply_id;
    const char *root_relay_hint;
    const char *reply_relay_hint;
    const char *root_addr;
    const char *root_addr_relay;
    gint root_kind;
    gboolean has_explicit_markers;
} GnostrNip10ThreadInfo;

/**
 * gnostr_nip10_parse_thread:
 * @event_json: a Nostr event as a JSON string
 * @info: (out caller-allocates): thread info to populate
 *
 * Parses NIP-10 e-tags from an event JSON string.
 * Results are cached by event ID for subsequent lookups.
 *
 * IMPORTANT: This is the canonical NIP-10 parsing function.
 * All application code should use this instead of implementing
 * custom e-tag scanning.
 *
 * The returned string pointers in @info are owned by the cache
 * and remain valid until gnostr_nip10_cache_clear() is called.
 * Copy them if you need to store them beyond the current scope.
 *
 * Returns: TRUE on success, FALSE if parsing failed
 */
gboolean gnostr_nip10_parse_thread(const char *event_json,
                                    GnostrNip10ThreadInfo *info);

/**
 * gnostr_nip10_lookup_cached:
 * @event_id: a 64-character hex event ID
 * @info: (out caller-allocates): thread info to populate
 *
 * Looks up previously parsed thread info by event ID.
 * This avoids re-parsing if the event was already processed.
 *
 * Returns: TRUE if found in cache, FALSE if not cached
 */
gboolean gnostr_nip10_lookup_cached(const char *event_id,
                                     GnostrNip10ThreadInfo *info);

/**
 * gnostr_nip10_cache_clear:
 *
 * Clears all cached thread parsing results.
 * After this call, any GnostrNip10ThreadInfo string pointers
 * from previous parse/lookup calls are invalid.
 */
void gnostr_nip10_cache_clear(void);

/**
 * gnostr_nip10_cache_size:
 *
 * Returns: the number of entries in the cache
 */
guint gnostr_nip10_cache_size(void);

/**
 * gnostr_nip10_is_thread_reply:
 * @event_json: a Nostr event as JSON string
 *
 * Quick check: does this event have any e-tags indicating it's
 * a reply in a thread? Uses the cache if available.
 *
 * Returns: TRUE if the event is a thread reply
 */
gboolean gnostr_nip10_is_thread_reply(const char *event_json);

/**
 * gnostr_nip10_get_thread_root:
 * @event_json: a Nostr event as JSON string
 *
 * Convenience: returns the thread root ID for an event.
 * Returns NULL if the event has no root reference.
 *
 * Returns: (transfer none) (nullable): the root event ID, or NULL.
 *          String is cache-owned, copy if needed.
 */
const char *gnostr_nip10_get_thread_root(const char *event_json);

G_END_DECLS

#endif /* GNOSTR_NIP10_THREAD_MANAGER_H */
