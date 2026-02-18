/*
 * gnostr-image-viewer.c - Full-size image viewer modal
 *
 * Implementation of a modal dialog for viewing images with zoom/pan support.
 */

#include "gnostr-image-viewer.h"
#include "gnostr-main-window.h"
#include "gnostr-avatar-cache.h"
#include "../util/utils.h"
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
  GtkWidget *save_button;
  GtkWidget *copy_link_button;
  GtkWidget *prev_button;
  GtkWidget *next_button;
  GtkWidget *nav_label;    /* Shows "1 / 5" style indicator */

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

  /* Gallery state */
  char **gallery_urls;    /* NULL-terminated array of URLs */
  guint gallery_count;    /* Number of images in gallery */
  guint gallery_index;    /* Current image index */

#ifdef HAVE_SOUP3
  /* Uses gnostr_get_shared_soup_session() instead of per-widget session */
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

/* Navigation handlers */
static void on_prev_clicked(GtkButton *button, gpointer user_data);
static void on_next_clicked(GtkButton *button, gpointer user_data);
static void on_save_clicked(GtkButton *button, gpointer user_data);
static void on_copy_link_clicked(GtkButton *button, gpointer user_data);
static void update_nav_display(GnostrImageViewer *self);

static void gnostr_image_viewer_dispose(GObject *obj) {
  GnostrImageViewer *self = GNOSTR_IMAGE_VIEWER(obj);

#ifdef HAVE_SOUP3
  if (self->cancellable) {
    g_cancellable_cancel(self->cancellable);
    g_clear_object(&self->cancellable);
  }
  /* Shared session is managed globally - do not clear here */
#endif

  g_clear_object(&self->texture);
  g_clear_pointer(&self->image_url, g_free);
  g_strfreev(self->gallery_urls);
  self->gallery_urls = NULL;
  self->gallery_count = 0;

  G_OBJECT_CLASS(gnostr_image_viewer_parent_class)->dispose(obj);
}

static void gnostr_image_viewer_class_init(GnostrImageViewerClass *klass) {
  GObjectClass *gclass = G_OBJECT_CLASS(klass);
  gclass->dispose = gnostr_image_viewer_dispose;
}

