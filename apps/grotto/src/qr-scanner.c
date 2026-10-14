#include "qr-scanner.h"
#include <string.h>
#include <errno.h>
#ifdef HAVE_GSTREAMER
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#endif

struct _GnQrScanner {
  GtkWidget parent_instance;
  GtkWidget *stack, *picture, *status;
  GnQrContentType *accepted;
  guint n_accepted, generation;
  gboolean active;
#ifdef HAVE_GSTREAMER
  GstElement *pipeline;
  guint watch;
  gint frame_pending;
  GMainContext *context;
#endif
};
G_DEFINE_TYPE(GnQrScanner, gn_qr_scanner, GTK_TYPE_WIDGET)
enum { QR_DETECTED, SCAN_ERROR, N_SIGNALS };
static guint signals[N_SIGNALS];

gboolean gn_qr_camera_available(void) {
#ifdef HAVE_GSTREAMER
  if (!gst_init_check(NULL, NULL, NULL)) return FALSE;
#ifdef __linux__
  const gchar *source = "v4l2src";
#elif defined(__APPLE__)
  const gchar *source = "avfvideosrc";
#else
  const gchar *source = "ksvideosrc";
#endif
  GstElementFactory *factory = gst_element_factory_find(source);
  if (!factory) return FALSE;
  gst_object_unref(factory); return TRUE;
#else
  return FALSE;
#endif
}
gchar **gn_qr_list_cameras(void) {
  GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
#ifdef HAVE_GSTREAMER
  if (gst_init_check(NULL, NULL, NULL)) {
    GstDeviceMonitor *monitor = gst_device_monitor_new();
    gst_device_monitor_add_filter(monitor, "Video/Source", NULL);
    if (gst_device_monitor_start(monitor)) {
      GList *devices = gst_device_monitor_get_devices(monitor);
      for (GList *it = devices; it; it = it->next) g_ptr_array_add(names, gst_device_get_display_name(it->data));
      g_list_free_full(devices, gst_object_unref); gst_device_monitor_stop(monitor);
    }
    gst_object_unref(monitor);
  }
#endif
  g_ptr_array_add(names, NULL); return (gchar **)g_ptr_array_free(names, FALSE);
}
static void unavailable(GnQrScanner *self, const gchar *message) {
  gtk_label_set_text(GTK_LABEL(self->status), message);
  gtk_stack_set_visible_child_name(GTK_STACK(self->stack), "unavailable");
}
#ifdef HAVE_GSTREAMER
typedef struct {
  GnQrScanner *scanner;
  guint generation;
  GdkPixbuf *frame;
  GnQrScanResult *result;
} Frame;
static void frame_free(gpointer data) {
  Frame *f = data; g_clear_object(&f->frame); gn_qr_scan_result_free(f->result);
  g_atomic_int_set(&f->scanner->frame_pending, 0);
  g_object_unref(f->scanner); g_free(f);
}
static gboolean frame_deliver(gpointer data) {
  Frame *f = data; GnQrScanner *s = f->scanner;
  if (f->generation != s->generation) return G_SOURCE_REMOVE;
  g_autoptr(GdkTexture) texture = gdk_texture_new_for_pixbuf(f->frame);
  gtk_picture_set_paintable(GTK_PICTURE(s->picture), GDK_PAINTABLE(texture));
  if (f->result) {
    gboolean accept = !s->n_accepted;
    for (guint i = 0; i < s->n_accepted; i++) if (s->accepted[i] == f->result->type) accept = TRUE;
    if (accept) g_signal_emit(s, signals[QR_DETECTED], 0, f->result);
  }
  return G_SOURCE_REMOVE;
}
static GstFlowReturn sample_received(GstAppSink *sink, gpointer data) {
  GnQrScanner *s = data;
  GstSample *sample = gst_app_sink_pull_sample(sink);
  if (!sample) return GST_FLOW_EOS;
  if (!g_atomic_int_compare_and_exchange(&s->frame_pending, 0, 1)) { gst_sample_unref(sample); return GST_FLOW_OK; }
  GstVideoInfo info; GstVideoFrame mapped;
  if (!gst_video_info_from_caps(&info, gst_sample_get_caps(sample)) ||
      GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_RGB ||
      !gst_video_frame_map(&mapped, &info, gst_sample_get_buffer(sample), GST_MAP_READ)) {
    gst_sample_unref(sample); g_atomic_int_set(&s->frame_pending, 0); return GST_FLOW_ERROR;
  }
  Frame *f = g_new0(Frame, 1); f->scanner = g_object_ref(s); f->generation = s->generation;
  f->frame = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, info.width, info.height);
  if (f->frame) {
    for (guint y = 0; y < info.height; y++) memcpy(gdk_pixbuf_get_pixels(f->frame) + y * gdk_pixbuf_get_rowstride(f->frame),
        (guint8 *)GST_VIDEO_FRAME_PLANE_DATA(&mapped, 0) + y * GST_VIDEO_FRAME_PLANE_STRIDE(&mapped, 0), info.width * 3);
    f->result = gn_qr_scan_pixbuf(f->frame, NULL);
  }
  gst_video_frame_unmap(&mapped); gst_sample_unref(sample);
  if (!f->frame) { frame_free(f); return GST_FLOW_ERROR; }
  GSource *idle = g_idle_source_new(); g_source_set_callback(idle, frame_deliver, f, frame_free);
  g_source_attach(idle, s->context); g_source_unref(idle); return GST_FLOW_OK;
}
static const gchar *camera_error_message(GError *error, const GstStructure *details) {
  gint system_errno = 0;
  if (details) gst_structure_get_int(details, "errno", &system_errno);
  if (error && error->domain == GST_RESOURCE_ERROR) {
    if (error->code == GST_RESOURCE_ERROR_NOT_AUTHORIZED || system_errno == EACCES || system_errno == EPERM)
      return "Camera access was denied. Paste a link instead.";
    if (error->code == GST_RESOURCE_ERROR_BUSY || system_errno == EBUSY)
      return "The camera is busy in another application. Paste a link instead.";
    if (error->code == GST_RESOURCE_ERROR_NOT_FOUND || system_errno == ENOENT)
      return "No camera was found. Paste a link instead.";
    if (error->code == GST_RESOURCE_ERROR_OPEN_READ || error->code == GST_RESOURCE_ERROR_OPEN_READ_WRITE)
      return "Camera access is unavailable. Check camera permissions, or Paste a link instead.";
  }
  return "The camera could not be opened. Check access and other camera applications, or paste a link.";
}
static gboolean bus_message(GstBus *bus, GstMessage *message, gpointer data) {
  (void)bus; GnQrScanner *s = data;
  if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
    GError *raw = NULL; const GstStructure *details = NULL;
    gst_message_parse_error(message, &raw, NULL); gst_message_parse_error_details(message, &details);
    g_autoptr(GError) error = g_error_new_literal(GN_QR_ERROR, GN_QR_ERROR_CAMERA_UNAVAILABLE, camera_error_message(raw, details));
    g_clear_error(&raw); s->watch = 0;
    gn_qr_scanner_stop(s); unavailable(s, error->message);
    g_signal_emit(s, signals[SCAN_ERROR], 0, error); return G_SOURCE_REMOVE;
  }
  if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) s->active = FALSE;
  return G_SOURCE_CONTINUE;
}
static gboolean start_pipeline(GnQrScanner *s, const gchar *description, GError **error) {
  gn_qr_scanner_stop(s);
  if (!gst_init_check(NULL, NULL, error)) return FALSE;
  GError *parse = NULL; s->pipeline = gst_parse_launch(description, &parse);
  if (parse || !s->pipeline) {
    g_clear_pointer(&s->pipeline, gst_object_unref);
    if (parse) g_propagate_error(error, parse); return FALSE;
  }
  GstElement *sink = gst_bin_get_by_name(GST_BIN(s->pipeline), "scan_sink");
  if (!sink || !GST_IS_APP_SINK(sink)) {
    if (sink) gst_object_unref(sink); gn_qr_scanner_stop(s);
    g_set_error_literal(error, GN_QR_ERROR, GN_QR_ERROR_CAMERA_UNAVAILABLE, "Camera frame decoder is unavailable"); return FALSE;
  }
  g_signal_connect(sink, "new-sample", G_CALLBACK(sample_received), s); gst_object_unref(sink);
  GstBus *bus = gst_element_get_bus(s->pipeline); s->watch = gst_bus_add_watch(bus, bus_message, s); gst_object_unref(bus);
  s->active = TRUE;
  if (gst_element_set_state(s->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    GstBus *failed_bus = gst_element_get_bus(s->pipeline);
    GstMessage *failure = gst_bus_pop_filtered(failed_bus, GST_MESSAGE_ERROR);
    GError *raw = NULL; const GstStructure *details = NULL;
    if (failure) { gst_message_parse_error(failure, &raw, NULL); gst_message_parse_error_details(failure, &details); }
    const gchar *text = camera_error_message(raw, details);
    gn_qr_scanner_stop(s); unavailable(s, text);
    g_set_error_literal(error, GN_QR_ERROR, GN_QR_ERROR_CAMERA_UNAVAILABLE, text);
    g_clear_error(&raw); if (failure) gst_message_unref(failure); gst_object_unref(failed_bus);
    return FALSE;
  }
  gtk_stack_set_visible_child_name(GTK_STACK(s->stack), "preview"); return TRUE;
}
#ifdef GROTTO_QR_TESTING
gboolean gn_qr_scanner_start_test_pipeline(GnQrScanner *s, const gchar *description, GError **error) {
  return start_pipeline(s, description, error);
}
gboolean gn_qr_scanner_test_has_pipeline(GnQrScanner *s) { return s->pipeline != NULL; }
void gn_qr_scanner_test_post_error(GnQrScanner *s, GError *error) {
  g_return_if_fail(s->pipeline != NULL);
  gst_element_post_message(s->pipeline, gst_message_new_error(GST_OBJECT(s->pipeline), error, NULL));
}
#endif
#endif

gboolean gn_qr_scanner_start(GnQrScanner *s, GError **error) {
  g_return_val_if_fail(GN_IS_QR_SCANNER(s), FALSE);
  if (s->active) return TRUE;
#ifdef HAVE_GSTREAMER
  if (gn_qr_camera_available()) {
#ifdef __linux__
    const gchar *source = "v4l2src";
#elif defined(__APPLE__)
    const gchar *source = "avfvideosrc";
#else
    const gchar *source = "ksvideosrc";
#endif
    g_autofree gchar *description = g_strdup_printf("%s ! videoconvert ! videoscale ! video/x-raw,format=RGB,width=640,height=480 ! appsink name=scan_sink emit-signals=true max-buffers=1 drop=true sync=false", source);
    return start_pipeline(s, description, error);
  }
#endif
  unavailable(s, "Camera support is unavailable. Paste a link instead.");
  g_set_error_literal(error, GN_QR_ERROR, GN_QR_ERROR_CAMERA_UNAVAILABLE, "Camera support is unavailable. Paste a link instead."); return FALSE;
}
void gn_qr_scanner_stop(GnQrScanner *s) {
  g_return_if_fail(GN_IS_QR_SCANNER(s));
#ifdef HAVE_GSTREAMER
  if (s->pipeline) {
    /* Join streaming callbacks before changing the generation they snapshot. */
    gst_element_set_state(s->pipeline, GST_STATE_NULL);
    g_clear_pointer(&s->pipeline, gst_object_unref);
  }
  if (s->watch) { g_source_remove(s->watch); s->watch = 0; }
#endif
  s->generation++; s->active = FALSE;
}
gboolean gn_qr_scanner_is_active(GnQrScanner *s) { return s->active; }
void gn_qr_scanner_set_accepted_types(GnQrScanner *s, const GnQrContentType *types) {
  g_free(s->accepted); s->accepted = NULL; s->n_accepted = 0;
  if (!types) return;
  while (types[s->n_accepted] != GN_QR_TYPE_UNKNOWN) s->n_accepted++;
  s->accepted = g_memdup2(types, s->n_accepted * sizeof(*types));
}
static void scanner_dispose(GObject *object) {
  GnQrScanner *s = GN_QR_SCANNER(object); gn_qr_scanner_stop(s);
  g_clear_pointer(&s->stack, gtk_widget_unparent);
  G_OBJECT_CLASS(gn_qr_scanner_parent_class)->dispose(object);
}
static void scanner_finalize(GObject *object) {
  GnQrScanner *s = GN_QR_SCANNER(object); g_free(s->accepted);
#ifdef HAVE_GSTREAMER
  g_main_context_unref(s->context);
#endif
  G_OBJECT_CLASS(gn_qr_scanner_parent_class)->finalize(object);
}
static void gn_qr_scanner_class_init(GnQrScannerClass *klass) {
  GObjectClass *object = G_OBJECT_CLASS(klass); object->dispose = scanner_dispose; object->finalize = scanner_finalize;
  gtk_widget_class_set_layout_manager_type(GTK_WIDGET_CLASS(klass), GTK_TYPE_BIN_LAYOUT);
  signals[QR_DETECTED] = g_signal_new("qr-detected", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_POINTER);
  signals[SCAN_ERROR] = g_signal_new("error", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_ERROR);
}
static void gn_qr_scanner_init(GnQrScanner *s) {
#ifdef HAVE_GSTREAMER
  s->context = g_main_context_ref_thread_default();
#endif
  s->stack = gtk_stack_new(); gtk_widget_set_parent(s->stack, GTK_WIDGET(s));
  s->picture = gtk_picture_new(); gtk_widget_set_size_request(s->picture, 320, 240);
  gtk_stack_add_named(GTK_STACK(s->stack), s->picture, "preview");
  s->status = gtk_label_new("Choose Scan to use your camera, or paste a link."); gtk_label_set_wrap(GTK_LABEL(s->status), TRUE);
  gtk_stack_add_named(GTK_STACK(s->stack), s->status, "unavailable");
  gtk_stack_set_visible_child_name(GTK_STACK(s->stack), "unavailable");
}
GnQrScanner *gn_qr_scanner_new(void) { return g_object_new(GN_TYPE_QR_SCANNER, NULL); }
