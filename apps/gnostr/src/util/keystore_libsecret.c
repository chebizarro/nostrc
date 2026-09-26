/**
 * Identity metadata shim - Linux libsecret (nostrc-e5nz). See keystore.h.
 *
 * Reads attributes of the signer's org.gnostr.Signer/identity items and of
 * the retired client keystore's org.gnostr.NostrKey items. Every search is
 * SECRET_SEARCH_ALL without SECRET_SEARCH_UNLOCK or SECRET_SEARCH_LOAD_SECRETS:
 * attributes are readable on a locked collection without a prompt, and no
 * secret ever enters this process. The schemas come from gnostr-secret
 * (gnome/seahorse) so the client and the signer daemon cannot drift.
 */

#ifdef HAVE_LIBSECRET

#include "keystore.h"
#include "seahorse/secret_store.h"
#include <string.h>

G_DEFINE_QUARK(gnostr-keystore-error-quark, gnostr_keystore_error)

void gnostr_key_info_free(GnostrKeyInfo *info) {
  if (!info) return;
  g_free(info->npub);
  g_free(info->label);
  g_free(info);
}

GnostrKeyInfo *gnostr_key_info_copy(const GnostrKeyInfo *info) {
  if (!info) return NULL;
  GnostrKeyInfo *copy = g_new0(GnostrKeyInfo, 1);
  copy->npub = g_strdup(info->npub);
  copy->label = g_strdup(info->label);
  copy->created_at = info->created_at;
  return copy;
}

gboolean gnostr_keystore_available(void) {
  SecretService *service = secret_service_get_sync(SECRET_SERVICE_NONE, NULL, NULL);
  if (!service) return FALSE;
  g_object_unref(service);
  return TRUE;
}

gboolean gnostr_keystore_legacy_migrates_automatically(void) {
  return TRUE;
}

/* Items of schema (optionally npub=...), attributes only. */
static GList *search_metadata(const SecretSchema *schema, const char *npub,
                              GError **error) {
  SecretService *service = secret_service_get_sync(SECRET_SERVICE_NONE, NULL, error);
  if (!service) return NULL;
  GHashTable *attrs = g_hash_table_new(g_str_hash, g_str_equal);
  if (npub) g_hash_table_insert(attrs, (gpointer)"npub", (gpointer)npub);
  GList *items = secret_service_search_sync(service, schema, attrs,
                                            SECRET_SEARCH_ALL, NULL, error);
  g_hash_table_unref(attrs);
  g_object_unref(service);
  return items;
}

static gint64 parse_created_at(const char *iso) {
  if (!iso || !*iso) return 0;
  GDateTime *dt = g_date_time_new_from_iso8601(iso, NULL);
  if (!dt) return 0;
  gint64 t = g_date_time_to_unix(dt);
  g_date_time_unref(dt);
  return t;
}

static gint compare_npub(gconstpointer a, gconstpointer b) {
  return g_strcmp0(((const GnostrKeyInfo *)a)->npub, ((const GnostrKeyInfo *)b)->npub);
}

GList *gnostr_keystore_list_keys(GError **error) {
  /* "npub|label" -> attribute table; one identity may have several items
   * (different key_id selectors), so collapse by npub. */
  GHashTable *all = gnostr_secret_store_find_all(error);
  GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
  GList *result = NULL;
  GHashTableIter it;
  gpointer value;
  g_hash_table_iter_init(&it, all);
  while (g_hash_table_iter_next(&it, NULL, &value)) {
    GHashTable *attrs = value;
    const char *npub = g_hash_table_lookup(attrs, "npub");
    if (!npub || !g_str_has_prefix(npub, "npub1")) continue;
    GnostrKeyInfo *info = g_hash_table_lookup(seen, npub);
    if (!info) {
      info = g_new0(GnostrKeyInfo, 1);
      info->npub = g_strdup(npub);
      info->created_at = parse_created_at(g_hash_table_lookup(attrs, "created_at"));
      g_hash_table_insert(seen, info->npub, info);
      result = g_list_prepend(result, info);
    }
    const char *label = g_hash_table_lookup(attrs, "label");
    if (!info->label && label && *label)
      info->label = g_strdup(label);
  }
  g_hash_table_unref(seen);
  g_hash_table_unref(all);
  return g_list_sort(result, compare_npub);
}

gboolean gnostr_keystore_has_key(const char *npub) {
  if (!npub || !g_str_has_prefix(npub, "npub1")) return FALSE;
  GList *items = search_metadata(&gnostr_secret_schema, npub, NULL);
  gboolean found = items != NULL;
  g_list_free_full(items, g_object_unref);
  return found;
}

GList *gnostr_keystore_list_legacy_keys(GError **error) {
  GList *items = search_metadata(&gnostr_secret_legacy_client_schema, NULL, error);
  GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
  GList *result = NULL;
  for (GList *l = items; l; l = l->next) {
    GHashTable *attrs = secret_item_get_attributes(l->data);
    if (!attrs) continue;
    const char *npub = g_hash_table_lookup(attrs, "npub");
    const char *app = g_hash_table_lookup(attrs, "application");
    /* Same filter as the daemon's migration: another program's item under
     * this schema name is neither ours nor going to be imported. */
    gboolean ours = g_strcmp0(app, GNOSTR_SECRET_LEGACY_CLIENT_APPLICATION) == 0;
    if (ours && npub && g_str_has_prefix(npub, "npub1") &&
        !g_hash_table_contains(seen, npub)) {
      GnostrKeyInfo *info = g_new0(GnostrKeyInfo, 1);
      info->npub = g_strdup(npub);
      g_hash_table_add(seen, info->npub);
      result = g_list_prepend(result, info);
    }
    g_hash_table_unref(attrs);
  }
  g_hash_table_unref(seen);
  g_list_free_full(items, g_object_unref);
  return g_list_sort(result, compare_npub);
}

#endif /* HAVE_LIBSECRET */
