/* qr-code.c - QR code generation and scanning for gnostr-signer
 *
 * Uses libqrencode for QR generation and zbar for scanning.
 * Falls back gracefully when libraries are not available.
 */
#include "qr-code.h"

#include <string.h>
#include <stdlib.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

/* Optional: libqrencode for QR generation */
#ifdef HAVE_QRENCODE
#include <qrencode.h>
#define QR_GENERATION_ENABLED 1
#else
#define QR_GENERATION_ENABLED 0
#endif

/* Optional: zbar for QR scanning */
#ifdef HAVE_ZBAR
#include <zbar.h>
#define QR_SCANNING_ENABLED 1
#else
#define QR_SCANNING_ENABLED 0
#endif

G_DEFINE_QUARK(gn-qr-error-quark, gn_qr_error)

/* ============================================================
 * Utility Functions
 * ============================================================ */

/* Check if string is 64-character hex */
static gboolean is_hex_64(const gchar *s) {
  if (!s) return FALSE;
  gsize len = strlen(s);
  if (len != 64) return FALSE;
  for (gsize i = 0; i < len; i++) {
    gchar c = s[i];
    if (!((c >= '0' && c <= '9') ||
          (c >= 'a' && c <= 'f') ||
          (c >= 'A' && c <= 'F')))
      return FALSE;
  }
  return TRUE;
}

GnQrContentType gn_qr_detect_content_type(const gchar *data) {
  if (!data || !*data) return GN_QR_TYPE_UNKNOWN;

  /* Check for nostr: URI prefix first */
  if (g_str_has_prefix(data, "nostr:")) {
    return GN_QR_TYPE_NOSTR_URI;
  }

  /* Check for bunker URIs */
  if (g_str_has_prefix(data, "nostrconnect://") ||
      g_str_has_prefix(data, "bunker://")) {
    return GN_QR_TYPE_BUNKER_URI;
  }

  /* Check for bech32 encoded keys */
  if (g_str_has_prefix(data, "npub1")) {
    return GN_QR_TYPE_NPUB;
  }
  if (g_str_has_prefix(data, "nsec1")) {
    return GN_QR_TYPE_NSEC;
  }
  if (g_str_has_prefix(data, "ncryptsec1")) {
    return GN_QR_TYPE_NCRYPTSEC;
  }

  /* Check for hex key */
  if (is_hex_64(data)) {
    return GN_QR_TYPE_HEX_KEY;
  }

  return GN_QR_TYPE_UNKNOWN;
}

const gchar *gn_qr_content_type_name(GnQrContentType type) {
  switch (type) {
    case GN_QR_TYPE_NPUB:       return "Public Key (npub)";
    case GN_QR_TYPE_NSEC:       return "Private Key (nsec)";
    case GN_QR_TYPE_NCRYPTSEC:  return "Encrypted Key (ncryptsec)";
    case GN_QR_TYPE_NOSTR_URI:  return "Nostr URI";
    case GN_QR_TYPE_BUNKER_URI: return "Bunker URI";
    case GN_QR_TYPE_HEX_KEY:    return "Hex Key";
    default:                    return "Unknown";
  }
}

gboolean gn_qr_generation_available(void) {
  return QR_GENERATION_ENABLED != 0;
}

gboolean gn_qr_scanning_available(void) {
  return QR_SCANNING_ENABLED != 0;
}

void gn_qr_scan_result_free(GnQrScanResult *result) {
  if (!result) return;
  g_free(result->data);
  g_free(result->decoded_key);
  g_free(result);
}

gboolean gn_qr_scan_result_is_importable(const GnQrScanResult *result) {
  if (!result) return FALSE;

  switch (result->type) {
    case GN_QR_TYPE_NSEC:
    case GN_QR_TYPE_NCRYPTSEC:
    case GN_QR_TYPE_HEX_KEY:
      return TRUE;
    case GN_QR_TYPE_NOSTR_URI:
      /* Check if the URI contains an nsec */
      if (result->data && strstr(result->data, "nsec1")) {
        return TRUE;
      }
      return FALSE;
    default:
      return FALSE;
  }
}

