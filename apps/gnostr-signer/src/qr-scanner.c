/* qr-scanner.c - Camera-based QR code scanner for gnostr-signer
 *
 * Uses GStreamer for camera access where available, with fallback
 * to platform-specific APIs (AVFoundation on macOS, etc.).
 *
 * When camera is not available, provides a fallback mode that
 * allows importing from image files or clipboard.
 */
#include "qr-scanner.h"
#include "qr-code.h"

#include <gtk/gtk.h>

/* GStreamer support - optional */
#ifdef HAVE_GSTREAMER
#include <gst/gst.h>
#include <gst/video/videooverlay.h>
#define CAMERA_AVAILABLE 1
#else
#define CAMERA_AVAILABLE 0
#endif

/* Scanner state */
typedef enum {
  SCANNER_STATE_IDLE,
  SCANNER_STATE_STARTING,
  SCANNER_STATE_RUNNING,
  SCANNER_STATE_STOPPING,
  SCANNER_STATE_ERROR
} ScannerState;

struct _GnQrScanner {
  GtkWidget parent_instance;

  /* State */
  ScannerState state;
  GError *last_error;

  /* Accepted content types (NULL = accept all) */
  GnQrContentType *accepted_types;
  gsize n_accepted_types;

  /* UI components */
  GtkWidget *stack;           /* Stack for preview/placeholder */
  GtkWidget *preview_area;    /* Camera preview drawing area */
  GtkWidget *placeholder;     /* Shown when camera unavailable */
  GtkWidget *status_label;    /* Status text */

#if CAMERA_AVAILABLE
  /* GStreamer elements */
  GstElement *pipeline;
  GstElement *camera_source;
  GstElement *video_sink;
  guint bus_watch_id;

  /* Frame processing */
  guint scan_timer_id;
  GdkPixbuf *last_frame;
#endif
};

G_DEFINE_TYPE(GnQrScanner, gn_qr_scanner, GTK_TYPE_WIDGET)

/* Signals */
enum {
  SIGNAL_QR_DETECTED,
  SIGNAL_ERROR,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

/* Forward declarations */
static void gn_qr_scanner_dispose(GObject *object);
static void gn_qr_scanner_finalize(GObject *object);
static void setup_ui(GnQrScanner *self);

/* ============================================================
 * Camera Availability
 * ============================================================ */

gboolean gn_qr_camera_available(void) {
#if CAMERA_AVAILABLE
  static gboolean initialized = FALSE;
  static gboolean available = FALSE;

  if (!initialized) {
    initialized = TRUE;
    /* Check if GStreamer video capture is available */
    GError *error = NULL;
    if (!gst_init_check(NULL, NULL, &error)) {
      if (error) g_error_free(error);
      available = FALSE;
    } else {
      /* Try to create a camera source to check availability */
      GstElement *test = gst_element_factory_make("v4l2src", NULL);
      if (!test) {
        test = gst_element_factory_make("avfvideosrc", NULL);
      }
      if (!test) {
        test = gst_element_factory_make("ksvideosrc", NULL);
      }
      if (test) {
        gst_object_unref(test);
        available = TRUE;
      }
    }
  }

  return available;
#else
  return FALSE;
#endif
}

gchar **gn_qr_list_cameras(void) {
#if CAMERA_AVAILABLE
  /* This is a simplified implementation - a full version would
   * enumerate devices using platform-specific APIs */
  GPtrArray *cameras = g_ptr_array_new();

  /* Add default camera */
  g_ptr_array_add(cameras, g_strdup("Default Camera"));

  g_ptr_array_add(cameras, NULL);
  return (gchar **)g_ptr_array_free(cameras, FALSE);
#else
  return g_new0(gchar *, 1);
#endif
}

/* ============================================================
 * GnQrScanner Implementation
 * ============================================================ */

static void gn_qr_scanner_class_init(GnQrScannerClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = gn_qr_scanner_dispose;
  object_class->finalize = gn_qr_scanner_finalize;

  /* Set layout manager */
  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);