static void gnostr_image_viewer_init(GnostrImageViewer *self) {
  /* Window setup - modal overlay style (no decorations, contained within parent) */
  gtk_window_set_decorated(GTK_WINDOW(self), FALSE);
  gtk_window_set_modal(GTK_WINDOW(self), TRUE);

  /* Prevent fullscreen mode - the modal should stay within parent bounds */
  gtk_window_set_resizable(GTK_WINDOW(self), FALSE);

  /* Add CSS class for styling */
  gtk_widget_add_css_class(GTK_WIDGET(self), "image-viewer");

  /* Initialize state */
  self->zoom_level = FIT_ZOOM;
  self->actual_zoom = 1.0;
  self->is_dragging = FALSE;

#ifdef HAVE_SOUP3
  /* Uses shared session from gnostr_get_shared_soup_session() */
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

  /* Create toolbar box for buttons in top-left */
  GtkWidget *toolbar_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(toolbar_box, GTK_ALIGN_START);
  gtk_widget_set_valign(toolbar_box, GTK_ALIGN_START);
  gtk_widget_set_margin_top(toolbar_box, 16);
  gtk_widget_set_margin_start(toolbar_box, 16);
  gtk_overlay_add_overlay(GTK_OVERLAY(self->overlay), toolbar_box);

  /* Save button */
  self->save_button = gtk_button_new_from_icon_name("document-save-symbolic");
  gtk_widget_add_css_class(self->save_button, "image-viewer-button");
  gtk_widget_add_css_class(self->save_button, "circular");
  gtk_widget_add_css_class(self->save_button, "osd");
  gtk_widget_set_tooltip_text(self->save_button, "Save image (Ctrl+S)");
  g_signal_connect(self->save_button, "clicked", G_CALLBACK(on_save_clicked), self);
  gtk_box_append(GTK_BOX(toolbar_box), self->save_button);

  /* Copy link button */
  self->copy_link_button = gtk_button_new_from_icon_name("edit-copy-symbolic");
  gtk_widget_add_css_class(self->copy_link_button, "image-viewer-button");
  gtk_widget_add_css_class(self->copy_link_button, "circular");
  gtk_widget_add_css_class(self->copy_link_button, "osd");
  gtk_widget_set_tooltip_text(self->copy_link_button, "Copy link (Ctrl+C)");
  g_signal_connect(self->copy_link_button, "clicked", G_CALLBACK(on_copy_link_clicked), self);
  gtk_box_append(GTK_BOX(toolbar_box), self->copy_link_button);

  /* Create navigation box in bottom center */
  GtkWidget *nav_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_add_css_class(nav_box, "image-viewer-nav");
  gtk_widget_add_css_class(nav_box, "osd");
  gtk_widget_set_halign(nav_box, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(nav_box, GTK_ALIGN_END);
  gtk_widget_set_margin_bottom(nav_box, 16);
  gtk_widget_set_visible(nav_box, FALSE);  /* Hidden until gallery is set */
  gtk_overlay_add_overlay(GTK_OVERLAY(self->overlay), nav_box);

  /* Previous button */
  self->prev_button = gtk_button_new_from_icon_name("go-previous-symbolic");
  gtk_widget_add_css_class(self->prev_button, "image-viewer-nav-button");
  gtk_widget_add_css_class(self->prev_button, "circular");
  gtk_widget_set_tooltip_text(self->prev_button, "Previous image (Left arrow)");
  g_signal_connect(self->prev_button, "clicked", G_CALLBACK(on_prev_clicked), self);
  gtk_box_append(GTK_BOX(nav_box), self->prev_button);

  /* Navigation label (e.g., "1 / 5") */
  self->nav_label = gtk_label_new("");
  gtk_widget_add_css_class(self->nav_label, "image-viewer-nav-label");
  gtk_box_append(GTK_BOX(nav_box), self->nav_label);

  /* Next button */
  self->next_button = gtk_button_new_from_icon_name("go-next-symbolic");
  gtk_widget_add_css_class(self->next_button, "image-viewer-nav-button");
  gtk_widget_add_css_class(self->next_button, "circular");
  gtk_widget_set_tooltip_text(self->next_button, "Next image (Right arrow)");
  g_signal_connect(self->next_button, "clicked", G_CALLBACK(on_next_clicked), self);
  gtk_box_append(GTK_BOX(nav_box), self->next_button);

  /* Store nav_box reference on overlay for visibility toggling */
  g_object_set_data(G_OBJECT(self->overlay), "nav-box", nav_box);

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
  g_autofree char *text = g_strdup_printf("%.0f%%", display_zoom * 100);
  gtk_label_set_text(GTK_LABEL(self->zoom_label), text);
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

    case GDK_KEY_s:
      /* Ctrl+S to save */
      if (state & GDK_CONTROL_MASK) {
        on_save_clicked(NULL, self);
        return TRUE;
      }
      break;

    case GDK_KEY_c:
      /* Ctrl+C to copy link */
      if (state & GDK_CONTROL_MASK) {
        on_copy_link_clicked(NULL, self);
        return TRUE;
      }
      break;

    case GDK_KEY_Left:
    case GDK_KEY_Right: {
      /* Left/Right for gallery navigation when not zoomed, or pan when zoomed */
      if (self->zoom_level != FIT_ZOOM && self->zoom_level > 1.0) {
        /* Pan mode */
        GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(self->scrolled_window));
        double step = 50.0;
        if (keyval == GDK_KEY_Left) {
          gtk_adjustment_set_value(hadj, gtk_adjustment_get_value(hadj) - step);
        } else {
          gtk_adjustment_set_value(hadj, gtk_adjustment_get_value(hadj) + step);
        }
        return TRUE;
      } else if (self->gallery_count > 1) {
        /* Gallery navigation mode */
        gnostr_image_viewer_navigate(self, (keyval == GDK_KEY_Left) ? -1 : 1);
        return TRUE;
      }
      break;
    }

    case GDK_KEY_Up:
    case GDK_KEY_Down: {
      /* Pan with arrow keys when zoomed in */
      if (self->zoom_level != FIT_ZOOM && self->zoom_level > 1.0) {
        GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(self->scrolled_window));
        double step = 50.0;
        if (keyval == GDK_KEY_Up) {
          gtk_adjustment_set_value(vadj, gtk_adjustment_get_value(vadj) - step);
        } else {
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

/* Context struct for image loading - prevents use-after-free */
typedef struct {
  GnostrImageViewer *viewer;  /* Weak reference */
  char *url;
#ifdef HAVE_SOUP3
  SoupMessage *msg;  /* For HTTP status checking */
#endif
} ImageLoadCtx;

static void on_image_viewer_destroyed(gpointer data, GObject *where_the_object_was) {
  ImageLoadCtx *ctx = (ImageLoadCtx *)data;
  (void)where_the_object_was;
  if (ctx) ctx->viewer = NULL;
}

static void image_load_ctx_free(ImageLoadCtx *ctx) {
  if (!ctx) return;
  if (ctx->viewer) {
    g_object_weak_unref(G_OBJECT(ctx->viewer), on_image_viewer_destroyed, ctx);
  }
#ifdef HAVE_SOUP3
  g_clear_object(&ctx->msg);
#endif
  g_free(ctx->url);
  g_free(ctx);
}

#ifdef HAVE_SOUP3
static void on_image_loaded(GObject *source, GAsyncResult *res, gpointer user_data) {
  ImageLoadCtx *ctx = (ImageLoadCtx *)user_data;
  GError *error = NULL;
  GBytes *bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), res, &error);

  if (error) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_warning("ImageViewer: Failed to load image '%s': %s",
                ctx->url ? ctx->url : "?", error->message);
    }
    g_error_free(error);
    image_load_ctx_free(ctx);
    return;
  }

  /* Check if viewer was destroyed via weak reference */
  if (!ctx->viewer || !GNOSTR_IS_IMAGE_VIEWER(ctx->viewer)) {
    if (bytes) g_bytes_unref(bytes);
    image_load_ctx_free(ctx);
    return;
  }

  GnostrImageViewer *self = ctx->viewer;

  /* hq-snq39: Check HTTP status code before trying to decode.
   * Non-2xx responses (403, 404, 5xx) return HTML error pages that
   * gdk_texture_new_from_bytes will fail to decode, leaving a blank viewer. */
  if (ctx->msg) {
    guint status = soup_message_get_status(ctx->msg);
    if (status < 200 || status >= 300) {
      g_warning("ImageViewer: HTTP %u for '%s'",
                status, ctx->url ? ctx->url : "?");
      if (bytes) g_bytes_unref(bytes);
      if (GTK_IS_WIDGET(self->spinner)) {
        gtk_widget_set_visible(self->spinner, FALSE);
        gtk_spinner_stop(GTK_SPINNER(self->spinner));
      }
      image_load_ctx_free(ctx);
      return;
    }
  }

  /* Hide spinner */
  if (GTK_IS_WIDGET(self->spinner)) {
    gtk_widget_set_visible(self->spinner, FALSE);
    gtk_spinner_stop(GTK_SPINNER(self->spinner));
  }

  if (!bytes || g_bytes_get_size(bytes) == 0) {
    if (bytes) g_bytes_unref(bytes);
    g_warning("ImageViewer: Empty image data for '%s'",
              ctx->url ? ctx->url : "?");
    image_load_ctx_free(ctx);
    return;
  }

  /* Create texture from bytes */
  GdkTexture *texture = gdk_texture_new_from_bytes(bytes, &error);
  g_bytes_unref(bytes);

  if (error) {
    g_warning("ImageViewer: Failed to create texture for '%s': %s",
              ctx->url ? ctx->url : "?", error->message);
    g_error_free(error);
    image_load_ctx_free(ctx);
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

  image_load_ctx_free(ctx);
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

  /* Always fetch the full-size image from the network.  The avatar cache
   * stores downscaled thumbnails which are too small for the image viewer.
   * DO NOT use gnostr_avatar_try_load_cached here. */

  /* Show loading spinner */
  gtk_widget_set_visible(self->spinner, TRUE);
  gtk_spinner_start(GTK_SPINNER(self->spinner));

  /* hq-snq39: Check shared soup session before starting fetch */
  SoupSession *session = gnostr_get_shared_soup_session();
  if (!session) {
    g_warning("ImageViewer: shared soup session unavailable, cannot load: %s", url);
    gtk_widget_set_visible(self->spinner, FALSE);
    gtk_spinner_stop(GTK_SPINNER(self->spinner));
    return;
  }

  /* Start async fetch */
  SoupMessage *msg = soup_message_new("GET", url);
  if (!msg) {
    g_warning("ImageViewer: Invalid URL: %s", url);
    gtk_widget_set_visible(self->spinner, FALSE);
    gtk_spinner_stop(GTK_SPINNER(self->spinner));
    return;
  }

  /* Create context with weak reference to viewer */
  ImageLoadCtx *ctx = g_new0(ImageLoadCtx, 1);
  ctx->viewer = self;
  ctx->url = g_strdup(url);
  ctx->msg = g_object_ref(msg);  /* Keep ref for HTTP status check in callback */
  g_object_weak_ref(G_OBJECT(self), on_image_viewer_destroyed, ctx);

  g_debug("ImageViewer: fetching image: %s", url);
  soup_session_send_and_read_async(
    session,
    msg,
    G_PRIORITY_DEFAULT,
    self->cancellable,
    on_image_loaded,
    ctx
  );

  g_object_unref(msg);
#else
  g_warning("ImageViewer: libsoup3 not available, cannot load remote images");
#endif
}

