/* nostrc-8xfib.4: nostr-gtk's ported media stack - bounded decode, the
 * injected GnMediaSource, GnMediaViewer and GnVideoPlayer. */
#include <nostr-gtk-1.0/gn-media-decode.h>
#include <nostr-gtk-1.0/gn-media-source.h>
#include <nostr-gtk-1.0/gn-animated-image.h>
#include <string.h>
#include "nostrc-test-gdk-frame.h"

/* 2x1, red/blue then blue/red, 100 ms per frame (as test_portable.c). */
static const guint8 gif_data[] = {
  0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 2,0, 1,0, 0x80, 0, 0,
  0xFF,0,0, 0,0,0xFF,
  0x21,0xF9,4, 0,10,0, 0,0,
  0x2C, 0,0,0,0, 2,0,1,0, 0, 2, 2, 0x44,0x0A, 0,
  0x21,0xF9,4, 0,10,0, 0,0,
  0x2C, 0,0,0,0, 2,0,1,0, 0, 2, 2, 0x0C,0x0A, 0,
  0x3B };

static GBytes *
two_frame_gif(void)
{
  return g_bytes_new_static(gif_data, sizeof gif_data);
}

static GBytes *
one_frame_gif(void)
{
  guint8 *still = g_memdup2(gif_data, 43);
  still[42] = 0x3B;
  return g_bytes_new_take(still, 43);
}

static GBytes *
png_bytes(int width, int height)
{
  gsize size = (gsize)width * height * 4;
  g_autofree guint8 *pixels = g_malloc0(size);
  for (gsize i = 0; i < size; i += 4) { pixels[i] = 0x20; pixels[i + 3] = 0xFF; }
  g_autoptr(GBytes) raw = g_bytes_new(pixels, size);
  g_autoptr(GdkTexture) texture =
    gdk_memory_texture_new(width, height, GDK_MEMORY_R8G8B8A8, raw, (gsize)width * 4);
  return gdk_texture_save_to_png_bytes(texture);
}

/* A JPEG that is only a header: APP0 then SOF0 declaring width x height. */
static GBytes *
jpeg_header(guint width, guint height)
{
  guint8 d[] = { 0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x04, 0x00, 0x00,
                 0xFF, 0xC0, 0x00, 0x11, 0x08, height >> 8, height & 0xFF, width >> 8, width & 0xFF,
                 3, 1, 0x22, 0, 2, 0x11, 1, 3, 0x11, 1, 0xFF, 0xD9 };
  return g_bytes_new(d, sizeof d);
}

static GBytes *
webp_bytes(void)
{
  static const guint8 d[] = { 0x52, 0x49, 0x46, 0x46, 0x10, 0, 0, 0,
                              0x57, 0x45, 0x42, 0x50, 0x56, 0x50, 0x38, 0x20 };
  return g_bytes_new_static(d, sizeof d);
}

