/* nostr-gtk-init.c — Library initialization
 *
 * Registers custom widget types so GTK can instantiate them from UI templates.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "nostr-gtk.h"
#include "nostr-gtk-error.h"

#include <gio/gio.h>
#include <gtk/gtk.h>

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

  /* Force GType registration for the standalone template widgets.
   * The Gnostr-coupled widgets (profile pane, note card row, thread view,
   * note embed) are deliberately not referenced here: doing so would pull
   * their objects, and therefore unresolved Gnostr app symbols, into every
   * consumer of this static library. Their types still register on first
   * use, and Gnostr ensures the ones its templates need before parsing. */
  g_type_ensure (NOSTR_GTK_TYPE_COMPOSER);
  g_type_ensure (NOSTR_GTK_TYPE_TIMELINE_TABS);
  g_type_ensure (NOSTR_GTK_TYPE_TIMELINE_VIEW);
}
