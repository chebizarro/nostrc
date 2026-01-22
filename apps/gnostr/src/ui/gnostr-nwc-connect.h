/**
 * GnostrNwcConnect - NIP-47 Nostr Wallet Connect Dialog
 *
 * Dialog for connecting to a remote lightning wallet via NWC.
 */

#ifndef GNOSTR_NWC_CONNECT_H
#define GNOSTR_NWC_CONNECT_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_NWC_CONNECT (gnostr_nwc_connect_get_type())
G_DECLARE_FINAL_TYPE(GnostrNwcConnect, gnostr_nwc_connect, GNOSTR, NWC_CONNECT, GtkWindow)

/**
 * gnostr_nwc_connect_new:
 * @parent: (nullable): parent window for transient-for
 *
 * Create a new NWC connection dialog.
 *
 * Returns: (transfer full): a new GnostrNwcConnect instance
 */
GnostrNwcConnect *gnostr_nwc_connect_new(GtkWindow *parent);

/**
 * gnostr_nwc_connect_refresh:
 * @self: the NWC connect dialog
 *
 * Refresh the dialog to reflect current connection state.
 */
void gnostr_nwc_connect_refresh(GnostrNwcConnect *self);

G_END_DECLS

#endif /* GNOSTR_NWC_CONNECT_H */
