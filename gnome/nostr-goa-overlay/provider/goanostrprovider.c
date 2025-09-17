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

static void goa_nostr_provider_class_init(GoaNostrProviderClass *klass) {
  (void)klass; /* Intentionally minimal; rely on PasswordProvider defaults */
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
