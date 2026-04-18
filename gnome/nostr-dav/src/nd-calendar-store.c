/* nd-calendar-store.c - In-memory NIP-52 event cache for CalDAV
 *
 * SPDX-License-Identifier: MIT
 */

#include "nd-calendar-store.h"
#include <string.h>

struct _NdCalendarStore {
  GHashTable *events;    /* uid → NdCalendarEvent* (owned) */
  guint64     version;   /* bumped on every mutation, used as ctag */
};

NdCalendarStore *
nd_calendar_store_new(void)
{
  NdCalendarStore *store = g_new0(NdCalendarStore, 1);
  store->events = g_hash_table_new_full(
    g_str_hash, g_str_equal, g_free,
    (GDestroyNotify)nd_calendar_event_free);
  store->version = 0;
  return store;
}

void
nd_calendar_store_free(NdCalendarStore *store)
{
  if (store == NULL) return;
  g_hash_table_unref(store->events);
  g_free(store);
}

void
nd_calendar_store_put(NdCalendarStore *store,
                      NdCalendarEvent *event)
{
  g_return_if_fail(store != NULL);
  g_return_if_fail(event != NULL);
  g_return_if_fail(event->uid != NULL);

  /* Takes ownership of event; key is a copy of uid */
  g_hash_table_replace(store->events, g_strdup(event->uid), event);
  store->version++;
}

const NdCalendarEvent *
nd_calendar_store_get(NdCalendarStore *store,
                      const gchar     *uid)
{
  g_return_val_if_fail(store != NULL, NULL);
  g_return_val_if_fail(uid != NULL, NULL);

  return g_hash_table_lookup(store->events, uid);
}

gboolean
nd_calendar_store_remove(NdCalendarStore *store,
                         const gchar     *uid)
{
  g_return_val_if_fail(store != NULL, FALSE);
  g_return_val_if_fail(uid != NULL, FALSE);

  gboolean removed = g_hash_table_remove(store->events, uid);
  if (removed) store->version++;
  return removed;
}

GPtrArray *
nd_calendar_store_list_all(NdCalendarStore *store)
{
  g_return_val_if_fail(store != NULL, NULL);

  GPtrArray *list = g_ptr_array_new();
  GHashTableIter iter;
  gpointer value;

  g_hash_table_iter_init(&iter, store->events);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    g_ptr_array_add(list, value);
  }

  return list;
}

guint
nd_calendar_store_count(NdCalendarStore *store)
{
  g_return_val_if_fail(store != NULL, 0);
  return g_hash_table_size(store->events);
}

gchar *
nd_calendar_store_get_ctag(NdCalendarStore *store)
{
  g_return_val_if_fail(store != NULL, NULL);
  return g_strdup_printf("%" G_GUINT64_FORMAT, store->version);
}
