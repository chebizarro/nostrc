/**
 * GnostrMuteList - Mute List Editor Dialog
 *
 * GTK4 dialog for viewing and editing the user's NIP-51 mute list.
 */

#ifndef GNOSTR_MUTE_LIST_DIALOG_H
#define GNOSTR_MUTE_LIST_DIALOG_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_MUTE_LIST (gnostr_mute_list_dialog_get_type())
G_DECLARE_FINAL_TYPE(GnostrMuteListDialog, gnostr_mute_list_dialog, GNOSTR, MUTE_LIST, GtkWindow)

/**
 * gnostr_mute_list_dialog_new:
 * @parent: (nullable): parent window for transient-for
 *
 * Create a new mute list editor dialog.
 *
 * Returns: (transfer full): a new GnostrMuteListDialog instance
 */
GnostrMuteListDialog *gnostr_mute_list_dialog_new(GtkWindow *parent);

/**
 * gnostr_mute_list_dialog_refresh:
 * @self: the mute list dialog
 *
 * Refresh the lists from the mute list service.
 */
void gnostr_mute_list_dialog_refresh(GnostrMuteListDialog *self);

/**
 * gnostr_mute_list_dialog_add_pubkey:
 * @self: the mute list dialog
 * @pubkey_hex: pubkey to add (will be displayed in list)
 *
 * Convenience function to add a pubkey and refresh the UI.
 * Used by "Mute user" action from note context menu.
 */
void gnostr_mute_list_dialog_add_pubkey(GnostrMuteListDialog *self,
                                         const char *pubkey_hex);

G_END_DECLS

#endif /* GNOSTR_MUTE_LIST_DIALOG_H */