void gnostr_image_viewer_set_texture(GnostrImageViewer *self, GdkTexture *texture) {
  g_return_if_fail(GNOSTR_IS_IMAGE_VIEWER(self));
  g_return_if_fail(GDK_IS_TEXTURE(texture));

#ifdef HAVE_SOUP3
  /* Cancel any pending HTTP request since we already have the texture */
  if (self->cancellable) {
    g_cancellable_cancel(self->cancellable);
    g_clear_object(&self->cancellable);
  }
#endif

  g_clear_object(&self->texture);
  self->texture = g_object_ref(texture);

  if (GTK_IS_PICTURE(self->picture)) {
    gtk_picture_set_paintable(GTK_PICTURE(self->picture), GDK_PAINTABLE(texture));
  }

  /* Hide spinner in case it was shown by a prior set_image_url */
  if (GTK_IS_WIDGET(self->spinner)) {
    gtk_widget_set_visible(self->spinner, FALSE);
    gtk_spinner_stop(GTK_SPINNER(self->spinner));
  }

  /* Apply initial fit zoom */
  zoom_to_fit(self);
}

void gnostr_image_viewer_set_url_hint(GnostrImageViewer *self, const char *url) {
  g_return_if_fail(GNOSTR_IS_IMAGE_VIEWER(self));
  g_clear_pointer(&self->image_url, g_free);
  self->image_url = url ? g_strdup(url) : NULL;
}