/* ============================================================
 * QR Code Generation
 * ============================================================ */

#if QR_GENERATION_ENABLED

GdkPixbuf *gn_qr_generate_pixbuf(const gchar *data,
                                  gint size,
                                  GError **error) {
  if (!data || !*data) {
    g_set_error(error, GN_QR_ERROR, GN_QR_ERROR_INVALID_DATA,
                "No data provided for QR code generation");
    return NULL;
  }

  /* Generate QR code */
  QRcode *qr = QRcode_encodeString(data, 0, QR_ECLEVEL_M, QR_MODE_8, 1);
  if (!qr) {
    g_set_error(error, GN_QR_ERROR, GN_QR_ERROR_GENERATION_FAILED,
                "Failed to generate QR code");
    return NULL;
  }

  /* Calculate module size (pixels per QR module) */
  gint qr_size = qr->width;
  gint margin = 4; /* Standard QR margin in modules */
  gint total_modules = qr_size + (margin * 2);
  gint module_size = size / total_modules;
  if (module_size < 1) module_size = 1;

  gint actual_size = total_modules * module_size;

  /* Create pixel buffer (RGBA) */
  gint rowstride = actual_size * 4;
  guchar *pixels = g_malloc0(actual_size * rowstride);

  /* Fill with white background */
  memset(pixels, 255, actual_size * rowstride);

  /* Draw QR code modules */
  for (gint y = 0; y < qr_size; y++) {
    for (gint x = 0; x < qr_size; x++) {
      if (qr->data[y * qr_size + x] & 1) {
        /* Black module */
        gint px = (x + margin) * module_size;
        gint py = (y + margin) * module_size;

        for (gint dy = 0; dy < module_size; dy++) {
          for (gint dx = 0; dx < module_size; dx++) {
            gint offset = ((py + dy) * rowstride) + ((px + dx) * 4);
            pixels[offset + 0] = 0;   /* R */
            pixels[offset + 1] = 0;   /* G */
            pixels[offset + 2] = 0;   /* B */
            pixels[offset + 3] = 255; /* A */
          }
        }
      }
    }
  }

  QRcode_free(qr);

  /* Create GdkPixbuf from pixel data */
  GdkPixbuf *pixbuf = gdk_pixbuf_new_from_data(
    pixels,
    GDK_COLORSPACE_RGB,
    TRUE,  /* has_alpha */
    8,     /* bits_per_sample */
    actual_size,
    actual_size,
    rowstride,
    (GdkPixbufDestroyNotify)g_free,
    NULL
  );

  if (!pixbuf) {
    g_free(pixels);
    g_set_error(error, GN_QR_ERROR, GN_QR_ERROR_GENERATION_FAILED,
                "Failed to create pixbuf from QR data");
    return NULL;
  }

  return pixbuf;
}

GdkTexture *gn_qr_generate_texture(const gchar *data,
                                    gint size,
                                    GError **error) {
  GdkPixbuf *pixbuf = gn_qr_generate_pixbuf(data, size, error);
  if (!pixbuf) return NULL;

  GdkTexture *texture = gdk_texture_new_for_pixbuf(pixbuf);
  g_object_unref(pixbuf);

  if (!texture) {
    g_set_error(error, GN_QR_ERROR, GN_QR_ERROR_GENERATION_FAILED,
                "Failed to create texture from pixbuf");
    return NULL;
  }

  return texture;
}

#else /* !QR_GENERATION_ENABLED */

GdkPixbuf *gn_qr_generate_pixbuf(const gchar *data,
                                  gint size,
                                  GError **error) {
  (void)data;
  (void)size;
  g_set_error(error, GN_QR_ERROR, GN_QR_ERROR_GENERATION_FAILED,
              "QR code generation is not available (libqrencode not found)");
  return NULL;
}

GdkTexture *gn_qr_generate_texture(const gchar *data,
                                    gint size,
                                    GError **error) {
  (void)data;
  (void)size;
  g_set_error(error, GN_QR_ERROR, GN_QR_ERROR_GENERATION_FAILED,
              "QR code generation is not available (libqrencode not found)");
  return NULL;
}

#endif /* QR_GENERATION_ENABLED */

