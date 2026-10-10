#include <nostr-gtk-1.0/gn-media-viewer.h>
#include <nostr-gtk-1.0/gn-animated-image.h>
#include "gn-portable-i18n-private.h"

/* nostrc-p15n5.5: Gnostr's viewer look (gnostr-image-viewer.c) - a black
 * full-window panel, the media centred on it, and every control an OSD
 * overlay: Close top right, the gallery position bottom centre, zoom bottom
 * right, a video's controls above them. Loading stays consent-gated: the
 * "Load image" button only emits load-requested. */

struct _GnMediaViewer {
  GtkWindow parent_instance;
  GtkWidget *picture, *scroll, *load, *position, *nav, *prev, *next, *zoom_box, *zoom_label;
  GtkWidget *controls_slot;
  GPtrArray *urls;        /* gchar* */
  GPtrArray *paintables;  /* GdkPaintable* or NULL */
  guint index;
  gdouble zoom;           /* 0 = fit */
  guint64 generation;
  gdouble drag_h, drag_v;
  GtkMediaStream *playing;
};
G_DEFINE_TYPE(GnMediaViewer, gn_media_viewer, GTK_TYPE_WINDOW)
static void paintable_free(gpointer paintable) { if (paintable) g_object_unref(paintable); }
static guint load_signal;

static const gchar *viewer_css =
  "window.gn-media-viewer { background-color: black; }\n"
  "window.gn-media-viewer scrolledwindow, window.gn-media-viewer picture,"
  " window.gn-media-viewer viewport { background: transparent; }\n"
  ".gn-media-viewer .gn-media-osd { background-color: alpha(black, 0.55); color: white;"
  " border-radius: 9999px; padding: 4px 10px; }\n"
  ".gn-media-viewer .gn-media-osd label { color: white; }\n"
  ".gn-media-viewer button.gn-media-button { color: white; background: transparent;"
  " min-width: 32px; min-height: 32px; border: none; box-shadow: none; }\n"
  ".gn-media-viewer button.gn-media-button:hover { background: alpha(white, 0.15); }\n"
  ".gn-media-viewer button.gn-media-button:disabled { opacity: 0.3; }\n"
  ".gn-media-viewer button.gn-media-close { background: alpha(black, 0.55); color: white;"
  " min-width: 36px; min-height: 36px; border: none; }\n"
  ".gn-media-viewer button.gn-media-close:hover { background: alpha(black, 0.75); }\n"
  ".gn-media-viewer .gn-media-controls { background-color: alpha(black, 0.55); color: white;"
  " border-radius: 12px; padding: 4px 8px; }\n";

static void
install_css(void)
{
  static gboolean installed;
  GdkDisplay *display = gdk_display_get_default();
  if (installed || !display) return;
  installed = TRUE;
  GtkCssProvider *provider = gtk_css_provider_new();
  gtk_css_provider_load_from_data(provider, viewer_css, -1);
  gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(provider),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(provider);
}

static GdkPaintable *
current(GnMediaViewer *self)
{
  return self->index < self->paintables->len ? g_ptr_array_index(self->paintables, self->index)
                                             : NULL;
}

static void
stop_playing(GnMediaViewer *self)
{
  if (self->playing) gtk_media_stream_pause(self->playing);
  g_clear_object(&self->playing);
}

static void
update_controls(GnMediaViewer *self, GdkPaintable *paintable)
{
  GtkWidget *child = gtk_widget_get_first_child(self->controls_slot);
  GtkMediaStream *stream = GTK_IS_MEDIA_STREAM(paintable) ? GTK_MEDIA_STREAM(paintable) : NULL;
  if (stream && self->playing == stream) return;
  if (child) gtk_box_remove(GTK_BOX(self->controls_slot), child);
  stop_playing(self);
  if (stream) {
    GtkWidget *controls = gtk_media_controls_new(stream);
    gtk_widget_set_size_request(controls, 360, -1);
    gtk_box_append(GTK_BOX(self->controls_slot), controls);
    self->playing = g_object_ref(stream);
    if (gtk_widget_get_mapped(GTK_WIDGET(self))) gtk_media_stream_play(stream);
  }
  gtk_widget_set_visible(self->controls_slot, stream != NULL);
}

