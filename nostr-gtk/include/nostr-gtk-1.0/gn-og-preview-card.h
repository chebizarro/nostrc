#ifndef GN_OG_PREVIEW_CARD_H
#define GN_OG_PREVIEW_CARD_H
#include <gtk/gtk.h>
G_BEGIN_DECLS
#define GN_TYPE_OG_PREVIEW_CARD (gn_og_preview_card_get_type())
G_DECLARE_FINAL_TYPE(GnOgPreviewCard, gn_og_preview_card, GN, OG_PREVIEW_CARD, GtkBox)
/* Neither construction nor set_url performs I/O. load-requested carries the URL. */
GnOgPreviewCard *gn_og_preview_card_new(void);
void gn_og_preview_card_set_url(GnOgPreviewCard *self, const gchar *url);
const gchar *gn_og_preview_card_get_url(GnOgPreviewCard *self);
void gn_og_preview_card_request_load(GnOgPreviewCard *self);
void gn_og_preview_card_set_result(GnOgPreviewCard *self, const gchar *title,
                                   const gchar *description, const gchar *site,
                                   const gchar *image_url);
/* Async callers should use the URL-checked variants to reject stale callbacks. */
void gn_og_preview_card_set_result_for_url(GnOgPreviewCard *self, const gchar *url,
                                           const gchar *title, const gchar *description,
                                           const gchar *site, const gchar *image_url);
void gn_og_preview_card_set_error(GnOgPreviewCard *self, const gchar *message);
void gn_og_preview_card_set_error_for_url(GnOgPreviewCard *self, const gchar *url,
                                          const gchar *message);
/* Artwork is a separate explicit request; setting metadata never requests it. */
void gn_og_preview_card_request_image(GnOgPreviewCard *self);
void gn_og_preview_card_set_image_texture(GnOgPreviewCard *self, GdkTexture *texture);
void gn_og_preview_card_clear(GnOgPreviewCard *self);
GCancellable *gn_og_preview_card_get_cancellable(GnOgPreviewCard *self);
G_END_DECLS
#endif
