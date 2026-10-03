#include "gh-identity.h"
#include "seahorse/secret_store.h"
#include "nostr/nip19/nip19.h"

#include <string.h>
#include <gio/gio.h>

void
gh_identity_info_free(GhIdentityInfo *info)
{
  if (!info) return;
  g_free(info->npub);
  g_free(info->label);
  g_free(info);
}

gchar *
gh_identity_pubkey_hex(const gchar *npub)
{
  guint8 bytes[32];
  if (!npub || nostr_nip19_decode_npub(npub, bytes) != 0) return NULL;
  gchar *hex = g_malloc0(65);
  for (guint i = 0; i < 32; i++)
    g_snprintf(hex + i * 2, 3, "%02x", bytes[i]);
  return hex;
}

static gint
compare_identity(gconstpointer a, gconstpointer b)
{
  const GhIdentityInfo *ia = *(GhIdentityInfo *const *)a;
  const GhIdentityInfo *ib = *(GhIdentityInfo *const *)b;
  return g_strcmp0(ia->npub, ib->npub);
}

GPtrArray *
gh_identity_list(GError **error)
{
#ifdef __APPLE__
  /* On macOS, ask the signer daemon for identities via D-Bus rather than
   * reading the signer's keyring directly (the daemon owns the Keychain
   * items, and Groundhog may not have access). */
  GError *bus_err = NULL;
  GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &bus_err);
  GPtrArray *result = g_ptr_array_new_with_free_func((GDestroyNotify)gh_identity_info_free);
  if (!bus) {
    g_debug("gh_identity_list: no session bus: %s", bus_err ? bus_err->message : "?");
    g_clear_error(&bus_err);
    if (error) g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                   "session bus unavailable");
    return result;
  }
  GVariant *ret = g_dbus_connection_call_sync(bus,
    "org.nostr.Signer", "/org/nostr/signer", "org.nostr.Signer",
    "ListIdentities", NULL, G_VARIANT_TYPE("(as)"),
    G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &bus_err);
  g_object_unref(bus);
  if (!ret) {
    g_debug("gh_identity_list: ListIdentities failed: %s", bus_err ? bus_err->message : "?");
    g_clear_error(&bus_err);
    return result;
  }
  GVariantIter *iter = NULL;
  g_variant_get(ret, "(as)", &iter);
  const gchar *npub_val = NULL;
  g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);
  while (g_variant_iter_next(iter, "&s", &npub_val)) {
    if (!npub_val || !*npub_val || g_hash_table_contains(seen, npub_val)) continue;
    g_autofree gchar *pubkey = gh_identity_pubkey_hex(npub_val);
    if (!pubkey) continue;
    GhIdentityInfo *info = g_new0(GhIdentityInfo, 1);
    info->npub = g_strdup(npub_val);
    info->label = NULL; /* ListIdentities returns npubs only */
    g_hash_table_add(seen, info->npub);
    g_ptr_array_add(result, info);
  }
  g_variant_iter_free(iter);
  g_variant_unref(ret);
  g_ptr_array_sort(result, compare_identity);
  return result;
#else
  g_autoptr(GHashTable) attrs = gnostr_secret_store_find_all(error);
  if (error && *error) return NULL;
  GPtrArray *result = g_ptr_array_new_with_free_func((GDestroyNotify)gh_identity_info_free);
  g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, attrs);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    GHashTable *item = value;
    const gchar *npub = g_hash_table_lookup(item, "npub");
    g_autofree gchar *pubkey = gh_identity_pubkey_hex(npub);
    if (!pubkey || g_hash_table_contains(seen, npub)) continue;
    GhIdentityInfo *info = g_new0(GhIdentityInfo, 1);
    info->npub = g_strdup(npub);
    info->label = g_strdup(g_hash_table_lookup(item, "label"));
    g_hash_table_add(seen, info->npub);
    g_ptr_array_add(result, info);
  }
  g_ptr_array_sort(result, compare_identity);
  return result;
#endif
}

static gboolean
write_current_npub(GSettings *settings, const gchar *npub, GError **error)
{
  if (g_settings_set_string(settings, "current-npub", npub)) return TRUE;
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                      "Groundhog could not save the selected identity");
  return FALSE;
}

gboolean
gh_identity_select_from_list(GSettings *settings, GPtrArray *identities,
                             const gchar *npub, GError **error)
{
  g_return_val_if_fail(G_IS_SETTINGS(settings), FALSE);
  if (!npub || !*npub) return write_current_npub(settings, "", error);
  g_autofree gchar *pubkey = gh_identity_pubkey_hex(npub);
  if (!pubkey || !identities) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Selected identity is invalid or unavailable");
    return FALSE;
  }
  for (guint i = 0; i < identities->len; i++) {
    const GhIdentityInfo *info = g_ptr_array_index(identities, i);
    if (g_strcmp0(info->npub, npub) == 0)
      return write_current_npub(settings, npub, error);
  }
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                      "Selected identity is not in the signer-owned identity store");
  return FALSE;
}

gboolean
gh_identity_select(GSettings *settings, const gchar *npub, GError **error)
{
  if (!npub || !*npub) return gh_identity_select_from_list(settings, NULL, "", error);
  g_autoptr(GPtrArray) identities = gh_identity_list(error);
  if (!identities) return FALSE;
  return gh_identity_select_from_list(settings, identities, npub, error);
}