static void
update(GnMediaViewer *self)
{
  guint count = self->urls ? self->urls->len : 0;
  GdkPaintable *paintable = count ? current(self) : NULL;
  gtk_picture_set_paintable(GTK_PICTURE(self->picture), paintable);
  gn_animated_image_attach(self->picture, paintable);
  update_controls(self, paintable);
  gtk_widget_set_visible(self->load, count && !paintable);
  g_autofree gchar *label = g_strdup_printf(C_("image position", "%u / %u"), count ? self->index + 1 : 0, count);
  gtk_label_set_text(GTK_LABEL(self->position), label);
  gtk_widget_set_visible(self->nav, count > 1);
  gtk_widget_set_sensitive(self->prev, self->index > 0);
  gtk_widget_set_sensitive(self->next, self->index + 1 < count);
  gtk_widget_set_visible(self->zoom_box, paintable && !GTK_IS_MEDIA_STREAM(paintable));
  g_autofree gchar *zoom = self->zoom == 0 ? g_strdup(_("Fit"))
                                           : g_strdup_printf("%d%%", (int)(self->zoom * 100 + 0.5));
  gtk_label_set_text(GTK_LABEL(self->zoom_label), zoom);
  if (!paintable || self->zoom == 0) {
    gtk_widget_set_size_request(self->picture, -1, -1);
    gtk_picture_set_can_shrink(GTK_PICTURE(self->picture), TRUE);
  } else {
    gtk_picture_set_can_shrink(GTK_PICTURE(self->picture), FALSE);
    gtk_widget_set_size_request(self->picture,
      (gint)(gdk_paintable_get_intrinsic_width(paintable) * self->zoom),
      (gint)(gdk_paintable_get_intrinsic_height(paintable) * self->zoom));
  }
}

static void
on_drag_begin(GtkGestureDrag *gesture, gdouble x, gdouble y, gpointer data)
{
  (void)gesture; (void)x; (void)y;
  GnMediaViewer *self = data;
  self->drag_h = gtk_adjustment_get_value(
    gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(self->scroll)));
  self->drag_v = gtk_adjustment_get_value(
    gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(self->scroll)));
}

static void
on_drag_update(GtkGestureDrag *gesture, gdouble dx, gdouble dy, gpointer data)
{
  (void)gesture;
  GnMediaViewer *self = data;
  GtkAdjustment *h = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(self->scroll));
  GtkAdjustment *v = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(self->scroll));
  gtk_adjustment_set_value(h, self->drag_h - dx);
  gtk_adjustment_set_value(v, self->drag_v - dy);
}

static void zoom_in(GnMediaViewer *self) { gn_media_viewer_set_zoom(self, self->zoom ? self->zoom * 1.25 : 1.25); }
static void zoom_out(GnMediaViewer *self) { gn_media_viewer_set_zoom(self, self->zoom ? self->zoom / 1.25 : 0.8); }

static void on_load(GtkButton *button, gpointer data) { (void)button; gn_media_viewer_request_load(GN_MEDIA_VIEWER(data)); }
static void on_prev(GtkButton *button, gpointer data) { (void)button; gn_media_viewer_navigate(GN_MEDIA_VIEWER(data), -1); }
static void on_next(GtkButton *button, gpointer data) { (void)button; gn_media_viewer_navigate(GN_MEDIA_VIEWER(data), 1); }
static void on_fit(GtkButton *button, gpointer data) { (void)button; gn_media_viewer_set_zoom(GN_MEDIA_VIEWER(data), 0); }
static void on_plus(GtkButton *button, gpointer data) { (void)button; zoom_in(data); }
static void on_minus(GtkButton *button, gpointer data) { (void)button; zoom_out(data); }
static void on_close(GtkButton *button, gpointer data) { (void)button; gtk_window_close(GTK_WINDOW(data)); }

static void
on_double_click(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y, gpointer data)
{
  (void)gesture; (void)x; (void)y;
  GnMediaViewer *self = data;
  if (n_press == 2) gn_media_viewer_set_zoom(self, self->zoom == 0 ? 1.0 : 0);
}

static gboolean
on_scroll(GtkEventControllerScroll *controller, gdouble dx, gdouble dy, gpointer data)
{
  (void)dx;
  GnMediaViewer *self = data;
  GdkModifierType state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(controller));
  if (!(state & GDK_CONTROL_MASK) || dy == 0) return FALSE;
  if (dy < 0) zoom_in(self); else zoom_out(self);
  return TRUE;
}

