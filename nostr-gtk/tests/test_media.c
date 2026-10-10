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

/* ---- a host source under test control ------------------------------------ */

#include <nostr-gtk-1.0/gn-media-viewer.h>
#include <nostr-gtk-1.0/gn-video-player.h>

typedef struct {
  GObject parent_instance;
  GnMediaPolicy policy;
  guint fetches, streams, adopted;
  GPtrArray *pending; /* GTask */
} FakeSource;
typedef GObjectClass FakeSourceClass;
static void fake_source_iface_init(GnMediaSourceInterface *iface);
G_DEFINE_TYPE_WITH_CODE(FakeSource, fake_source, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(GN_TYPE_MEDIA_SOURCE, fake_source_iface_init))
G_DEFINE_AUTOPTR_CLEANUP_FUNC(FakeSource, g_object_unref)

static GnMediaPolicy
fake_policy(GnMediaSource *source, const char *url, GnMediaKind kind)
{
  (void)url; (void)kind;
  return ((FakeSource *)source)->policy;
}

static void
fake_fetch_async(GnMediaSource *source, const char *url, GnMediaKind kind, gsize max_bytes,
                 GCancellable *cancellable, GAsyncReadyCallback callback, gpointer data)
{
  (void)kind; (void)max_bytes;
  FakeSource *self = (FakeSource *)source;
  GTask *task = g_task_new(source, cancellable, callback, data);
  g_task_set_task_data(task, g_strdup(url), g_free);
  self->fetches++;
  g_ptr_array_add(self->pending, task);
}

static GBytes *
fake_fetch_finish(GnMediaSource *source, GAsyncResult *result, GError **error)
{
  (void)source;
  return g_task_propagate_pointer(G_TASK(result), error);
}

static GtkMediaStream *
fake_open_stream(GnMediaSource *source, const char *url, GError **error)
{
  (void)url; (void)error;
  ((FakeSource *)source)->streams++;
  return gtk_media_file_new();
}

static void
fake_adopt(GnMediaSource *source, GtkWidget *widget)
{
  (void)widget;
  ((FakeSource *)source)->adopted++;
}

static void
fake_source_iface_init(GnMediaSourceInterface *iface)
{
  iface->get_policy = fake_policy;
  iface->fetch_async = fake_fetch_async;
  iface->fetch_finish = fake_fetch_finish;
  iface->open_stream = fake_open_stream;
  iface->adopt = fake_adopt;
}

static void
fake_source_finalize(GObject *object)
{
  g_ptr_array_unref(((FakeSource *)object)->pending);
  G_OBJECT_CLASS(fake_source_parent_class)->finalize(object);
}

static void fake_source_class_init(FakeSourceClass *klass) { klass->finalize = fake_source_finalize; }
static void fake_source_init(FakeSource *self) { self->pending = g_ptr_array_new_with_free_func(g_object_unref); }

static FakeSource *
fake_source_new(GnMediaPolicy policy)
{
  FakeSource *self = g_object_new(fake_source_get_type(), NULL);
  self->policy = policy;
  return self;
}

/* Answers the nth request with bytes (NULL: an error). */
static GCancellable *
fake_cancellable(FakeSource *self, guint nth)
{
  return g_task_get_cancellable(g_ptr_array_index(self->pending, nth));
}

static void
fake_complete(FakeSource *self, guint nth, GBytes *bytes)
{
  GTask *task = g_ptr_array_index(self->pending, nth);
  if (bytes) g_task_return_pointer(task, g_bytes_ref(bytes), (GDestroyNotify)g_bytes_unref);
  else g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "HTTP 404");
}

static gboolean
viewer_shows_something(gpointer data)
{
  return gn_media_viewer_get_paintable(data) != NULL || gn_media_viewer_get_message(data) != NULL;
}

static void
spin_until(GSourceFunc done, gpointer data)
{
  gint64 deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
  while (!done(data) && g_get_monotonic_time() < deadline) g_main_context_iteration(NULL, FALSE);
  g_assert_true(done(data));
}

static void
spin_idle(void)
{
  for (int i = 0; i < 50; i++) g_main_context_iteration(NULL, FALSE);
}

