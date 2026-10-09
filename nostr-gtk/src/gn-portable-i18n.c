#include "gn-portable-i18n-private.h"

#ifndef NOSTR_GTK_LOCALEDIR
#error "NOSTR_GTK_LOCALEDIR must be defined by the build"
#endif

const char *
gn_portable_gettext_domain(void)
{
  static gsize bound = 0;
  if (g_once_init_enter(&bound)) {
    bindtextdomain(GN_PORTABLE_GETTEXT_DOMAIN, NOSTR_GTK_LOCALEDIR);
    bind_textdomain_codeset(GN_PORTABLE_GETTEXT_DOMAIN, "UTF-8");
    g_once_init_leave(&bound, 1);
  }
  return GN_PORTABLE_GETTEXT_DOMAIN;
}
