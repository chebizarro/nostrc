/*
 * gn-media-viewer.c - Full-size media viewer modal
 *
 * Ported from Gnostr (gnostr-image-viewer.c, nostrc-8xfib.4): zoom and pan
 * (fit never upscales, additive steps from the fitted scale, pinch, wheel,
 * drag, arrow keys), click outside the image to close, a loading spinner,
 * blocked and unavailable states, Save (Ctrl+S) and Copy link (Ctrl+C),
 * gallery navigation. Grafted from the alpha-6 GnMediaViewer: nothing is
 * fetched by the widget (a GnMediaSource or the "load-requested" signal
 * does), results are bound to a generation and cancelled on navigation,
 * close and dispose, any paintable is shown (animated GIF plays, a
 * GtkMediaStream gets the GnVideoPlayer controls), the gallery is capped,
 * and decoding is bounded (gn_media_decode). GTK 4.6 API only.
 */

#include <nostr-gtk-1.0/gn-media-viewer.h>
#include <nostr-gtk-1.0/gn-animated-image.h>
#include <nostr-gtk-1.0/gn-video-player.h>
#include "gn-media-decode-private.h"
#include "gn-portable-i18n-private.h"
#include <math.h>
#include <string.h>

#define MIN_ZOOM 0.1
#define MAX_ZOOM 10.0
#define ZOOM_STEP 0.25
#define FIT_ZOOM 0.0 /* "fit to window" */
#define MAX_GALLERY 256
#define PAN_STEP 50.0

struct _GnMediaViewer {
  GtkWindow parent_instance;

  GtkWidget *overlay;
  GtkWidget *scrolled_window;
  GtkWidget *picture;
  GtkWidget *video;          /* GnVideoPlayer for GtkMediaStream slots */
  GtkWidget *close_button;
  GtkWidget *zoom_box;
  GtkWidget *zoom_label;
  GtkWidget *spinner;
  GtkWidget *blocked_overlay;
  GtkWidget *blocked_label;
  GtkWidget *load_button;
  GtkWidget *save_button;
  GtkWidget *copy_link_button;
  GtkWidget *nav_box;
  GtkWidget *prev_button;
  GtkWidget *next_button;
  GtkWidget *nav_label;

  GPtrArray *urls;           /* gchar* */
  GPtrArray *paintables;     /* GdkPaintable* or NULL */
  guint index;
  guint64 generation;

  double zoom_level;         /* FIT_ZOOM or a fixed scale */
  double actual_zoom;        /* the scale shown, also when fitting */
  gboolean is_dragging;
  double scroll_start_h;
  double scroll_start_v;
  double pinch_base;         /* the scale when a pinch began */

  /* State of the current slot while it has no paintable. */
  gboolean loading;
  char *message;             /* blocked or unavailable text, or NULL */
  gboolean offer_load;

  GnMediaSource *source;
  GCancellable *cancellable; /* this generation: fetch and decode */
  GnMediaDecodeLimits limits;
  gboolean can_save;
};

G_DEFINE_TYPE(GnMediaViewer, gn_media_viewer, GTK_TYPE_WINDOW)

