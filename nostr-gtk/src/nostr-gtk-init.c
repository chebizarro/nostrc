/* nostr-gtk-init.c — Library initialization
 *
 * Registers custom widget types so GTK can instantiate them from UI templates.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "nostr-gtk.h"
#include "nostr-gtk-error.h"

#include <gio/gio.h>
#include <gtk/gtk.h>

/* --- Error domain --- */

G_DEFINE_QUARK(nostr-gtk-error-quark, nostr_gtk_error)

/* Generated resource accessor (from nostr-gtk-resources.c) */
extern GResource *nostr_gtk_get_resource(void);

static gboolean nostr_gtk_initialized = FALSE;

/**
 * nostr_gtk_init:
 *
 * Initializes the nostr-gtk library. Call this once before using any
 * nostr-gtk widgets. Ensures GType registration for template-based widgets
 * and registers the library's GResource bundle.
 */
void
nostr_gtk_init (void)
{
  if (nostr_gtk_initialized)
    return;
  nostr_gtk_initialized = TRUE;

  /* Register the library's GResource bundle.
   * This is necessary because nostr-gtk is a static library and the
   * auto-generated constructor may not be invoked by the linker. */
  g_resources_register (nostr_gtk_get_resource ());

  /* Force GType registration for widgets used in UI templates */
  g_type_ensure (nostr_gtk_profile_pane_get_type ());
  g_type_ensure (NOSTR_GTK_TYPE_NOTE_CARD_ROW);
}
