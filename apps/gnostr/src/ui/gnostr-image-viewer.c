/*
 * gnostr-image-viewer.c - Full-size image viewer modal
 *
 * Implementation of a modal dialog for viewing images with zoom/pan support.
 */

#include "gnostr-image-viewer.h"
#include <glib/gi18n.h>
#include <math.h>

#ifdef HAVE_SOUP3
#include <libsoup/soup.h>
#endif

/* Zoom constants */
#define MIN_ZOOM 0.1
#define MAX_ZOOM 10.0
#define ZOOM_STEP 0.25
#define FIT_ZOOM -1.0  /* Special value meaning "fit to window" */

struct _GnostrImageViewer {
  GtkWindow parent_instance;

  /* Widgets */
  GtkWidget *overlay;
  GtkWidget *scrolled_window;
  GtkWidget *picture;
  GtkWidget *close_button;
  GtkWidget *zoom_label;
  GtkWidget *spinner;

  /* State */
  GdkTexture *texture;
  double zoom_level;      /* Current zoom level, or FIT_ZOOM for fit-to-window */
  double actual_zoom;     /* Computed actual zoom when fitting */
  gboolean is_dragging;
  double drag_start_x;
  double drag_start_y;
  double scroll_start_h;
  double scroll_start_v;
  char *image_url;

#ifdef HAVE_SOUP3
  SoupSession *session;
  GCancellable *cancellable;
#endif
};

G_DEFINE_TYPE(GnostrImageViewer, gnostr_image_viewer, GTK_TYPE_WINDOW)

/* Forward declarations */
static void update_zoom_display(GnostrImageViewer *self);
static void apply_zoom(GnostrImageViewer *self);
static void zoom_to_fit(GnostrImageViewer *self);
static void zoom_to_actual(GnostrImageViewer *self);
static gboolean on_key_pressed(GtkEventControllerKey *controller,
                               guint keyval,
                               guint keycode,
                               GdkModifierType state,
                               gpointer user_data);
static gboolean on_scroll(GtkEventControllerScroll *controller,
                          double dx, double dy,
                          gpointer user_data);
static void on_drag_begin(GtkGestureDrag *gesture,
                          double start_x, double start_y,
                          gpointer user_data);
static void on_drag_update(GtkGestureDrag *gesture,
                           double offset_x, double offset_y,
                           gpointer user_data);
static void on_drag_end(GtkGestureDrag *gesture,
                        double offset_x, double offset_y,
                        gpointer user_data);
static void on_double_click(GtkGestureClick *gesture,
                            int n_press,
                            double x, double y,
                            gpointer user_data);
static void on_close_clicked(GtkButton *button, gpointer user_data);
static void on_background_clicked(GtkGestureClick *gesture,
                                  int n_press,
                                  double x, double y,
                                  gpointer user_data);

/* Pinch zoom gesture handler */
static void on_zoom_scale_changed(GtkGestureZoom *gesture,
                                  gdouble scale,
                                  gpointer user_data);

static void gnostr_image_viewer_dispose(GObject *obj) {
  GnostrImageViewer *self = GNOSTR_IMAGE_VIEWER(obj);

#ifdef HAVE_SOUP3
  if (self->cancellable) {
    g_cancellable_cancel(self->cancellable);
    g_clear_object(&self->cancellable);
  }
  g_clear_object(&self->session);
#endif

  g_clear_object(&self->texture);
  g_clear_pointer(&self->image_url, g_free);

  G_OBJECT_CLASS(gnostr_image_viewer_parent_class)->dispose(obj);
}

static void gnostr_image_viewer_class_init(GnostrImageViewerClass *klass) {
  GObjectClass *gclass = G_OBJECT_CLASS(klass);
  gclass->dispose = gnostr_image_viewer_dispose;
}

