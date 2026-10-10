#include "gnostr-og-provider.h"

struct _GnostrOgProvider {
  GObject parent_instance;
  GnostrMediaService *service; /* nullable: default service */
};

static void provider_iface_init(GnOgPreviewProviderInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE(GnostrOgProvider, gnostr_og_provider, G_TYPE_OBJECT,
  G_IMPLEMENT_INTERFACE(GN_TYPE_OG_PREVIEW_PROVIDER, provider_iface_init))

static GnostrMediaService *
service_of(GnostrOgProvider *self)
{
  return self->service ? self->service : gnostr_media_service_get_default();
}

/* The media service owns the user data and drops it after its callback (or
 * without one, when it is shut down); either way the task returns once. */
static void
task_release(gpointer data)
{
  GTask *task = data;
  if (!g_task_get_task_data(task))
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Preview request dropped");
  g_object_unref(task);
}

static void
mark_returned(GTask *task)
{
  g_task_set_task_data(task, GINT_TO_POINTER(1), NULL);
}

static void
on_metadata(GnostrMediaService *service, const char *url, GnostrOgMetadata *metadata,
            const GError *error, gpointer data)
{
  (void)service;
  GTask *task = data;
  mark_returned(task);
  if (metadata) {
    /* gnostr does not store og:site_name; the card names the host. */
    g_task_return_pointer(task,
      gn_og_metadata_new(gnostr_og_metadata_get_source_url(metadata) ?
                           gnostr_og_metadata_get_source_url(metadata) : url,
                         gnostr_og_metadata_get_title(metadata),
                         gnostr_og_metadata_get_description(metadata), NULL,
                         gnostr_og_metadata_get_image_url(metadata)),
      (GDestroyNotify)gn_og_metadata_unref);
  } else if (error) {
    g_task_return_error(task, g_error_copy(error));
  } else {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Preview unavailable");
  }
}

static void
load_metadata_async(GnOgPreviewProvider *provider, const char *url, GCancellable *cancellable,
                    GAsyncReadyCallback callback, gpointer user_data)
{
  GnostrOgProvider *self = GNOSTR_OG_PROVIDER(provider);
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, load_metadata_async);
  /* The card only asks after the user's setting allowed it or the user
   * pressed "Load preview": both are user-initiated for the service. */
  gnostr_media_service_request_og_metadata_with_intent(
      service_of(self), url, GNOSTR_MEDIA_FETCH_USER_INITIATED, cancellable,
      on_metadata, task, task_release);
}

static GnOgMetadata *
load_metadata_finish(GnOgPreviewProvider *provider, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, provider), NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

static void
on_texture(GnostrMediaService *service, const char *url, GdkTexture *texture,
           const GError *error, gpointer data)
{
  (void)service;
  (void)url;
  GTask *task = data;
  mark_returned(task);
  if (texture)
    g_task_return_pointer(task, g_object_ref(texture), g_object_unref);
  else if (error)
    g_task_return_error(task, g_error_copy(error));
  else
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Image unavailable");
}

static void
load_image_async(GnOgPreviewProvider *provider, const char *image_url, int width_hint,
                 int height_hint, GCancellable *cancellable, GAsyncReadyCallback callback,
                 gpointer user_data)
{
  GnostrOgProvider *self = GNOSTR_OG_PROVIDER(provider);
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, load_image_async);
  gnostr_media_service_request_texture_with_intent(
      service_of(self), image_url, GNOSTR_MEDIA_RESOURCE_OG_IMAGE, width_hint, height_hint,
      GNOSTR_MEDIA_FETCH_USER_INITIATED, cancellable, on_texture, task, task_release);
}

static GdkPaintable *
load_image_finish(GnOgPreviewProvider *provider, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, provider), NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

static void
provider_iface_init(GnOgPreviewProviderInterface *iface)
{
  iface->load_metadata_async = load_metadata_async;
  iface->load_metadata_finish = load_metadata_finish;
  iface->load_image_async = load_image_async;
  iface->load_image_finish = load_image_finish;
}

static void
gnostr_og_provider_dispose(GObject *object)
{
  g_clear_object(&GNOSTR_OG_PROVIDER(object)->service);
  G_OBJECT_CLASS(gnostr_og_provider_parent_class)->dispose(object);
}

static void
gnostr_og_provider_class_init(GnostrOgProviderClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = gnostr_og_provider_dispose;
}

static void
gnostr_og_provider_init(GnostrOgProvider *self)
{
  (void)self;
}

GnostrOgProvider *
gnostr_og_provider_new(GnostrMediaService *service)
{
  GnostrOgProvider *self = g_object_new(GNOSTR_TYPE_OG_PROVIDER, NULL);
  if (service)
    self->service = g_object_ref(service);
  return self;
}

GnOgPreviewProvider *
gnostr_og_provider_get_default(void)
{
  static GnostrOgProvider *provider;
  static gsize once;
  if (g_once_init_enter(&once)) {
    provider = gnostr_og_provider_new(NULL);
    g_once_init_leave(&once, 1);
  }
  return GN_OG_PREVIEW_PROVIDER(provider);
}
