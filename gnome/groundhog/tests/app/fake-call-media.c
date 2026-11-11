#include "fake-call-media.h"
static void iface_init(GhCallMediaBackendInterface *iface);
G_DEFINE_TYPE_WITH_CODE(GhFakeCallMedia, gh_fake_call_media, G_TYPE_OBJECT,
  G_IMPLEMENT_INTERFACE(GH_TYPE_CALL_MEDIA_BACKEND, iface_init))
static gboolean configure(GhCallMediaBackend *backend, gboolean local, GError **error)
{ (void)error; GH_FAKE_CALL_MEDIA(backend)->allow_local = local; return TRUE; }
static void local_async(GhCallMediaBackend *backend, GhCallType type, gboolean answer,
                        GCancellable *cancel, GAsyncReadyCallback callback, gpointer data)
{
  GhFakeCallMedia *self = GH_FAKE_CALL_MEDIA(backend);
  if (answer) self->answers++; else self->offers++;
  GTask *task = g_task_new(backend, cancel, callback, data);
  /* Deliberately emit ICE before SDP completion to exercise production ordering. */
  g_signal_emit_by_name(backend, "local-ice", "candidate:1 1 UDP 1 192.0.2.1 9 typ relay", "0", 0u);
  if (self->fail) g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "fake SDP failure");
  else g_task_return_pointer(task, g_strdup(type == GH_CALL_VIDEO ?
    "v=0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\nm=video 9 UDP/TLS/RTP/SAVPF 96\r\n" :
    "v=0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"), g_free);
  g_object_unref(task);
}
static gchar *local_finish(GhCallMediaBackend *backend, GAsyncResult *result, GError **error)
{ (void)backend; return g_task_propagate_pointer(G_TASK(result), error); }
static void remote_async(GhCallMediaBackend *backend, const gchar *sdp, gboolean offer,
                         GCancellable *cancel, GAsyncReadyCallback callback, gpointer data)
{
  GhFakeCallMedia *self = GH_FAKE_CALL_MEDIA(backend); g_assert_true(g_str_has_prefix(sdp, "v=0"));
  if (offer) self->remote_offers++; else self->remote_answers++;
  self->remote_ready = FALSE;
  GTask *task = g_task_new(backend, cancel, callback, data);
  g_task_return_boolean(task, TRUE); g_object_unref(task);
}
static gboolean remote_finish(GhCallMediaBackend *backend, GAsyncResult *result, GError **error)
{
  gboolean ok = g_task_propagate_boolean(G_TASK(result), error);
  if (ok) GH_FAKE_CALL_MEDIA(backend)->remote_ready = TRUE;
  return ok;
}
static gboolean ice(GhCallMediaBackend *backend, const gchar *candidate,
                     const gchar *mid, guint index, GError **error)
{
  (void)candidate; (void)mid; (void)index; (void)error;
  GhFakeCallMedia *self = GH_FAKE_CALL_MEDIA(backend);
  g_assert_true(self->remote_ready); self->ice++; return TRUE;
}
static void teardown(GhCallMediaBackend *backend)
{
  GhFakeCallMedia *self = GH_FAKE_CALL_MEDIA(backend);
  if (!self->torn_down) { self->torn_down = TRUE; self->teardowns++; }
}
static void iface_init(GhCallMediaBackendInterface *iface)
{
  iface->configure = configure; iface->create_local_async = local_async;
  iface->create_local_finish = local_finish; iface->apply_remote_async = remote_async;
  iface->apply_remote_finish = remote_finish; iface->add_remote_ice = ice; iface->teardown = teardown;
}
static void gh_fake_call_media_init(GhFakeCallMedia *self) { (void)self; }
static void gh_fake_call_media_class_init(GhFakeCallMediaClass *klass) { (void)klass; }
GhCallMediaBackend *gh_fake_call_media_factory(gpointer data, GError **error)
{
  (void)error;
  GhFakeCallMedia *self = g_object_new(GH_TYPE_FAKE_CALL_MEDIA, NULL);
  if (data) { GhFakeCallMedia **last = data; g_set_object(last, self); }
  return GH_CALL_MEDIA_BACKEND(self);
}