/* The OSD button named name (tooltip), or NULL. */
static GtkWidget *
find_button(GtkWidget *root, const char *name)
{
  if (GTK_IS_BUTTON(root) && g_strcmp0(gtk_widget_get_tooltip_text(root), name) == 0) return root;
  for (GtkWidget *c = gtk_widget_get_first_child(root); c; c = gtk_widget_get_next_sibling(c)) {
    GtkWidget *found = find_button(c, name);
    if (found) return found;
  }
  return NULL;
}

static const char *two_urls[] = { "https://example.org/a.png", "https://example.org/b.png", NULL };

static void
viewer_ask_waits_for_load(void)
{
  g_autoptr(FakeSource) source = fake_source_new(GN_MEDIA_POLICY_ASK);
  GnMediaViewer *viewer = gn_media_viewer_new(NULL);
  gn_media_viewer_set_source(viewer, GN_MEDIA_SOURCE(source));
  g_assert_cmpuint(source->adopted, ==, 1);
  gn_media_viewer_set_gallery(viewer, two_urls, 0);
  /* Shown, navigated: nothing fetched; Load is offered. */
  g_assert_true(gn_media_viewer_get_load_offered(viewer));
  g_assert_cmpstr(gn_media_viewer_get_message(viewer), ==, "Remote media is blocked");
  g_assert_true(gn_media_viewer_navigate(viewer, 1));
  g_assert_true(gn_media_viewer_navigate(viewer, -1));
  g_assert_cmpuint(source->fetches, ==, 0);
  /* The user asks: exactly one fetch, decoded off-thread, then shown. */
  gn_media_viewer_request_load(viewer);
  gn_media_viewer_request_load(viewer);
  g_assert_cmpuint(source->fetches, ==, 1);
  g_assert_true(gn_media_viewer_get_loading(viewer));
  g_assert_false(gn_media_viewer_get_load_offered(viewer));
  g_autoptr(GBytes) png = png_bytes(20, 10);
  fake_complete(source, 0, png);
  spin_until(viewer_shows_something, viewer);
  g_assert_true(GDK_IS_TEXTURE(gn_media_viewer_get_paintable(viewer)));
  g_assert_false(gn_media_viewer_get_loading(viewer));
  gtk_window_destroy(GTK_WINDOW(viewer));
}

static void
viewer_allow_loads_once_and_drops_late_results(void)
{
  g_autoptr(FakeSource) source = fake_source_new(GN_MEDIA_POLICY_ALLOW);
  GnMediaViewer *viewer = gn_media_viewer_new(NULL);
  gn_media_viewer_set_gallery(viewer, two_urls, 0);
  g_assert_cmpuint(source->fetches, ==, 0); /* no source yet */
  gn_media_viewer_set_source(viewer, GN_MEDIA_SOURCE(source));
  g_assert_cmpuint(source->fetches, ==, 1);
  g_assert_true(gn_media_viewer_get_loading(viewer));
  /* Navigating cancels the first request and starts the second. */
  g_assert_true(gn_media_viewer_navigate(viewer, 1));
  g_assert_true(g_cancellable_is_cancelled(fake_cancellable(source, 0)));
  g_assert_cmpuint(source->fetches, ==, 2);
  /* The late first answer is dropped: slot 0 stays empty. */
  g_autoptr(GBytes) png = png_bytes(4, 4);
  fake_complete(source, 0, png);
  spin_idle();
  g_assert_null(gn_media_viewer_get_paintable(viewer));
  g_assert_true(gn_media_viewer_get_loading(viewer));
  /* An error for the shown slot says so, without Load. */
  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*cannot load*");
  fake_complete(source, 1, NULL);
  spin_until(viewer_shows_something, viewer);
  g_test_assert_expected_messages();
  g_assert_cmpstr(gn_media_viewer_get_message(viewer), ==, "Remote images unavailable");
  g_assert_false(gn_media_viewer_get_load_offered(viewer));
  /* Back to slot 0: a new request, cancelled when the viewer closes. */
  g_assert_true(gn_media_viewer_navigate(viewer, -1));
  g_assert_cmpuint(source->fetches, ==, 3);
  GCancellable *pending = g_object_ref(fake_cancellable(source, 2));
  gtk_window_destroy(GTK_WINDOW(viewer));
  g_assert_true(g_cancellable_is_cancelled(pending));
  g_object_unref(pending);
}

