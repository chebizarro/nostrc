#include <nostr-gtk-1.0/gn-media-decode.h>
#include <nostr-gtk-1.0/gn-animated-image.h>
#include "gn-media-decode-private.h"
#include <string.h>

/* nostrc-8xfib.4: one bounded decoder for every nostr-gtk media widget,
 * replacing Gnostr's unbounded gdk_texture_new_from_bytes() on downloaded
 * bytes (gnostr-image-viewer.c). The header is read before any pixel. */

static const guint8 png_magic[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

static guint
be16(const guint8 *p)
{
  return (guint)p[0] << 8 | p[1];
}

static guint32
be32(const guint8 *p)
{
  return (guint32)p[0] << 24 | (guint32)p[1] << 16 | (guint32)p[2] << 8 | p[3];
}

void
gn_media_decode_limits_init_default(GnMediaDecodeLimits *limits)
{
  g_return_if_fail(limits != NULL);
  limits->max_bytes = 16u * 1024u * 1024u;
  limits->max_dimension = 8192;
  limits->allowed = GN_MEDIA_FORMAT_PNG | GN_MEDIA_FORMAT_JPEG | GN_MEDIA_FORMAT_GIF;
}

GnMediaFormats
gn_media_sniff(GBytes *bytes)
{
  gsize len = 0;
  const guint8 *d = bytes ? g_bytes_get_data(bytes, &len) : NULL;
  if (!d) return GN_MEDIA_FORMAT_NONE;
  if (len >= sizeof png_magic && memcmp(d, png_magic, sizeof png_magic) == 0)
    return GN_MEDIA_FORMAT_PNG;
  if (len >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return GN_MEDIA_FORMAT_JPEG;
  if (gn_animated_image_probe(bytes, NULL, NULL)) return GN_MEDIA_FORMAT_GIF;
  return GN_MEDIA_FORMAT_NONE;
}

static gboolean
jpeg_size(const guint8 *d, gsize len, guint *w, guint *h)
{
  gsize pos = 2;
  while (pos < len) {
    if (d[pos] != 0xFF) return FALSE;
    while (pos < len && d[pos] == 0xFF) pos++;
    if (pos >= len) return FALSE;
    guint8 m = d[pos++];
    if (m == 0x01 || (m >= 0xD0 && m <= 0xD8)) continue;
    if (m == 0xD9 || m == 0xDA) return FALSE;
    if (pos + 2 > len) return FALSE;
    guint seg = be16(d + pos);
    if (seg < 2 || pos + seg > len) return FALSE;
    if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
      if (seg < 7) return FALSE;
      *h = be16(d + pos + 3);
      *w = be16(d + pos + 5);
      return TRUE;
    }
    pos += seg;
  }
  return FALSE;
}

gboolean
gn_media_probe_dimensions(GBytes *bytes, GnMediaFormats *out_format, guint *out_width,
                          guint *out_height, GError **error)
{
  GnMediaFormats format = gn_media_sniff(bytes);
  if (out_format) *out_format = format;
  gsize len = 0;
  const guint8 *d = bytes ? g_bytes_get_data(bytes, &len) : NULL;
  guint w = 0, h = 0;
  gboolean ok = FALSE;
  switch (format) {
  case GN_MEDIA_FORMAT_PNG:
    /* signature, IHDR length (13) and type, then width and height. */
    ok = len >= 24 && be32(d + 8) == 13 && memcmp(d + 12, "IHDR", 4) == 0;
    if (ok) {
      guint32 pw = be32(d + 16), ph = be32(d + 20);
      ok = pw <= G_MAXINT && ph <= G_MAXINT;
      w = pw;
      h = ph;
    }
    break;
  case GN_MEDIA_FORMAT_JPEG:
    ok = jpeg_size(d, len, &w, &h);
    break;
  case GN_MEDIA_FORMAT_GIF:
    ok = gn_animated_image_probe(bytes, &w, &h);
    break;
  default:
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Unsupported image format");
    return FALSE;
  }
  if (!ok) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Damaged image header");
    return FALSE;
  }
  if (out_width) *out_width = w;
  if (out_height) *out_height = h;
  return TRUE;
}