static void gnostr_image_viewer_init(GnostrImageViewer *self) {
  /* Window setup - make it fullscreen-like */
  gtk_window_set_decorated(GTK_WINDOW(self), FALSE);
  gtk_window_set_modal(GTK_WINDOW(self), TRUE);

  /* Add CSS class for styling */
  gtk_widget_add_css_class(GTK_WIDGET(self), "image-viewer");

  /* Initialize state */
  self->zoom_level = FIT_ZOOM;
  self->actual_zoom = 1.0;
  self->is_dragging = FALSE;

#ifdef HAVE_SOUP3
  self->session = soup_session_new();
  soup_session_set_timeout(self->session, 60);
  self->cancellable = g_cancellable_new();
#endif

  /* Create overlay as the main container */
  self->overlay = gtk_overlay_new();
  gtk_widget_add_css_class(self->overlay, "image-viewer-overlay");
  gtk_window_set_child(GTK_WINDOW(self), self->overlay);

  /* Create scrolled window for panning */
  self->scrolled_window = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(self->scrolled_window),
                                  GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_hexpand(self->scrolled_window, TRUE);
  gtk_widget_set_vexpand(self->scrolled_window, TRUE);
  gtk_overlay_set_child(GTK_OVERLAY(self->overlay), self->scrolled_window);

  /* Create picture widget */
  self->picture = gtk_picture_new();
  gtk_picture_set_can_shrink(GTK_PICTURE(self->picture), TRUE);
  gtk_picture_set_content_fit(GTK_PICTURE(self->picture), GTK_CONTENT_FIT_CONTAIN);
  gtk_widget_set_halign(self->picture, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(self->picture, GTK_ALIGN_CENTER);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(self->scrolled_window), self->picture);

  /* Create close button in top-right corner */
  self->close_button = gtk_button_new_from_icon_name("window-close-symbolic");
  gtk_widget_add_css_class(self->close_button, "image-viewer-close");
  gtk_widget_add_css_class(self->close_button, "circular");
  gtk_widget_add_css_class(self->close_button, "osd");
  gtk_widget_set_halign(self->close_button, GTK_ALIGN_END);
  gtk_widget_set_valign(self->close_button, GTK_ALIGN_START);
  gtk_widget_set_margin_top(self->close_button, 16);
  gtk_widget_set_margin_end(self->close_button, 16);
  gtk_overlay_add_overlay(GTK_OVERLAY(self->overlay), self->close_button);
  g_signal_connect(self->close_button, "clicked", G_CALLBACK(on_close_clicked), self);

  /* Create zoom indicator in bottom-right corner */
  self->zoom_label = gtk_label_new("100%");
  gtk_widget_add_css_class(self->zoom_label, "image-viewer-zoom");
  gtk_widget_add_css_class(self->zoom_label, "osd");
  gtk_widget_set_halign(self->zoom_label, GTK_ALIGN_END);
  gtk_widget_set_valign(self->zoom_label, GTK_ALIGN_END);
  gtk_widget_set_margin_bottom(self->zoom_label, 16);
  gtk_widget_set_margin_end(self->zoom_label, 16);
  gtk_overlay_add_overlay(GTK_OVERLAY(self->overlay), self->zoom_label);

  /* Create loading spinner (hidden by default) */
  self->spinner = gtk_spinner_new();
  gtk_widget_set_halign(self->spinner, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(self->spinner, GTK_ALIGN_CENTER);
  gtk_widget_set_size_request(self->spinner, 48, 48);
  gtk_widget_set_visible(self->spinner, FALSE);
  gtk_overlay_add_overlay(GTK_OVERLAY(self->overlay), self->spinner);

  /* Add keyboard controller */
  GtkEventController *key_controller = gtk_event_controller_key_new();
  g_signal_connect(key_controller, "key-pressed", G_CALLBACK(on_key_pressed), self);
  gtk_widget_add_controller(GTK_WIDGET(self), key_controller);

  /* Add scroll controller for zoom */
  GtkEventController *scroll_controller = gtk_event_controller_scroll_new(
      GTK_EVENT_CONTROLLER_SCROLL_VERTICAL | GTK_EVENT_CONTROLLER_SCROLL_DISCRETE);
  g_signal_connect(scroll_controller, "scroll", G_CALLBACK(on_scroll), self);
  gtk_widget_add_controller(self->picture, scroll_controller);

  /* Add drag gesture for panning */
  GtkGesture *drag_gesture = gtk_gesture_drag_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag_gesture), GDK_BUTTON_PRIMARY);
  g_signal_connect(drag_gesture, "drag-begin", G_CALLBACK(on_drag_begin), self);
  g_signal_connect(drag_gesture, "drag-update", G_CALLBACK(on_drag_update), self);
  g_signal_connect(drag_gesture, "drag-end", G_CALLBACK(on_drag_end), self);
  gtk_widget_add_controller(self->scrolled_window, GTK_EVENT_CONTROLLER(drag_gesture));

  /* Add double-click gesture to toggle fit/100% */
  GtkGesture *click_gesture = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click_gesture), GDK_BUTTON_PRIMARY);
  g_signal_connect(click_gesture, "pressed", G_CALLBACK(on_double_click), self);
  gtk_widget_add_controller(self->picture, GTK_EVENT_CONTROLLER(click_gesture));

  /* Add click gesture on overlay background to close */
  GtkGesture *bg_click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(bg_click), GDK_BUTTON_PRIMARY);
  g_signal_connect(bg_click, "pressed", G_CALLBACK(on_background_clicked), self);
  gtk_widget_add_controller(self->scrolled_window, GTK_EVENT_CONTROLLER(bg_click));

  /* Add pinch-to-zoom gesture for touch devices */
  GtkGesture *zoom_gesture = gtk_gesture_zoom_new();
  g_signal_connect(zoom_gesture, "scale-changed", G_CALLBACK(on_zoom_scale_changed), self);
  gtk_widget_add_controller(self->picture, GTK_EVENT_CONTROLLER(zoom_gesture));
}