static void
viewer_blocked_never_fetches(void)
{
  g_autoptr(FakeSource) source = fake_source_new(GN_MEDIA_POLICY_BLOCKED);
  GnMediaViewer *viewer = gn_media_viewer_new(NULL);
  gn_media_viewer_set_source(viewer, GN_MEDIA_SOURCE(source));
  gn_media_viewer_set_gallery(viewer, two_urls, 0);
  g_assert_false(gn_media_viewer_get_load_offered(viewer));
  gn_media_viewer_request_load(viewer);
  g_assert_cmpuint(source->fetches, ==, 0);
  g_assert_nonnull(gn_media_viewer_get_message(viewer));
  gtk_window_destroy(GTK_WINDOW(viewer));
}

/* Undecodable bytes (WebP without the fallback) end in a message. */
static void
viewer_refuses_unbounded_formats(void)
{
  g_autoptr(FakeSource) source = fake_source_new(GN_MEDIA_POLICY_ALLOW);
  GnMediaViewer *viewer = gn_media_viewer_new(NULL);
  gn_media_viewer_set_source(viewer, GN_MEDIA_SOURCE(source));
  const char *one[] = { "https://example.org/x.webp", NULL };
  gn_media_viewer_set_gallery(viewer, one, 0);
  g_autoptr(GBytes) webp = webp_bytes();
  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*cannot show*");
  fake_complete(source, 0, webp);
  spin_until(viewer_shows_something, viewer);
  g_test_assert_expected_messages();
  g_assert_null(gn_media_viewer_get_paintable(viewer));
  g_assert_cmpstr(gn_media_viewer_get_message(viewer), ==, "This image cannot be shown");
  gtk_window_destroy(GTK_WINDOW(viewer));
}

static gboolean
on_save_requested(GnMediaViewer *viewer, guint index, const char *url, GdkPaintable *shown,
                  gpointer data)
{
  (void)viewer;
  guint *calls = data;
  g_assert_cmpuint(index, ==, 0);
  g_assert_cmpstr(url, ==, "https://example.org/a.png");
  g_assert_true(GDK_IS_TEXTURE(shown));
  (*calls)++;
  return TRUE; /* no native dialog in tests */
}

static void
on_link_copied(GnMediaViewer *viewer, const char *url, gpointer data)
{
  (void)viewer;
  g_assert_cmpstr(url, ==, "https://example.org/a.png");
  (*(guint *)data)++;
}

static GdkTexture *
texture_of(int width, int height)
{
  g_autoptr(GBytes) png = png_bytes(width, height);
  return gdk_texture_new_from_bytes(png, NULL);
}

