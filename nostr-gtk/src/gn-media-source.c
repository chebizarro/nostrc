#include <nostr-gtk-1.0/gn-media-source.h>

/* nostrc-8xfib.4: the host's media plumbing, injected. See the header. */

G_DEFINE_INTERFACE(GnMediaSource, gn_media_source, G_TYPE_OBJECT)

static void
gn_media_source_default_init(GnMediaSourceInterface *iface)
{
  (void)iface;
}

GnMediaPolicy
gn_media_source_get_policy(GnMediaSource *self, const char *url, GnMediaKind kind)
{
  g_return_val_if_fail(GN_IS_MEDIA_SOURCE(self), GN_MEDIA_POLICY_BLOCKED);
  GnMediaSourceInterface *iface = GN_MEDIA_SOURCE_GET_IFACE(self);
  if (!iface->get_policy || !url || !*url) return GN_MEDIA_POLICY_BLOCKED;
  return iface->get_policy(self, url, kind);
}

void
gn_media_source_fetch_async(GnMediaSource *self, const char *url, GnMediaKind kind, gsize max_bytes,
                            GCancellable *cancellable, GAsyncReadyCallback callback,
                            gpointer user_data)
{
  g_return_if_fail(GN_IS_MEDIA_SOURCE(self));
  GnMediaSourceInterface *iface = GN_MEDIA_SOURCE_GET_IFACE(self);
  if (!iface->fetch_async || !iface->fetch_finish) {
    g_task_report_new_error(self, callback, user_data, gn_media_source_fetch_async, G_IO_ERROR,
                            G_IO_ERROR_NOT_SUPPORTED, "This media source cannot fetch");
    return;
  }
  iface->fetch_async(self, url, kind, max_bytes, cancellable, callback, user_data);
}

GBytes *
gn_media_source_fetch_finish(GnMediaSource *self, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(GN_IS_MEDIA_SOURCE(self), NULL);
  if (g_async_result_is_tagged(result, gn_media_source_fetch_async))
    return g_task_propagate_pointer(G_TASK(result), error);
  return GN_MEDIA_SOURCE_GET_IFACE(self)->fetch_finish(self, result, error);
}

GtkMediaStream *
gn_media_source_open_stream(GnMediaSource *self, const char *url, GError **error)
{
  g_return_val_if_fail(GN_IS_MEDIA_SOURCE(self), NULL);
  GnMediaSourceInterface *iface = GN_MEDIA_SOURCE_GET_IFACE(self);
  if (!iface->open_stream) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                        "This media source cannot open videos");
    return NULL;
  }
  return iface->open_stream(self, url, error);
}

void
gn_media_source_adopt(GnMediaSource *self, GtkWidget *widget)
{
  g_return_if_fail(GN_IS_MEDIA_SOURCE(self));
  GnMediaSourceInterface *iface = GN_MEDIA_SOURCE_GET_IFACE(self);
  if (iface->adopt) iface->adopt(self, widget);
}

static GnMediaSource *default_source;

void
gn_media_source_set_default(GnMediaSource *source)
{
  g_return_if_fail(!source || GN_IS_MEDIA_SOURCE(source));
  g_set_object(&default_source, source);
}

GnMediaSource *
gn_media_source_get_default(void)
{
  return default_source;
}
