#include <glib.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <goa/goabackend.h>
#include <goa/goabackendprovider.h>
#include <goa/goabackendpasswordbased.h>

#include "goanostrprovider.h"

struct _GoaNostrProvider {
  GoaPasswordBased parent_instance;
};

G_DEFINE_TYPE(GoaNostrProvider, goa_nostr_provider, GOA_TYPE_PASSWORD_BASED)

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
  (void)provider; (void)key_file; (void)group; (void)error;
  /* Enable services by default: calendar, contacts, files */
  GoaAccount *acc = goa_object_skeleton_get_account(object);
  if (!acc) {
    acc = goa_account_skeleton_new();
    goa_object_skeleton_set_account(object, acc);
    g_object_unref(acc);
  }
  goa_account_set_identity(acc, "nostr");
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
