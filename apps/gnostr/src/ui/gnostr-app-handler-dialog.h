/**
 * GnostrAppHandlerDialog - "Open with..." dialog for NIP-89 app handlers
 *
 * Shows available app handlers for a specific event kind and allows
 * the user to select one to open the event in an external application.
 *
 * Features:
 * - Lists available handlers for the event's kind
 * - Shows handler icon, name, and description
 * - Allows setting a default handler preference
 * - Opens the event in the selected handler's web or native app
 */

#ifndef GNOSTR_APP_HANDLER_DIALOG_H
#define GNOSTR_APP_HANDLER_DIALOG_H

#include <adwaita.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_APP_HANDLER_DIALOG (gnostr_app_handler_dialog_get_type())

G_DECLARE_FINAL_TYPE(GnostrAppHandlerDialog, gnostr_app_handler_dialog, GNOSTR, APP_HANDLER_DIALOG, AdwDialog)

/**
 * gnostr_app_handler_dialog_new:
 * @parent: Parent widget
 *
 * Creates a new app handler dialog.
 *
 * Returns: (transfer full): A new #GnostrAppHandlerDialog
 */
GnostrAppHandlerDialog *gnostr_app_handler_dialog_new(GtkWidget *parent);

/**
 * gnostr_app_handler_dialog_set_event:
 * @self: The dialog
 * @event_id_hex: Event ID in hex format
 * @event_kind: The event kind number
 * @event_pubkey_hex: (nullable): Event author pubkey (for naddr encoding)
 * @d_tag: (nullable): d-tag for addressable events (for naddr encoding)
 *
 * Sets the event to open with a handler.
 */
void gnostr_app_handler_dialog_set_event(GnostrAppHandlerDialog *self,
                                          const char *event_id_hex,
                                          guint event_kind,
                                          const char *event_pubkey_hex,
                                          const char *d_tag);

/**
 * gnostr_app_handler_dialog_set_handlers:
 * @self: The dialog
 * @handlers: (element-type GnostrNip89HandlerInfo): Array of handlers
 *
 * Sets the list of available handlers to display.
 */
void gnostr_app_handler_dialog_set_handlers(GnostrAppHandlerDialog *self,
                                             GPtrArray *handlers);

/**
 * gnostr_app_handler_dialog_get_selected_handler:
 * @self: The dialog
 *
 * Gets the currently selected handler.
 *
 * Returns: (transfer none) (nullable): Selected handler or NULL
 */
gpointer gnostr_app_handler_dialog_get_selected_handler(GnostrAppHandlerDialog *self);

/**
 * gnostr_app_handler_dialog_get_remember_choice:
 * @self: The dialog
 *
 * Gets whether the user wants to remember this choice.
 *
 * Returns: TRUE if "remember" is checked
 */
gboolean gnostr_app_handler_dialog_get_remember_choice(GnostrAppHandlerDialog *self);

G_END_DECLS

#endif /* GNOSTR_APP_HANDLER_DIALOG_H */
