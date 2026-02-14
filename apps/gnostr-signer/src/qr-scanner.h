/* qr-scanner.h - Camera-based QR code scanner for gnostr-signer
 *
 * Provides camera access for real-time QR code scanning using
 * GStreamer pipelines or platform-specific camera APIs.
 */
#ifndef APPS_GNOSTR_SIGNER_QR_SCANNER_H
#define APPS_GNOSTR_SIGNER_QR_SCANNER_H

#include "qr-code.h"
#include <gtk/gtk.h>

G_BEGIN_DECLS

/* ============================================================
 * GnQrScanner - Camera-based QR scanner widget
 * ============================================================
 *
 * This is a GTK widget that shows a camera preview and scans
 * for QR codes in real-time.
 */

#define GN_TYPE_QR_SCANNER (gn_qr_scanner_get_type())
G_DECLARE_FINAL_TYPE(GnQrScanner, gn_qr_scanner, GN, QR_SCANNER, GtkWidget)

/* Create a new QR scanner widget.
 *
 * Returns: A new GnQrScanner widget
 */
GnQrScanner *gn_qr_scanner_new(void);

/* Start the camera and begin scanning.
 *
 * @self: The scanner widget
 * @error: Error information
 *
 * Returns: TRUE if camera started successfully
 */
gboolean gn_qr_scanner_start(GnQrScanner *self,
                              GError **error);

/* Stop the camera and scanning.
 *
 * @self: The scanner widget
 */
void gn_qr_scanner_stop(GnQrScanner *self);

/* Check if the scanner is currently active.
 *
 * @self: The scanner widget
 *
 * Returns: TRUE if scanning is active
 */
gboolean gn_qr_scanner_is_active(GnQrScanner *self);

/* Set the accepted content types for scanning.
 * If set, only QR codes matching these types will trigger the signal.
 *
 * @self: The scanner widget
 * @types: Array of accepted types, terminated by GN_QR_TYPE_UNKNOWN
 */
void gn_qr_scanner_set_accepted_types(GnQrScanner *self,
                                       const GnQrContentType *types);

/* Signals:
 *
 * "qr-detected" - Emitted when a QR code is successfully scanned
 *   void handler(GnQrScanner *scanner, GnQrScanResult *result, gpointer user_data)
 *
 * "error" - Emitted when an error occurs
 *   void handler(GnQrScanner *scanner, GError *error, gpointer user_data)
 */

/* ============================================================
 * Camera Availability
 * ============================================================ */

/* Check if camera scanning is available on this system.
 *
 * Returns: TRUE if camera scanning can be used
 */
gboolean gn_qr_camera_available(void);

/* Get a list of available cameras.
 *
 * Returns: A NULL-terminated array of camera device names.
 *          Caller must free with g_strfreev().
 */
gchar **gn_qr_list_cameras(void);

G_END_DECLS
#endif /* APPS_GNOSTR_SIGNER_QR_SCANNER_H */