static void
decode_sniff_and_probe(void)
{
  g_autoptr(GBytes) png = png_bytes(3, 2);
  g_autoptr(GBytes) gif = two_frame_gif();
  g_autoptr(GBytes) jpeg = jpeg_header(640, 480);
  g_autoptr(GBytes) webp = webp_bytes();
  g_assert_cmpuint(gn_media_sniff(png), ==, GN_MEDIA_FORMAT_PNG);
  g_assert_cmpuint(gn_media_sniff(gif), ==, GN_MEDIA_FORMAT_GIF);
  g_assert_cmpuint(gn_media_sniff(jpeg), ==, GN_MEDIA_FORMAT_JPEG);
  g_assert_cmpuint(gn_media_sniff(webp), ==, GN_MEDIA_FORMAT_NONE);
  guint w = 0, h = 0;
  GnMediaFormats format = GN_MEDIA_FORMAT_NONE;
  g_assert_true(gn_media_probe_dimensions(png, &format, &w, &h, NULL));
  g_assert_cmpuint(format, ==, GN_MEDIA_FORMAT_PNG);
  g_assert_cmpuint(w, ==, 3);
  g_assert_cmpuint(h, ==, 2);
  g_assert_true(gn_media_probe_dimensions(jpeg, &format, &w, &h, NULL));
  g_assert_cmpuint(w, ==, 640);
  g_assert_cmpuint(h, ==, 480);
  g_autoptr(GError) error = NULL;
  g_assert_false(gn_media_probe_dimensions(webp, NULL, NULL, NULL, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
  g_clear_error(&error);
  /* Malformed headers: a truncated PNG, a JPEG whose segment runs off the end. */
  g_autoptr(GBytes) short_png = g_bytes_new(g_bytes_get_data(png, NULL), 20);
  g_assert_false(gn_media_probe_dimensions(short_png, NULL, NULL, NULL, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error(&error);
  static const guint8 bad_jpeg[] = { 0xFF, 0xD8, 0xFF, 0xE0, 0xFF, 0xFF };
  g_autoptr(GBytes) bad = g_bytes_new_static(bad_jpeg, sizeof bad_jpeg);
  g_assert_false(gn_media_probe_dimensions(bad, NULL, NULL, NULL, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error(&error);
  /* Every truncation of every fixture is refused or measured, never overread. */
  GBytes *fixtures[] = { png, gif, jpeg };
  for (guint f = 0; f < G_N_ELEMENTS(fixtures); f++) {
    gsize len = 0;
    const guint8 *d = g_bytes_get_data(fixtures[f], &len);
    for (gsize cut = 0; cut < MIN(len, 64); cut++) {
      g_autoptr(GBytes) part = g_bytes_new(d, cut);
      gn_media_probe_dimensions(part, NULL, NULL, NULL, NULL);
      g_autoptr(GdkPaintable) maybe = gn_media_decode(part, NULL, NULL);
      (void)maybe;
    }
  }
}

static void
decode_limits(void)
{
  GnMediaDecodeLimits limits;
  gn_media_decode_limits_init_default(&limits);
  g_assert_false(limits.allowed & GN_MEDIA_FORMAT_PIXBUF_FALLBACK);
  g_autoptr(GError) error = NULL;

  g_autoptr(GBytes) png = png_bytes(3, 2);
  g_autoptr(GdkPaintable) texture = gn_media_decode(png, &limits, &error);
  g_assert_no_error(error);
  g_assert_true(GDK_IS_TEXTURE(texture));
  g_assert_cmpint(gdk_paintable_get_intrinsic_width(texture), ==, 3);

  /* Over the byte limit: refused without looking further. */
  limits.max_bytes = g_bytes_get_size(png) / 2;
  g_assert_null(gn_media_decode(png, &limits, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE);
  g_clear_error(&error);
  gn_media_decode_limits_init_default(&limits);

  /* Over the dimension limit: refused from the header. A header-only JPEG
   * cannot decode, so the size error proves nothing was decoded. */
  limits.max_dimension = 100;
  g_autoptr(GBytes) big_jpeg = jpeg_header(5000, 10);
  g_assert_null(gn_media_decode(big_jpeg, &limits, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_assert_cmpstr(error->message, ==, "Image dimensions exceed the limit");
  g_clear_error(&error);
  limits.max_dimension = 2;
  g_autoptr(GBytes) png_wide = png_bytes(3, 1);
  g_assert_null(gn_media_decode(png_wide, &limits, &error));
  g_assert_cmpstr(error->message, ==, "Image dimensions exceed the limit");
  g_clear_error(&error);
  gn_media_decode_limits_init_default(&limits);

  /* Neither PNG, JPEG nor GIF: refused without the host's opt-in. */
  g_autoptr(GBytes) webp = webp_bytes();
  g_assert_null(gn_media_decode(webp, &limits, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
  g_clear_error(&error);

  /* A format left out of allowed is refused. */
  limits.allowed = GN_MEDIA_FORMAT_PNG;
  g_autoptr(GBytes) gif = two_frame_gif();
  g_assert_null(gn_media_decode(gif, &limits, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
  g_clear_error(&error);
  gn_media_decode_limits_init_default(&limits);

  /* GIF: an animation, or a still texture, both without gdk-pixbuf. */
  g_autoptr(GdkPaintable) anim = gn_media_decode(gif, &limits, &error);
  g_assert_no_error(error);
  g_assert_true(GN_IS_ANIMATED_IMAGE(anim));
  g_autoptr(GBytes) still_gif = one_frame_gif();
  g_autoptr(GdkPaintable) still = gn_media_decode(still_gif, &limits, &error);
  g_assert_no_error(error);
  g_assert_true(GDK_IS_TEXTURE(still));
  g_assert_cmpint(gdk_paintable_get_intrinsic_width(still), ==, 2);
  limits.max_dimension = 1;
  g_assert_null(gn_media_decode(gif, &limits, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error(&error);

  g_autoptr(GBytes) empty = g_bytes_new(NULL, 0);
  g_assert_null(gn_media_decode(empty, NULL, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

static void
on_decoded(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  GdkPaintable **out = data;
  *out = gn_media_decode_finish(result, NULL);
  g_main_context_wakeup(NULL);
}

static void
decode_async(void)
{
  g_autoptr(GBytes) png = png_bytes(4, 4);
  GdkPaintable *result = NULL;
  gn_media_decode_async(png, NULL, NULL, on_decoded, &result);
  while (!result) g_main_context_iteration(NULL, TRUE);
  g_assert_true(GDK_IS_TEXTURE(result));
  g_object_unref(result);
}

int
main(int argc, char **argv)
{
  gtk_test_init(&argc, &argv, NULL);
  nostrc_test_tolerate_gdk_frame_warning();
  g_test_add_func("/media/decode/sniff-probe", decode_sniff_and_probe);
  g_test_add_func("/media/decode/limits", decode_limits);
  g_test_add_func("/media/decode/async", decode_async);
  return g_test_run();
}