static void
viewer_zoom_save_copy(void)
{
  GnMediaViewer *viewer = gn_media_viewer_new(NULL);
  gtk_window_set_default_size(GTK_WINDOW(viewer), 100, 100);
  gn_media_viewer_set_gallery(viewer, two_urls, 0);
  /* Fit never upscales: a small image shows at 100%. */
  g_autoptr(GdkTexture) small = texture_of(10, 10);
  gn_media_viewer_set_texture(viewer, 0, small);
  g_assert_cmpfloat(gn_media_viewer_get_zoom(viewer), ==, 0);
  g_assert_cmpfloat_with_epsilon(gn_media_viewer_get_shown_zoom(viewer), 1.0, 0.001);
  /* A large one fits at its real scale, and Zoom In steps from there. */
  g_autoptr(GdkTexture) large = texture_of(400, 200);
  gn_media_viewer_set_texture(viewer, 0, large);
  g_assert_cmpfloat_with_epsilon(gn_media_viewer_get_shown_zoom(viewer), 0.25, 0.001);
  GtkWidget *zoom_in = find_button(GTK_WIDGET(viewer), "Zoom In");
  g_assert_nonnull(zoom_in);
  g_signal_emit_by_name(zoom_in, "clicked");
  g_assert_cmpfloat_with_epsilon(gn_media_viewer_get_zoom(viewer), 0.5, 0.001);
  gn_media_viewer_set_zoom(viewer, 50);
  g_assert_cmpfloat(gn_media_viewer_get_zoom(viewer), ==, 10.0);
  gn_media_viewer_set_zoom(viewer, 0.01);
  g_assert_cmpfloat_with_epsilon(gn_media_viewer_get_zoom(viewer), 0.1, 0.001);

  /* Every OSD button has a name (tooltip and accessible label). */
  const char *names[] = { "Close", "Zoom In", "Zoom Out", "Fit", "Previous (Left)",
                          "Next (Right)", "Save Image (Ctrl+S)", "Copy Link (Ctrl+C)" };
  for (guint i = 0; i < G_N_ELEMENTS(names); i++)
    g_assert_nonnull(find_button(GTK_WIDGET(viewer), names[i]));

  /* Save: the host may replace the dialog; nothing without an image. */
  guint saves = 0, copies = 0;
  g_signal_connect(viewer, "save-requested", G_CALLBACK(on_save_requested), &saves);
  g_signal_connect(viewer, "link-copied", G_CALLBACK(on_link_copied), &copies);
  GtkWidget *save = find_button(GTK_WIDGET(viewer), "Save Image (Ctrl+S)");
  g_assert_true(gtk_widget_get_visible(save));
  gn_media_viewer_save(viewer);
  g_assert_cmpuint(saves, ==, 1);
  gn_media_viewer_set_can_save(viewer, FALSE);
  g_assert_false(gtk_widget_get_visible(save));
  gn_media_viewer_save(viewer);
  g_assert_cmpuint(saves, ==, 1);
  gn_media_viewer_set_can_save(viewer, TRUE);

  /* Copy link: web URLs only; the host hears about it. */
  GtkWidget *copy = find_button(GTK_WIDGET(viewer), "Copy Link (Ctrl+C)");
  g_assert_true(gtk_widget_get_visible(copy));
  gn_media_viewer_copy_link(viewer);
  g_assert_cmpuint(copies, ==, 1);
  g_assert_true(gn_media_viewer_navigate(viewer, 1));
  g_assert_false(gtk_widget_get_visible(save)); /* nothing loaded */
  gn_media_viewer_save(viewer);
  g_assert_cmpuint(saves, ==, 1);
  const char *slots[] = { "attachment:abc/0", NULL };
  gn_media_viewer_set_gallery(viewer, slots, 0);
  g_assert_false(gtk_widget_get_visible(copy));
  gn_media_viewer_copy_link(viewer);
  g_assert_cmpuint(copies, ==, 1);
  gtk_window_destroy(GTK_WINDOW(viewer));
}

/* ---- GnVideoPlayer ------------------------------------------------------- */

static void
on_video_load(GnVideoPlayer *player, const char *url, gpointer data)
{
  (void)player;
  g_assert_cmpstr(url, ==, "https://example.org/v.mp4");
  (*(guint *)data)++;
}

static void
player_loads_only_with_consent(void)
{
  GnVideoPlayer *player = g_object_ref_sink(gn_video_player_new());
  /* Built and bound: no stream (no GtkMediaFile) yet. */
  g_assert_null(gn_video_player_get_stream(player));
  guint asked = 0;
  g_signal_connect(player, "load-requested", G_CALLBACK(on_video_load), &asked);
  /* No source: only the signal; the URL never reaches a backend. */
  gn_video_player_set_url(player, "https://example.org/v.mp4");
  g_assert_null(gn_video_player_get_stream(player));
  gn_video_player_request_load(player);
  g_assert_cmpuint(asked, ==, 1);
  g_assert_null(gn_video_player_get_stream(player));

  /* ASK: opened through the source only after the Load action. */
  g_autoptr(FakeSource) source = fake_source_new(GN_MEDIA_POLICY_ASK);
  gn_video_player_set_source(player, GN_MEDIA_SOURCE(source));
  g_assert_cmpuint(source->adopted, ==, 1);
  gn_video_player_set_url(player, NULL);
  gn_video_player_set_url(player, "https://example.org/v.mp4");
  g_assert_cmpuint(source->streams, ==, 0);
  gn_video_player_request_load(player);
  g_assert_cmpuint(source->streams, ==, 1);
  g_assert_nonnull(gn_video_player_get_stream(player));

  /* BLOCKED: never, even when asked. */
  source->policy = GN_MEDIA_POLICY_BLOCKED;
  gn_video_player_set_url(player, NULL);
  gn_video_player_set_url(player, "https://example.org/v.mp4");
  gn_video_player_request_load(player);
  g_assert_cmpuint(source->streams, ==, 1);
  g_assert_null(gn_video_player_get_stream(player));

  /* ALLOW: opened when the URL is set (the Gnostr default). */
  source->policy = GN_MEDIA_POLICY_ALLOW;
  gn_video_player_set_url(player, NULL);
  gn_video_player_set_url(player, "https://example.org/v.mp4");
  g_assert_cmpuint(source->streams, ==, 2);
  g_object_unref(player);
}

