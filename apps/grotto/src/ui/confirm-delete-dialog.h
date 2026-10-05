/* confirm-delete-dialog.h - Confirmation dialog for destructive delete operations
 *
 * Provides a secure confirmation dialog for destructive operations like:
 * - Deleting private keys
 * - Removing backup files
 * - Clearing session data
 * - Wiping all application data
 *
 * Features:
 * - Clear warning messages about irreversibility
 * - Confirmation text input for high-risk operations
 * - Multiple security levels (simple, confirm, password)
 * - Integration with secure-delete module
 */

#ifndef GN_CONFIRM_DELETE_DIALOG_H
#define GN_CONFIRM_DELETE_DIALOG_H

#include <adwaita.h>
#include <gtk/gtk.h>
#include "../secure-delete.h"

G_BEGIN_DECLS

#define GN_TYPE_CONFIRM_DELETE_DIALOG (gn_confirm_delete_dialog_get_type())
G_DECLARE_FINAL_TYPE(GnConfirmDeleteDialog, gn_confirm_delete_dialog, GN,
                     CONFIRM_DELETE_DIALOG, AdwDialog)

/**
 * GnDeleteSeverity:
 * @GN_DELETE_SEVERITY_LOW: Low risk, simple confirmation (e.g., clearing cache)
 * @GN_DELETE_SEVERITY_MEDIUM: Medium risk, explicit confirmation (e.g., deleting backups)
 * @GN_DELETE_SEVERITY_HIGH: High risk, type confirmation required (e.g., deleting keys)
 * @GN_DELETE_SEVERITY_CRITICAL: Critical, password + type confirmation (e.g., wipe all data)
 *
 * Severity level determines the confirmation requirements.
 */
typedef enum {
  GN_DELETE_SEVERITY_LOW = 0,
  GN_DELETE_SEVERITY_MEDIUM,
  GN_DELETE_SEVERITY_HIGH,
  GN_DELETE_SEVERITY_CRITICAL
} GnDeleteSeverity;

/**
 * GnConfirmDeleteCallback:
 * @confirmed: TRUE if user confirmed deletion, FALSE if cancelled
 * @user_data: user data passed to the callback
 *
 * Callback signature for delete confirmation.
 */
typedef void (*GnConfirmDeleteCallback)(gboolean confirmed, gpointer user_data);

/**
 * gn_confirm_delete_dialog_new:
 *
 * Creates a new confirmation dialog for delete operations.
 *
 * Returns: (transfer full): a new #GnConfirmDeleteDialog
 */
GnConfirmDeleteDialog *gn_confirm_delete_dialog_new(void);

/**
 * gn_confirm_delete_dialog_set_title:
 * @self: a #GnConfirmDeleteDialog
 * @title: the dialog title
 *
 * Sets the dialog title (e.g., "Delete Private Key").
 */
void gn_confirm_delete_dialog_set_title(GnConfirmDeleteDialog *self,
                                         const char *title);

/**
 * gn_confirm_delete_dialog_set_message:
 * @self: a #GnConfirmDeleteDialog
 * @message: the main warning message
 *
 * Sets the primary warning message explaining what will be deleted.
 */
void gn_confirm_delete_dialog_set_message(GnConfirmDeleteDialog *self,
                                           const char *message);

/**
 * gn_confirm_delete_dialog_set_detail:
 * @self: a #GnConfirmDeleteDialog
 * @detail: additional detail text
 *
 * Sets secondary text with additional context or warnings.
 */
void gn_confirm_delete_dialog_set_detail(GnConfirmDeleteDialog *self,
                                          const char *detail);

/**
 * gn_confirm_delete_dialog_set_severity:
 * @self: a #GnConfirmDeleteDialog
 * @severity: the severity level
 *
 * Sets the severity level which determines confirmation requirements.
 */
void gn_confirm_delete_dialog_set_severity(GnConfirmDeleteDialog *self,
                                            GnDeleteSeverity severity);

