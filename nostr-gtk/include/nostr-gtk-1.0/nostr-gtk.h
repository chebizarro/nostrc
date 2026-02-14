#pragma once

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

#include "gnostr-thread-view.h"

G_END_DECLS

#undef NOSTR_GTK_INSIDE