  /* Signals */
  signals[SIGNAL_QR_DETECTED] = g_signal_new(
    "qr-detected",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0,
    NULL, NULL,
    NULL,
    G_TYPE_NONE,
    1,
    G_TYPE_POINTER  /* GnQrScanResult* */
  );

  signals[SIGNAL_ERROR] = g_signal_new(
    "error",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0,
    NULL, NULL,
    NULL,
    G_TYPE_NONE,
    1,
    G_TYPE_POINTER  /* GError* */
  );
}

static void gn_qr_scanner_init(GnQrScanner *self) {
  self->state = SCANNER_STATE_IDLE;
  self->last_error = NULL;
  self->accepted_types = NULL;
  self->n_accepted_types = 0;

#if CAMERA_AVAILABLE
  self->pipeline = NULL;
  self->camera_source = NULL;
  self->video_sink = NULL;
  self->bus_watch_id = 0;
  self->scan_timer_id = 0;
  self->last_frame = NULL;
#endif

  setup_ui(self);
}

static void setup_ui(GnQrScanner *self) {
  /* Create stack for switching between preview and placeholder */
  self->stack = gtk_stack_new();
  gtk_widget_set_parent(self->stack, GTK_WIDGET(self));

  /* Preview area for camera */
  self->preview_area = gtk_drawing_area_new();
  gtk_widget_set_hexpand(self->preview_area, TRUE);
  gtk_widget_set_vexpand(self->preview_area, TRUE);
  gtk_widget_set_size_request(self->preview_area, 320, 240);
  gtk_stack_add_named(GTK_STACK(self->stack), self->preview_area, "preview");

  /* Placeholder when camera is unavailable */
  GtkWidget *placeholder_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_valign(placeholder_box, GTK_ALIGN_CENTER);
  gtk_widget_set_halign(placeholder_box, GTK_ALIGN_CENTER);

  GtkWidget *icon = gtk_image_new_from_icon_name("camera-disabled-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(icon), 64);
  gtk_widget_add_css_class(icon, "dim-label");
  gtk_box_append(GTK_BOX(placeholder_box), icon);

  self->status_label = gtk_label_new("Camera not available");
  gtk_widget_add_css_class(self->status_label, "dim-label");
  gtk_box_append(GTK_BOX(placeholder_box), self->status_label);

  GtkWidget *hint = gtk_label_new("Paste an image from clipboard instead");
  gtk_widget_add_css_class(hint, "dim-label");
  gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
  gtk_box_append(GTK_BOX(placeholder_box), hint);

  self->placeholder = placeholder_box;
  gtk_stack_add_named(GTK_STACK(self->stack), self->placeholder, "placeholder");

  /* Show appropriate page */
  if (gn_qr_camera_available()) {
    gtk_stack_set_visible_child_name(GTK_STACK(self->stack), "preview");
  } else {
    gtk_stack_set_visible_child_name(GTK_STACK(self->stack), "placeholder");
  }
}

static void gn_qr_scanner_dispose(GObject *object) {
  GnQrScanner *self = GN_QR_SCANNER(object);

  gn_qr_scanner_stop(self);

  g_clear_pointer(&self->stack, gtk_widget_unparent);

  G_OBJECT_CLASS(gn_qr_scanner_parent_class)->dispose(object);
}

static void gn_qr_scanner_finalize(GObject *object) {
  GnQrScanner *self = GN_QR_SCANNER(object);

  g_clear_error(&self->last_error);
  g_free(self->accepted_types);

#if CAMERA_AVAILABLE
  g_clear_object(&self->last_frame);
#endif

  G_OBJECT_CLASS(gn_qr_scanner_parent_class)->finalize(object);
}

GnQrScanner *gn_qr_scanner_new(void) {
  return g_object_new(GN_TYPE_QR_SCANNER, NULL);
}

#if CAMERA_AVAILABLE

/* Check if scanned result matches accepted types */
static gboolean is_type_accepted(GnQrScanner *self, GnQrContentType type) {
  if (!self->accepted_types || self->n_accepted_types == 0) {
    return TRUE; /* Accept all */
  }

  for (gsize i = 0; i < self->n_accepted_types; i++) {
    if (self->accepted_types[i] == type) {
      return TRUE;
    }
  }

  return FALSE;
}

/* Process a frame for QR codes */
static gboolean process_frame(GnQrScanner *self);

/* Proper GSourceFunc wrapper for process_frame (ABI-safe) */
static gboolean process_frame_timeout_cb(gpointer data) {
  return process_frame(GN_QR_SCANNER(data));
}

static gboolean process_frame(GnQrScanner *self) {
  if (self->state != SCANNER_STATE_RUNNING) {
    return G_SOURCE_REMOVE;
  }

  if (!self->last_frame) {
    return G_SOURCE_CONTINUE;
  }

  GError *error = NULL;
  GnQrScanResult *result = gn_qr_scan_pixbuf(self->last_frame, &error);

  if (result) {
    if (is_type_accepted(self, result->type)) {
      /* Emit signal with result */
      g_signal_emit(self, signals[SIGNAL_QR_DETECTED], 0, result);
    }
    gn_qr_scan_result_free(result);
  }

  /* Don't report scan errors - just keep trying */
  if (error) {
    g_error_free(error);
  }

  return G_SOURCE_CONTINUE;
}

/* GStreamer bus message handler */
static gboolean bus_callback(GstBus *bus, GstMessage *message, gpointer data) {
  (void)bus;
  GnQrScanner *self = GN_QR_SCANNER(data);

  switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_ERROR: {
      GError *error = NULL;
      gchar *debug = NULL;
      gst_message_parse_error(message, &error, &debug);

      g_clear_error(&self->last_error);
      self->last_error = error;
      self->state = SCANNER_STATE_ERROR;

      g_signal_emit(self, signals[SIGNAL_ERROR], 0, error);

      g_free(debug);
      break;
    }

    case GST_MESSAGE_EOS:
      self->state = SCANNER_STATE_IDLE;
      break;

    case GST_MESSAGE_STATE_CHANGED:
      if (GST_MESSAGE_SRC(message) == GST_OBJECT(self->pipeline)) {
        GstState old_state, new_state;
        gst_message_parse_state_changed(message, &old_state, &new_state, NULL);

        if (new_state == GST_STATE_PLAYING) {
          self->state = SCANNER_STATE_RUNNING;
        }
      }
      break;

    default:
      break;
  }

  return TRUE;
}

