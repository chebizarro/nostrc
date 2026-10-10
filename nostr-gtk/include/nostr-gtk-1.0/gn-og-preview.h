#ifndef GN_OG_PREVIEW_H
#define GN_OG_PREVIEW_H
/* Open Graph link previews: metadata, the shared <head> parser and the
 * provider interface a GnOgPreviewCard drives (nostrc-8xfib.3).
 *
 * nostr-gtk performs no network I/O for previews. An application supplies a
 * GnOgPreviewProvider that fetches through its own transport and policy
 * (Groundhog: GhNetHttp, consent, network mode; gnostr: its media service and
 * caches) and hands back parsed metadata and decoded paintables. */
#include <gtk/gtk.h>
G_BEGIN_DECLS

typedef struct _GnOgMetadata GnOgMetadata;
#define GN_TYPE_OG_METADATA (gn_og_metadata_get_type())
GType gn_og_metadata_get_type(void) G_GNUC_CONST;
/* All strings are copied; any may be NULL. */
GnOgMetadata *gn_og_metadata_new(const char *source_url, const char *title,
                                 const char *description, const char *site_name,
                                 const char *image_url);
GnOgMetadata *gn_og_metadata_ref(GnOgMetadata *metadata);
void gn_og_metadata_unref(GnOgMetadata *metadata);
const char *gn_og_metadata_get_source_url(const GnOgMetadata *metadata);
const char *gn_og_metadata_get_title(const GnOgMetadata *metadata);
const char *gn_og_metadata_get_description(const GnOgMetadata *metadata);
const char *gn_og_metadata_get_site_name(const GnOgMetadata *metadata);
const char *gn_og_metadata_get_image_url(const GnOgMetadata *metadata);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GnOgMetadata, gn_og_metadata_unref)

/* Returns TRUE to accept an absolute og:image / twitter:image address. */
typedef gboolean (*GnOgImagePolicy)(const char *image_url, gpointer user_data);

/**
 * gn_og_metadata_parse_html:
 * @head_prefix: the page, or only its first bytes (truncation is not an error)
 * @source_url: (nullable): the page address; relative image addresses resolve
 *   against it
 * @accept_image: (nullable) (scope call): image address policy; NULL accepts
 *   http(s) only
 *
 * Pure: no I/O, no scripts, no entity or external resource loading. Reads
 * only <head>. Precedence per field is og: > twitter: > plain HTML,
 * independent of document order; every field is valid UTF-8, at most 512
 * characters. Fails with G_IO_ERROR_INVALID_DATA when the head has neither a
 * title nor a description.
 */
GnOgMetadata *gn_og_metadata_parse_html(GBytes *head_prefix, const char *source_url,
                                        GnOgImagePolicy accept_image, gpointer user_data,
                                        GError **error);

#define GN_TYPE_OG_PREVIEW_PROVIDER (gn_og_preview_provider_get_type())
G_DECLARE_INTERFACE(GnOgPreviewProvider, gn_og_preview_provider, GN, OG_PREVIEW_PROVIDER, GObject)
struct _GnOgPreviewProviderInterface {
  GTypeInterface parent_iface;
  void (*load_metadata_async)(GnOgPreviewProvider *self, const char *url,
                              GCancellable *cancellable, GAsyncReadyCallback callback,
                              gpointer user_data);
  GnOgMetadata *(*load_metadata_finish)(GnOgPreviewProvider *self, GAsyncResult *result,
                                        GError **error);
  /* Optional: without it the card offers no artwork. */
  void (*load_image_async)(GnOgPreviewProvider *self, const char *image_url,
                           int width_hint, int height_hint, GCancellable *cancellable,
                           GAsyncReadyCallback callback, gpointer user_data);
  GdkPaintable *(*load_image_finish)(GnOgPreviewProvider *self, GAsyncResult *result,
                                     GError **error);
  gpointer padding[8];
};
void gn_og_preview_provider_load_metadata_async(GnOgPreviewProvider *self, const char *url,
                                                GCancellable *cancellable,
                                                GAsyncReadyCallback callback,
                                                gpointer user_data);
GnOgMetadata *gn_og_preview_provider_load_metadata_finish(GnOgPreviewProvider *self,
                                                          GAsyncResult *result,
                                                          GError **error);
gboolean gn_og_preview_provider_can_load_image(GnOgPreviewProvider *self);
void gn_og_preview_provider_load_image_async(GnOgPreviewProvider *self, const char *image_url,
                                             int width_hint, int height_hint,
                                             GCancellable *cancellable,
                                             GAsyncReadyCallback callback,
                                             gpointer user_data);
GdkPaintable *gn_og_preview_provider_load_image_finish(GnOgPreviewProvider *self,
                                                       GAsyncResult *result,
                                                       GError **error);
G_END_DECLS
#endif