/* Convenience functions for specific content types */

GdkTexture *gn_qr_generate_npub(const gchar *npub,
                                 gint size,
                                 GError **error) {
  if (!npub || !g_str_has_prefix(npub, "npub1")) {
    g_set_error(error, GN_QR_ERROR, GN_QR_ERROR_INVALID_DATA,
                "Invalid npub format");
    return NULL;
  }

  /* Generate with nostr: URI prefix for better compatibility */
  g_autofree gchar *uri = g_strdup_printf("nostr:%s", npub);
  return gn_qr_generate_texture(uri, size, error);
}

GdkTexture *gn_qr_generate_ncryptsec(const gchar *ncryptsec,
                                      gint size,
                                      GError **error) {
  if (!ncryptsec || !g_str_has_prefix(ncryptsec, "ncryptsec1")) {
    g_set_error(error, GN_QR_ERROR, GN_QR_ERROR_INVALID_DATA,
                "Invalid ncryptsec format");
    return NULL;
  }

  /* ncryptsec is used directly without URI prefix */
  return gn_qr_generate_texture(ncryptsec, size, error);
}

GdkTexture *gn_qr_generate_bunker_uri(const gchar *bunker_uri,
                                       gint size,
                                       GError **error) {
  if (!bunker_uri ||
      (!g_str_has_prefix(bunker_uri, "nostrconnect://") &&
       !g_str_has_prefix(bunker_uri, "bunker://"))) {
    g_set_error(error, GN_QR_ERROR, GN_QR_ERROR_INVALID_DATA,
                "Invalid bunker URI format");
    return NULL;
  }

  return gn_qr_generate_texture(bunker_uri, size, error);
}

/* ============================================================
 * QR Code Scanning
 * ============================================================ */

#if QR_SCANNING_ENABLED

GnQrScanResult *gn_qr_scan_pixbuf(GdkPixbuf *pixbuf,
                                   GError **error) {
  if (!pixbuf) {
    g_set_error(error, GN_QR_ERROR, GN_QR_ERROR_INVALID_DATA,
                "No image provided for scanning");
    return NULL;
  }

  /* Get image data */
  gint width = gdk_pixbuf_get_width(pixbuf);
  gint height = gdk_pixbuf_get_height(pixbuf);
  gint n_channels = gdk_pixbuf_get_n_channels(pixbuf);
  gint rowstride = gdk_pixbuf_get_rowstride(pixbuf);
  guchar *pixels = gdk_pixbuf_get_pixels(pixbuf);

  /* Create zbar scanner */
  zbar_image_scanner_t *scanner = zbar_image_scanner_create();
  zbar_image_scanner_set_config(scanner, ZBAR_QRCODE, ZBAR_CFG_ENABLE, 1);

  /* Convert to grayscale for zbar */
  guchar *gray = g_malloc(width * height);
  for (gint y = 0; y < height; y++) {
    for (gint x = 0; x < width; x++) {
      guchar *p = pixels + y * rowstride + x * n_channels;
      /* Simple grayscale conversion: 0.299*R + 0.587*G + 0.114*B */
      gray[y * width + x] = (guchar)(0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2]);
    }
  }

  /* Create zbar image */
  zbar_image_t *image = zbar_image_create();
  zbar_image_set_format(image, zbar_fourcc('Y', '8', '0', '0'));
  zbar_image_set_size(image, width, height);
  zbar_image_set_data(image, gray, width * height, zbar_image_free_data);

  /* Scan for QR codes */
  gint n = zbar_scan_image(scanner, image);

  GnQrScanResult *result = NULL;

  if (n > 0) {
    /* Get first result */
    const zbar_symbol_t *symbol = zbar_image_first_symbol(image);
    if (symbol) {
      const char *data = zbar_symbol_get_data(symbol);
      if (data && *data) {
        result = g_new0(GnQrScanResult, 1);
        result->data = g_strdup(data);
        result->type = gn_qr_detect_content_type(data);

        /* For nostr: URIs, extract the actual key */
        if (result->type == GN_QR_TYPE_NOSTR_URI &&
            g_str_has_prefix(data, "nostr:")) {
          result->decoded_key = g_strdup(data + 6); /* Skip "nostr:" */
        }
      }
    }
  }

  zbar_image_destroy(image);
  zbar_image_scanner_destroy(scanner);

  if (!result) {
    g_set_error(error, GN_QR_ERROR, GN_QR_ERROR_NO_QR_FOUND,
                "No QR code found in image");
    return NULL;
  }

  return result;
}