gboolean gn_qr_scanner_start(GnQrScanner *self,
                              GError **error) {
  g_return_val_if_fail(GN_IS_QR_SCANNER(self), FALSE);

  if (self->state == SCANNER_STATE_RUNNING) {
    return TRUE; /* Already running */
  }

  if (!gn_qr_camera_available()) {
    g_set_error(error, GN_QR_ERROR, GN_QR_ERROR_CAMERA_UNAVAILABLE,
                "Camera is not available on this system");
    return FALSE;
  }

  self->state = SCANNER_STATE_STARTING;

  /* Create GStreamer pipeline */
  /* Try different camera sources based on platform */
  const gchar *source_element = NULL;
#ifdef __linux__
  source_element = "v4l2src";
#elif defined(__APPLE__)
  source_element = "avfvideosrc";
#elif defined(_WIN32)
  source_element = "ksvideosrc";
#else
  source_element = "autovideosrc";
#endif

  gchar *pipeline_desc = g_strdup_printf(
    "%s name=source ! videoconvert ! videoscale ! "
    "video/x-raw,width=640,height=480 ! "
    "gtksink name=sink",
    source_element
  );

  GError *parse_error = NULL;
  self->pipeline = gst_parse_launch(pipeline_desc, &parse_error);
  g_free(pipeline_desc);

  if (!self->pipeline) {
    g_propagate_error(error, parse_error);
    self->state = SCANNER_STATE_ERROR;
    return FALSE;
  }

  /* Get elements */
  self->camera_source = gst_bin_get_by_name(GST_BIN(self->pipeline), "source");
  self->video_sink = gst_bin_get_by_name(GST_BIN(self->pipeline), "sink");

  /* Set up bus watch */
  GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(self->pipeline));
  self->bus_watch_id = gst_bus_add_watch(bus, bus_callback, self);
  gst_object_unref(bus);

  /* Start pipeline */
  GstStateChangeReturn ret = gst_element_set_state(self->pipeline, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    g_set_error(error, GN_QR_ERROR, GN_QR_ERROR_CAMERA_UNAVAILABLE,
                "Failed to start camera pipeline");
    gst_object_unref(self->pipeline);
    self->pipeline = NULL;
    self->state = SCANNER_STATE_ERROR;
    return FALSE;
  }

  /* Start frame processing timer (10 FPS for scanning) */
  self->scan_timer_id = g_timeout_add(100, process_frame_timeout_cb, self);

  /* Show preview */
  gtk_stack_set_visible_child_name(GTK_STACK(self->stack), "preview");

  return TRUE;
}

