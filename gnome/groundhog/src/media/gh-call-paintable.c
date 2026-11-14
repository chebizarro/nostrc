#include "gh-call-paintable.h"
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>

struct _GhCallPaintable {
  GObject parent;
  GMainContext *context;
  GdkTexture *texture;
  GMutex lock;
  GstSample *pending;
  GSource *dispatch;
  gboolean stopped;
};
static void paintable_init(GdkPaintableInterface *iface);
G_DEFINE_TYPE_WITH_CODE(GhCallPaintable, gh_call_paintable, G_TYPE_OBJECT,
                       G_IMPLEMENT_INTERFACE(GDK_TYPE_PAINTABLE, paintable_init))

static void snapshot(GdkPaintable *paintable, GdkSnapshot *snapshot, double w, double h)
{
  GhCallPaintable *self = GH_CALL_PAINTABLE(paintable);
  if (self->texture) gdk_paintable_snapshot(GDK_PAINTABLE(self->texture), snapshot, w, h);
}
static int width(GdkPaintable *p) {
  GhCallPaintable *s = GH_CALL_PAINTABLE(p);
  return s->texture ? gdk_texture_get_width(s->texture) : 0;
}
static int height(GdkPaintable *p) {
  GhCallPaintable *s = GH_CALL_PAINTABLE(p);
  return s->texture ? gdk_texture_get_height(s->texture) : 0;
}
static GdkPaintable *current(GdkPaintable *p) {
  GhCallPaintable *s = GH_CALL_PAINTABLE(p);
  return s->texture ? g_object_ref(GDK_PAINTABLE(s->texture)) : gdk_paintable_new_empty(0, 0);
}
static void paintable_init(GdkPaintableInterface *iface) {
  iface->snapshot = snapshot;
  iface->get_intrinsic_width = width;
  iface->get_intrinsic_height = height;
  iface->get_current_image = current;
}

static gboolean present(gpointer data)
{
  GhCallPaintable *self = data;
  g_mutex_lock(&self->lock);
  GstSample *sample = g_steal_pointer(&self->pending);
  g_clear_pointer(&self->dispatch, g_source_unref);
  gboolean stopped = self->stopped;
  g_mutex_unlock(&self->lock);
  if (sample && !stopped) {
    GstVideoInfo info;
    GstVideoFrame frame;
    if (gst_video_info_from_caps(&info, gst_sample_get_caps(sample)) &&
        gst_video_frame_map(&frame, &info, gst_sample_get_buffer(sample), GST_MAP_READ)) {
      gint width = GST_VIDEO_INFO_WIDTH(&info);
      gint height = GST_VIDEO_INFO_HEIGHT(&info);
      gsize stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
      GBytes *bytes = g_bytes_new(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0),
                                  stride * height);
      GdkTexture *texture = gdk_memory_texture_new(width,
          height, GDK_MEMORY_R8G8B8A8, bytes, stride);
      gboolean resized = !self->texture ||
          gdk_texture_get_width(self->texture) != gdk_texture_get_width(texture) ||
          gdk_texture_get_height(self->texture) != gdk_texture_get_height(texture);
      g_set_object(&self->texture, texture);
      g_object_unref(texture);
      g_bytes_unref(bytes);
      gst_video_frame_unmap(&frame);
      if (resized) gdk_paintable_invalidate_size(GDK_PAINTABLE(self));
      gdk_paintable_invalidate_contents(GDK_PAINTABLE(self));
    }
  }
  if (sample) gst_sample_unref(sample);
  return G_SOURCE_REMOVE;
}
static GstFlowReturn sample_ready(GstAppSink *sink, gpointer data)
{
  GhCallPaintable *self = data;
  GstSample *sample = gst_app_sink_pull_sample(sink);
  g_mutex_lock(&self->lock);
  if (!self->stopped) {
    g_clear_pointer(&self->pending, gst_sample_unref);
    self->pending = g_steal_pointer(&sample);
    if (!self->dispatch) {
      self->dispatch = g_idle_source_new();
      g_source_set_callback(self->dispatch, present, g_object_ref(self), g_object_unref);
      g_source_attach(self->dispatch, self->context);
    }
  }
  g_mutex_unlock(&self->lock);
  if (sample) gst_sample_unref(sample);
  return GST_FLOW_OK;
}
void gh_call_paintable_stop(GhCallPaintable *self)
{
  g_mutex_lock(&self->lock);
  self->stopped = TRUE;
  if (self->dispatch) g_source_destroy(self->dispatch);
  g_clear_pointer(&self->dispatch, g_source_unref);
  g_clear_pointer(&self->pending, gst_sample_unref);
  g_mutex_unlock(&self->lock);
  g_clear_object(&self->texture);
  gdk_paintable_invalidate_size(GDK_PAINTABLE(self));
  gdk_paintable_invalidate_contents(GDK_PAINTABLE(self));
}
static void dispose(GObject *object) {
  gh_call_paintable_stop(GH_CALL_PAINTABLE(object));
  G_OBJECT_CLASS(gh_call_paintable_parent_class)->dispose(object);
}
static void finalize(GObject *object) {
  GhCallPaintable *self = GH_CALL_PAINTABLE(object);
  g_main_context_unref(self->context);
  g_mutex_clear(&self->lock);
  G_OBJECT_CLASS(gh_call_paintable_parent_class)->finalize(object);
}
static void gh_call_paintable_class_init(GhCallPaintableClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = dispose;
  G_OBJECT_CLASS(klass)->finalize = finalize;
}
static void gh_call_paintable_init(GhCallPaintable *self) { g_mutex_init(&self->lock); }
GhCallPaintable *gh_call_paintable_new(GMainContext *context) {
  GhCallPaintable *self = g_object_new(GH_TYPE_CALL_PAINTABLE, NULL);
  self->context = g_main_context_ref(context);
  return self;
}
GstElement *gh_call_paintable_create_sink(GhCallPaintable *self) {
  GstElement *sink = gst_element_factory_make("appsink", NULL);
  GstCaps *caps = gst_caps_from_string("video/x-raw,format=RGBA");
  g_object_set(sink, "caps", caps, "max-buffers", 1u, "drop", TRUE,
               "enable-last-sample", FALSE, NULL);
  gst_caps_unref(caps);
  GstAppSinkCallbacks callbacks = { .new_sample = sample_ready };
  gst_app_sink_set_callbacks(GST_APP_SINK(sink), &callbacks, g_object_ref(self), g_object_unref);
  return sink;
}
