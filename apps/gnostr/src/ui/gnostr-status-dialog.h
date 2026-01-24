/**
 * GnostrStatusDialog - NIP-38 User Status Setting Dialog
 *
 * A dialog for setting or clearing user status (general/music).
 * Supports NIP-40 expiration for temporary statuses.
 */

#ifndef GNOSTR_STATUS_DIALOG_H
#define GNOSTR_STATUS_DIALOG_H

#include <gtk/gtk.h>
#include <adwaita.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_STATUS_DIALOG (gnostr_status_dialog_get_type())

G_DECLARE_FINAL_TYPE(GnostrStatusDialog, gnostr_status_dialog, GNOSTR, STATUS_DIALOG, AdwDialog)

/**
 * gnostr_status_dialog_new:
 *
 * Creates a new status dialog.
 *
 * Returns: (transfer full): A new #GnostrStatusDialog
 */
GnostrStatusDialog *gnostr_status_dialog_new(void);

/**
 * gnostr_status_dialog_present:
 * @self: The status dialog
 * @parent: (nullable): Parent widget
 *
 * Presents the dialog.
 */
void gnostr_status_dialog_present(GnostrStatusDialog *self, GtkWidget *parent);

/**
 * gnostr_status_dialog_set_current_status:
 * @self: The status dialog
 * @general_status: (nullable): Current general status text
 * @music_status: (nullable): Current music status text
 *
 * Pre-fills the dialog with current status values.
 */
void gnostr_status_dialog_set_current_status(GnostrStatusDialog *self,
                                              const gchar *general_status,
                                              const gchar *music_status);

G_END_DECLS

#endif /* GNOSTR_STATUS_DIALOG_H */
