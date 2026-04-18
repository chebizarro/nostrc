/* nd-contact-store.c - In-memory contact cache for CardDAV
 *
 * SPDX-License-Identifier: MIT
 */

#include "nd-contact-store.h"
#include <string.h>

struct _NdContactStore {
  GHashTable *contacts;   /* uid → NdContact* (owned) */
  guint64     version;    /* bumped on every mutation, used as ctag */
};

NdContactStore *
nd_contact_store_new(void)
{
  NdContactStore *store = g_new0(NdContactStore, 1);
  store->contacts = g_hash_table_new_full(
    g_str_hash, g_str_equal, g_free,
    (GDestroyNotify)nd_contact_free);
  store->version = 0;
  return store;
}

void
nd_contact_store_free(NdContactStore *store)
{
  if (store == NULL) return;
  g_hash_table_unref(store->contacts);
  g_free(store);
}

void
nd_contact_store_put(NdContactStore *store, NdContact *contact)
{
  g_return_if_fail(store != NULL);
  g_return_if_fail(contact != NULL);
  g_return_if_fail(contact->uid != NULL);

  g_hash_table_replace(store->contacts, g_strdup(contact->uid), contact);
  store->version++;
}

const NdContact *
nd_contact_store_get(NdContactStore *store, const gchar *uid)
{
  g_return_val_if_fail(store != NULL, NULL);
  g_return_val_if_fail(uid != NULL, NULL);
  return g_hash_table_lookup(store->contacts, uid);
}

gboolean
nd_contact_store_remove(NdContactStore *store, const gchar *uid)
{
  g_return_val_if_fail(store != NULL, FALSE);
  g_return_val_if_fail(uid != NULL, FALSE);

  gboolean removed = g_hash_table_remove(store->contacts, uid);
  if (removed) store->version++;
  return removed;
}

GPtrArray *
nd_contact_store_list_all(NdContactStore *store)
{
  g_return_val_if_fail(store != NULL, NULL);

  GPtrArray *list = g_ptr_array_new();
  GHashTableIter iter;
  gpointer value;

  g_hash_table_iter_init(&iter, store->contacts);
  while (g_hash_table_iter_next(&iter, NULL, &value))
    g_ptr_array_add(list, value);

  return list;
}

guint
nd_contact_store_count(NdContactStore *store)
{
  g_return_val_if_fail(store != NULL, 0);
  return g_hash_table_size(store->contacts);
}

gchar *
nd_contact_store_get_ctag(NdContactStore *store)
{
  g_return_val_if_fail(store != NULL, NULL);
  return g_strdup_printf("%" G_GUINT64_FORMAT, store->version);
}