void gn_qr_scanner_stop(GnQrScanner *self) {
  g_return_if_fail(GN_IS_QR_SCANNER(self));

  if (self->state == SCANNER_STATE_IDLE) {
    return;
  }

  self->state = SCANNER_STATE_STOPPING;

  /* Stop timer */
  if (self->scan_timer_id) {
    g_source_remove(self->scan_timer_id);
    self->scan_timer_id = 0;
  }

  /* Stop pipeline */
  if (self->pipeline) {
    gst_element_set_state(self->pipeline, GST_STATE_NULL);

    if (self->bus_watch_id) {
      g_source_remove(self->bus_watch_id);
      self->bus_watch_id = 0;
    }

    gst_object_unref(self->pipeline);
    self->pipeline = NULL;
    self->camera_source = NULL;
    self->video_sink = NULL;
  }

  g_clear_object(&self->last_frame);

  self->state = SCANNER_STATE_IDLE;
}

#else /* !CAMERA_AVAILABLE */

gboolean gn_qr_scanner_start(GnQrScanner *self,
                              GError **error) {
  g_return_val_if_fail(GN_IS_QR_SCANNER(self), FALSE);

  g_set_error(error, GN_QR_ERROR, GN_QR_ERROR_CAMERA_UNAVAILABLE,
              "Camera scanning is not available (GStreamer not found)");
  return FALSE;
}

void gn_qr_scanner_stop(GnQrScanner *self) {
  g_return_if_fail(GN_IS_QR_SCANNER(self));
  /* Nothing to do */
}

#endif /* CAMERA_AVAILABLE */

gboolean gn_qr_scanner_is_active(GnQrScanner *self) {
  g_return_val_if_fail(GN_IS_QR_SCANNER(self), FALSE);
  return self->state == SCANNER_STATE_RUNNING;
}

void gn_qr_scanner_set_accepted_types(GnQrScanner *self,
                                       const GnQrContentType *types) {
  g_return_if_fail(GN_IS_QR_SCANNER(self));

  g_free(self->accepted_types);
  self->accepted_types = NULL;
  self->n_accepted_types = 0;

  if (!types) return;

  /* Count types */
  gsize count = 0;
  while (types[count] != GN_QR_TYPE_UNKNOWN) {
    count++;
  }

  if (count > 0) {
    self->accepted_types = g_new(GnQrContentType, count);
    memcpy(self->accepted_types, types, count * sizeof(GnQrContentType));
    self->n_accepted_types = count;
  }
}
