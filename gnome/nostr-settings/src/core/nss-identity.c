/* nss-identity.c — see nss-identity.h.
 * SPDX-License-Identifier: MIT
 */
#include "nss-identity.h"
#include "nss-lists.h"

#include <json-glib/json-glib.h>
#include <string.h>

#ifdef NSS_HAVE_SECRET
#include "seahorse/secret_store.h"
#endif

void
nss_identity_free(NssIdentity *id)
{
  if (id == NULL)
    return;
  g_free(id->npub);
  g_free(id->label);
  g_free(id->origin);
  g_free(id);
}

gchar *
nss_signer_get_npub(GDBusConnection *bus, GError **error)
{
  g_autoptr(GVariant) r = g_dbus_connection_call_sync(
    bus, "org.nostr.Signer", "/org/nostr/signer", "org.nostr.Signer", "GetPublicKey",
    NULL, G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 10000, NULL, error);
  if (r == NULL) {
    if (error && *error)
      g_dbus_error_strip_remote_error(*error);
    return NULL;
  }
  gchar *npub = NULL;
  g_variant_get(r, "(s)", &npub);
  if (npub == NULL || !g_str_has_prefix(npub, "npub1")) {
    g_free(npub);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                        "the signer has no active identity");
    return NULL;
  }
  return npub;
}

gchar **
nss_signer_parse_relays_json(const gchar *json, GError **error)
{
  g_autoptr(JsonParser) p = json_parser_new();
  if (!json_parser_load_from_data(p, json ? json : "", -1, NULL) ||
      json_parser_get_root(p) == NULL ||
      !JSON_NODE_HOLDS_ARRAY(json_parser_get_root(p))) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "GetRelays did not return a JSON array");
    return NULL;
  }
  JsonArray *a = json_node_get_array(json_parser_get_root(p));
  g_autoptr(GStrvBuilder) b = g_strv_builder_new();
  g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  for (guint i = 0; i < json_array_get_length(a); i++) {
    JsonNode *n = json_array_get_element(a, i);
    const gchar *u = NULL;
    if (JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING)
      u = json_node_get_string(n);
    else if (JSON_NODE_HOLDS_OBJECT(n))
      u = json_object_get_string_member_with_default(json_node_get_object(n), "url", NULL);
    gchar *norm = u ? nss_relay_url_normalize(u, NULL) : NULL;
    if (norm && g_hash_table_add(seen, norm))
      g_strv_builder_add(b, norm);
  }
  return g_strv_builder_end(b);
}

gchar **
nss_signer_get_relays(GDBusConnection *bus, GError **error)
{
  GError *local = NULL;
  g_autoptr(GVariant) r = g_dbus_connection_call_sync(
    bus, "org.nostr.Signer", "/org/nostr/signer", "org.nostr.Signer", "GetRelays",
    NULL, G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 10000, NULL, &local);
  if (r == NULL) {
    g_autofree gchar *remote = g_dbus_error_get_remote_error(local);
    if (remote && g_str_has_suffix(remote, ".NotFound")) {
      g_error_free(local);
      return g_new0(gchar *, 1);
    }
    g_dbus_error_strip_remote_error(local);
    g_propagate_error(error, local);
    return NULL;
  }
  const gchar *json = NULL;
  g_variant_get(r, "(&s)", &json);
  return nss_signer_parse_relays_json(json, error);
}

gboolean
nss_keyring_available(void)
{
#ifdef NSS_HAVE_SECRET
  return TRUE;
#else
  return FALSE;
#endif
}

#ifdef NSS_HAVE_SECRET
static gint
by_label(gconstpointer a, gconstpointer b)
{
  const NssIdentity *x = *(NssIdentity *const *)a, *y = *(NssIdentity *const *)b;
  gint c = g_strcmp0(x->label ? x->label : "\xff", y->label ? y->label : "\xff");
  return c ? c : g_strcmp0(x->npub, y->npub);
}
#endif

GPtrArray *
nss_keyring_identities(const gchar *active_npub, GError **error)
{
  GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)nss_identity_free);
#ifdef NSS_HAVE_SECRET
  GHashTable *all = gnostr_secret_store_find_all(error);
  if (all == NULL)
    return out;
  g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);
  GHashTableIter it;
  gpointer key, val;
  g_hash_table_iter_init(&it, all);
  while (g_hash_table_iter_next(&it, &key, &val)) {
    GHashTable *attrs = val;
    const gchar *npub = g_hash_table_lookup(attrs, "npub");
    if (npub == NULL || *npub == '\0' || !g_hash_table_add(seen, (gpointer)npub))
      continue;
    NssIdentity *id = g_new0(NssIdentity, 1);
    id->npub = g_strdup(npub);
    const gchar *label = g_hash_table_lookup(attrs, "label");
    id->label = label && *label ? g_strdup(label) : NULL;
    id->origin = g_strdup(g_hash_table_lookup(attrs, "origin"));
    id->active = active_npub && g_str_equal(active_npub, npub);
    g_ptr_array_add(out, id);
  }
  g_ptr_array_sort(out, by_label);
  g_hash_table_unref(all);
#else
  (void)active_npub;
  (void)error;
#endif
  return out;
}
