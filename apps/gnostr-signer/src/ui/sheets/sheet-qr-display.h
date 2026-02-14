/* sheet-qr-display.h - QR code display dialog
 *
 * Shows a QR code for:
 * - npub (public key sharing)
 * - ncryptsec (encrypted backup)
 * - nostr: URIs
 * - bunker URIs (NIP-46)
 */
#ifndef APPS_GNOSTR_SIGNER_UI_SHEETS_SHEET_QR_DISPLAY_H
#define APPS_GNOSTR_SIGNER_UI_SHEETS_SHEET_QR_DISPLAY_H

#include <gtk/gtk.h>
#include <adwaita.h>
#include "../../qr-code.h"

G_BEGIN_DECLS

#define TYPE_SHEET_QR_DISPLAY (sheet_qr_display_get_type())
G_DECLARE_FINAL_TYPE(SheetQrDisplay, sheet_qr_display, SHEET, QR_DISPLAY, AdwDialog)

/* Create a new QR display dialog.
 *
 * Returns: A new SheetQrDisplay
 */
SheetQrDisplay *sheet_qr_display_new(void);

/* Set the data to display as a QR code.
 *
 * @self: The dialog
 * @data: The string to encode (npub, ncryptsec, URI, etc.)
 * @type: The type of content (for display purposes)
 * @title: Optional title to show above the QR code
 */
void sheet_qr_display_set_data(SheetQrDisplay *self,
                                const gchar *data,
                                GnQrContentType type,
                                const gchar *title);

/* Convenience: Set an npub for display.
 *
 * Will automatically add nostr: prefix and set appropriate title.
 */
void sheet_qr_display_set_npub(SheetQrDisplay *self,
                                const gchar *npub);

/* Convenience: Set an ncryptsec for display.
 *
 * Will show security warning and appropriate title.
 */
void sheet_qr_display_set_ncryptsec(SheetQrDisplay *self,
                                     const gchar *ncryptsec);

/* Convenience: Set a bunker URI for display.
 *
 * For NIP-46 remote signer connection.
 */
void sheet_qr_display_set_bunker_uri(SheetQrDisplay *self,
                                      const gchar *bunker_uri);

G_END_DECLS
#endif /* APPS_GNOSTR_SIGNER_UI_SHEETS_SHEET_QR_DISPLAY_H */
