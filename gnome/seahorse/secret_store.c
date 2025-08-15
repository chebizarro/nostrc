#include "secret_store.h"

const SecretSchema gnostr_secret_schema = {
  GNOSTR_SECRET_SCHEMA_NAME,
  SECRET_SCHEMA_NONE,
  {
    {"type", SECRET_SCHEMA_ATTRIBUTE_STRING},
    {"npub", SECRET_SCHEMA_ATTRIBUTE_STRING},
    {"uid",  SECRET_SCHEMA_ATTRIBUTE_STRING},
    {"curve", SECRET_SCHEMA_ATTRIBUTE_STRING},
    {"origin", SECRET_SCHEMA_ATTRIBUTE_STRING},
    {"hardware_slot", SECRET_SCHEMA_ATTRIBUTE_STRING},
    {NULL, 0}
  }
};

gboolean gnostr_secret_store_save_software_key(const gchar *npub,
                                                const gchar *uid,
                                                const gchar *secret,
                                                GError **error){
  if (!npub || !secret) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "npub and secret required");
    return FALSE;
  }
  return secret_password_store_sync(&gnostr_secret_schema,
                                    SECRET_COLLECTION_DEFAULT,
                                    "Nostr key",
                                    secret,
                                    NULL, /* cancellable */
                                    "type", "nostr-key",
                                    "npub", npub,
                                    "uid", uid ? uid : "",
                                    "curve", "secp256k1",
                                    "origin", "software",
                                    NULL, /* end attributes */
                                    error);
}

GHashTable *gnostr_secret_store_find_all(GError **error){
  GHashTable *result = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_hash_table_unref);
  SecretService *service = secret_service_get_sync(SECRET_SERVICE_NONE, NULL, error);
  if (!service) return result;
  GHashTable *attrs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  g_hash_table_insert(attrs, g_strdup("type"), g_strdup("nostr-key"));
  GList *items = secret_service_search_sync(service, &gnostr_secret_schema,
                                            attrs,
                                            SECRET_SEARCH_ALL | SECRET_SEARCH_UNLOCK,
                                            NULL, error);
  g_hash_table_destroy(attrs);
  for (GList *l=items; l; l=l->next){
    SecretItem *it = l->data;
    GHashTable *ia = secret_item_get_attributes(it);
    const gchar *npub = g_hash_table_lookup(ia, "npub");
    const gchar *uid  = g_hash_table_lookup(ia, "uid");
    gchar *key = g_strdup_printf("%s|%s", npub?npub:"", uid?uid:"");
    g_hash_table_insert(result, key, ia); // transfer ia to result
  }
  g_list_free_full(items, g_object_unref);
  g_object_unref(service);
  return result;
}