static void update_nav_display(GnostrImageViewer *self) {
  GtkWidget *nav_box = g_object_get_data(G_OBJECT(self->overlay), "nav-box");

  if (self->gallery_count <= 1) {
    /* Hide navigation for single images */
    if (nav_box) gtk_widget_set_visible(nav_box, FALSE);
    return;
  }

  /* Show navigation */
  if (nav_box) gtk_widget_set_visible(nav_box, TRUE);

  /* Update label */
  g_autofree char *text = g_strdup_printf("%u / %u", self->gallery_index + 1, self->gallery_count);
  gtk_label_set_text(GTK_LABEL(self->nav_label), text);

  /* Update button sensitivity */
  gtk_widget_set_sensitive(self->prev_button, self->gallery_index > 0);
  gtk_widget_set_sensitive(self->next_button, self->gallery_index < self->gallery_count - 1);
}

static void on_prev_clicked(GtkButton *button, gpointer user_data) {
  (void)button;
  GnostrImageViewer *self = GNOSTR_IMAGE_VIEWER(user_data);
  gnostr_image_viewer_navigate(self, -1);
}

static void on_next_clicked(GtkButton *button, gpointer user_data) {
  (void)button;
  GnostrImageViewer *self = GNOSTR_IMAGE_VIEWER(user_data);
  gnostr_image_viewer_navigate(self, 1);
}

/* Extract filename from URL for save dialog */
static char *get_filename_from_url(const char *url) {
  if (!url || !*url) return g_strdup("image.jpg");

  const char *last_slash = strrchr(url, '/');
  if (last_slash && *(last_slash + 1)) {
    const char *filename = last_slash + 1;
    /* Remove query string if present */
    const char *query = strchr(filename, '?');
    if (query) {
      return g_strndup(filename, query - filename);
    }
    return g_strdup(filename);
  }
  return g_strdup("image.jpg");
}

#ifdef HAVE_SOUP3
static void on_save_response(GObject *source, GAsyncResult *result, gpointer user_data);
#endif

static void on_save_clicked(GtkButton *button, gpointer user_data) {
  (void)button;
  GnostrImageViewer *self = GNOSTR_IMAGE_VIEWER(user_data);

  if (!self->texture && !self->image_url) {
    g_warning("ImageViewer: No image to save");
    return;
  }

  /* Get parent window for dialog */
  GtkWindow *parent = GTK_WINDOW(self);

  /* Create file chooser dialog */
  GtkFileDialog *dialog = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dialog, "Save Image");

  /* Suggest filename based on URL */
  char *suggested_name = get_filename_from_url(self->image_url);
  gtk_file_dialog_set_initial_name(dialog, suggested_name);
  g_free(suggested_name);

  /* If we have a texture, save it directly */
  if (self->texture) {
    gtk_file_dialog_save(dialog, parent, NULL,
                         (GAsyncReadyCallback)on_save_response, self);
  }
#ifdef HAVE_SOUP3
  else if (self->image_url) {
    /* Download and save */
    gtk_file_dialog_save(dialog, parent, NULL,
                         (GAsyncReadyCallback)on_save_response, self);
  }
#endif

  g_object_unref(dialog);
}

