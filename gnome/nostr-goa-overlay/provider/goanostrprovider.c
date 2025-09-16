#include <glib.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <goa/goabackend.h>
#include <goa/goabackendprovider.h>
#include <goa/goabackendprovider.h>

#include "goanostrprovider.h"

struct _GoaNostrProvider {
  GoaProvider parent_instance;
};

G_DEFINE_TYPE(GoaNostrProvider, goa_nostr_provider, GOA_TYPE_PROVIDER)

static const gchar *nostr_get_provider_type(GoaProvider *provider) {
  (void)provider; return "nostr"; /* stable id */
}

static const gchar *nostr_get_provider_name(GoaProvider *provider) {
  (void)provider; return "Nostr";
}

static gboolean nostr_build_object(GoaProvider *provider,
                                   GoaObjectSkeleton *object,
                                   GKeyFile *key_file,
                                   const gchar *group,
                                   GError **error) {
  (void)provider; (void)error;
  /* Enable services by default: calendar, contacts, files */
  GoaAccount *acc = goa_object_skeleton_get_account(object);
  if (!acc) {
    acc = goa_account_skeleton_new();
    goa_object_skeleton_set_account(object, acc);
    g_object_unref(acc);
  }
  /* Determine identity: prefer persisted keyfile, else local file, else signer npub, else default */
  const gchar *ident = NULL;
  gchar *persisted = NULL;
  if (key_file && group) {
    persisted = g_key_file_get_string(key_file, group, "Identity", NULL);
    if (persisted && *persisted) ident = persisted;
  }
  if (!ident) {
    /* Local fallback: ~/.config/nostr-goa-overlay/identity */
    gchar *path = g_build_filename(g_get_user_config_dir(), "nostr-goa-overlay", "identity", NULL);
    gchar *contents = NULL; gsize len=0; GError *err2=NULL;
    if (g_file_get_contents(path, &contents, &len, &err2)){
      if (contents && *contents) ident = contents; else { if (contents) g_free(contents); contents=NULL; }
    }
    if (err2) g_error_free(err2);
    if (ident) {
      goa_account_set_identity(acc, ident);
      g_free(contents); g_free(path);
      goto post_ident;
    }
    if (contents) g_free(contents); g_free(path);
  }
  if (!ident) {
    gchar *npub = nostr_get_signer_npub();
    if (npub && *npub) ident = npub; else ident = "nostr";
    /* npub allocated; if used, we free after set */
    goa_account_set_identity(acc, ident);
    if (npub) g_free(npub);
  } else {
    goa_account_set_identity(acc, ident);
  }
  if (persisted) g_free(persisted);
post_ident:
  goa_account_set_provider_type(acc, "nostr");
  goa_account_set_provider_name(acc, "Nostr");
  goa_account_set_calendar_disabled(acc, FALSE);
  goa_account_set_contacts_disabled(acc, FALSE);
  goa_account_set_files_disabled(acc, FALSE);
  goa_account_set_mail_disabled(acc, TRUE);
  return TRUE;
}

/* --- Minimal flows: signer DBus query and provisioning --- */
static gchar *nostr_get_signer_npub(void) {
  GError *err=NULL; gchar *npub=NULL;
  GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  if (!bus) { if (err) g_error_free(err); return NULL; }
  GVariant *ret = g_dbus_connection_call_sync(bus,
                  "org.nostr.Signer", "/org/nostr/Signer", "org.nostr.Signer", "GetPublicKey",
                  NULL, G_VARIANT_TYPE_TUPLE, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);
  if (ret) {
    const gchar *s=NULL; g_variant_get(ret, "(s)", &s);
    if (s) npub = g_strdup(s);
    g_variant_unref(ret);
  }
  if (err) { g_error_free(err); }
  g_object_unref(bus);
  return npub;
}

static gboolean run_shims(const gchar *cmd, const gchar *user) {
  if (!cmd || !user) return FALSE;
  gchar *prog = g_find_program_in_path("goa_shims");
  if (!prog) return FALSE;
  gchar *argvv[] = { prog, (gchar*)cmd, "--user", (gchar*)user, "--host", "127.0.0.1", "--port", "7680", NULL };
  GError *err=NULL; gint status=0;
  gboolean ok = g_spawn_sync(NULL, argvv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, &status, &err);
  g_free(prog);
  if (!ok) { if (err){ g_error_free(err);} return FALSE; }
  return status==0;
}

