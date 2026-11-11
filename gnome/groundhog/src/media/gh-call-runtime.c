/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gh-call-runtime.h"
#include "gh-call-capabilities.h"

#ifdef GROUNDHOG_HAVE_CALLS
#include "gh-call-media.h"
#endif

/* The ring cadence: on, then silent (seconds). */
#define RING_ON_MS 1000
#define RING_OFF_MS 2000

struct _GhCallRuntime {
  GObject parent_instance;
  GSettings *settings;
  gboolean probed;
  GhCallUiMedia media;
  gchar *reason;
  GObject *headset;      /* GstDevice for the next call */
  gboolean next_camera;
  GListStore *outputs;
  gboolean ringing;
#ifdef GROUNDHOG_HAVE_CALLS
  GstDeviceMonitor *monitor;
  guint monitor_watch;
  GstElement *ring;
  guint cadence;
  gboolean ring_on;
#endif
};

G_DEFINE_FINAL_TYPE(GhCallRuntime, gh_call_runtime, G_TYPE_OBJECT)

GhCallUiMedia
gh_call_runtime_probe(GhCallRuntime *self, gchar **reason)
{
  g_return_val_if_fail(GH_IS_CALL_RUNTIME(self), GH_CALL_UI_MEDIA_MISSING);
  if (!self->probed) {
    g_autoptr(GError) error = NULL;
    self->probed = TRUE;
    if (!gh_call_capabilities_check(FALSE, &error)) {
      self->media = GH_CALL_UI_MEDIA_MISSING;
      self->reason = g_strdup(error->message);
    } else if (!gh_call_capabilities_check(TRUE, &error)) {
      self->media = GH_CALL_UI_MEDIA_HEADSET_ONLY;
      self->reason = g_strdup(error->message);
    } else {
      self->media = GH_CALL_UI_MEDIA_READY;
    }
    if (self->reason)
      g_message("%s", self->reason);
  }
  if (reason)
    *reason = g_strdup(self->reason);
  return self->media;
}

void
gh_call_runtime_set_next_camera(GhCallRuntime *self, gboolean enabled)
{
  g_return_if_fail(GH_IS_CALL_RUNTIME(self));
  self->next_camera = enabled;
}

#ifdef GROUNDHOG_HAVE_CALLS
static GhCallMediaBackend *
factory(gpointer data, GError **error)
{
  GhCallRuntime *self = data;
  g_autofree gchar *reason = NULL;
  GhCallUiMedia media = gh_call_runtime_probe(self, &reason);
  if (media == GH_CALL_UI_MEDIA_MISSING) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, reason);
    return NULL;
  }
  g_autoptr(GObject) headset = g_steal_pointer(&self->headset);
  if (media == GH_CALL_UI_MEDIA_HEADSET_ONLY && !headset) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                        "Calls unavailable: no echo cancellation; choose a headset output");
    return NULL;
  }
  g_autoptr(GhCallMedia) call = gh_call_media_new(self->settings);
  gh_call_media_set_camera_enabled(call, self->next_camera);
  self->next_camera = TRUE;
  if (headset && (!gh_call_media_select_device(call, "Audio/Sink", GST_DEVICE(headset), error) ||
                  !gh_call_media_set_headset(call, TRUE, error)))
    return NULL;
  return GH_CALL_MEDIA_BACKEND(g_steal_pointer(&call));
}

static gboolean
monitor_message(GstBus *bus, GstMessage *message, gpointer data)
{
  (void)bus;
  GhCallRuntime *self = data;
  GstDevice *device = NULL;
  guint position;
  if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_DEVICE_ADDED) {
    gst_message_parse_device_added(message, &device);
    if (!g_list_store_find(self->outputs, device, &position))
      g_list_store_append(self->outputs, device);
  } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_DEVICE_REMOVED) {
    gst_message_parse_device_removed(message, &device);
    if (g_list_store_find(self->outputs, device, &position))
      g_list_store_remove(self->outputs, position);
  }
  g_clear_object(&device);
  return G_SOURCE_CONTINUE;
}

static void mute_op(GhCallMediaBackend *m, gboolean muted) { gh_call_media_set_muted(GH_CALL_MEDIA(m), muted); }
static void camera_op(GhCallMediaBackend *m, gboolean on) { gh_call_media_set_camera_enabled(GH_CALL_MEDIA(m), on); }
static GdkPaintable *remote_op(GhCallMediaBackend *m) { return gh_call_media_get_remote_video(GH_CALL_MEDIA(m)); }
static GdkPaintable *local_op(GhCallMediaBackend *m) { return gh_call_media_get_local_preview(GH_CALL_MEDIA(m)); }
static GListModel *
devices_op(GhCallMediaBackend *m, GError **error)
{
  return gh_call_media_start_devices(GH_CALL_MEDIA(m), error) ? gh_call_media_get_devices(GH_CALL_MEDIA(m)) : NULL;
}
static gboolean
select_op(GhCallMediaBackend *m, const gchar *klass, GObject *device, GError **error)
{
  return gh_call_media_select_device(GH_CALL_MEDIA(m), klass, device ? GST_DEVICE(device) : NULL, error);
}
static const GhCallMediaOps media_ops = {
  mute_op, camera_op, remote_op, local_op, devices_op, select_op,
};

static gboolean
ring_cadence(gpointer data)
{
  GhCallRuntime *self = data;
  self->ring_on = !self->ring_on;
  gst_element_set_state(self->ring, self->ring_on ? GST_STATE_PLAYING : GST_STATE_PAUSED);
  self->cadence = g_timeout_add(self->ring_on ? RING_ON_MS : RING_OFF_MS, ring_cadence, self);
  return G_SOURCE_REMOVE;
}