static gboolean
on_key(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state,
       gpointer data)
{
  (void)controller; (void)keycode; (void)state;
  GnMediaViewer *self = data;
  if (keyval == GDK_KEY_Left) return gn_media_viewer_navigate(self, -1);
  if (keyval == GDK_KEY_Right) return gn_media_viewer_navigate(self, 1);
  if (keyval == GDK_KEY_plus || keyval == GDK_KEY_equal || keyval == GDK_KEY_KP_Add) {
    zoom_in(self);
    return TRUE;
  }
  if (keyval == GDK_KEY_minus || keyval == GDK_KEY_KP_Subtract) {
    zoom_out(self);
    return TRUE;
  }
  if (keyval == GDK_KEY_0 || keyval == GDK_KEY_f) {
    gn_media_viewer_set_zoom(self, 0);
    return TRUE;
  }
  if (keyval == GDK_KEY_space && self->playing) {
    if (gtk_media_stream_get_playing(self->playing)) gtk_media_stream_pause(self->playing);
    else gtk_media_stream_play(self->playing);
    return TRUE;
  }
  if (keyval == GDK_KEY_Escape) { gtk_window_close(GTK_WINDOW(self)); return TRUE; }
  return FALSE;
}

static void
viewer_map(GtkWidget *widget)
{
  GTK_WIDGET_CLASS(gn_media_viewer_parent_class)->map(widget);
  GnMediaViewer *self = GN_MEDIA_VIEWER(widget);
  if (self->playing) gtk_media_stream_play(self->playing);
}

static void
viewer_unmap(GtkWidget *widget)
{
  GnMediaViewer *self = GN_MEDIA_VIEWER(widget);
  if (self->playing) gtk_media_stream_pause(self->playing);
  GTK_WIDGET_CLASS(gn_media_viewer_parent_class)->unmap(widget);
}

static void
dispose(GObject *object)
{
  GnMediaViewer *self = GN_MEDIA_VIEWER(object);
  stop_playing(self);
  if (self->picture) gn_animated_image_attach(self->picture, NULL);
  G_OBJECT_CLASS(gn_media_viewer_parent_class)->dispose(object);
}

static void
finalize(GObject *object)
{
  GnMediaViewer *self = GN_MEDIA_VIEWER(object);
  g_ptr_array_unref(self->urls);
  g_ptr_array_unref(self->paintables);
  G_OBJECT_CLASS(gn_media_viewer_parent_class)->finalize(object);
}

static void
gn_media_viewer_class_init(GnMediaViewerClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = dispose;
  G_OBJECT_CLASS(klass)->finalize = finalize;
  GTK_WIDGET_CLASS(klass)->map = viewer_map;
  GTK_WIDGET_CLASS(klass)->unmap = viewer_unmap;
  gn_portable_gettext_domain();
  load_signal = g_signal_new("load-requested", G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_UINT, G_TYPE_STRING);
}

/* An icon-only OSD button; its name is for tooltips and screen readers. */
static GtkWidget *
osd_button(const gchar *icon, const gchar *name, const gchar *css_class)
{
  GtkWidget *button = gtk_button_new_from_icon_name(icon);
  gtk_widget_add_css_class(button, css_class);
  gtk_widget_add_css_class(button, "circular");
  gtk_widget_set_tooltip_text(button, name);
  gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
  return button;
}

static GtkWidget *
osd_box(GtkAlign halign, GtkAlign valign)
{
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class(box, "gn-media-osd");
  gtk_widget_set_halign(box, halign);
  gtk_widget_set_valign(box, valign);
  gtk_widget_set_margin_start(box, 16);
  gtk_widget_set_margin_end(box, 16);
  gtk_widget_set_margin_top(box, 16);
  gtk_widget_set_margin_bottom(box, 16);
  return box;
}