#else /* !QR_SCANNING_ENABLED */

GnQrScanResult *gn_qr_scan_pixbuf(GdkPixbuf *pixbuf,
                                   GError **error) {
  (void)pixbuf;
  g_set_error(error, GN_QR_ERROR, GN_QR_ERROR_SCAN_FAILED,
              "QR code scanning is not available (zbar not found)");
  return NULL;
}

#endif /* QR_SCANNING_ENABLED */

GnQrScanResult *gn_qr_scan_texture(GdkTexture *texture,
                                    GError **error) {
  if (!texture) {
    g_set_error(error, GN_QR_ERROR, GN_QR_ERROR_INVALID_DATA,
                "No texture provided for scanning");
    return NULL;
  }

  /* Download texture to pixbuf */
  gint width = gdk_texture_get_width(texture);
  gint height = gdk_texture_get_height(texture);

  /* Create a pixbuf and download texture data */
  GdkPixbuf *pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, width, height);
  if (!pixbuf) {
    g_set_error(error, GN_QR_ERROR, GN_QR_ERROR_SCAN_FAILED,
                "Failed to allocate pixbuf for texture download");
    return NULL;
  }

  /* Get pixel data from texture */
  gint rowstride = gdk_pixbuf_get_rowstride(pixbuf);
  guchar *pixels = gdk_pixbuf_get_pixels(pixbuf);

  /* Download texture data (RGBA format) */
  gdk_texture_download(texture, pixels, rowstride);

  GnQrScanResult *result = gn_qr_scan_pixbuf(pixbuf, error);
  g_object_unref(pixbuf);

  return result;
}

/* Clipboard scanning context */
typedef struct {
  GnQrScanCallback callback;
  gpointer user_data;
} ClipboardScanContext;

static void clipboard_texture_ready(GObject *source,
                                     GAsyncResult *res,
                                     gpointer user_data) {
  ClipboardScanContext *ctx = user_data;
  GdkClipboard *clipboard = GDK_CLIPBOARD(source);
  GError *error = NULL;

  GdkTexture *texture = gdk_clipboard_read_texture_finish(clipboard, res, &error);

  if (error) {
    if (ctx->callback) {
      ctx->callback(NULL, error, ctx->user_data);
    }
    g_error_free(error);
    g_free(ctx);
    return;
  }

  if (!texture) {
    GError *no_image = g_error_new(GN_QR_ERROR, GN_QR_ERROR_CLIPBOARD_EMPTY,
                                    "Clipboard does not contain an image");
    if (ctx->callback) {
      ctx->callback(NULL, no_image, ctx->user_data);
    }
    g_error_free(no_image);
    g_free(ctx);
    return;
  }

  /* Scan the texture */
  GnQrScanResult *result = gn_qr_scan_texture(texture, &error);
  g_object_unref(texture);

  if (ctx->callback) {
    ctx->callback(result, error, ctx->user_data);
  }

  if (error) g_error_free(error);
  if (result) gn_qr_scan_result_free(result);
  g_free(ctx);
}

void gn_qr_scan_clipboard_async(GdkClipboard *clipboard,
                                 GnQrScanCallback callback,
                                 gpointer user_data) {
  if (!clipboard) {
    GError *error = g_error_new(GN_QR_ERROR, GN_QR_ERROR_CLIPBOARD_EMPTY,
                                 "No clipboard provided");
    if (callback) {
      callback(NULL, error, user_data);
    }
    g_error_free(error);
    return;
  }

  ClipboardScanContext *ctx = g_new0(ClipboardScanContext, 1);
  ctx->callback = callback;
  ctx->user_data = user_data;

  gdk_clipboard_read_texture_async(clipboard, NULL, clipboard_texture_ready, ctx);
}