static void
ring_stop(GhCallRuntime *self)
{
  g_clear_handle_id(&self->cadence, g_source_remove);
  if (self->ring) {
    gst_element_set_state(self->ring, GST_STATE_NULL);
    g_clear_pointer(&self->ring, gst_object_unref);
  }
  self->ring_on = FALSE;
}

static void
ring_start(GhCallRuntime *self)
{
  if (self->ring || gh_call_runtime_probe(self, NULL) == GH_CALL_UI_MEDIA_MISSING)
    return;
  g_autoptr(GError) error = NULL;
  self->ring = gst_parse_launch("audiotestsrc wave=sine freq=440 volume=0.2 is-live=true ! "
                                "audioconvert ! audioresample ! autoaudiosink", &error);
  if (!self->ring) {
    g_message("Groundhog could not start the ringer: %s", error ? error->message : "unknown");
    return;
  }
  self->ring_on = FALSE;
  ring_cadence(self);
}
#endif

void
gh_call_runtime_install(GhCallRuntime *self, GhCallService *service)
{
  g_return_if_fail(GH_IS_CALL_RUNTIME(self));
  g_return_if_fail(GH_IS_CALL_SERVICE(service));
#ifdef GROUNDHOG_HAVE_CALLS
  gh_call_service_set_media_factory(service, factory, g_object_ref(self), g_object_unref);
#else
  (void)service; /* no factory: calls stay unavailable (NOT_SUPPORTED) */
#endif
}

GListModel *
gh_call_runtime_get_outputs(GhCallRuntime *self)
{
  g_return_val_if_fail(GH_IS_CALL_RUNTIME(self), NULL);
#ifdef GROUNDHOG_HAVE_CALLS
  if (!self->monitor && gh_call_runtime_probe(self, NULL) != GH_CALL_UI_MEDIA_MISSING) {
    self->monitor = gst_device_monitor_new();
    gst_device_monitor_add_filter(self->monitor, "Audio/Sink", NULL);
    if (gst_device_monitor_start(self->monitor)) {
      GList *devices = gst_device_monitor_get_devices(self->monitor);
      for (GList *l = devices; l; l = l->next)
        g_list_store_append(self->outputs, l->data);
      g_list_free_full(devices, gst_object_unref);
      GstBus *bus = gst_device_monitor_get_bus(self->monitor);
      self->monitor_watch = gst_bus_add_watch(bus, monitor_message, self);
      gst_object_unref(bus);
    }
  }
#endif
  return G_LIST_MODEL(self->outputs);
}

gboolean
gh_call_runtime_choose_headset(GhCallRuntime *self, GObject *output, GError **error)
{
  g_return_val_if_fail(GH_IS_CALL_RUNTIME(self), FALSE);
#ifdef GROUNDHOG_HAVE_CALLS
  if (!GST_IS_DEVICE(output) || !gst_device_has_classes(GST_DEVICE(output), "Audio/Sink")) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Not an audio output");
    return FALSE;
  }
  g_set_object(&self->headset, output);
  return TRUE;
#else
  (void)output;
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Built without calls");
  return FALSE;
#endif
}

const GhCallMediaOps *
gh_call_runtime_get_media_ops(void)
{
#ifdef GROUNDHOG_HAVE_CALLS
  return &media_ops;
#else
  return NULL;
#endif
}

void
gh_call_runtime_set_ringing(GhCallRuntime *self, gboolean ringing)
{
  g_return_if_fail(GH_IS_CALL_RUNTIME(self));
  if (self->ringing == !!ringing)
    return;
  self->ringing = !!ringing;
#ifdef GROUNDHOG_HAVE_CALLS
  if (ringing)
    ring_start(self);
  else
    ring_stop(self);
#endif
}

gboolean
gh_call_runtime_get_ringing(GhCallRuntime *self)
{
  g_return_val_if_fail(GH_IS_CALL_RUNTIME(self), FALSE);
  return self->ringing;
}

static void
gh_call_runtime_dispose(GObject *object)
{
  GhCallRuntime *self = GH_CALL_RUNTIME(object);
#ifdef GROUNDHOG_HAVE_CALLS
  ring_stop(self);
  g_clear_handle_id(&self->monitor_watch, g_source_remove);
  if (self->monitor) {
    gst_device_monitor_stop(self->monitor);
    g_clear_pointer(&self->monitor, gst_object_unref);
  }
#endif
  g_clear_object(&self->headset);
  g_clear_object(&self->outputs);
  g_clear_object(&self->settings);
  G_OBJECT_CLASS(gh_call_runtime_parent_class)->dispose(object);
}

static void
gh_call_runtime_finalize(GObject *object)
{
  g_free(GH_CALL_RUNTIME(object)->reason);
  G_OBJECT_CLASS(gh_call_runtime_parent_class)->finalize(object);
}

static void
gh_call_runtime_class_init(GhCallRuntimeClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = gh_call_runtime_dispose;
  G_OBJECT_CLASS(klass)->finalize = gh_call_runtime_finalize;
}

static void
gh_call_runtime_init(GhCallRuntime *self)
{
  self->next_camera = TRUE;
#ifdef GROUNDHOG_HAVE_CALLS
  self->outputs = g_list_store_new(GST_TYPE_DEVICE);
#else
  self->outputs = g_list_store_new(G_TYPE_OBJECT);
#endif
}

GhCallRuntime *
gh_call_runtime_new(GSettings *settings)
{
  g_return_val_if_fail(G_IS_SETTINGS(settings), NULL);
  GhCallRuntime *self = g_object_new(GH_TYPE_CALL_RUNTIME, NULL);
  self->settings = g_object_ref(settings);
  return self;
}