static void on_save_response(GObject *source, GAsyncResult *result, gpointer user_data) {
  GtkFileDialog *dialog = GTK_FILE_DIALOG(source);
  GnostrImageViewer *self = GNOSTR_IMAGE_VIEWER(user_data);

  GError *error = NULL;
  GFile *file = gtk_file_dialog_save_finish(dialog, result, &error);

  if (error) {
    if (!g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_CANCELLED)) {
      g_warning("ImageViewer: Save dialog error: %s", error->message);
    }
    g_error_free(error);
    return;
  }

  if (!file) return;

  /* Save the texture to file */
  if (self->texture) {
    char *path = g_file_get_path(file);
    if (path) {
      gboolean saved = gdk_texture_save_to_png(self->texture, path);
      if (!saved) {
        g_warning("ImageViewer: Failed to save image to %s", path);
      }
      g_free(path);
    }
  }

  g_object_unref(file);
}

static void on_copy_link_clicked(GtkButton *button, gpointer user_data) {
  (void)button;
  GnostrImageViewer *self = GNOSTR_IMAGE_VIEWER(user_data);

  if (!self->image_url || !*self->image_url) {
    g_warning("ImageViewer: No image URL to copy");
    return;
  }

  /* Get the clipboard and set the URL */
  GdkDisplay *display = gtk_widget_get_display(GTK_WIDGET(self));
  GdkClipboard *clipboard = gdk_display_get_clipboard(display);
  gdk_clipboard_set_text(clipboard, self->image_url);

  /* Show toast via the main window */
  GtkWindow *parent = gtk_window_get_transient_for(GTK_WINDOW(self));
  if (parent) {
    gnostr_main_window_show_toast(GTK_WIDGET(parent), "Link copied");
  }
}

void gnostr_image_viewer_set_gallery(GnostrImageViewer *self,
                                     const char * const *urls,
                                     guint current_index) {
  g_return_if_fail(GNOSTR_IS_IMAGE_VIEWER(self));

  /* Free old gallery */
  g_strfreev(self->gallery_urls);
  self->gallery_urls = NULL;
  self->gallery_count = 0;
  self->gallery_index = 0;

  if (!urls || !urls[0]) {
    update_nav_display(self);
    return;
  }

  /* Count URLs */
  guint count = 0;
  while (urls[count]) count++;

  /* Copy URLs */
  self->gallery_urls = g_strdupv((char **)urls);
  self->gallery_count = count;
  self->gallery_index = (current_index < count) ? current_index : 0;

  /* Load the current image */
  gnostr_image_viewer_set_image_url(self, self->gallery_urls[self->gallery_index]);

  /* Update navigation display */
  update_nav_display(self);
}

gboolean gnostr_image_viewer_navigate(GnostrImageViewer *self, int delta) {
  g_return_val_if_fail(GNOSTR_IS_IMAGE_VIEWER(self), FALSE);

  if (self->gallery_count <= 1) return FALSE;

  int new_index = (int)self->gallery_index + delta;
  if (new_index < 0 || new_index >= (int)self->gallery_count) {
    return FALSE;
  }

  self->gallery_index = (guint)new_index;

  /* Load the new image */
  gnostr_image_viewer_set_image_url(self, self->gallery_urls[self->gallery_index]);

  /* Update navigation display */
  update_nav_display(self);

  /* Reset zoom to fit */
  zoom_to_fit(self);

  return TRUE;
}

void gnostr_image_viewer_present(GnostrImageViewer *self) {
  g_return_if_fail(GNOSTR_IS_IMAGE_VIEWER(self));

  /* Size the viewer to fit within the parent window bounds */
  GtkWindow *parent = gtk_window_get_transient_for(GTK_WINDOW(self));
  if (parent) {
    int parent_width = gtk_widget_get_width(GTK_WIDGET(parent));
    int parent_height = gtk_widget_get_height(GTK_WIDGET(parent));

    /* Constrain viewer to exactly parent window size */
    /* This ensures the modal cannot extend beyond the gnostr window (nostrc-zqb) */
    int viewer_width = MAX(400, parent_width);
    int viewer_height = MAX(300, parent_height);

    /* Set default size to match parent bounds exactly */
    gtk_window_set_default_size(GTK_WINDOW(self), viewer_width, viewer_height);
  } else {
    /* Fallback: use a reasonable default size if no parent */
    gtk_window_set_default_size(GTK_WINDOW(self), 900, 700);
  }

  /* Present the window */
  gtk_window_present(GTK_WINDOW(self));

  /* Apply zoom after window is realized */
  if (self->texture) {
    apply_zoom(self);
  }

  /* Update navigation display */
  update_nav_display(self);
}
