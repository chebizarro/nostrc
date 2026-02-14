#ifndef NOSTR_GTK_NOSTR_GTK_H
#define NOSTR_GTK_NOSTR_GTK_H

/**
 * SECTION:nostr-gtk
 * @short_description: Master include for nostr-gtk
 *
 * Include this header for the nostr-gtk widget library.
 * Provides GTK4/libadwaita widgets built on top of nostr-gobject.
 */

#define NOSTR_GTK_INSIDE

#include <glib.h>

#include "nostr-gtk-version.h"

G_BEGIN_DECLS

/**
 * nostr_gtk_init:
 *
 * Initializes the nostr-gtk library. Call once before using widgets.
 */
void nostr_gtk_init (void);

/* Widget headers */
#include "gnostr-thread-view.h"
#include "gnostr-composer.h"
#include "gnostr-profile-pane.h"
#include "nostr-note-card-row.h"
#include "gn-timeline-tabs.h"
#include "gnostr-timeline-view.h"

G_END_DECLS

#undef NOSTR_GTK_INSIDE
#endif /* NOSTR_GTK_NOSTR_GTK_H */
