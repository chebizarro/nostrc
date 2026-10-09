/* gn-portable-i18n.h — gettext domain of the portable nostr-gtk widgets.
 *
 * Portable widgets translate their labels through their own "nostr-gtk"
 * domain, so any host application (Groundhog, Gnostr, third parties) gets
 * the same catalogs without the library knowing the host's domain.
 */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define GN_PORTABLE_GETTEXT_DOMAIN "nostr-gtk"

/* Binds GN_PORTABLE_GETTEXT_DOMAIN to the library's install locale
 * directory with UTF-8 output, once, and returns the domain name. Widgets call
 * it from class_init; a host that relocates catalogs may call
 * bindtextdomain(GN_PORTABLE_GETTEXT_DOMAIN, dir) afterwards. */
const char *gn_portable_gettext_domain(void);

G_END_DECLS