enum { PROP_0, PROP_CAN_SAVE, N_PROPS };
static GParamSpec *props[N_PROPS];
enum { SIG_LOAD_REQUESTED, SIG_SAVE_REQUESTED, SIG_LINK_COPIED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void update(GnMediaViewer *self);
static void apply_zoom(GnMediaViewer *self);
static void begin_fetch(GnMediaViewer *self);

static void paintable_free(gpointer paintable) { if (paintable) g_object_unref(paintable); }

static GdkPaintable *
current(GnMediaViewer *self)
{
  return self->index < self->paintables->len ? g_ptr_array_index(self->paintables, self->index)
                                             : NULL;
}

static const char *
current_url(GnMediaViewer *self)
{
  return self->index < self->urls->len ? g_ptr_array_index(self->urls, self->index) : NULL;
}

/* Only a web link is worth copying; a host slot name is not. */
static gboolean
is_web_url(const char *url)
{
  return url && (g_ascii_strncasecmp(url, "https://", 8) == 0 ||
                 g_ascii_strncasecmp(url, "http://", 7) == 0);
}

/* The still image to save, borrowed, or NULL (video, nothing loaded). */
static GdkTexture *
current_texture(GnMediaViewer *self)
{
  GdkPaintable *paintable = current(self);
  if (GDK_IS_TEXTURE(paintable)) return GDK_TEXTURE(paintable);
  if (GN_IS_ANIMATED_IMAGE(paintable))
    return gn_animated_image_get_current_texture(GN_ANIMATED_IMAGE(paintable));
  return NULL;
}

static void
cancel_pending(GnMediaViewer *self)
{
  if (self->cancellable) g_cancellable_cancel(self->cancellable);
  g_clear_object(&self->cancellable);
}

/* A new item is shown: earlier results are stale, requests are cancelled. */
static void
reset_slot_state(GnMediaViewer *self)
{
  self->generation++;
  cancel_pending(self);
  self->loading = FALSE;
  g_clear_pointer(&self->message, g_free);
  self->offer_load = FALSE;
  self->zoom_level = FIT_ZOOM;
}

static void
update_zoom_display(GnMediaViewer *self)
{
  double shown = self->zoom_level == FIT_ZOOM ? self->actual_zoom : self->zoom_level;
  g_autofree char *text = g_strdup_printf("%.0f%%", shown * 100);
  gtk_label_set_text(GTK_LABEL(self->zoom_label), text);
}

/* Fit: the picture gets the window and never more than its own size, so it
 * is never upscaled; the label shows the real scale. Fixed: the picture is
 * sized to the scale and the scrolled window pans it. (Gnostr used
 * gtk_picture_set_content_fit, GTK 4.8; size requests and the default
 * keep-aspect-ratio do the same on 4.6.) */
static void
apply_zoom(GnMediaViewer *self)
{
  GdkPaintable *paintable = current(self);
  if (!paintable || GTK_IS_MEDIA_STREAM(paintable)) {
    gtk_widget_set_size_request(self->picture, -1, -1);
    gtk_picture_set_can_shrink(GTK_PICTURE(self->picture), TRUE);
    return;
  }
  int img_width = gdk_paintable_get_intrinsic_width(paintable);
  int img_height = gdk_paintable_get_intrinsic_height(paintable);
  if (self->zoom_level == FIT_ZOOM) {
    gtk_picture_set_can_shrink(GTK_PICTURE(self->picture), TRUE);
    gtk_widget_set_size_request(self->picture, -1, -1);
    int win_width = gtk_widget_get_width(GTK_WIDGET(self));
    int win_height = gtk_widget_get_height(GTK_WIDGET(self));
    if (win_width <= 0 || win_height <= 0) {
      gtk_window_get_default_size(GTK_WINDOW(self), &win_width, &win_height);
    }
    self->actual_zoom = 1.0;
    if (win_width > 0 && win_height > 0 && img_width > 0 && img_height > 0) {
      double scale = fmin((double)win_width / img_width, (double)win_height / img_height);
      self->actual_zoom = fmin(scale, 1.0); /* Do not upscale */
    }
  } else {
    gtk_picture_set_can_shrink(GTK_PICTURE(self->picture), FALSE);
    gtk_widget_set_size_request(self->picture, (int)(img_width * self->zoom_level),
                                (int)(img_height * self->zoom_level));
    self->actual_zoom = self->zoom_level;
  }
  update_zoom_display(self);
}

static void
zoom_to(GnMediaViewer *self, double zoom)
{
  self->zoom_level = zoom == FIT_ZOOM ? FIT_ZOOM : CLAMP(zoom, MIN_ZOOM, MAX_ZOOM);
  apply_zoom(self);
}

static double
shown_zoom(GnMediaViewer *self)
{
  return self->zoom_level == FIT_ZOOM ? self->actual_zoom : self->zoom_level;
}

/* Additive steps from what is shown, so leaving fit never jumps. */
static void zoom_in(GnMediaViewer *self) { zoom_to(self, fmin(shown_zoom(self) + ZOOM_STEP, MAX_ZOOM)); }
static void zoom_out(GnMediaViewer *self) { zoom_to(self, fmax(shown_zoom(self) - ZOOM_STEP, MIN_ZOOM)); }

/* Gnostr: arrow keys pan only when magnified beyond 100%. */
static gboolean
is_zoomed(GnMediaViewer *self)
{
  return self->zoom_level != FIT_ZOOM && self->zoom_level > 1.0;
}

static void
update(GnMediaViewer *self)
{
  guint count = self->urls->len;
  GdkPaintable *paintable = count ? current(self) : NULL;
  gboolean stream = GTK_IS_MEDIA_STREAM(paintable);

  gtk_picture_set_paintable(GTK_PICTURE(self->picture), stream ? NULL : paintable);
  gn_animated_image_attach(self->picture, stream ? NULL : paintable);
  gn_video_player_set_stream(GN_VIDEO_PLAYER(self->video),
                             stream ? GTK_MEDIA_STREAM(paintable) : NULL);
  gtk_widget_set_visible(self->video, stream);
  gtk_widget_set_visible(self->scrolled_window, !stream);
  if (stream && gtk_widget_get_mapped(GTK_WIDGET(self)))
    gtk_media_stream_play(GTK_MEDIA_STREAM(paintable));

  gboolean empty = count && !paintable;
  gtk_widget_set_visible(self->spinner, empty && self->loading);
  if (empty && self->loading) gtk_spinner_start(GTK_SPINNER(self->spinner));
  else gtk_spinner_stop(GTK_SPINNER(self->spinner));
  gboolean overlay = empty && !self->loading && (self->message || self->offer_load);
  gtk_label_set_text(GTK_LABEL(self->blocked_label), self->message ? self->message : "");
  gtk_widget_set_visible(self->blocked_label, self->message != NULL);
  gtk_widget_set_visible(self->load_button, self->offer_load);
  gtk_widget_set_visible(self->blocked_overlay, overlay);

  gtk_widget_set_visible(self->save_button, self->can_save && current_texture(self) != NULL);
  gtk_widget_set_visible(self->copy_link_button, is_web_url(current_url(self)));

  g_autofree char *position =
    g_strdup_printf(C_("image position", "%u / %u"), count ? self->index + 1 : 0, count);
  gtk_label_set_text(GTK_LABEL(self->nav_label), position);
  gtk_widget_set_visible(self->nav_box, count > 1);
  gtk_widget_set_sensitive(self->prev_button, self->index > 0);
  gtk_widget_set_sensitive(self->next_button, self->index + 1 < count);
  gtk_widget_set_visible(self->zoom_box, paintable && !stream);
  apply_zoom(self);
}

/* What the current slot needs when it has nothing to show. */
static void
evaluate_slot(GnMediaViewer *self)
{
  const char *url = current_url(self);
  if (!url || current(self)) return;
  if (!self->source) {
    self->offer_load = TRUE; /* The host answers "load-requested". */
    return;
  }
  switch (gn_media_source_get_policy(self->source, url, GN_MEDIA_KIND_IMAGE)) {
  case GN_MEDIA_POLICY_ALLOW:
    begin_fetch(self);
    break;
  case GN_MEDIA_POLICY_ASK:
    self->message = g_strdup(_("Remote media is blocked"));
    self->offer_load = TRUE;
    break;
  default:
    self->message = g_strdup(_("Remote media is blocked"));
    break;
  }
}

/* ---- loading through the host source ----------------------------------- */

typedef struct {
  GWeakRef viewer;
  guint64 generation;
  guint index;
  char *url;
  GCancellable *cancellable;
} FetchCtx;

static void
fetch_ctx_free(FetchCtx *ctx)
{
  g_weak_ref_clear(&ctx->viewer);
  g_clear_object(&ctx->cancellable);
  g_free(ctx->url);
  g_free(ctx);
}

/* The viewer when ctx is still what it shows, else NULL (closed, moved on). */
static GnMediaViewer *
fetch_ctx_viewer(FetchCtx *ctx)
{
  if (g_cancellable_is_cancelled(ctx->cancellable)) return NULL;
  GnMediaViewer *self = g_weak_ref_get(&ctx->viewer);
  if (self && self->generation != ctx->generation) g_clear_object(&self);
  return self;
}

static void
on_decoded(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  FetchCtx *ctx = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GdkPaintable) paintable = gn_media_decode_finish(result, &error);
  g_autoptr(GnMediaViewer) self = fetch_ctx_viewer(ctx);
  if (self) {
    if (paintable)
      gn_media_viewer_set_paintable_for_generation(self, ctx->generation, ctx->index, paintable);
    else {
      g_warning("GnMediaViewer: cannot show %s: %s", ctx->url, error ? error->message : "?");
      gn_media_viewer_set_error(self, ctx->generation, ctx->index,
                                _("This image cannot be shown"));
    }
  }
  fetch_ctx_free(ctx);
}

