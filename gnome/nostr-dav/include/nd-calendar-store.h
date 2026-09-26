/* nd-calendar-store.h - NIP-52 event store for CalDAV (SQLite-backed)
 *
 * SPDX-License-Identifier: MIT
 *
 * Stores NdCalendarEvent objects indexed by UID (d-tag) in the `events`
 * table of an NdStoreDb. Objects are materialized per call, so getters
 * return owned copies.
 */
#ifndef ND_CALENDAR_STORE_H
#define ND_CALENDAR_STORE_H

#include <glib.h>
#include "nd-ical.h"
#include "nd-store-db.h"

G_BEGIN_DECLS

typedef struct _NdCalendarStore NdCalendarStore;

/**
 * nd_calendar_store_new:
 * @db: (transfer none): database; the store takes its own reference
 *
 * Returns: (transfer full): a new store.
 */
NdCalendarStore *nd_calendar_store_new(NdStoreDb *db);

void nd_calendar_store_free(NdCalendarStore *store);

/**
 * nd_calendar_store_put:
 * @store: the store
 * @event: (transfer none): event to store
 * @out_created: (out) (optional): TRUE if no event with this UID existed
 * @error: (out) (optional): location for error
 *
 * Adds or replaces an event by UID and bumps the ctag, atomically.
 * Publish-state columns of an existing row are preserved.
 *
 * Returns: TRUE on success.
 */
gboolean nd_calendar_store_put(NdCalendarStore       *store,
                               const NdCalendarEvent *event,
                               gboolean              *out_created,
                               GError               **error);

/**
 * nd_calendar_store_get:
 *
 * Returns: (transfer full) (nullable): the event, or NULL if not found
 *   (@error unset) or on failure (@error set).
 */
NdCalendarEvent *nd_calendar_store_get(NdCalendarStore *store,
                                       const gchar     *uid,
                                       GError         **error);

/**
 * nd_calendar_store_remove:
 * @out_removed: (out) (optional): TRUE if an event was removed
 *
 * Returns: TRUE on success (including "nothing to remove").
 */
gboolean nd_calendar_store_remove(NdCalendarStore *store,
                                  const gchar     *uid,
                                  gboolean        *out_removed,
                                  GError         **error);

/**
 * nd_calendar_store_list_all:
 *
 * Returns: (transfer full) (nullable): GPtrArray of NdCalendarEvent*
 *   ordered by UID, with an element free function set; NULL on error.
 */
GPtrArray *nd_calendar_store_list_all(NdCalendarStore *store,
                                      GError         **error);

gboolean nd_calendar_store_count(NdCalendarStore *store,
                                 guint           *out_count,
                                 GError         **error);

/**
 * nd_calendar_store_get_ctag:
 *
 * Returns: (transfer full) (nullable): the collection's persistent,
 *   monotonically increasing generation as a string; NULL on error.
 */
gchar *nd_calendar_store_get_ctag(NdCalendarStore *store,
                                  GError         **error);

G_END_DECLS
#endif /* ND_CALENDAR_STORE_H */