static void
gn_media_viewer_init(GnMediaViewer *self)
{
  install_css();
  self->generation = 1;
  self->urls = g_ptr_array_new_with_free_func(g_free);
  self->paintables = g_ptr_array_new_with_free_func(paintable_free);
  gtk_widget_add_css_class(GTK_WIDGET(self), "gn-media-viewer");
  gtk_window_set_decorated(GTK_WINDOW(self), FALSE);
  gtk_window_set_modal(GTK_WINDOW(self), TRUE);
  gtk_window_set_title(GTK_WINDOW(self), _("Media Viewer"));
  gtk_window_set_default_size(GTK_WINDOW(self), 800, 600);

  GtkWidget *overlay = gtk_overlay_new();
  gtk_window_set_child(GTK_WINDOW(self), overlay);

  self->scroll = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(self->scroll, TRUE);
  gtk_widget_set_hexpand(self->scroll, TRUE);
  gtk_overlay_set_child(GTK_OVERLAY(overlay), self->scroll);
  self->picture = gtk_picture_new();
  gtk_picture_set_can_shrink(GTK_PICTURE(self->picture), TRUE);
  gtk_widget_set_halign(self->picture, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(self->picture, GTK_ALIGN_CENTER);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(self->scroll), self->picture);
  GtkGesture *drag = gtk_gesture_drag_new();
  g_signal_connect(drag, "drag-begin", G_CALLBACK(on_drag_begin), self);
  g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), self);
  gtk_widget_add_controller(self->picture, GTK_EVENT_CONTROLLER(drag));
  GtkGesture *click = gtk_gesture_click_new();
  g_signal_connect(click, "pressed", G_CALLBACK(on_double_click), self);
  gtk_widget_add_controller(self->picture, GTK_EVENT_CONTROLLER(click));
  GtkEventController *scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
  g_signal_connect(scroll, "scroll", G_CALLBACK(on_scroll), self);
  gtk_widget_add_controller(self->scroll, scroll);

  GtkWidget *close = osd_button("window-close-symbolic", _("Close"), "gn-media-close");
  gtk_widget_set_halign(close, GTK_ALIGN_END);
  gtk_widget_set_valign(close, GTK_ALIGN_START);
  gtk_widget_set_margin_top(close, 16);
  gtk_widget_set_margin_end(close, 16);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), close);

  self->load = gtk_button_new_with_label(_("Load image"));
  gtk_widget_add_css_class(self->load, "pill");
  gtk_widget_add_css_class(self->load, "suggested-action");
  gtk_widget_set_halign(self->load, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(self->load, GTK_ALIGN_CENTER);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), self->load);

  GtkWidget *bottom = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_valign(bottom, GTK_ALIGN_END);
  gtk_widget_set_halign(bottom, GTK_ALIGN_FILL);
  gtk_widget_set_can_target(bottom, TRUE);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), bottom);
  self->controls_slot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class(self->controls_slot, "gn-media-controls");
  gtk_widget_set_halign(self->controls_slot, GTK_ALIGN_CENTER);
  gtk_widget_set_visible(self->controls_slot, FALSE);
  gtk_box_append(GTK_BOX(bottom), self->controls_slot);
  GtkWidget *row = gtk_center_box_new();
  gtk_box_append(GTK_BOX(bottom), row);

  self->nav = osd_box(GTK_ALIGN_CENTER, GTK_ALIGN_END);
  self->prev = osd_button("go-previous-symbolic", _("Previous"), "gn-media-button");
  self->next = osd_button("go-next-symbolic", _("Next"), "gn-media-button");
  self->position = gtk_label_new(NULL);
  gtk_box_append(GTK_BOX(self->nav), self->prev);
  gtk_box_append(GTK_BOX(self->nav), self->position);
  gtk_box_append(GTK_BOX(self->nav), self->next);
  gtk_center_box_set_center_widget(GTK_CENTER_BOX(row), self->nav);

  self->zoom_box = osd_box(GTK_ALIGN_END, GTK_ALIGN_END);
  GtkWidget *minus = osd_button("zoom-out-symbolic", _("Zoom Out"), "gn-media-button");
  GtkWidget *plus = osd_button("zoom-in-symbolic", _("Zoom In"), "gn-media-button");
  GtkWidget *fit = osd_button("zoom-fit-best-symbolic", _("Fit"), "gn-media-button");
  self->zoom_label = gtk_label_new(NULL);
  gtk_label_set_width_chars(GTK_LABEL(self->zoom_label), 5);
  gtk_box_append(GTK_BOX(self->zoom_box), minus);
  gtk_box_append(GTK_BOX(self->zoom_box), self->zoom_label);
  gtk_box_append(GTK_BOX(self->zoom_box), plus);
  gtk_box_append(GTK_BOX(self->zoom_box), fit);
  gtk_center_box_set_end_widget(GTK_CENTER_BOX(row), self->zoom_box);

  g_signal_connect(close, "clicked", G_CALLBACK(on_close), self);
  g_signal_connect(self->prev, "clicked", G_CALLBACK(on_prev), self);
  g_signal_connect(self->next, "clicked", G_CALLBACK(on_next), self);
  g_signal_connect(fit, "clicked", G_CALLBACK(on_fit), self);
  g_signal_connect(plus, "clicked", G_CALLBACK(on_plus), self);
  g_signal_connect(minus, "clicked", G_CALLBACK(on_minus), self);
  g_signal_connect(self->load, "clicked", G_CALLBACK(on_load), self);
  GtkEventController *key = gtk_event_controller_key_new();
  g_signal_connect(key, "key-pressed", G_CALLBACK(on_key), self);
  gtk_widget_add_controller(GTK_WIDGET(self), key);
  update(self);
}