static void
on_fetched(GObject *source, GAsyncResult *result, gpointer data)
{
  FetchCtx *ctx = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) bytes = gn_media_source_fetch_finish(GN_MEDIA_SOURCE(source), result, &error);
  g_autoptr(GnMediaViewer) self = fetch_ctx_viewer(ctx);
  if (!self) {
    fetch_ctx_free(ctx);
    return;
  }
  if (!bytes || g_bytes_get_size(bytes) == 0) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_warning("GnMediaViewer: cannot load %s: %s", ctx->url,
                error ? error->message : "empty response");
    gn_media_viewer_set_error(self, ctx->generation, ctx->index, _("Remote images unavailable"));
    fetch_ctx_free(ctx);
    return;
  }
  /* Decoded off the main thread, within the limits. */
  gn_media_decode_async(bytes, &self->limits, ctx->cancellable, on_decoded, ctx);
}

static void
begin_fetch(GnMediaViewer *self)
{
  const char *url = current_url(self);
  if (!self->source || !url || current(self)) return;
  cancel_pending(self);
  self->cancellable = g_cancellable_new();
  g_clear_pointer(&self->message, g_free);
  self->offer_load = FALSE;
  self->loading = TRUE;
  FetchCtx *ctx = g_new0(FetchCtx, 1);
  g_weak_ref_init(&ctx->viewer, self);
  ctx->generation = self->generation;
  ctx->index = self->index;
  ctx->url = g_strdup(url);
  ctx->cancellable = g_object_ref(self->cancellable);
  gn_media_source_fetch_async(self->source, url, GN_MEDIA_KIND_IMAGE, self->limits.max_bytes,
                              self->cancellable, on_fetched, ctx);
}

/* ---- save and copy ------------------------------------------------------ */

/* The last path element without a query, named .png: the saved file is
 * always re-encoded as PNG, which also drops embedded metadata. */
static char *
suggested_name(const char *url)
{
  g_autofree char *base = NULL;
  const char *slash = url && is_web_url(url) ? strrchr(url, '/') : NULL;
  if (slash && slash[1]) base = g_strndup(slash + 1, strcspn(slash + 1, "?#"));
  if (!base || !*base) {
    g_free(base);
    base = g_strdup("image");
  }
  char *dot = strrchr(base, '.');
  if (dot && dot != base) *dot = '\0';
  return g_strconcat(base, ".png", NULL);
}

typedef struct {
  GtkFileChooserNative *dialog;
  GdkTexture *texture;
} SaveCtx;

static void
on_save_response(GtkNativeDialog *dialog, int response, gpointer data)
{
  SaveCtx *ctx = data;
  if (response == GTK_RESPONSE_ACCEPT) {
    g_autoptr(GFile) file = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(dialog));
    g_autofree char *path = file ? g_file_get_path(file) : NULL;
    if (path && !gdk_texture_save_to_png(ctx->texture, path))
      g_warning("GnMediaViewer: failed to save image to %s", path);
  }
  gtk_native_dialog_destroy(dialog);
  g_object_unref(ctx->dialog);
  g_object_unref(ctx->texture);
  g_free(ctx);
}

/* Default "save-requested": a portal-capable native chooser (GTK 4.0; the
 * GtkFileDialog Gnostr used needs 4.10), then a PNG of the shown frame. */
static gboolean
default_save(GnMediaViewer *self, guint index, const char *url, GdkPaintable *paintable)
{
  (void)index;
  GdkTexture *texture = GDK_IS_TEXTURE(paintable) ? GDK_TEXTURE(paintable)
    : GN_IS_ANIMATED_IMAGE(paintable)
      ? gn_animated_image_get_current_texture(GN_ANIMATED_IMAGE(paintable)) : NULL;
  if (!texture) return TRUE; /* Nothing to save: no dialog at all. */
  SaveCtx *ctx = g_new0(SaveCtx, 1);
  ctx->texture = g_object_ref(texture);
  ctx->dialog = gtk_file_chooser_native_new(_("Save Image"), GTK_WINDOW(self),
                                            GTK_FILE_CHOOSER_ACTION_SAVE, _("_Save"),
                                            _("_Cancel"));
  gtk_native_dialog_set_modal(GTK_NATIVE_DIALOG(ctx->dialog), TRUE);
  g_autofree char *name = suggested_name(url);
  gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(ctx->dialog), name);
  g_signal_connect(ctx->dialog, "response", G_CALLBACK(on_save_response), ctx);
  gtk_native_dialog_show(GTK_NATIVE_DIALOG(ctx->dialog));
  return TRUE;
}

void
gn_media_viewer_save(GnMediaViewer *self)
{
  g_return_if_fail(GN_IS_MEDIA_VIEWER(self));
  if (!self->can_save || !current_texture(self)) return;
  gboolean handled = FALSE;
  g_signal_emit(self, signals[SIG_SAVE_REQUESTED], 0, self->index, current_url(self),
                current(self), &handled);
}

void
gn_media_viewer_copy_link(GnMediaViewer *self)
{
  g_return_if_fail(GN_IS_MEDIA_VIEWER(self));
  const char *url = current_url(self);
  if (!is_web_url(url)) return;
  gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(self)), url);
  g_signal_emit(self, signals[SIG_LINK_COPIED], 0, url);
}

/* ---- input -------------------------------------------------------------- */

static void
pan(GnMediaViewer *self, GtkOrientation orientation, double delta)
{
  GtkScrolledWindow *sw = GTK_SCROLLED_WINDOW(self->scrolled_window);
  GtkAdjustment *adj = orientation == GTK_ORIENTATION_HORIZONTAL
    ? gtk_scrolled_window_get_hadjustment(sw) : gtk_scrolled_window_get_vadjustment(sw);
  gtk_adjustment_set_value(adj, gtk_adjustment_get_value(adj) + delta);
}