static void
player_host_stream_and_properties(void)
{
  GnVideoPlayer *player = g_object_ref_sink(gn_video_player_new());
  g_autoptr(GtkMediaStream) stream = gtk_media_file_new();
  gn_video_player_set_stream(player, stream);
  g_assert_true(gn_video_player_get_stream(player) == stream);
  gn_video_player_set_loop(player, TRUE);
  g_assert_true(gtk_media_stream_get_loop(stream));
  gn_video_player_set_muted(player, TRUE);
  g_assert_true(gtk_media_stream_get_muted(stream));
  gn_video_player_set_volume(player, 3.0);
  g_assert_cmpfloat(gn_video_player_get_volume(player), ==, 1.0);
  gboolean autoplay = FALSE;
  g_object_set(player, "autoplay", TRUE, NULL);
  g_object_get(player, "autoplay", &autoplay, NULL);
  g_assert_true(autoplay);
  /* A host stream is let go, not torn down. */
  gn_video_player_set_stream(player, NULL);
  g_assert_null(gn_video_player_get_stream(player));
  g_assert_false(gtk_media_stream_get_playing(stream));
  g_object_unref(player);
}

static void
viewer_plays_streams_in_player(void)
{
  GnMediaViewer *viewer = gn_media_viewer_new(NULL);
  gn_media_viewer_set_gallery(viewer, two_urls, 0);
  g_autoptr(GtkMediaStream) stream = gtk_media_file_new();
  gn_media_viewer_set_paintable(viewer, 0, GDK_PAINTABLE(stream));
  g_assert_true(gn_media_viewer_get_paintable(viewer) == GDK_PAINTABLE(stream));
  g_assert_false(gtk_widget_get_visible(find_button(GTK_WIDGET(viewer), "Save Image (Ctrl+S)")));
  g_assert_true(gn_media_viewer_navigate(viewer, 1));
  g_assert_false(gtk_media_stream_get_playing(stream));
  gtk_window_destroy(GTK_WINDOW(viewer));
}

int
main(int argc, char **argv)
{
  gtk_test_init(&argc, &argv, NULL);
  nostrc_test_tolerate_gdk_frame_warning();
  g_test_add_func("/media/decode/sniff-probe", decode_sniff_and_probe);
  g_test_add_func("/media/decode/limits", decode_limits);
  g_test_add_func("/media/decode/async", decode_async);
  g_test_add_func("/media/viewer/ask-waits-for-load", viewer_ask_waits_for_load);
  g_test_add_func("/media/viewer/allow-loads-once-drops-late", viewer_allow_loads_once_and_drops_late_results);
  g_test_add_func("/media/viewer/blocked-never-fetches", viewer_blocked_never_fetches);
  g_test_add_func("/media/viewer/refuses-unbounded-formats", viewer_refuses_unbounded_formats);
  g_test_add_func("/media/viewer/zoom-save-copy", viewer_zoom_save_copy);
  g_test_add_func("/media/viewer/streams-in-player", viewer_plays_streams_in_player);
  g_test_add_func("/media/player/consent", player_loads_only_with_consent);
  g_test_add_func("/media/player/host-stream", player_host_stream_and_properties);
  return g_test_run();
}
