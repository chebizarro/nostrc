/* sheet-qr-scanner.h - QR code scanner dialog
 *
 * Provides QR code scanning for importing keys:
 * - Camera-based scanning (if available)
 * - Clipboard image paste
 * - File import
 *
 * Accepts:
 * - nsec (private key)
 * - ncryptsec (encrypted key)
 * - hex keys
 * - nostr: URIs containing keys
 */
#ifndef APPS_GNOSTR_SIGNER_UI_SHEETS_SHEET_QR_SCANNER_H
#define APPS_GNOSTR_SIGNER_UI_SHEETS_SHEET_QR_SCANNER_H

#include <gtk/gtk.h>
#include <adwaita.h>
#include "../../qr-code.h"

G_BEGIN_DECLS

#define TYPE_SHEET_QR_SCANNER (sheet_qr_scanner_get_type())
G_DECLARE_FINAL_TYPE(SheetQrScanner, sheet_qr_scanner, SHEET, QR_SCANNER, AdwDialog)

/* Callback when a key is successfully scanned */
typedef void (*SheetQrScannerSuccessCb)(const gchar *data,
                                         GnQrContentType type,
                                         gpointer user_data);

/* Create a new QR scanner dialog.
 *
 * Returns: A new SheetQrScanner
 */
SheetQrScanner *sheet_qr_scanner_new(void);

/* Set the success callback.
 *
 * @self: The dialog
 * @callback: Function to call when a valid key is scanned
 * @user_data: Data to pass to callback
 */
void sheet_qr_scanner_set_on_success(SheetQrScanner *self,
                                      SheetQrScannerSuccessCb callback,
                                      gpointer user_data);

/* Set which content types to accept.
 * By default, accepts nsec, ncryptsec, and hex keys.
 *
 * @self: The dialog
 * @types: Array of accepted types, terminated by GN_QR_TYPE_UNKNOWN
 */
void sheet_qr_scanner_set_accepted_types(SheetQrScanner *self,
                                          const GnQrContentType *types);

/* Get the last scanned data (if any).
 *
 * @self: The dialog
 *
 * Returns: The scanned data, or NULL. Do not free.
 */
const gchar *sheet_qr_scanner_get_scanned_data(SheetQrScanner *self);

/* Get the type of the last scanned data.
 *
 * @self: The dialog
 *
 * Returns: The content type
 */
GnQrContentType sheet_qr_scanner_get_scanned_type(SheetQrScanner *self);

G_END_DECLS
#endif /* APPS_GNOSTR_SIGNER_UI_SHEETS_SHEET_QR_SCANNER_H */