/**
 * gn_confirm_delete_dialog_set_confirm_text:
 * @self: a #GnConfirmDeleteDialog
 * @text: text the user must type to confirm (e.g., "DELETE" or identifier)
 *
 * For HIGH and CRITICAL severity, sets the text the user must type.
 * If NULL, a default "DELETE" is used.
 */
void gn_confirm_delete_dialog_set_confirm_text(GnConfirmDeleteDialog *self,
                                                const char *text);

/**
 * gn_confirm_delete_dialog_set_items:
 * @self: a #GnConfirmDeleteDialog
 * @items: NULL-terminated array of item descriptions to be deleted
 *
 * Sets the list of items that will be deleted for display.
 */
void gn_confirm_delete_dialog_set_items(GnConfirmDeleteDialog *self,
                                         const char **items);

/**
 * gn_confirm_delete_dialog_set_callback:
 * @self: a #GnConfirmDeleteDialog
 * @callback: the callback function
 * @user_data: user data for the callback
 *
 * Sets the callback to be invoked when the user confirms or cancels.
 */
void gn_confirm_delete_dialog_set_callback(GnConfirmDeleteDialog *self,
                                            GnConfirmDeleteCallback callback,
                                            gpointer user_data);

/**
 * gn_confirm_delete_dialog_present:
 * @self: a #GnConfirmDeleteDialog
 * @parent: the parent widget
 *
 * Presents the dialog to the user.
 */
void gn_confirm_delete_dialog_present(GnConfirmDeleteDialog *self,
                                       GtkWidget *parent);

/* ============================================================
 * Convenience Functions
 * ============================================================ */

/**
 * gn_show_delete_key_confirmation:
 * @parent: parent widget
 * @npub: the npub of the key to delete
 * @label: display label for the key (optional)
 * @callback: callback for confirmation result
 * @user_data: user data for callback
 *
 * Shows a HIGH severity confirmation dialog for deleting a private key.
 */
void gn_show_delete_key_confirmation(GtkWidget *parent,
                                      const char *npub,
                                      const char *label,
                                      GnConfirmDeleteCallback callback,
                                      gpointer user_data);

/**
 * gn_show_delete_backup_confirmation:
 * @parent: parent widget
 * @filepath: path to the backup file
 * @callback: callback for confirmation result
 * @user_data: user data for callback
 *
 * Shows a MEDIUM severity confirmation dialog for deleting a backup file.
 */
void gn_show_delete_backup_confirmation(GtkWidget *parent,
                                         const char *filepath,
                                         GnConfirmDeleteCallback callback,
                                         gpointer user_data);

/**
 * gn_show_delete_session_confirmation:
 * @parent: parent widget
 * @client_name: name of the client whose session will be deleted
 * @callback: callback for confirmation result
 * @user_data: user data for callback
 *
 * Shows a LOW severity confirmation dialog for deleting a session.
 */
void gn_show_delete_session_confirmation(GtkWidget *parent,
                                          const char *client_name,
                                          GnConfirmDeleteCallback callback,
                                          gpointer user_data);

/**
 * gn_show_wipe_all_data_confirmation:
 * @parent: parent widget
 * @callback: callback for confirmation result
 * @user_data: user data for callback
 *
 * Shows a CRITICAL severity confirmation dialog for wiping all application data.
 * Requires typing "DELETE ALL DATA" and entering the session password.
 */
void gn_show_wipe_all_data_confirmation(GtkWidget *parent,
                                         GnConfirmDeleteCallback callback,
                                         gpointer user_data);

/**
 * gn_show_delete_logs_confirmation:
 * @parent: parent widget
 * @log_count: number of log files to delete
 * @callback: callback for confirmation result
 * @user_data: user data for callback
 *
 * Shows a LOW severity confirmation dialog for deleting log files.
 */
void gn_show_delete_logs_confirmation(GtkWidget *parent,
                                       guint log_count,
                                       GnConfirmDeleteCallback callback,
                                       gpointer user_data);

G_END_DECLS

#endif /* GN_CONFIRM_DELETE_DIALOG_H */
