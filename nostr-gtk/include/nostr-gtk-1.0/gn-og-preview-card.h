#ifndef GN_OG_PREVIEW_CARD_H
#define GN_OG_PREVIEW_CARD_H
#include <gtk/gtk.h>
#include "gn-og-preview.h"
G_BEGIN_DECLS
#define GN_TYPE_OG_PREVIEW_CARD (gn_og_preview_card_get_type())
G_DECLARE_FINAL_TYPE(GnOgPreviewCard, gn_og_preview_card, GN, OG_PREVIEW_CARD, GtkBox)
/* Neither construction nor set_url performs I/O (unless the application
 * opted in with set_auto_load). load-requested carries the URL. */
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
/* Artwork is a separate explicit request; setting metadata never requests it
 * (unless set_auto_load_image). */
void gn_og_preview_card_request_image(GnOgPreviewCard *self);
void gn_og_preview_card_set_image_texture(GnOgPreviewCard *self, GdkTexture *texture);
void gn_og_preview_card_clear(GnOgPreviewCard *self);
GCancellable *gn_og_preview_card_get_cancellable(GnOgPreviewCard *self);

/* Since 1.2 (nostrc-8xfib.3). */
typedef enum {
  GN_OG_PREVIEW_LAYOUT_STACKED, /* text over artwork (default) */
  GN_OG_PREVIEW_LAYOUT_COMPACT  /* 120 px cover artwork left, 1/2/1-line text right */
} GnOgPreviewLayout;
void gn_og_preview_card_set_layout(GnOgPreviewCard *self, GnOgPreviewLayout layout);
GnOgPreviewLayout gn_og_preview_card_get_layout(GnOgPreviewCard *self);
/* With a provider the card fetches through it on request_load/request_image
 * (the signals are still emitted first). The provider is referenced. */
void gn_og_preview_card_set_provider(GnOgPreviewCard *self, GnOgPreviewProvider *provider);
GnOgPreviewProvider *gn_og_preview_card_get_provider(GnOgPreviewCard *self);
/* Default FALSE: set_url never loads. TRUE: set_url requests the load. */
void gn_og_preview_card_set_auto_load(GnOgPreviewCard *self, gboolean auto_load);
/* Default FALSE. TRUE: artwork is requested as soon as metadata names it. */
void gn_og_preview_card_set_auto_load_image(GnOgPreviewCard *self, gboolean auto_load);
/* Cancelling @parent (e.g. a list row's bind cancellable) cancels in-flight
 * loads. Rebinding the same URL keeps the card and only swaps the parent. */
void gn_og_preview_card_set_url_with_cancellable(GnOgPreviewCard *self, const gchar *url,
                                                 GCancellable *parent);
/* An icon over the artwork (e.g. "media-playback-start-symbolic"); NULL hides. */
void gn_og_preview_card_set_media_badge(GnOgPreviewCard *self, const gchar *icon_name);
/* Quiesce before a GtkListView recycles the row: cancels and clears. */
void gn_og_preview_card_prepare_for_unbind(GnOgPreviewCard *self);
/* Signal "activate" (const char *url): the loaded card was clicked. Nothing
 * happens by default; the application decides how to open the link. */
G_END_DECLS
#endif
