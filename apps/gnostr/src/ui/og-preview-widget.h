#ifndef OG_PREVIEW_WIDGET_H
#define OG_PREVIEW_WIDGET_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define OG_TYPE_PREVIEW_WIDGET (og_preview_widget_get_type())
G_DECLARE_FINAL_TYPE(OgPreviewWidget, og_preview_widget, OG, PREVIEW_WIDGET, GtkWidget)

/**
 * og_preview_widget_new:
 *
 * Creates a new Open Graph preview widget.
 *
 * Returns: (transfer full): A new #OgPreviewWidget
 */
OgPreviewWidget *og_preview_widget_new(void);

/**
 * og_preview_widget_set_url:
 * @self: An #OgPreviewWidget
 * @url: The URL to fetch and preview
 *
 * Sets the URL to fetch Open Graph metadata from.
 * This will cancel any in-flight request and start a new fetch.
 */
void og_preview_widget_set_url(OgPreviewWidget *self, const char *url);

/**
 * og_preview_widget_clear:
 * @self: An #OgPreviewWidget
 *
 * Clears the preview and cancels any in-flight requests.
 */
void og_preview_widget_clear(OgPreviewWidget *self);

G_END_DECLS

#endif /* OG_PREVIEW_WIDGET_H */
