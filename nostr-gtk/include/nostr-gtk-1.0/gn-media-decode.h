#ifndef GN_MEDIA_DECODE_H
#define GN_MEDIA_DECODE_H
#include <gtk/gtk.h>
G_BEGIN_DECLS
/*
 * Bounded image decoding for untrusted bytes (nostrc-8xfib.4).
 *
 * The format comes from the magic bytes, never from a name or MIME type.
 * PNG and JPEG are measured from their headers (IHDR, first SOFn) before any
 * pixel is decoded and then decoded by GTK's built-in loaders; GIF goes
 * through GnAnimatedImage's own decoder (an animation when it has frames,
 * else a still texture). Anything else is refused unless the limits include
 * GN_MEDIA_FORMAT_PIXBUF_FALLBACK, which hands it to
 * gdk_texture_new_from_bytes() - on most systems gdk-pixbuf - and checks the
 * size afterwards. Groundhog never sets that flag.
 */
typedef enum {
  GN_MEDIA_FORMAT_NONE = 0,
  GN_MEDIA_FORMAT_PNG = 1 << 0,
  GN_MEDIA_FORMAT_JPEG = 1 << 1,
  GN_MEDIA_FORMAT_GIF = 1 << 2,
  GN_MEDIA_FORMAT_PIXBUF_FALLBACK = 1 << 3
} GnMediaFormats;

typedef struct {
  gsize max_bytes;          /* 0: no byte limit */
  guint max_dimension;      /* per side, required */
  GnMediaFormats allowed;
} GnMediaDecodeLimits;

/* 16 MiB, 8192 px, PNG | JPEG | GIF (no fallback). */
void gn_media_decode_limits_init_default(GnMediaDecodeLimits *limits);

/* GN_MEDIA_FORMAT_PNG, _JPEG, _GIF or _NONE. */
GnMediaFormats gn_media_sniff(GBytes *bytes);
/* The size a PNG, JPEG or GIF header declares, without decoding.
 * G_IO_ERROR_NOT_SUPPORTED for another format, G_IO_ERROR_INVALID_DATA for a
 * missing or malformed header. out_* are nullable. */
gboolean gn_media_probe_dimensions(GBytes *bytes, GnMediaFormats *out_format, guint *out_width,
                                   guint *out_height, GError **error);

/* (transfer full): a GdkTexture, or a GnAnimatedImage for an animated GIF.
 * Blocking; safe to call from a worker thread. limits NULL: the defaults. */
GdkPaintable *gn_media_decode(GBytes *bytes, const GnMediaDecodeLimits *limits, GError **error);
/* gn_media_decode() on a worker thread. */
void gn_media_decode_async(GBytes *bytes, const GnMediaDecodeLimits *limits,
                           GCancellable *cancellable, GAsyncReadyCallback callback,
                           gpointer user_data);
GdkPaintable *gn_media_decode_finish(GAsyncResult *result, GError **error);
G_END_DECLS
#endif