static gboolean
on_key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode,
               GdkModifierType state, gpointer user_data)
{
  (void)controller; (void)keycode;
  GnMediaViewer *self = GN_MEDIA_VIEWER(user_data);
  GdkPaintable *paintable = current(self);
  switch (keyval) {
  case GDK_KEY_Escape:
    gtk_window_close(GTK_WINDOW(self));
    return TRUE;
  case GDK_KEY_plus:
  case GDK_KEY_equal:
  case GDK_KEY_KP_Add:
    zoom_in(self);
    return TRUE;
  case GDK_KEY_minus:
  case GDK_KEY_KP_Subtract:
    zoom_out(self);
    return TRUE;
  case GDK_KEY_0:
  case GDK_KEY_KP_0:
  case GDK_KEY_f:
    zoom_to(self, FIT_ZOOM);
    return TRUE;
  case GDK_KEY_1:
  case GDK_KEY_KP_1:
    zoom_to(self, 1.0);
    return TRUE;
  case GDK_KEY_s:
    if (state & GDK_CONTROL_MASK) {
      gn_media_viewer_save(self);
      return TRUE;
    }
    break;
  case GDK_KEY_c:
    if (state & GDK_CONTROL_MASK) {
      gn_media_viewer_copy_link(self);
      return TRUE;
    }
    break;
  case GDK_KEY_space:
    if (GTK_IS_MEDIA_STREAM(paintable)) {
      gn_video_player_toggle_playback(GN_VIDEO_PLAYER(self->video));
      return TRUE;
    }
    break;
  case GDK_KEY_Left:
  case GDK_KEY_Right:
    /* Pan when zoomed in, else move through the gallery. */
    if (is_zoomed(self)) {
      pan(self, GTK_ORIENTATION_HORIZONTAL, keyval == GDK_KEY_Left ? -PAN_STEP : PAN_STEP);
      return TRUE;
    }
    return gn_media_viewer_navigate(self, keyval == GDK_KEY_Left ? -1 : 1);
  case GDK_KEY_Up:
  case GDK_KEY_Down:
    if (is_zoomed(self)) {
      pan(self, GTK_ORIENTATION_VERTICAL, keyval == GDK_KEY_Up ? -PAN_STEP : PAN_STEP);
      return TRUE;
    }
    break;
  default:
    break;
  }
  return FALSE;
}

static gboolean
on_scroll(GtkEventControllerScroll *controller, double dx, double dy, gpointer user_data)
{
  (void)controller; (void)dx;
  GnMediaViewer *self = GN_MEDIA_VIEWER(user_data);
  if (dy < 0) zoom_in(self);
  else if (dy > 0) zoom_out(self);
  return TRUE;
}

static void
on_drag_begin(GtkGestureDrag *gesture, double x, double y, gpointer user_data)
{
  (void)gesture; (void)x; (void)y;
  GnMediaViewer *self = GN_MEDIA_VIEWER(user_data);
  if (self->zoom_level == FIT_ZOOM) return;
  self->is_dragging = TRUE;
  GtkScrolledWindow *sw = GTK_SCROLLED_WINDOW(self->scrolled_window);
  self->scroll_start_h = gtk_adjustment_get_value(gtk_scrolled_window_get_hadjustment(sw));
  self->scroll_start_v = gtk_adjustment_get_value(gtk_scrolled_window_get_vadjustment(sw));
}

static void
on_drag_update(GtkGestureDrag *gesture, double dx, double dy, gpointer user_data)
{
  (void)gesture;
  GnMediaViewer *self = GN_MEDIA_VIEWER(user_data);
  if (!self->is_dragging) return;
  GtkScrolledWindow *sw = GTK_SCROLLED_WINDOW(self->scrolled_window);
  gtk_adjustment_set_value(gtk_scrolled_window_get_hadjustment(sw), self->scroll_start_h - dx);
  gtk_adjustment_set_value(gtk_scrolled_window_get_vadjustment(sw), self->scroll_start_v - dy);
}

static void
on_drag_end(GtkGestureDrag *gesture, double dx, double dy, gpointer user_data)
{
  (void)gesture; (void)dx; (void)dy;
  GN_MEDIA_VIEWER(user_data)->is_dragging = FALSE;
}

static void
on_double_click(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data)
{
  (void)gesture; (void)x; (void)y;
  GnMediaViewer *self = GN_MEDIA_VIEWER(user_data);
  if (n_press == 2) zoom_to(self, self->zoom_level == FIT_ZOOM ? 1.0 : FIT_ZOOM);
}

/* A click on the dark background, outside the image, closes. */
static void
on_background_clicked(GtkGestureClick *gesture, int n_press, double x, double y,
                      gpointer user_data)
{
  (void)n_press;
  GnMediaViewer *self = GN_MEDIA_VIEWER(user_data);
  GtkWidget *widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
  graphene_rect_t bounds;
  if (!current(self) || !gtk_widget_compute_bounds(self->picture, widget, &bounds)) return;
  if (x < bounds.origin.x || x > bounds.origin.x + bounds.size.width ||
      y < bounds.origin.y || y > bounds.origin.y + bounds.size.height)
    gtk_window_close(GTK_WINDOW(self));
}

static void
on_zoom_begin(GtkGesture *gesture, GdkEventSequence *sequence, gpointer user_data)
{
  (void)gesture; (void)sequence;
  GnMediaViewer *self = GN_MEDIA_VIEWER(user_data);
  self->pinch_base = shown_zoom(self);
}

/* scale is relative to the start of the pinch, so apply it to that scale
 * (Gnostr compounded it on every event). */
static void
on_zoom_scale_changed(GtkGestureZoom *gesture, double scale, gpointer user_data)
{
  (void)gesture;
  GnMediaViewer *self = GN_MEDIA_VIEWER(user_data);
  double base = self->pinch_base > 0 ? self->pinch_base : shown_zoom(self);
  zoom_to(self, fmax(MIN_ZOOM, fmin(MAX_ZOOM, base * scale)));
}

