#define GOA_API_IS_SUBJECT_TO_CHANGE 1
#define GOA_BACKEND_API_IS_SUBJECT_TO_CHANGE 1
#include <glib.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <goabackend/goabackend.h>

#include "goanostrprovider.h"

/* Minimal stub plugin: export no provider types to satisfy build on systems
 * where GoaProviderClass is intentionally opaque to third parties. */
G_MODULE_EXPORT void goa_provider_get_types(const GType **types, gint *n_types) {
  (void)types;
  if (n_types) *n_types = 0;
}