GnMediaViewer *
gn_media_viewer_new(GtkWindow *parent)
{
  GnMediaViewer *self = g_object_new(GN_TYPE_MEDIA_VIEWER, NULL);
  if (parent) {
    gtk_window_set_transient_for(GTK_WINDOW(self), parent);
    /* The panel covers its parent window, as Gnostr's does. */
    gint width = gtk_widget_get_width(GTK_WIDGET(parent));
    gint height = gtk_widget_get_height(GTK_WIDGET(parent));
    if (width > 0 && height > 0) gtk_window_set_default_size(GTK_WINDOW(self), width, height);
  }
  return self;
}

void
gn_media_viewer_set_gallery(GnMediaViewer *self, const gchar *const *urls, guint current)
{
  g_return_if_fail(GN_IS_MEDIA_VIEWER(self));
  self->generation++;
  g_ptr_array_set_size(self->urls, 0);
  g_ptr_array_set_size(self->paintables, 0);
  if (urls) for (guint i = 0; urls[i] && i < 256; i++) {
    g_ptr_array_add(self->urls, g_strdup(urls[i]));
    g_ptr_array_add(self->paintables, NULL);
  }
  self->index = self->urls->len ? MIN(current, self->urls->len - 1) : 0;
  self->zoom = 0;
  update(self);
}

gboolean
gn_media_viewer_navigate(GnMediaViewer *self, gint delta)
{
  g_return_val_if_fail(GN_IS_MEDIA_VIEWER(self), FALSE);
  gint64 next = (gint64)self->index + delta;
  if (next < 0 || next >= (gint64)self->urls->len) return FALSE;
  self->index = (guint)next;
  self->generation++;
  self->zoom = 0;
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
  if (index == self->index) update(self);
  if (old) g_object_unref(old);
}

void
gn_media_viewer_set_texture(GnMediaViewer *self, guint index, GdkTexture *texture)
{
  gn_media_viewer_set_paintable(self, index,
    texture ? gn_animated_image_paintable_for_texture(texture) : NULL);
}

void
gn_media_viewer_set_texture_for_generation(GnMediaViewer *self, guint64 generation, guint index,
                                           GdkTexture *texture)
{
  g_return_if_fail(GN_IS_MEDIA_VIEWER(self));
  if (generation == self->generation) gn_media_viewer_set_texture(self, index, texture);
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

void
gn_media_viewer_request_load(GnMediaViewer *self)
{
  g_return_if_fail(GN_IS_MEDIA_VIEWER(self));
  if (self->index < self->urls->len && !current(self))
    g_signal_emit(self, load_signal, 0, self->index,
                  (const char *)g_ptr_array_index(self->urls, self->index));
}

void
gn_media_viewer_set_zoom(GnMediaViewer *self, gdouble zoom)
{
  g_return_if_fail(GN_IS_MEDIA_VIEWER(self));
  self->zoom = zoom == 0 ? 0 : CLAMP(zoom, 0.1, 10.0);
  update(self);
}

gdouble
gn_media_viewer_get_zoom(GnMediaViewer *self)
{
  g_return_val_if_fail(GN_IS_MEDIA_VIEWER(self), 0);
  return self->zoom;
}

guint
gn_media_viewer_get_index(GnMediaViewer *self)
{
  g_return_val_if_fail(GN_IS_MEDIA_VIEWER(self), 0);
  return self->index;
}