GnostrImageViewer *gnostr_image_viewer_new(GtkWindow *parent) {
  GnostrImageViewer *self = g_object_new(GNOSTR_TYPE_IMAGE_VIEWER,
                                          "transient-for", parent,
                                          NULL);
  return self;
}

static void update_zoom_display(GnostrImageViewer *self) {
  if (!GTK_IS_LABEL(self->zoom_label)) return;

  double display_zoom = (self->zoom_level == FIT_ZOOM) ? self->actual_zoom : self->zoom_level;
  char *text = g_strdup_printf("%.0f%%", display_zoom * 100);
  gtk_label_set_text(GTK_LABEL(self->zoom_label), text);
  g_free(text);
}

static void apply_zoom(GnostrImageViewer *self) {
  if (!self->texture || !GTK_IS_PICTURE(self->picture)) return;

  int img_width = gdk_texture_get_width(self->texture);
  int img_height = gdk_texture_get_height(self->texture);

  if (self->zoom_level == FIT_ZOOM) {
    /* Fit to window - let GtkPicture handle it */
    gtk_picture_set_content_fit(GTK_PICTURE(self->picture), GTK_CONTENT_FIT_CONTAIN);
    gtk_widget_set_size_request(self->picture, -1, -1);

    /* Calculate actual zoom for display */
    int win_width = gtk_widget_get_width(GTK_WIDGET(self));
    int win_height = gtk_widget_get_height(GTK_WIDGET(self));
    if (win_width > 0 && win_height > 0 && img_width > 0 && img_height > 0) {
      double scale_x = (double)win_width / (double)img_width;
      double scale_y = (double)win_height / (double)img_height;
      self->actual_zoom = fmin(scale_x, scale_y);
      if (self->actual_zoom > 1.0) self->actual_zoom = 1.0;  /* Don't upscale */
    }
  } else {
    /* Fixed zoom level */
    gtk_picture_set_content_fit(GTK_PICTURE(self->picture), GTK_CONTENT_FIT_FILL);
    int new_width = (int)(img_width * self->zoom_level);
    int new_height = (int)(img_height * self->zoom_level);
    gtk_widget_set_size_request(self->picture, new_width, new_height);
    self->actual_zoom = self->zoom_level;
  }

  update_zoom_display(self);
}

static void zoom_to_fit(GnostrImageViewer *self) {
  self->zoom_level = FIT_ZOOM;
  apply_zoom(self);
}

static void zoom_to_actual(GnostrImageViewer *self) {
  self->zoom_level = 1.0;
  apply_zoom(self);
}

static void zoom_in(GnostrImageViewer *self) {
  double current = (self->zoom_level == FIT_ZOOM) ? self->actual_zoom : self->zoom_level;
  self->zoom_level = fmin(current + ZOOM_STEP, MAX_ZOOM);
  apply_zoom(self);
}

static void zoom_out(GnostrImageViewer *self) {
  double current = (self->zoom_level == FIT_ZOOM) ? self->actual_zoom : self->zoom_level;
  self->zoom_level = fmax(current - ZOOM_STEP, MIN_ZOOM);
  apply_zoom(self);
}

static gboolean on_key_pressed(GtkEventControllerKey *controller,
                               guint keyval,
                               guint keycode,
                               GdkModifierType state,
                               gpointer user_data) {
  GnostrImageViewer *self = GNOSTR_IMAGE_VIEWER(user_data);
  (void)controller;
  (void)keycode;

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
      if (state & GDK_CONTROL_MASK) {
        zoom_to_fit(self);
      } else {
        zoom_to_actual(self);
      }
      return TRUE;

    case GDK_KEY_1:
    case GDK_KEY_KP_1:
      zoom_to_actual(self);
      return TRUE;

    case GDK_KEY_Left:
    case GDK_KEY_Right:
    case GDK_KEY_Up:
    case GDK_KEY_Down: {
      /* Pan with arrow keys when zoomed in */
      if (self->zoom_level != FIT_ZOOM && self->zoom_level > 1.0) {
        GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(self->scrolled_window));
        GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(self->scrolled_window));
        double step = 50.0;

        if (keyval == GDK_KEY_Left) {
          gtk_adjustment_set_value(hadj, gtk_adjustment_get_value(hadj) - step);
        } else if (keyval == GDK_KEY_Right) {
          gtk_adjustment_set_value(hadj, gtk_adjustment_get_value(hadj) + step);
        } else if (keyval == GDK_KEY_Up) {
          gtk_adjustment_set_value(vadj, gtk_adjustment_get_value(vadj) - step);
        } else if (keyval == GDK_KEY_Down) {
          gtk_adjustment_set_value(vadj, gtk_adjustment_get_value(vadj) + step);
        }
        return TRUE;
      }
      break;
    }

    default:
      break;
  }

  return FALSE;
}

