#include <nostr-gtk-1.0/gn-media-viewer.h>
#include "gn-portable-i18n-private.h"

struct _GnMediaViewer {
  GtkWindow parent_instance;
  GtkWidget *picture, *scroll, *load, *position;
  GPtrArray *urls;      /* gchar* */
  GPtrArray *textures;  /* GdkTexture* or NULL */
  guint index;
  gdouble zoom;         /* 0 = fit */
  guint64 generation;
  gdouble drag_h, drag_v;
};
G_DEFINE_TYPE(GnMediaViewer, gn_media_viewer, GTK_TYPE_WINDOW)
static void texture_free(gpointer texture) { if (texture) g_object_unref(texture); }
static guint load_signal;
static void update(GnMediaViewer *self) {
  guint count = self->urls ? self->urls->len : 0;
  GdkTexture *texture = count ? g_ptr_array_index(self->textures, self->index) : NULL;
  gtk_picture_set_paintable(GTK_PICTURE(self->picture), texture ? GDK_PAINTABLE(texture) : NULL);
  gtk_widget_set_visible(self->load, count && !texture);
  g_autofree gchar *label = g_strdup_printf(C_("image position", "%u / %u"), count ? self->index + 1 : 0, count);
  gtk_label_set_text(GTK_LABEL(self->position), label);
  if (!texture || self->zoom == 0) {
    gtk_widget_set_size_request(self->picture, -1, -1);
    gtk_picture_set_can_shrink(GTK_PICTURE(self->picture), TRUE);
  } else {
    gtk_picture_set_can_shrink(GTK_PICTURE(self->picture), FALSE);
    gtk_widget_set_size_request(self->picture,
      (gint)(gdk_texture_get_width(texture) * self->zoom),
      (gint)(gdk_texture_get_height(texture) * self->zoom));
  }
}
static void on_drag_begin(GtkGestureDrag *gesture, gdouble x, gdouble y,
                          gpointer data) {
  (void)gesture; (void)x; (void)y;
  GnMediaViewer *self = data;
  self->drag_h = gtk_adjustment_get_value(
    gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(self->scroll)));
  self->drag_v = gtk_adjustment_get_value(
    gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(self->scroll)));
}
static void on_drag_update(GtkGestureDrag *gesture, gdouble dx, gdouble dy,
                           gpointer data) {
  (void)gesture;
  GnMediaViewer *self = data;
  GtkAdjustment *h = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(self->scroll));
  GtkAdjustment *v = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(self->scroll));
  gtk_adjustment_set_value(h, self->drag_h - dx);
  gtk_adjustment_set_value(v, self->drag_v - dy);
}
static void on_load(GtkButton *button, gpointer data) {
  (void)button;
  gn_media_viewer_request_load(GN_MEDIA_VIEWER(data));
}
static void on_prev(GtkButton *button, gpointer data) {
  (void)button;
  gn_media_viewer_navigate(GN_MEDIA_VIEWER(data), -1);
}
static void on_next(GtkButton *button, gpointer data) {
  (void)button;
  gn_media_viewer_navigate(GN_MEDIA_VIEWER(data), 1);
}
static void on_fit(GtkButton *button, gpointer data) {
  (void)button;
  gn_media_viewer_set_zoom(GN_MEDIA_VIEWER(data), 0);
}
static void on_plus(GtkButton *button, gpointer data) {
  (void)button;
  GnMediaViewer *self = data;
  gn_media_viewer_set_zoom(self, self->zoom ? self->zoom * 1.25 : 1.25);
}
static void on_minus(GtkButton *button, gpointer data) {
  (void)button;
  GnMediaViewer *self = data;
  gn_media_viewer_set_zoom(self, self->zoom ? self->zoom / 1.25 : 0.8);
}
static gboolean on_key(GtkEventControllerKey *controller, guint keyval,
                       guint keycode, GdkModifierType state, gpointer data) {
  (void)controller; (void)keycode; (void)state;
  GnMediaViewer *self = data;
  if (keyval == GDK_KEY_Left) return gn_media_viewer_navigate(self, -1);
  if (keyval == GDK_KEY_Right) return gn_media_viewer_navigate(self, 1);
  if (keyval == GDK_KEY_plus || keyval == GDK_KEY_equal) {
    gn_media_viewer_set_zoom(self, self->zoom ? self->zoom * 1.25 : 1.25);
    return TRUE;
  }
  if (keyval == GDK_KEY_minus) {
    gn_media_viewer_set_zoom(self, self->zoom ? self->zoom / 1.25 : 0.8);
    return TRUE;
  }
  if (keyval == GDK_KEY_Escape) { gtk_window_close(GTK_WINDOW(self)); return TRUE; }
  return FALSE;
}
static void finalize(GObject *object) {
  GnMediaViewer *self = GN_MEDIA_VIEWER(object);
  g_ptr_array_unref(self->urls);
  g_ptr_array_unref(self->textures);
  G_OBJECT_CLASS(gn_media_viewer_parent_class)->finalize(object);
}
static void gn_media_viewer_class_init(GnMediaViewerClass *klass) {
  G_OBJECT_CLASS(klass)->finalize = finalize;
  gn_portable_gettext_domain();
  load_signal = g_signal_new("load-requested", G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_UINT, G_TYPE_STRING);
}
static void gn_media_viewer_init(GnMediaViewer *self) {
  self->generation = 1;
  self->urls = g_ptr_array_new_with_free_func(g_free);
  self->textures = g_ptr_array_new_with_free_func(texture_free);
  gtk_window_set_default_size(GTK_WINDOW(self), 800, 600);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  GtkWidget *prev = gtk_button_new_with_label(_("Previous"));
  GtkWidget *next = gtk_button_new_with_label(_("Next"));
  GtkWidget *fit = gtk_button_new_with_label(_("Fit"));
  GtkWidget *plus = gtk_button_new_with_label("+");
  GtkWidget *minus = gtk_button_new_with_label("−");
  /* The zoom buttons show only a symbol; name them for screen readers. */
  gtk_widget_set_tooltip_text(plus, _("Zoom In"));
  gtk_accessible_update_property(GTK_ACCESSIBLE(plus), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Zoom In"), -1);
  gtk_widget_set_tooltip_text(minus, _("Zoom Out"));
  gtk_accessible_update_property(GTK_ACCESSIBLE(minus), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Zoom Out"), -1);
  self->position = gtk_label_new(NULL);
  self->load = gtk_button_new_with_label(_("Load image"));
  gtk_box_append(GTK_BOX(bar), prev);
  gtk_box_append(GTK_BOX(bar), self->position);
  gtk_box_append(GTK_BOX(bar), next);
  gtk_box_append(GTK_BOX(bar), minus);
  gtk_box_append(GTK_BOX(bar), fit);
  gtk_box_append(GTK_BOX(bar), plus);
  gtk_box_append(GTK_BOX(bar), self->load);
  self->scroll = gtk_scrolled_window_new();
  self->picture = gtk_picture_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(self->scroll), self->picture);
  GtkGesture *drag = gtk_gesture_drag_new();
  g_signal_connect(drag, "drag-begin", G_CALLBACK(on_drag_begin), self);
  g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), self);
  gtk_widget_add_controller(self->picture, GTK_EVENT_CONTROLLER(drag));
  gtk_widget_set_vexpand(self->scroll, TRUE);
  gtk_widget_set_hexpand(self->scroll, TRUE);
  gtk_box_append(GTK_BOX(box), bar);
  gtk_box_append(GTK_BOX(box), self->scroll);
  gtk_window_set_child(GTK_WINDOW(self), box);
  g_signal_connect(prev, "clicked", G_CALLBACK(on_prev), self);
  g_signal_connect(next, "clicked", G_CALLBACK(on_next), self);
  g_signal_connect(fit, "clicked", G_CALLBACK(on_fit), self);
  g_signal_connect(plus, "clicked", G_CALLBACK(on_plus), self);
  g_signal_connect(minus, "clicked", G_CALLBACK(on_minus), self);
  g_signal_connect(self->load, "clicked", G_CALLBACK(on_load), self);
  GtkEventController *key = gtk_event_controller_key_new();
  g_signal_connect(key, "key-pressed", G_CALLBACK(on_key), self);
  gtk_widget_add_controller(GTK_WIDGET(self), key);
  update(self);
}
GnMediaViewer *gn_media_viewer_new(GtkWindow *parent) {
  GnMediaViewer *self = g_object_new(GN_TYPE_MEDIA_VIEWER, NULL);
  if (parent) gtk_window_set_transient_for(GTK_WINDOW(self), parent);
  return self;
}
void gn_media_viewer_set_gallery(GnMediaViewer *self, const gchar *const *urls, guint current) {
  g_return_if_fail(GN_IS_MEDIA_VIEWER(self));
  self->generation++;
  g_ptr_array_set_size(self->urls, 0);
  g_ptr_array_set_size(self->textures, 0);
  if (urls) for (guint i = 0; urls[i] && i < 256; i++) {
    g_ptr_array_add(self->urls, g_strdup(urls[i]));
    g_ptr_array_add(self->textures, NULL);
  }
  self->index = self->urls->len ? MIN(current, self->urls->len - 1) : 0;
  self->zoom = 0;
  update(self);
}
gboolean gn_media_viewer_navigate(GnMediaViewer *self, gint delta) {
  g_return_val_if_fail(GN_IS_MEDIA_VIEWER(self), FALSE);
  gint64 next = (gint64)self->index + delta;
  if (next < 0 || next >= (gint64)self->urls->len) return FALSE;
  self->index = (guint)next;
  self->generation++;
  self->zoom = 0;
  update(self);
  return TRUE;
}
void gn_media_viewer_set_texture(GnMediaViewer *self, guint index, GdkTexture *texture) {
  g_return_if_fail(GN_IS_MEDIA_VIEWER(self));
  if (index >= self->textures->len) return;
  gpointer old = g_ptr_array_index(self->textures, index);
  g_ptr_array_index(self->textures, index) = texture ? g_object_ref(texture) : NULL;
  if (old) g_object_unref(old);
  if (index == self->index) update(self);
}
void gn_media_viewer_set_texture_for_generation(GnMediaViewer *self,
                                                 guint64 generation, guint index,
                                                 GdkTexture *texture) {
  g_return_if_fail(GN_IS_MEDIA_VIEWER(self));
  if (generation == self->generation) gn_media_viewer_set_texture(self, index, texture);
}
guint64 gn_media_viewer_get_generation(GnMediaViewer *self) {
  g_return_val_if_fail(GN_IS_MEDIA_VIEWER(self), 0);
  return self->generation;
}
void gn_media_viewer_request_load(GnMediaViewer *self) {
  g_return_if_fail(GN_IS_MEDIA_VIEWER(self));
  if (self->index < self->urls->len && !g_ptr_array_index(self->textures, self->index))
    g_signal_emit(self, load_signal, 0, self->index,
                  (const char *)g_ptr_array_index(self->urls, self->index));
}
void gn_media_viewer_set_zoom(GnMediaViewer *self, gdouble zoom) {
  g_return_if_fail(GN_IS_MEDIA_VIEWER(self));
  self->zoom = zoom == 0 ? 0 : CLAMP(zoom, 0.1, 10.0);
  update(self);
}
gdouble gn_media_viewer_get_zoom(GnMediaViewer *self) {
  g_return_val_if_fail(GN_IS_MEDIA_VIEWER(self), 0);
  return self->zoom;
}
guint gn_media_viewer_get_index(GnMediaViewer *self) {
  g_return_val_if_fail(GN_IS_MEDIA_VIEWER(self), 0);
  return self->index;
}