static void on_close_clicked(GtkButton *b, gpointer d) { (void)b; gtk_window_close(GTK_WINDOW(d)); }
static void on_prev_clicked(GtkButton *b, gpointer d) { (void)b; gn_media_viewer_navigate(d, -1); }
static void on_next_clicked(GtkButton *b, gpointer d) { (void)b; gn_media_viewer_navigate(d, 1); }
static void on_save_clicked(GtkButton *b, gpointer d) { (void)b; gn_media_viewer_save(d); }
static void on_copy_clicked(GtkButton *b, gpointer d) { (void)b; gn_media_viewer_copy_link(d); }
static void on_load_clicked(GtkButton *b, gpointer d) { (void)b; gn_media_viewer_request_load(d); }
static void on_fit_clicked(GtkButton *b, gpointer d) { (void)b; zoom_to(d, FIT_ZOOM); }
static void on_zoom_in_clicked(GtkButton *b, gpointer d) { (void)b; zoom_in(d); }
static void on_zoom_out_clicked(GtkButton *b, gpointer d) { (void)b; zoom_out(d); }

/* ---- lifecycle ---------------------------------------------------------- */

static void
viewer_map(GtkWidget *widget)
{
  GTK_WIDGET_CLASS(gn_media_viewer_parent_class)->map(widget);
  GnMediaViewer *self = GN_MEDIA_VIEWER(widget);
  GdkPaintable *paintable = current(self);
  if (GTK_IS_MEDIA_STREAM(paintable)) gtk_media_stream_play(GTK_MEDIA_STREAM(paintable));
  apply_zoom(self);
}

static void
viewer_unmap(GtkWidget *widget)
{
  GnMediaViewer *self = GN_MEDIA_VIEWER(widget);
  GdkPaintable *paintable = current(self);
  if (GTK_IS_MEDIA_STREAM(paintable)) gtk_media_stream_pause(GTK_MEDIA_STREAM(paintable));
  GTK_WIDGET_CLASS(gn_media_viewer_parent_class)->unmap(widget);
}

static void
viewer_size_allocate(GtkWidget *widget, int width, int height, int baseline)
{
  GTK_WIDGET_CLASS(gn_media_viewer_parent_class)->size_allocate(widget, width, height, baseline);
  GnMediaViewer *self = GN_MEDIA_VIEWER(widget);
  if (self->zoom_level == FIT_ZOOM && current(self)) {
    double before = self->actual_zoom;
    apply_zoom(self);
    if (before != self->actual_zoom) update_zoom_display(self);
  }
}

static void
gn_media_viewer_dispose(GObject *object)
{
  GnMediaViewer *self = GN_MEDIA_VIEWER(object);
  self->generation++;
  cancel_pending(self);
  if (self->video) gn_video_player_set_stream(GN_VIDEO_PLAYER(self->video), NULL);
  if (self->picture) gn_animated_image_attach(self->picture, NULL);
  g_clear_object(&self->source);
  G_OBJECT_CLASS(gn_media_viewer_parent_class)->dispose(object);
}

static void
gn_media_viewer_finalize(GObject *object)
{
  GnMediaViewer *self = GN_MEDIA_VIEWER(object);
  g_ptr_array_unref(self->urls);
  g_ptr_array_unref(self->paintables);
  g_free(self->message);
  G_OBJECT_CLASS(gn_media_viewer_parent_class)->finalize(object);
}

static void
gn_media_viewer_get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
  GnMediaViewer *self = GN_MEDIA_VIEWER(object);
  if (id == PROP_CAN_SAVE) g_value_set_boolean(value, self->can_save);
  else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}

static void
gn_media_viewer_set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
  GnMediaViewer *self = GN_MEDIA_VIEWER(object);
  if (id == PROP_CAN_SAVE) gn_media_viewer_set_can_save(self, g_value_get_boolean(value));
  else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}

