/* qr-code.h - QR code generation and scanning for gnostr-signer
 *
 * Provides QR code generation using libqrencode and scanning using
 * zbar (or gstreamer-based camera scanning where available).
 *
 * Supports:
 * - npub (public key) display
 * - ncryptsec (encrypted backup) display
 * - nostr: URIs
 * - Camera-based QR scanning
 * - Clipboard image paste scanning
 */
#ifndef APPS_GNOSTR_SIGNER_QR_CODE_H
#define APPS_GNOSTR_SIGNER_QR_CODE_H

#include <glib.h>
#include <gdk/gdk.h>
#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Error domain for QR operations */
#define GN_QR_ERROR (gn_qr_error_quark())
GQuark gn_qr_error_quark(void);

/* Error codes for QR operations */
typedef enum {
  GN_QR_ERROR_GENERATION_FAILED,   /* Failed to generate QR code */
  GN_QR_ERROR_DATA_TOO_LONG,       /* Input data too long for QR code */
  GN_QR_ERROR_INVALID_DATA,        /* Invalid input data */
  GN_QR_ERROR_SCAN_FAILED,         /* Failed to scan QR code */
  GN_QR_ERROR_NO_QR_FOUND,         /* No QR code found in image */
  GN_QR_ERROR_CAMERA_UNAVAILABLE,  /* Camera not available */
  GN_QR_ERROR_CLIPBOARD_EMPTY,     /* Clipboard has no image */
  GN_QR_ERROR_INVALID_FORMAT       /* Scanned data is not valid nostr format */
} GnQrError;

/* QR code content type */
typedef enum {
  GN_QR_TYPE_UNKNOWN,
  GN_QR_TYPE_NPUB,        /* npub1... public key */
  GN_QR_TYPE_NSEC,        /* nsec1... private key */
  GN_QR_TYPE_NCRYPTSEC,   /* ncryptsec1... encrypted key */
  GN_QR_TYPE_NOSTR_URI,   /* nostr:npub1... or nostr:note1... etc */
  GN_QR_TYPE_BUNKER_URI,  /* nostrconnect:// or bunker:// */
  GN_QR_TYPE_HEX_KEY      /* 64-character hex key */
} GnQrContentType;

/* Result of QR code scan */
typedef struct {
  gchar *data;              /* Raw scanned data */
  GnQrContentType type;     /* Detected content type */
  gchar *decoded_key;       /* Decoded key if applicable (hex format for import) */
} GnQrScanResult;

/* Free a scan result */
void gn_qr_scan_result_free(GnQrScanResult *result);

/* ============================================================
 * QR Code Generation
 * ============================================================ */

/* Generate a QR code as a GdkTexture (for display in GTK4).
 *
 * @data: The string to encode in the QR code
 * @size: Desired size in pixels (will be rounded to module size)
 * @error: Error information
 *
 * Returns: A new GdkTexture containing the QR code, or NULL on error.
 *          Caller owns the reference.
 */
GdkTexture *gn_qr_generate_texture(const gchar *data,
                                    gint size,
                                    GError **error);

/* Generate a QR code as a GdkPixbuf (for more flexibility).
 *
 * @data: The string to encode in the QR code
 * @size: Desired size in pixels (will be rounded to module size)
 * @error: Error information
 *
 * Returns: A new GdkPixbuf containing the QR code, or NULL on error.
 *          Caller owns the reference.
 */
GdkPixbuf *gn_qr_generate_pixbuf(const gchar *data,
                                  gint size,
                                  GError **error);

/* Generate a QR code for an npub with nostr: URI prefix.
 *
 * @npub: The npub1... string
 * @size: Desired size in pixels
 * @error: Error information
 *
 * Returns: A new GdkTexture, or NULL on error.
 */
GdkTexture *gn_qr_generate_npub(const gchar *npub,
                                 gint size,
                                 GError **error);

/* Generate a QR code for an ncryptsec (encrypted backup).
 *
 * @ncryptsec: The ncryptsec1... string
 * @size: Desired size in pixels
 * @error: Error information
 *
 * Returns: A new GdkTexture, or NULL on error.
 */
GdkTexture *gn_qr_generate_ncryptsec(const gchar *ncryptsec,
                                      gint size,
                                      GError **error);

/* Generate a QR code for a bunker URI (NIP-46).
 *
 * @bunker_uri: The nostrconnect:// or bunker:// URI
 * @size: Desired size in pixels
 * @error: Error information
 *
 * Returns: A new GdkTexture, or NULL on error.
 */
GdkTexture *gn_qr_generate_bunker_uri(const gchar *bunker_uri,
                                       gint size,
                                       GError **error);

/* ============================================================
 * QR Code Scanning
 * ============================================================ */

/* Callback type for async scan completion */
typedef void (*GnQrScanCallback)(GnQrScanResult *result,
                                  GError *error,
                                  gpointer user_data);

/* Scan a QR code from a GdkPixbuf image.
 *
 * @pixbuf: The image to scan
 * @error: Error information
 *
 * Returns: A new GnQrScanResult, or NULL on error.
 *          Caller owns and must free with gn_qr_scan_result_free().
 */
GnQrScanResult *gn_qr_scan_pixbuf(GdkPixbuf *pixbuf,
                                   GError **error);

/* Scan a QR code from a GdkTexture.
 *
 * @texture: The texture to scan
 * @error: Error information
 *
 * Returns: A new GnQrScanResult, or NULL on error.
 */
GnQrScanResult *gn_qr_scan_texture(GdkTexture *texture,
                                    GError **error);

/* Scan a QR code from the clipboard (if it contains an image).
 *
 * @clipboard: The GdkClipboard to read from
 * @callback: Function to call when scan completes
 * @user_data: Data to pass to callback
 *
 * This is async because clipboard reading is async in GTK4.
 */
void gn_qr_scan_clipboard_async(GdkClipboard *clipboard,
                                 GnQrScanCallback callback,
                                 gpointer user_data);

/* Check if the scanned data is a valid nostr format for import.
 *
 * @result: The scan result to validate
 *
 * Returns: TRUE if the data can be used for key import
 */
gboolean gn_qr_scan_result_is_importable(const GnQrScanResult *result);

/* ============================================================
 * Utility Functions
 * ============================================================ */

/* Detect the content type of a string.
 *
 * @data: The string to analyze
 *
 * Returns: The detected content type
 */
GnQrContentType gn_qr_detect_content_type(const gchar *data);

/* Get a human-readable name for a content type.
 *
 * @type: The content type
 *
 * Returns: A static string describing the type
 */
const gchar *gn_qr_content_type_name(GnQrContentType type);

/* Check if libqrencode is available at runtime.
 *
 * Returns: TRUE if QR generation is available
 */
gboolean gn_qr_generation_available(void);

/* Check if zbar is available at runtime.
 *
 * Returns: TRUE if QR scanning is available
 */
gboolean gn_qr_scanning_available(void);

G_END_DECLS
#endif /* APPS_GNOSTR_SIGNER_QR_CODE_H */