static gboolean on_scroll(GtkEventControllerScroll *controller,
                          double dx, double dy,
                          gpointer user_data) {
  GnostrImageViewer *self = GNOSTR_IMAGE_VIEWER(user_data);
  (void)controller;
  (void)dx;

  /* Zoom with scroll wheel */
  if (dy < 0) {
    zoom_in(self);
  } else if (dy > 0) {
    zoom_out(self);
  }

  return TRUE;
}

static void on_drag_begin(GtkGestureDrag *gesture,
                          double start_x, double start_y,
                          gpointer user_data) {
  GnostrImageViewer *self = GNOSTR_IMAGE_VIEWER(user_data);
  (void)gesture;
  (void)start_x;
  (void)start_y;

  /* Only enable dragging when zoomed in */
  if (self->zoom_level != FIT_ZOOM && self->zoom_level > 1.0) {
    self->is_dragging = TRUE;
    GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(self->scrolled_window));
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(self->scrolled_window));
    self->scroll_start_h = gtk_adjustment_get_value(hadj);
    self->scroll_start_v = gtk_adjustment_get_value(vadj);
    self->drag_start_x = start_x;
    self->drag_start_y = start_y;
  }
}

static void on_drag_update(GtkGestureDrag *gesture,
                           double offset_x, double offset_y,
                           gpointer user_data) {
  GnostrImageViewer *self = GNOSTR_IMAGE_VIEWER(user_data);
  (void)gesture;

  if (!self->is_dragging) return;

  GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(self->scrolled_window));
  GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(self->scrolled_window));

  gtk_adjustment_set_value(hadj, self->scroll_start_h - offset_x);
  gtk_adjustment_set_value(vadj, self->scroll_start_v - offset_y);
}

static void on_drag_end(GtkGestureDrag *gesture,
                        double offset_x, double offset_y,
                        gpointer user_data) {
  GnostrImageViewer *self = GNOSTR_IMAGE_VIEWER(user_data);
  (void)gesture;
  (void)offset_x;
  (void)offset_y;

  self->is_dragging = FALSE;
}

static void on_double_click(GtkGestureClick *gesture,
                            int n_press,
                            double x, double y,
                            gpointer user_data) {
  GnostrImageViewer *self = GNOSTR_IMAGE_VIEWER(user_data);
  (void)gesture;
  (void)x;
  (void)y;

  if (n_press == 2) {
    /* Toggle between fit and 100% */
    if (self->zoom_level == FIT_ZOOM) {
      zoom_to_actual(self);
    } else {
      zoom_to_fit(self);
    }
  }
}

static void on_close_clicked(GtkButton *button, gpointer user_data) {
  GnostrImageViewer *self = GNOSTR_IMAGE_VIEWER(user_data);
  (void)button;
  gtk_window_close(GTK_WINDOW(self));
}

static void on_background_clicked(GtkGestureClick *gesture,
                                  int n_press,
                                  double x, double y,
                                  gpointer user_data) {
  GnostrImageViewer *self = GNOSTR_IMAGE_VIEWER(user_data);
  (void)gesture;
  (void)n_press;

  /* Check if click was on the scrolled window background (not the image) */
  GtkWidget *widget = gtk_event_controller_get_widget(
      GTK_EVENT_CONTROLLER(gesture));

  /* Get the picture's allocation relative to the scrolled window */
  if (GTK_IS_PICTURE(self->picture)) {
    graphene_rect_t bounds;
    if (gtk_widget_compute_bounds(self->picture, widget, &bounds)) {
      /* If click is outside the picture bounds, close */
      if (x < bounds.origin.x || x > bounds.origin.x + bounds.size.width ||
          y < bounds.origin.y || y > bounds.origin.y + bounds.size.height) {
        gtk_window_close(GTK_WINDOW(self));
      }
    }
  }
}

