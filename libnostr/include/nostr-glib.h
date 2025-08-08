#ifndef __NOSTR_GLIB_H__
#define __NOSTR_GLIB_H__

/* Optional GLib integration helpers. Keep minimal initially. */

#include "nostr-config.h"

#if defined(NOSTR_HAVE_GLIB) && NOSTR_HAVE_GLIB
#include <glib.h>
#include <glib-object.h>

G_BEGIN_DECLS

/* Future: GLib-friendly wrappers and typedefs, e.g., GPtrArray helpers,
 * GError-based variants, and signal/source utilities for relay/subscription. */

G_END_DECLS
#endif /* NOSTR_HAVE_GLIB */

#endif /* __NOSTR_GLIB_H__ */