static gboolean
within(guint w, guint h, guint max_dimension, GError **error)
{
  if (w && h && w <= max_dimension && h <= max_dimension) return TRUE;
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                      "Image dimensions exceed the limit");
  return FALSE;
}

GdkPaintable *
gn_media_decode(GBytes *bytes, const GnMediaDecodeLimits *limits, GError **error)
{
  g_return_val_if_fail(bytes != NULL, NULL);
  GnMediaDecodeLimits defaults;
  if (!limits) {
    gn_media_decode_limits_init_default(&defaults);
    limits = &defaults;
  }
  gsize len = g_bytes_get_size(bytes);
  if (!len) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Empty image data");
    return NULL;
  }
  if (limits->max_bytes && len > limits->max_bytes) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE, "Image is too large");
    return NULL;
  }
  guint max_dim = limits->max_dimension ? limits->max_dimension : 4096;
  GnMediaFormats format = gn_media_sniff(bytes);
  if (format != GN_MEDIA_FORMAT_NONE && !(limits->allowed & format)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Image format not allowed");
    return NULL;
  }
  if (format == GN_MEDIA_FORMAT_NONE) {
    if (!(limits->allowed & GN_MEDIA_FORMAT_PIXBUF_FALLBACK)) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                          "Unsupported image format");
      return NULL;
    }
    /* Host opt-in: GTK's other loaders (WebP, AVIF ... via gdk-pixbuf). */
    GdkTexture *texture = gdk_texture_new_from_bytes(bytes, error);
    if (!texture) return NULL;
    if (!within((guint)gdk_texture_get_width(texture), (guint)gdk_texture_get_height(texture),
                max_dim, error)) {
      g_object_unref(texture);
      return NULL;
    }
    return GDK_PAINTABLE(texture);
  }
  guint w = 0, h = 0;
  if (!gn_media_probe_dimensions(bytes, NULL, &w, &h, error) || !within(w, h, max_dim, error))
    return NULL;
  if (format == GN_MEDIA_FORMAT_GIF) {
    g_autoptr(GError) anim_error = NULL;
    GnAnimatedImage *animation = gn_animated_image_new_from_bytes(bytes, max_dim, &anim_error);
    if (animation) return GDK_PAINTABLE(animation);
    if (!g_error_matches(anim_error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED)) {
      g_propagate_error(error, g_steal_pointer(&anim_error));
      return NULL;
    }
    GdkTexture *still = gn_animated_image_decode_first_frame(bytes, max_dim, error);
    return still ? GDK_PAINTABLE(still) : NULL;
  }
  /* PNG or JPEG by magic and within bounds: GTK's built-in loaders. */
  GdkTexture *texture = gdk_texture_new_from_bytes(bytes, error);
  return texture ? GDK_PAINTABLE(texture) : NULL;
}

typedef struct {
  GBytes *bytes;
  GnMediaDecodeLimits limits;
} DecodeJob;

static void
decode_job_free(gpointer data)
{
  DecodeJob *job = data;
  g_bytes_unref(job->bytes);
  g_free(job);
}

static void
decode_thread(GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  (void)source; (void)cancellable;
  DecodeJob *job = data;
  GError *error = NULL;
  GdkPaintable *paintable = gn_media_decode(job->bytes, &job->limits, &error);
  if (paintable) g_task_return_pointer(task, paintable, g_object_unref);
  else g_task_return_error(task, error);
}

void
gn_media_decode_async(GBytes *bytes, const GnMediaDecodeLimits *limits, GCancellable *cancellable,
                      GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail(bytes != NULL);
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  g_task_set_source_tag(task, gn_media_decode_async);
  g_task_set_return_on_cancel(task, TRUE);
  DecodeJob *job = g_new0(DecodeJob, 1);
  job->bytes = g_bytes_ref(bytes);
  if (limits) job->limits = *limits;
  else gn_media_decode_limits_init_default(&job->limits);
  g_task_set_task_data(task, job, decode_job_free);
  g_task_run_in_thread(task, decode_thread);
  g_object_unref(task);
}

GdkPaintable *
gn_media_decode_finish(GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, NULL), NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}
