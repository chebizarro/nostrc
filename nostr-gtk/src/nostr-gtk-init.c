/* nostr-gtk-init.c — Library initialization
 *
 * Registers custom widget types so GTK can instantiate them from UI templates.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "nostr-gtk.h"

#include <gtk/gtk.h>

/**
 * nostr_gtk_init:
 *
 * Initializes the nostr-gtk library. Call this once before using any
 * nostr-gtk widgets. Ensures GType registration for template-based widgets.
 */
void
nostr_gtk_init (void)
{
  /* Force GType registration for widgets used in UI templates */
  g_type_ensure (gnostr_profile_pane_get_type ());
  g_type_ensure (GNOSTR_TYPE_NOTE_CARD_ROW);
}
