#include "provider-gnostr.h"
#include <gtk/gtk.h>
#include <libsecret/secret.h>
#include "../seahorse/secret_store.h"
#include <gio/gio.h>

struct _ProviderGnostr { GoaProvider parent_instance; };
G_DEFINE_TYPE(ProviderGnostr, provider_gnostr, GOA_TYPE_PROVIDER)

static const gchar *provider_get_provider_type(GoaProvider *provider) {
  (void)provider; return "Gnostr"; // symbolic id
}

static gchar *shorten_npub(const gchar *npub){
  if (!npub) return g_strdup("(unknown)");
  if (g_utf8_strlen(npub, -1) <= 16) return g_strdup(npub);
  return g_strdup_printf("%.*s…", 16, npub);
}

static gchar *hash_npub_path(const gchar *npub){
  if (!npub) npub = "";
  guint32 h = g_str_hash(npub);
  return g_strdup_printf("/org/gnostr/goa/%08x/", h);
}

static gboolean provider_add_account(GoaProvider *provider,
                                     GDBusMethodInvocation *invocation,
                                     GVariant *params,
                                     GCancellable *cancellable) {
  (void)params; (void)cancellable;

  /* nostrc-n63f: Account selection UI not implemented. Currently picks first
   * existing key from Secret Service. A full implementation would load
   * goa-add-account.ui and let user choose from available keys. */
  GError *err = NULL;
  GHashTable *all = gnostr_secret_store_find_all(&err);
  const gchar *chosen_npub = NULL;
  if (all){
    GHashTableIter it; gpointer k, v; g_hash_table_iter_init(&it, all);
    if (g_hash_table_iter_next(&it, &k, &v)){
      GHashTable *attrs = v; (void)k;
      chosen_npub = g_hash_table_lookup(attrs, "npub");
    }
  }
  if (!chosen_npub){
    // No existing key; succeed anyway (UI would generate/import).
    goa_provider_respond_add_account(provider, invocation, TRUE, NULL);
    if (all) g_hash_table_unref(all);
    return TRUE;
  }

  // Store per-account JSON defaults using relocatable schema
  gchar *path = hash_npub_path(chosen_npub);
  GSettings *gs = g_settings_new_with_path("org.gnostr.goa", path);
  g_settings_set_string(gs, "relays-json", "");
  g_settings_set_string(gs, "grants-json", "");
  g_settings_set_string(gs, "profile-json", "");
  g_object_unref(gs);
  g_free(path);

  // Respond success; GOA will create/persist account object. Presentation identity handled externally for now.
  goa_provider_respond_add_account(provider, invocation, TRUE, NULL);
  if (all) g_hash_table_unref(all);
  return TRUE;
}

static void provider_gnostr_class_init(ProviderGnostrClass *klass) {
  GoaProviderClass *pc = GOA_PROVIDER_CLASS(klass);
  pc->get_provider_type = provider_get_provider_type;
  pc->add_account = provider_add_account;
}

static void provider_gnostr_init(ProviderGnostr *self) { (void)self; }
