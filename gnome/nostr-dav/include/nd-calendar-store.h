/* nd-calendar-store.h - In-memory NIP-52 event cache for CalDAV
 *
 * SPDX-License-Identifier: MIT
 *
 * Stores NdCalendarEvent objects indexed by UID (d-tag) and provides
 * CalDAV-oriented queries (list all, get by UID, time-range filter).
 */
#ifndef ND_CALENDAR_STORE_H
#define ND_CALENDAR_STORE_H

#include <glib.h>
#include "nd-ical.h"

G_BEGIN_DECLS

typedef struct _NdCalendarStore NdCalendarStore;

/**
 * nd_calendar_store_new:
 *
 * Creates a new empty calendar store.
 * Returns: (transfer full): a new store.
 */
NdCalendarStore *nd_calendar_store_new(void);

/**
 * nd_calendar_store_free:
 * @store: (transfer full): the store to free
 */
void nd_calendar_store_free(NdCalendarStore *store);

/**
 * nd_calendar_store_put:
 * @store: the store
 * @event: (transfer full): event to store (takes ownership)
 *
 * Adds or replaces an event by UID.
 */
void nd_calendar_store_put(NdCalendarStore *store,
                           NdCalendarEvent *event);

/**
 * nd_calendar_store_get:
 * @store: the store
 * @uid: the event UID (d-tag)
 *
 * Returns: (transfer none) (nullable): the event, or NULL if not found.
 */
const NdCalendarEvent *nd_calendar_store_get(NdCalendarStore *store,
                                             const gchar     *uid);

/**
 * nd_calendar_store_remove:
 * @store: the store
 * @uid: the event UID to remove
 *
 * Returns: TRUE if an event was removed.
 */
gboolean nd_calendar_store_remove(NdCalendarStore *store,
                                  const gchar     *uid);

/**
 * nd_calendar_store_list_all:
 * @store: the store
 *
 * Returns: (transfer container): GPtrArray of const NdCalendarEvent*.
 *   Caller owns the array but not the elements.
 */
GPtrArray *nd_calendar_store_list_all(NdCalendarStore *store);

/**
 * nd_calendar_store_count:
 * @store: the store
 *
 * Returns: number of events.
 */
guint nd_calendar_store_count(NdCalendarStore *store);

/**
 * nd_calendar_store_get_ctag:
 * @store: the store
 *
 * Returns a string that changes whenever the store is modified.
 * Used as the CalDAV getctag property for sync.
 *
 * Returns: (transfer full): ctag string.
 */
gchar *nd_calendar_store_get_ctag(NdCalendarStore *store);

G_END_DECLS
#endif /* ND_CALENDAR_STORE_H */
