#include "gh-identity.h"
#include "seahorse/secret_store.h"
#include "nostr/nip19/nip19.h"

#include <string.h>

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
}

gboolean
gh_identity_select_from_list(GSettings *settings, GPtrArray *identities,
                             const gchar *npub, GError **error)
{
  g_return_val_if_fail(G_IS_SETTINGS(settings), FALSE);
  if (!npub || !*npub) return g_settings_set_string(settings, "current-npub", "");
  g_autofree gchar *pubkey = gh_identity_pubkey_hex(npub);
  if (!pubkey || !identities) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Selected identity is invalid or unavailable");
    return FALSE;
  }
  for (guint i = 0; i < identities->len; i++) {
    const GhIdentityInfo *info = g_ptr_array_index(identities, i);
    if (g_strcmp0(info->npub, npub) == 0)
      return g_settings_set_string(settings, "current-npub", npub);
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
