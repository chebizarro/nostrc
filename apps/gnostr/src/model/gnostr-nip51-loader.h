/* gnostr-nip51-loader.h — Load the current user's NIP-51 kind-30000
 * categorized people lists from the local NDB cache and convert them
 * into GnostrFilterSet objects.
 *
 * SPDX-License-Identifier: MIT
 *
 * This module is the bridge between NIP-51 storage (kind 30000 events,
 * NostrList objects) and the FilterSet subsystem. Users typically
 * maintain their own curated people lists on other clients (Amethyst,
 * Nostrudel, etc.); this loader surfaces them inside the FilterSet
 * creation dialog so a user can import a list as a new custom filter
 * set in a couple of clicks.
 *
 * The loader is strictly local: it reads from NDB without issuing any
 * relay queries. The caller is expected to have triggered relay
 * fetches through other plumbing (NIP-02 / NIP-51 settings sync). If
 * NDB has no kind-30000 events for the user, the loader returns an
 * empty result set rather than reaching out to the network.
 *
 * nostrc-yg8j.8: List-based filter sets (NIP-51).
 */

#ifndef GNOSTR_NIP51_LOADER_H
#define GNOSTR_NIP51_LOADER_H

#include <glib.h>
#include <gio/gio.h>

#include "model/gnostr-filter-set.h"

G_BEGIN_DECLS

/**
 * GnostrNip51UserLists:
 *
 * Opaque container for a user's kind-30000 people lists. Each entry is
 * a <type>NostrList*</type> (from nips/nip51). Iterate with
 * gnostr_nip51_user_lists_get_count() /
 * gnostr_nip51_user_lists_get_nth().
 */
typedef struct _GnostrNip51UserLists GnostrNip51UserLists;

/**
 * gnostr_nip51_user_lists_free:
 * @lists: (transfer full) (nullable): the lists container to free.
 *
 * Releases @lists and every <type>NostrList</type> it owns. Safe to
 * call with %NULL.
 */
void gnostr_nip51_user_lists_free(GnostrNip51UserLists *lists);

/**
 * gnostr_nip51_user_lists_get_count:
 * @lists: a lists container
 *
 * Returns: number of NIP-51 lists available.
 */
gsize gnostr_nip51_user_lists_get_count(const GnostrNip51UserLists *lists);

/**
 * gnostr_nip51_user_lists_get_nth:
 * @lists: a lists container
 * @index: 0-based index into the container
 *
 * Returns: (transfer none) (nullable): an internal <type>NostrList*</type>
 *   pointer, or %NULL if @index is out of range. The returned pointer
 *   is owned by @lists and must not be freed.
 */
const void *gnostr_nip51_user_lists_get_nth(const GnostrNip51UserLists *lists,
                                             gsize index);

/**
 * gnostr_nip51_load_user_lists_async:
 * @pubkey_hex: 64-hex-character pubkey of the user whose lists to load.
 *   npub/nprofile inputs are rejected; the caller is expected to
 *   normalize via gnostr_ensure_hex_pubkey() first.
 * @cancellable: (nullable): optional #GCancellable.
 * @callback: (scope async): completion handler.
 * @user_data: passed through to @callback.
 *
 * Asynchronously scans the local NDB store for kind-30000 events
 * authored by @pubkey_hex, deduplicates by d-tag (most recent
 * `created_at` wins), parses each surviving event into a
 * <type>NostrList</type>, and drops lists that contain no p-tag
 * entries (they would produce an empty FilterSet).
 *
 * The scan runs on a GLib worker thread. @callback is invoked on the
 * main thread.
 */
void gnostr_nip51_load_user_lists_async(const gchar *pubkey_hex,
                                         GCancellable *cancellable,
                                         GAsyncReadyCallback callback,
                                         gpointer user_data);

/**
 * gnostr_nip51_load_user_lists_finish:
 * @result: #GAsyncResult passed to the async callback
 * @error: (out) (nullable): error location
 *
 * Returns: (transfer full) (nullable): a #GnostrNip51UserLists on
 *   success (possibly empty if the user has no cached lists), or %NULL
 *   on cancellation / error.
 */
GnostrNip51UserLists *gnostr_nip51_load_user_lists_finish(GAsyncResult *result,
                                                           GError **error);

/**
 * gnostr_nip51_list_to_filter_set:
 * @nostr_list: (type gpointer): a <type>NostrList*</type> (typically
 *   obtained from gnostr_nip51_user_lists_get_nth()).
 * @proposed_name: (nullable): UI-visible name for the new FilterSet.
 *   When %NULL the list's title (or identifier) is used.
 *
 * Converts a NIP-51 list into a #GnostrFilterSet. Only "p" entries are
 * extracted; other tag types ("e", "t", "a", "r", "word") are ignored
 * because they don't map cleanly onto the FilterSet author axis.
 * Each pubkey is normalized via gnostr_ensure_hex_pubkey(); invalid
 * entries are silently dropped.
 *
 * The returned set has:
 *   - source = %GNOSTR_FILTER_SET_SOURCE_CUSTOM
 *   - authors = the normalized p-tag hex pubkeys
 *   - description = the list's description (if any)
 *
 * Returns: (transfer full) (nullable): a new #GnostrFilterSet, or %NULL
 *   if the list contains no usable p-tag entries.
 */
GnostrFilterSet *gnostr_nip51_list_to_filter_set(const void *nostr_list,
                                                  const gchar *proposed_name);

G_END_DECLS

#endif /* GNOSTR_NIP51_LOADER_H */
