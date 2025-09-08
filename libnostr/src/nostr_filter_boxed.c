#include "nostr-config.h"
#if NOSTR_HAVE_GLIB
#include <glib-object.h>
#include "nostr-filter.h"

/* Manual boxed type registration to avoid macro union incompatibilities. */
GType nostr_filter_get_type(void) {
  static volatile gsize g_define_type_id__volatile = 0;
  if (g_once_init_enter(&g_define_type_id__volatile)) {
    GType t = g_boxed_type_register_static(
        "NostrFilter",
        (GBoxedCopyFunc)nostr_filter_copy,
        (GBoxedFreeFunc)nostr_filter_free);
    g_once_init_leave(&g_define_type_id__volatile, t);
  }
  return g_define_type_id__volatile;
}

#endif /* NOSTR_HAVE_GLIB */
