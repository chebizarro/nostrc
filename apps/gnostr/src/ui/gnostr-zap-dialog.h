/**
 * GnostrZapDialog - NIP-57 Zap Dialog
 *
 * Dialog for selecting zap amount and sending lightning zaps.
 */

#ifndef GNOSTR_ZAP_DIALOG_H
#define GNOSTR_ZAP_DIALOG_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_ZAP_DIALOG (gnostr_zap_dialog_get_type())
G_DECLARE_FINAL_TYPE(GnostrZapDialog, gnostr_zap_dialog, GNOSTR, ZAP_DIALOG, GtkWindow)

/**
 * Signals:
 * "zap-sent" - Emitted when zap is successfully sent
 *   void handler(GnostrZapDialog *self, gchar *event_id, gint64 amount_msat, gpointer user_data)
 * "zap-failed" - Emitted when zap fails
 *   void handler(GnostrZapDialog *self, gchar *error_message, gpointer user_data)
 */

/**
 * gnostr_zap_dialog_new:
 * @parent: (nullable): Parent window
 *
 * Create a new zap dialog.
 *
 * Returns: (transfer full): New zap dialog
 */
GnostrZapDialog *gnostr_zap_dialog_new(GtkWindow *parent);

/**
 * gnostr_zap_dialog_set_recipient:
 * @self: the zap dialog
 * @pubkey_hex: Recipient's pubkey (hex)
 * @display_name: (nullable): Recipient's display name
 * @lud16: Recipient's lightning address
 *
 * Set the zap recipient info.
 */
void gnostr_zap_dialog_set_recipient(GnostrZapDialog *self,
                                     const gchar *pubkey_hex,
                                     const gchar *display_name,
                                     const gchar *lud16);

/**
 * gnostr_zap_dialog_set_event:
 * @self: the zap dialog
 * @event_id: Event ID being zapped (hex)
 * @event_kind: Kind of the event (1 for note, etc.)
 *
 * Set the event being zapped (for note zaps, not profile zaps).
 */
void gnostr_zap_dialog_set_event(GnostrZapDialog *self,
                                 const gchar *event_id,
                                 gint event_kind);

/**
 * gnostr_zap_dialog_set_relays:
 * @self: the zap dialog
 * @relays: NULL-terminated array of relay URLs
 *
 * Set the relays for the zap receipt.
 */
void gnostr_zap_dialog_set_relays(GnostrZapDialog *self,
                                  const gchar * const *relays);

/**
 * gnostr_zap_dialog_show:
 * @parent: (nullable): Parent window
 * @pubkey_hex: Recipient's public key (hex)
 * @lud16: Recipient's lightning address
 * @event_id: (nullable): Event ID being zapped (hex), or NULL for profile zap
 *
 * Convenience function to create, configure, and present a zap dialog.
 * This is the primary entry point for initiating a zap from anywhere in the app.
 *
 * If NWC (Nostr Wallet Connect) is configured, the dialog will automatically
 * pay the invoice. If not, it will display a QR code for external wallet payment.
 *
 * Returns: (transfer full): The presented zap dialog
 */
GnostrZapDialog *gnostr_zap_dialog_show(GtkWindow *parent,
                                        const gchar *pubkey_hex,
                                        const gchar *lud16,
                                        const gchar *event_id);

G_END_DECLS

#endif /* GNOSTR_ZAP_DIALOG_H */