static void
gn_media_viewer_class_init(GnMediaViewerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  object_class->dispose = gn_media_viewer_dispose;
  object_class->finalize = gn_media_viewer_finalize;
  object_class->get_property = gn_media_viewer_get_property;
  object_class->set_property = gn_media_viewer_set_property;
  widget_class->map = viewer_map;
  widget_class->unmap = viewer_unmap;
  widget_class->size_allocate = viewer_size_allocate;
  gn_portable_gettext_domain();

  props[PROP_CAN_SAVE] = g_param_spec_boolean("can-save", NULL, NULL, TRUE,
    G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties(object_class, N_PROPS, props);

  signals[SIG_LOAD_REQUESTED] = g_signal_new("load-requested", G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_UINT, G_TYPE_STRING);
  /* A handler returning TRUE replaces the default native Save dialog. */
  signals[SIG_SAVE_REQUESTED] = g_signal_new_class_handler("save-requested",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, G_CALLBACK(default_save),
    g_signal_accumulator_true_handled, NULL, NULL, G_TYPE_BOOLEAN, 3,
    G_TYPE_UINT, G_TYPE_STRING, GDK_TYPE_PAINTABLE);
  signals[SIG_LINK_COPIED] = g_signal_new("link-copied", G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
}

/* An icon-only OSD button; its name is for tooltips and screen readers. */
static GtkWidget *
osd_button(const char *icon, const char *name, const char *css_class)
{
  GtkWidget *button = gtk_button_new_from_icon_name(icon);
  gtk_widget_add_css_class(button, css_class);
  gtk_widget_add_css_class(button, "circular");
  gtk_widget_set_tooltip_text(button, name);
  gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
  return button;
}

static void
place(GtkWidget *widget, GtkAlign halign, GtkAlign valign)
{
  gtk_widget_set_halign(widget, halign);
  gtk_widget_set_valign(widget, valign);
  gtk_widget_set_margin_start(widget, 16);
  gtk_widget_set_margin_end(widget, 16);
  gtk_widget_set_margin_top(widget, 16);
  gtk_widget_set_margin_bottom(widget, 16);
}

static void
gn_media_viewer_init(GnMediaViewer *self)
{
  gn_media_install_css();
  self->urls = g_ptr_array_new_with_free_func(g_free);
  self->paintables = g_ptr_array_new_with_free_func(paintable_free);
  self->generation = 1;
  self->zoom_level = FIT_ZOOM;
  self->actual_zoom = 1.0;
  self->can_save = TRUE;
  gn_media_decode_limits_init_default(&self->limits);

  /* A modal panel over its parent: no decorations, not resizable. */
  gtk_window_set_decorated(GTK_WINDOW(self), FALSE);
  gtk_window_set_modal(GTK_WINDOW(self), TRUE);
  gtk_window_set_resizable(GTK_WINDOW(self), FALSE);
  gtk_window_set_title(GTK_WINDOW(self), _("Media Viewer"));
  gtk_window_set_default_size(GTK_WINDOW(self), 900, 700);
  gtk_widget_add_css_class(GTK_WIDGET(self), "gn-media-viewer");

  self->overlay = gtk_overlay_new();
  gtk_window_set_child(GTK_WINDOW(self), self->overlay);

  self->scrolled_window = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(self->scrolled_window),
                                 GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_hexpand(self->scrolled_window, TRUE);
  gtk_widget_set_vexpand(self->scrolled_window, TRUE);
  gtk_overlay_set_child(GTK_OVERLAY(self->overlay), self->scrolled_window);

  self->picture = gtk_picture_new();
  gtk_picture_set_can_shrink(GTK_PICTURE(self->picture), TRUE);
  gtk_widget_set_halign(self->picture, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(self->picture, GTK_ALIGN_CENTER);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(self->scrolled_window), self->picture);

  /* One video UI: GtkMediaStream slots play in a GnVideoPlayer. */
  self->video = GTK_WIDGET(gn_video_player_new());
  gn_video_player_set_autoplay(GN_VIDEO_PLAYER(self->video), TRUE);
  gtk_widget_set_hexpand(self->video, TRUE);
  gtk_widget_set_vexpand(self->video, TRUE);
  gtk_widget_set_visible(self->video, FALSE);
  gtk_overlay_add_overlay(GTK_OVERLAY(self->overlay), self->video);

  self->close_button = osd_button("window-close-symbolic", _("Close"), "gn-media-close");
  place(self->close_button, GTK_ALIGN_END, GTK_ALIGN_START);
  gtk_overlay_add_overlay(GTK_OVERLAY(self->overlay), self->close_button);
  g_signal_connect(self->close_button, "clicked", G_CALLBACK(on_close_clicked), self);

  GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  place(toolbar, GTK_ALIGN_START, GTK_ALIGN_START);
  gtk_overlay_add_overlay(GTK_OVERLAY(self->overlay), toolbar);
  self->save_button = osd_button("document-save-symbolic", _("Save Image (Ctrl+S)"), "gn-media-tool");
  g_signal_connect(self->save_button, "clicked", G_CALLBACK(on_save_clicked), self);
  gtk_box_append(GTK_BOX(toolbar), self->save_button);
  self->copy_link_button = osd_button("edit-copy-symbolic", _("Copy Link (Ctrl+C)"), "gn-media-tool");
  g_signal_connect(self->copy_link_button, "clicked", G_CALLBACK(on_copy_clicked), self);
  gtk_box_append(GTK_BOX(toolbar), self->copy_link_button);

  self->spinner = gtk_spinner_new();
  gtk_widget_set_halign(self->spinner, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(self->spinner, GTK_ALIGN_CENTER);
  gtk_widget_set_size_request(self->spinner, 48, 48);
  gtk_widget_set_visible(self->spinner, FALSE);
  gtk_overlay_add_overlay(GTK_OVERLAY(self->overlay), self->spinner);

  self->blocked_overlay = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_halign(self->blocked_overlay, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(self->blocked_overlay, GTK_ALIGN_CENTER);
  gtk_widget_set_visible(self->blocked_overlay, FALSE);
  gtk_overlay_add_overlay(GTK_OVERLAY(self->overlay), self->blocked_overlay);
  self->blocked_label = gtk_label_new(NULL);
  gtk_widget_add_css_class(self->blocked_label, "gn-media-message");
  gtk_label_set_wrap(GTK_LABEL(self->blocked_label), TRUE);
  gtk_box_append(GTK_BOX(self->blocked_overlay), self->blocked_label);
  self->load_button = gtk_button_new_with_label(_("Load image"));
  gtk_widget_add_css_class(self->load_button, "pill");
  gtk_widget_add_css_class(self->load_button, "suggested-action");
  g_signal_connect(self->load_button, "clicked", G_CALLBACK(on_load_clicked), self);
  gtk_box_append(GTK_BOX(self->blocked_overlay), self->load_button);

  self->nav_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_add_css_class(self->nav_box, "gn-media-osd");
  place(self->nav_box, GTK_ALIGN_CENTER, GTK_ALIGN_END);
  gtk_widget_set_visible(self->nav_box, FALSE);
  gtk_overlay_add_overlay(GTK_OVERLAY(self->overlay), self->nav_box);
  self->prev_button = osd_button("go-previous-symbolic", _("Previous (Left)"), "gn-media-button");
  g_signal_connect(self->prev_button, "clicked", G_CALLBACK(on_prev_clicked), self);
  gtk_box_append(GTK_BOX(self->nav_box), self->prev_button);
  self->nav_label = gtk_label_new(NULL);
  gtk_box_append(GTK_BOX(self->nav_box), self->nav_label);
  self->next_button = osd_button("go-next-symbolic", _("Next (Right)"), "gn-media-button");
  g_signal_connect(self->next_button, "clicked", G_CALLBACK(on_next_clicked), self);
  gtk_box_append(GTK_BOX(self->nav_box), self->next_button);

  self->zoom_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class(self->zoom_box, "gn-media-osd");
  place(self->zoom_box, GTK_ALIGN_END, GTK_ALIGN_END);
  gtk_overlay_add_overlay(GTK_OVERLAY(self->overlay), self->zoom_box);
  GtkWidget *minus = osd_button("zoom-out-symbolic", _("Zoom Out"), "gn-media-button");
  GtkWidget *plus = osd_button("zoom-in-symbolic", _("Zoom In"), "gn-media-button");
  GtkWidget *fit = osd_button("zoom-fit-best-symbolic", _("Fit"), "gn-media-button");
  self->zoom_label = gtk_label_new("100%");
  gtk_label_set_width_chars(GTK_LABEL(self->zoom_label), 5);
  gtk_box_append(GTK_BOX(self->zoom_box), minus);
  gtk_box_append(GTK_BOX(self->zoom_box), self->zoom_label);
  gtk_box_append(GTK_BOX(self->zoom_box), plus);
  gtk_box_append(GTK_BOX(self->zoom_box), fit);
  g_signal_connect(minus, "clicked", G_CALLBACK(on_zoom_out_clicked), self);
  g_signal_connect(plus, "clicked", G_CALLBACK(on_zoom_in_clicked), self);
  g_signal_connect(fit, "clicked", G_CALLBACK(on_fit_clicked), self);

  GtkEventController *key = gtk_event_controller_key_new();
  g_signal_connect(key, "key-pressed", G_CALLBACK(on_key_pressed), self);
  gtk_widget_add_controller(GTK_WIDGET(self), key);

  GtkEventController *scroll = gtk_event_controller_scroll_new(
    GTK_EVENT_CONTROLLER_SCROLL_VERTICAL | GTK_EVENT_CONTROLLER_SCROLL_DISCRETE);
  g_signal_connect(scroll, "scroll", G_CALLBACK(on_scroll), self);
  gtk_widget_add_controller(self->picture, scroll);

  GtkGesture *drag = gtk_gesture_drag_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
  g_signal_connect(drag, "drag-begin", G_CALLBACK(on_drag_begin), self);
  g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), self);
  g_signal_connect(drag, "drag-end", G_CALLBACK(on_drag_end), self);
  gtk_widget_add_controller(self->scrolled_window, GTK_EVENT_CONTROLLER(drag));

  GtkGesture *click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
  g_signal_connect(click, "pressed", G_CALLBACK(on_double_click), self);
  gtk_widget_add_controller(self->picture, GTK_EVENT_CONTROLLER(click));

  GtkGesture *background = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(background), GDK_BUTTON_PRIMARY);
  g_signal_connect(background, "pressed", G_CALLBACK(on_background_clicked), self);
  gtk_widget_add_controller(self->scrolled_window, GTK_EVENT_CONTROLLER(background));

  GtkGesture *pinch = gtk_gesture_zoom_new();
  g_signal_connect(pinch, "begin", G_CALLBACK(on_zoom_begin), self);
  g_signal_connect(pinch, "scale-changed", G_CALLBACK(on_zoom_scale_changed), self);
  gtk_widget_add_controller(self->picture, GTK_EVENT_CONTROLLER(pinch));

  update(self);
}

/* ---- public API --------------------------------------------------------- */

GnMediaViewer *
gn_media_viewer_new(GtkWindow *parent)
{
  GnMediaViewer *self = g_object_new(GN_TYPE_MEDIA_VIEWER, NULL);
  if (parent) {
    gtk_window_set_transient_for(GTK_WINDOW(self), parent);
    int width = gtk_widget_get_width(GTK_WIDGET(parent));
    int height = gtk_widget_get_height(GTK_WIDGET(parent));
    if (width > 0 && height > 0)
      gtk_window_set_default_size(GTK_WINDOW(self), MAX(400, width), MAX(300, height));
  }
  return self;
}

void
gn_media_viewer_present(GnMediaViewer *self)
{
  g_return_if_fail(GN_IS_MEDIA_VIEWER(self));
  /* Sized to the parent now that it is allocated (nostrc-zqb: never beyond
   * the parent window). */
  GtkWindow *parent = gtk_window_get_transient_for(GTK_WINDOW(self));
  int width = parent ? gtk_widget_get_width(GTK_WIDGET(parent)) : 0;
  int height = parent ? gtk_widget_get_height(GTK_WIDGET(parent)) : 0;
  if (width > 0 && height > 0)
    gtk_window_set_default_size(GTK_WINDOW(self), MAX(400, width), MAX(300, height));
  gtk_window_present(GTK_WINDOW(self));
  update(self);
}

void
gn_media_viewer_set_source(GnMediaViewer *self, GnMediaSource *source)
{
  g_return_if_fail(GN_IS_MEDIA_VIEWER(self));
  g_return_if_fail(!source || GN_IS_MEDIA_SOURCE(source));
  if (!g_set_object(&self->source, source)) return;
  if (source) gn_media_source_adopt(source, GTK_WIDGET(self));
  if (!current(self)) {
    reset_slot_state(self);
    evaluate_slot(self);
    update(self);
  }
}

GnMediaSource *
gn_media_viewer_get_source(GnMediaViewer *self)
{
  g_return_val_if_fail(GN_IS_MEDIA_VIEWER(self), NULL);
  return self->source;
}

void
gn_media_viewer_set_decode_limits(GnMediaViewer *self, const GnMediaDecodeLimits *limits)
{
  g_return_if_fail(GN_IS_MEDIA_VIEWER(self));
  if (limits) self->limits = *limits;
  else gn_media_decode_limits_init_default(&self->limits);
}

void
gn_media_viewer_set_gallery(GnMediaViewer *self, const gchar *const *urls, guint current_index)
{
  g_return_if_fail(GN_IS_MEDIA_VIEWER(self));
  reset_slot_state(self);
  g_ptr_array_set_size(self->urls, 0);
  g_ptr_array_set_size(self->paintables, 0);
  if (urls)
    for (guint i = 0; urls[i] && i < MAX_GALLERY; i++) {
      g_ptr_array_add(self->urls, g_strdup(urls[i]));
      g_ptr_array_add(self->paintables, NULL);
    }
  self->index = self->urls->len ? MIN(current_index, self->urls->len - 1) : 0;
  evaluate_slot(self);
  update(self);
}

gboolean
gn_media_viewer_navigate(GnMediaViewer *self, gint delta)
{
  g_return_val_if_fail(GN_IS_MEDIA_VIEWER(self), FALSE);
  gint64 next = (gint64)self->index + delta;
  if (next < 0 || next >= (gint64)self->urls->len) return FALSE;
  self->index = (guint)next;
  reset_slot_state(self);
  evaluate_slot(self);
  update(self);
  return TRUE;
}

void
gn_media_viewer_set_paintable(GnMediaViewer *self, guint index, GdkPaintable *paintable)
{
  g_return_if_fail(GN_IS_MEDIA_VIEWER(self));
  g_return_if_fail(!paintable || GDK_IS_PAINTABLE(paintable));
  if (index >= self->paintables->len) return;
  gpointer old = g_ptr_array_index(self->paintables, index);
  g_ptr_array_index(self->paintables, index) = paintable ? g_object_ref(paintable) : NULL;
  if (index == self->index) {
    if (paintable) {
      cancel_pending(self);
      self->loading = FALSE;
      g_clear_pointer(&self->message, g_free);
      self->offer_load = FALSE;
    }
    self->zoom_level = FIT_ZOOM;
    update(self);
  }
  if (old) g_object_unref(old);
}

void
gn_media_viewer_set_texture(GnMediaViewer *self, guint index, GdkTexture *texture)
{
  gn_media_viewer_set_paintable(self, index,
    texture ? gn_animated_image_paintable_for_texture(texture) : NULL);
}

void
gn_media_viewer_set_paintable_for_generation(GnMediaViewer *self, guint64 generation, guint index,
                                             GdkPaintable *paintable)
{
  g_return_if_fail(GN_IS_MEDIA_VIEWER(self));
  if (generation == self->generation) gn_media_viewer_set_paintable(self, index, paintable);
}

void
gn_media_viewer_set_texture_for_generation(GnMediaViewer *self, guint64 generation, guint index,
                                           GdkTexture *texture)
{
  g_return_if_fail(GN_IS_MEDIA_VIEWER(self));
  if (generation == self->generation) gn_media_viewer_set_texture(self, index, texture);
}

void
gn_media_viewer_set_loading(GnMediaViewer *self, guint64 generation, guint index)
{
  g_return_if_fail(GN_IS_MEDIA_VIEWER(self));
  if (generation != self->generation || index != self->index || current(self)) return;
  self->loading = TRUE;
  g_clear_pointer(&self->message, g_free);
  self->offer_load = FALSE;
  update(self);
}

void
gn_media_viewer_set_error(GnMediaViewer *self, guint64 generation, guint index, const char *message)
{
  g_return_if_fail(GN_IS_MEDIA_VIEWER(self));
  if (generation != self->generation || index != self->index || current(self)) return;
  self->loading = FALSE;
  g_free(self->message);
  self->message = g_strdup(message ? message : _("Remote images unavailable"));
  self->offer_load = FALSE;
  update(self);
}

void
gn_media_viewer_set_blocked(GnMediaViewer *self, gboolean blocked, const char *reason)
{
  g_return_if_fail(GN_IS_MEDIA_VIEWER(self));
  self->loading = FALSE;
  g_clear_pointer(&self->message, g_free);
  if (blocked) self->message = g_strdup(reason ? reason : _("Remote media is blocked"));
  self->offer_load = blocked;
  update(self);
}

GdkPaintable *
gn_media_viewer_get_paintable(GnMediaViewer *self)
{
  g_return_val_if_fail(GN_IS_MEDIA_VIEWER(self), NULL);
  return current(self);
}

guint64
gn_media_viewer_get_generation(GnMediaViewer *self)
{
  g_return_val_if_fail(GN_IS_MEDIA_VIEWER(self), 0);
  return self->generation;
}

GCancellable *
gn_media_viewer_get_cancellable(GnMediaViewer *self)
{
  g_return_val_if_fail(GN_IS_MEDIA_VIEWER(self), NULL);
  return self->cancellable;
}

void
gn_media_viewer_request_load(GnMediaViewer *self)
{
  g_return_if_fail(GN_IS_MEDIA_VIEWER(self));
  const char *url = current_url(self);
  if (!url || current(self) || self->loading) return;
  g_signal_emit(self, signals[SIG_LOAD_REQUESTED], 0, self->index, url);
  if (current(self) || self->loading || !self->source) {
    update(self);
    return;
  }
  /* The user asked: ASK becomes a fetch; BLOCKED stays blocked. */
  if (gn_media_source_get_policy(self->source, url, GN_MEDIA_KIND_IMAGE) != GN_MEDIA_POLICY_BLOCKED)
    begin_fetch(self);
  update(self);
}

gboolean
gn_media_viewer_get_load_offered(GnMediaViewer *self)
{
  g_return_val_if_fail(GN_IS_MEDIA_VIEWER(self), FALSE);
  return gtk_widget_get_visible(self->blocked_overlay) && self->offer_load;
}

gboolean
gn_media_viewer_get_loading(GnMediaViewer *self)
{
  g_return_val_if_fail(GN_IS_MEDIA_VIEWER(self), FALSE);
  return self->loading;
}

const char *
gn_media_viewer_get_message(GnMediaViewer *self)
{
  g_return_val_if_fail(GN_IS_MEDIA_VIEWER(self), NULL);
  return self->message;
}

void
gn_media_viewer_set_zoom(GnMediaViewer *self, gdouble zoom)
{
  g_return_if_fail(GN_IS_MEDIA_VIEWER(self));
  zoom_to(self, zoom);
}

gdouble
gn_media_viewer_get_zoom(GnMediaViewer *self)
{
  g_return_val_if_fail(GN_IS_MEDIA_VIEWER(self), 0);
  return self->zoom_level;
}

gdouble
gn_media_viewer_get_shown_zoom(GnMediaViewer *self)
{
  g_return_val_if_fail(GN_IS_MEDIA_VIEWER(self), 1.0);
  return shown_zoom(self);
}

guint
gn_media_viewer_get_index(GnMediaViewer *self)
{
  g_return_val_if_fail(GN_IS_MEDIA_VIEWER(self), 0);
  return self->index;
}

void
gn_media_viewer_set_can_save(GnMediaViewer *self, gboolean can_save)
{
  g_return_if_fail(GN_IS_MEDIA_VIEWER(self));
  can_save = !!can_save;
  if (self->can_save == can_save) return;
  self->can_save = can_save;
  update(self);
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_CAN_SAVE]);
}

gboolean
gn_media_viewer_get_can_save(GnMediaViewer *self)
{
  g_return_val_if_fail(GN_IS_MEDIA_VIEWER(self), FALSE);
  return self->can_save;
}