static void on_zoom_scale_changed(GtkGestureZoom *gesture,
                                  gdouble scale,
                                  gpointer user_data) {
  GnostrImageViewer *self = GNOSTR_IMAGE_VIEWER(user_data);
  (void)gesture;

  /* Apply pinch zoom */
  double base_zoom = (self->zoom_level == FIT_ZOOM) ? self->actual_zoom : self->zoom_level;
  double new_zoom = base_zoom * scale;
  new_zoom = fmax(MIN_ZOOM, fmin(MAX_ZOOM, new_zoom));
  self->zoom_level = new_zoom;
  apply_zoom(self);
}

#ifdef HAVE_SOUP3
static void on_image_loaded(GObject *source, GAsyncResult *res, gpointer user_data) {
  GnostrImageViewer *self = GNOSTR_IMAGE_VIEWER(user_data);

  /* Check if we're still valid */
  if (!GNOSTR_IS_IMAGE_VIEWER(self)) return;

  /* Hide spinner */
  gtk_widget_set_visible(self->spinner, FALSE);
  gtk_spinner_stop(GTK_SPINNER(self->spinner));

  GError *error = NULL;
  GBytes *bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), res, &error);

  if (error) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_warning("ImageViewer: Failed to load image: %s", error->message);
    }
    g_error_free(error);
    return;
  }

  if (!bytes || g_bytes_get_size(bytes) == 0) {
    if (bytes) g_bytes_unref(bytes);
    g_warning("ImageViewer: Empty image data");
    return;
  }

  /* Create texture from bytes */
  GdkTexture *texture = gdk_texture_new_from_bytes(bytes, &error);
  g_bytes_unref(bytes);

  if (error) {
    g_warning("ImageViewer: Failed to create texture: %s", error->message);
    g_error_free(error);
    return;
  }

  /* Store and display texture */
  g_clear_object(&self->texture);
  self->texture = texture;  /* Takes ownership */

  if (GTK_IS_PICTURE(self->picture)) {
    gtk_picture_set_paintable(GTK_PICTURE(self->picture), GDK_PAINTABLE(texture));
  }

  /* Apply initial fit zoom */
  zoom_to_fit(self);
}
#endif

void gnostr_image_viewer_set_image_url(GnostrImageViewer *self, const char *url) {
  g_return_if_fail(GNOSTR_IS_IMAGE_VIEWER(self));

  g_clear_pointer(&self->image_url, g_free);
  self->image_url = g_strdup(url);

#ifdef HAVE_SOUP3
  if (!url || !*url) return;

  /* Cancel any pending request */
  if (self->cancellable) {
    g_cancellable_cancel(self->cancellable);
    g_clear_object(&self->cancellable);
  }
  self->cancellable = g_cancellable_new();

  /* Show loading spinner */
  gtk_widget_set_visible(self->spinner, TRUE);
  gtk_spinner_start(GTK_SPINNER(self->spinner));

  /* Start async fetch */
  SoupMessage *msg = soup_message_new("GET", url);
  if (!msg) {
    g_warning("ImageViewer: Invalid URL: %s", url);
    gtk_widget_set_visible(self->spinner, FALSE);
    gtk_spinner_stop(GTK_SPINNER(self->spinner));
    return;
  }

  soup_session_send_and_read_async(
    self->session,
    msg,
    G_PRIORITY_DEFAULT,
    self->cancellable,
    on_image_loaded,
    self
  );

  g_object_unref(msg);
#else
  g_warning("ImageViewer: libsoup3 not available, cannot load remote images");
#endif
}

void gnostr_image_viewer_set_texture(GnostrImageViewer *self, GdkTexture *texture) {
  g_return_if_fail(GNOSTR_IS_IMAGE_VIEWER(self));
  g_return_if_fail(GDK_IS_TEXTURE(texture));

  g_clear_object(&self->texture);
  self->texture = g_object_ref(texture);

  if (GTK_IS_PICTURE(self->picture)) {
    gtk_picture_set_paintable(GTK_PICTURE(self->picture), GDK_PAINTABLE(texture));
  }

  /* Apply initial fit zoom */
  zoom_to_fit(self);
}

void gnostr_image_viewer_present(GnostrImageViewer *self) {
  g_return_if_fail(GNOSTR_IS_IMAGE_VIEWER(self));

  /* Maximize the window to fill the screen */
  gtk_window_maximize(GTK_WINDOW(self));

  /* Present the window */
  gtk_window_present(GTK_WINDOW(self));

  /* Apply zoom after window is realized */
  if (self->texture) {
    apply_zoom(self);
  }
}
