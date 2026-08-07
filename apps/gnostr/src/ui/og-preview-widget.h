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
 * @url: The URL to preview
 *
 * Stores the URL and displays a privacy-preserving placeholder. This function
 * never starts a network request.
 */
void og_preview_widget_set_url(OgPreviewWidget *self, const char *url);

/* Starts loading after an explicit user activation. */
void og_preview_widget_load(OgPreviewWidget *self);

/**
 * og_preview_widget_clear:
 * @self: An #OgPreviewWidget
 *
 * Clears the preview and cancels any in-flight requests.
 */
void og_preview_widget_clear(OgPreviewWidget *self);

/**
 * og_preview_widget_prepare_for_unbind:
 * @self: An #OgPreviewWidget
 *
 * Prepares the widget for unbinding from a list item. This cancels all async
 * operations and marks the widget as disposed to prevent callbacks from
 * accessing widget state during the unbind/dispose process.
 *
 * CRITICAL: Call this from the parent widget's prepare_for_unbind BEFORE
 * the parent starts its own cleanup. This prevents crashes where async
 * callbacks try to access widget memory during disposal.
 */
void og_preview_widget_prepare_for_unbind(OgPreviewWidget *self);

/**
 * og_preview_widget_set_url_with_cancellable:
 * @self: An #OgPreviewWidget
 * @url: The URL to fetch and preview
 * @cancellable: (nullable): External cancellable from parent widget
 *
 * Stores the URL using a parent cancellable without fetching. The widget
 * keeps an independent cancellable subscription, linked to (but never
 * cancelling) the parent.
 */
void og_preview_widget_set_url_with_cancellable(OgPreviewWidget *self, 
                                                 const char *url,
                                                 GCancellable *cancellable);

G_END_DECLS

#endif /* OG_PREVIEW_WIDGET_H */
