/*
 * gnostr-image-viewer.h - Full-size image viewer modal
 *
 * A modal dialog for viewing images at full size with zoom and pan support.
 * Features:
 *   - Zoom with scroll wheel or +/- keys
 *   - Pan when zoomed (drag or arrow keys)
 *   - Double-click to toggle fit/100%
 *   - Escape or click outside to close
 *   - Dark semi-transparent background
 */

#ifndef GNOSTR_IMAGE_VIEWER_H
#define GNOSTR_IMAGE_VIEWER_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_IMAGE_VIEWER (gnostr_image_viewer_get_type())
G_DECLARE_FINAL_TYPE(GnostrImageViewer, gnostr_image_viewer, GNOSTR, IMAGE_VIEWER, GtkWindow)

/**
 * gnostr_image_viewer_new:
 * @parent: (nullable): parent window for transient-for
 *
 * Create a new image viewer dialog.
 *
 * Returns: (transfer full): a new GnostrImageViewer instance
 */
GnostrImageViewer *gnostr_image_viewer_new(GtkWindow *parent);

/**
 * gnostr_image_viewer_set_image_url:
 * @self: the image viewer
 * @url: the URL of the image to display
 *
 * Load and display an image from a URL.
 */
void gnostr_image_viewer_set_image_url(GnostrImageViewer *self, const char *url);

/**
 * gnostr_image_viewer_set_texture:
 * @self: the image viewer
 * @texture: a GdkTexture to display
 *
 * Display an already-loaded texture (useful when image is already cached).
 */
void gnostr_image_viewer_set_texture(GnostrImageViewer *self, GdkTexture *texture);

/**
 * gnostr_image_viewer_present:
 * @self: the image viewer
 *
 * Present the image viewer window.
 */
void gnostr_image_viewer_present(GnostrImageViewer *self);

G_END_DECLS

#endif /* GNOSTR_IMAGE_VIEWER_H */
