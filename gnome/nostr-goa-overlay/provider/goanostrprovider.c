#define GOA_API_IS_SUBJECT_TO_CHANGE 1
#define GOA_BACKEND_API_IS_SUBJECT_TO_CHANGE 1
#include <glib.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <goabackend/goabackend.h>

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
  GoaAccount *acc = goa_object_peek_account((GoaObject*)object);
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
      if (contents && *contents) ident = contents;
      else {
        if (contents) g_free(contents);
        contents = NULL;
      }
    }
    if (err2) g_error_free(err2);
    if (ident) {
      goa_account_set_identity(acc, ident);
      g_free(contents);
      g_free(path);
      goto post_ident;
    }
    if (contents) g_free(contents);
    g_free(path);
  }
  if (!ident) {
    gchar *npub = NULL; /* signer lookup optional: not used in minimal build */
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

static void goa_nostr_provider_class_init(GoaNostrProviderClass *klass) {
  GoaProviderClass *pklass = GOA_PROVIDER_CLASS(klass);
  pklass->get_provider_type = nostr_get_provider_type;
  pklass->get_provider_name = nostr_get_provider_name;
  pklass->build_object = nostr_build_object;
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
