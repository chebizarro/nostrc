/* GnOgPreviewProvider: the application's fetch side of a link preview. */
#include <nostr-gtk-1.0/gn-og-preview.h>

G_DEFINE_INTERFACE(GnOgPreviewProvider, gn_og_preview_provider, G_TYPE_OBJECT)

static void
gn_og_preview_provider_default_init(GnOgPreviewProviderInterface *iface)
{
  (void)iface;
}

void
gn_og_preview_provider_load_metadata_async(GnOgPreviewProvider *self, const char *url,
                                           GCancellable *cancellable,
                                           GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail(GN_IS_OG_PREVIEW_PROVIDER(self));
  g_return_if_fail(url != NULL);
  GnOgPreviewProviderInterface *iface = GN_OG_PREVIEW_PROVIDER_GET_IFACE(self);
  g_return_if_fail(iface->load_metadata_async != NULL);
  iface->load_metadata_async(self, url, cancellable, callback, user_data);
}

GnOgMetadata *
gn_og_preview_provider_load_metadata_finish(GnOgPreviewProvider *self, GAsyncResult *result,
                                            GError **error)
{
  g_return_val_if_fail(GN_IS_OG_PREVIEW_PROVIDER(self), NULL);
  GnOgPreviewProviderInterface *iface = GN_OG_PREVIEW_PROVIDER_GET_IFACE(self);
  g_return_val_if_fail(iface->load_metadata_finish != NULL, NULL);
  return iface->load_metadata_finish(self, result, error);
}

gboolean
gn_og_preview_provider_can_load_image(GnOgPreviewProvider *self)
{
  g_return_val_if_fail(GN_IS_OG_PREVIEW_PROVIDER(self), FALSE);
  GnOgPreviewProviderInterface *iface = GN_OG_PREVIEW_PROVIDER_GET_IFACE(self);
  return iface->load_image_async && iface->load_image_finish;
}

void
gn_og_preview_provider_load_image_async(GnOgPreviewProvider *self, const char *image_url,
                                        int width_hint, int height_hint,
                                        GCancellable *cancellable,
                                        GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail(GN_IS_OG_PREVIEW_PROVIDER(self));
  g_return_if_fail(image_url != NULL);
  GnOgPreviewProviderInterface *iface = GN_OG_PREVIEW_PROVIDER_GET_IFACE(self);
  g_return_if_fail(iface->load_image_async != NULL);
  iface->load_image_async(self, image_url, width_hint, height_hint, cancellable, callback,
                          user_data);
}

GdkPaintable *
gn_og_preview_provider_load_image_finish(GnOgPreviewProvider *self, GAsyncResult *result,
                                         GError **error)
{
  g_return_val_if_fail(GN_IS_OG_PREVIEW_PROVIDER(self), NULL);
  GnOgPreviewProviderInterface *iface = GN_OG_PREVIEW_PROVIDER_GET_IFACE(self);
  g_return_val_if_fail(iface->load_image_finish != NULL, NULL);
  return iface->load_image_finish(self, result, error);
}