/* Some backends provide these vfuncs; implement best-effort versions. */
static gboolean nostr_refresh_account(GoaProvider *provider, GoaClient *client, GoaObject *object, gpointer parent, GError **error) {
  (void)provider; (void)client; (void)parent; (void)error;
  /* Ping signer; if npub present, treat as OK */
  gchar *npub = nostr_get_signer_npub();
  if (!npub) return FALSE;
  g_free(npub);
  /* Try to (re)provision to ensure services running */
  GoaAccount *acc = goa_object_peek_account(object);
  const gchar *identity = acc ? goa_account_get_identity(acc) : NULL;
  const gchar *user = identity && *identity ? identity : "nostr";
  (void)run_shims("provision", user);
  return TRUE;
}

static gboolean nostr_remove_account(GoaProvider *provider, GoaClient *client, GoaObject *object, gpointer parent, GError **error) {
  (void)provider; (void)client; (void)parent; (void)error;
  GoaAccount *acc = goa_object_peek_account(object);
  const gchar *identity = acc ? goa_account_get_identity(acc) : NULL;
  const gchar *user = identity && *identity ? identity : "nostr";
  (void)run_shims("teardown", user);
  return TRUE;
}

static gboolean nostr_add_account(GoaProvider *provider, GoaClient *client, gpointer parent, GError **error) {
  (void)provider; (void)client; (void)parent; (void)error;
  /* Minimal flow: pick first signer identity and provision services */
  gchar *npub = nostr_get_signer_npub();
  if (!npub) return FALSE;
  /* Persist identity locally under ~/.config/nostr-goa-overlay/identity */
  {
    gchar *dir = g_build_filename(g_get_user_config_dir(), "nostr-goa-overlay", NULL);
    (void)g_mkdir_with_parents(dir, 0700);
    gchar *path = g_build_filename(dir, "identity", NULL);
    GError *errw=NULL; if (!g_file_set_contents(path, npub, -1, &errw)) { if (errw) g_error_free(errw); }
    g_free(path); g_free(dir);
  }
  /* Best-effort: write Identity into GOA keyfile for nostr provider */
  {
    gchar *goa_conf = g_build_filename(g_get_user_config_dir(), "goa-1.0", "accounts.conf", NULL);
    GKeyFile *kf = g_key_file_new(); GError *kerr=NULL;
    if (g_key_file_load_from_file(kf, goa_conf, G_KEY_FILE_KEEP_COMMENTS|G_KEY_FILE_KEEP_TRANSLATIONS, &kerr)){
      gsize ng=0; gchar **groups = g_key_file_get_groups(kf, &ng);
      for (gsize i=0;i<ng;i++){
        gchar *prov = g_key_file_get_string(kf, groups[i], "Provider", NULL);
        if (prov && g_strcmp0(prov, "nostr")==0){
          g_key_file_set_string(kf, groups[i], "Identity", npub);
        }
        if (prov) g_free(prov);
      }
      gsize len=0; gchar *data = g_key_file_to_data(kf, &len, NULL);
      if (data){ GError *werr=NULL; (void)g_file_set_contents(goa_conf, data, len, &werr); if (werr) g_error_free(werr); g_free(data);} 
      g_strfreev(groups);
    }
    if (kerr) g_error_free(kerr); g_key_file_unref(kf); g_free(goa_conf);
  }
  gboolean ok = run_shims("provision", npub);
  g_free(npub);
  return ok;
}

static void goa_nostr_provider_class_init(GoaNostrProviderClass *klass) {
  GoaProviderClass *pklass = GOA_PROVIDER_CLASS(klass);
  pklass->get_provider_type = nostr_get_provider_type;
  pklass->get_provider_name = nostr_get_provider_name;
  pklass->build_object = nostr_build_object;
  /* Best-effort flows; exact signatures may vary across GOA releases */
  pklass->add_account = (gpointer)nostr_add_account;
  pklass->refresh_account = (gpointer)nostr_refresh_account;
  pklass->remove_account = (gpointer)nostr_remove_account;
}

static void goa_nostr_provider_init(GoaNostrProvider *self) {
  (void)self;
}

/* plugin entry points */
G_MODULE_EXPORT void goa_provider_get_types(const GType **types, gint *n_types) {
  static GType type_list[1]; static gsize inited = 0;
  if (g_once_init_enter(&inited)) {
    type_list[0] = goa_nostr_provider_get_type();
    g_once_init_leave(&inited, 1);
  }
  *types = type_list; *n_types = 1;
}
