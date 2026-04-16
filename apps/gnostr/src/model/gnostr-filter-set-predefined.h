/* gnostr-filter-set-predefined.h — Predefined filter sets driven by the
 * user's NIP-02 / NIP-51 lists.
 *
 * SPDX-License-Identifier: MIT
 *
 * This module owns three list-driven predefined filter sets that are
 * installed into a #GnostrFilterSetManager alongside the static built-in
 * ones (Global, Follows, Mentions, Media):
 *
 *   - "predefined-following"      authors = follow list pubkeys (NIP-02)
 *   - "predefined-bookmarks"      ids     = bookmarked event ids (NIP-51)
 *   - "predefined-muted-inverse"  excluded_authors = muted pubkeys
 *
 * The builders in this module are pure functions — they produce or update
 * a filter set from explicit inputs, which makes them easy to unit-test
 * without depending on the follow / bookmark / mute-list singletons.
 *
 * Higher-level helpers then wire the builders up to the real data
 * sources and install the sets into a manager.
 *
 * nostrc-yg8j.3: Predefined filter sets from user lists.
 */

#ifndef GNOSTR_FILTER_SET_PREDEFINED_H
#define GNOSTR_FILTER_SET_PREDEFINED_H

#include <glib-object.h>

#include "model/gnostr-filter-set.h"
#include "model/gnostr-filter-set-manager.h"

G_BEGIN_DECLS

/* Canonical ids for the list-driven predefined filter sets. */
#define GNOSTR_FILTER_SET_ID_FOLLOWING       "predefined-following"
#define GNOSTR_FILTER_SET_ID_BOOKMARKS       "predefined-bookmarks"
#define GNOSTR_FILTER_SET_ID_MUTED_INVERSE   "predefined-muted-inverse"

/* ------------------------------------------------------------------------
 * Pure builders
 * ------------------------------------------------------------------------ */

/**
 * gnostr_filter_set_predefined_build_following:
 * @follow_pubkeys: (nullable) (array zero-terminated=1): hex pubkeys
 *   of accounts the user follows, or %NULL for an empty follow list.
 *
 * Constructs the "Following" filter set from a NIP-02 follow list.
 * Source is %GNOSTR_FILTER_SET_SOURCE_PREDEFINED, kinds are fixed to
 * [1, 6] (text notes + reposts), authors is the given list.
 *
 * Returns: (transfer full): a new #GnostrFilterSet.
 */
GnostrFilterSet *
gnostr_filter_set_predefined_build_following(const gchar * const *follow_pubkeys);

/**
 * gnostr_filter_set_predefined_build_bookmarks:
 * @event_ids: (nullable) (array zero-terminated=1): hex event ids of
 *   bookmarked notes, or %NULL for an empty bookmark list.
 *
 * Constructs the "Bookmarks" filter set from a NIP-51 bookmark list.
 * Source is predefined, ids is the given list, no kind restriction.
 *
 * Returns: (transfer full): a new #GnostrFilterSet.
 */
GnostrFilterSet *
gnostr_filter_set_predefined_build_bookmarks(const gchar * const *event_ids);

/**
 * gnostr_filter_set_predefined_build_muted_inverse:
 * @muted_pubkeys: (nullable) (array zero-terminated=1): hex pubkeys of
 *   muted authors, or %NULL for an empty mute list.
 *
 * Constructs the "Muted (inverse)" filter set. Kinds are fixed to [1],
 * excluded_authors is the given list. Consumers must honour
 * gnostr_filter_set_get_excluded_authors() client-side when rendering.
 *
 * Returns: (transfer full): a new #GnostrFilterSet.
 */
GnostrFilterSet *
gnostr_filter_set_predefined_build_muted_inverse(const gchar * const *muted_pubkeys);

/* ------------------------------------------------------------------------
 * Manager wiring
 * ------------------------------------------------------------------------ */

/**
 * gnostr_filter_set_predefined_install:
 * @manager: target manager
 * @follow_pubkeys: (nullable): hex pubkeys for the Following set
 * @bookmark_ids:   (nullable): hex event ids for the Bookmarks set
 * @muted_pubkeys:  (nullable): hex pubkeys for the Muted-inverse set
 *
 * Registers the three list-driven predefined filter sets on @manager,
 * or updates them if they already exist. Safe to call repeatedly; the
 * call is idempotent when inputs don't change.
 *
 * Any argument may be %NULL, in which case the corresponding filter set
 * is installed with an empty list (still a valid predefined feed).
 */
void gnostr_filter_set_predefined_install(GnostrFilterSetManager *manager,
                                          const gchar * const *follow_pubkeys,
                                          const gchar * const *bookmark_ids,
                                          const gchar * const *muted_pubkeys);

/**
 * gnostr_filter_set_predefined_refresh_from_services:
 * @manager: target manager
 * @user_pubkey_hex: (nullable): the active user's pubkey. When %NULL,
 *   the follow-list and muted-inverse sets are installed with empty
 *   lists.
 *
 * Pulls current data from the live follow-list cache, bookmark list and
 * mute list singletons and updates the three predefined sets on
 * @manager. Call this after login, after sync events, or whenever one
 * of the underlying lists has changed.
 *
 * Note: the follow / bookmark / mute list services expose no signals
 * yet, so consumers are expected to call this function explicitly after
 * mutations. Once those services gain change notifications, this can be
 * hooked up internally.
 */
void gnostr_filter_set_predefined_refresh_from_services(
    GnostrFilterSetManager *manager,
    const gchar *user_pubkey_hex);

G_END_DECLS

#endif /* GNOSTR_FILTER_SET_PREDEFINED_H */
