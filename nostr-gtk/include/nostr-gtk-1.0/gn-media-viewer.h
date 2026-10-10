#ifndef GN_MEDIA_VIEWER_H
#define GN_MEDIA_VIEWER_H
#include <gtk/gtk.h>
G_BEGIN_DECLS
#define GN_TYPE_MEDIA_VIEWER (gn_media_viewer_get_type())
G_DECLARE_FINAL_TYPE(GnMediaViewer, gn_media_viewer, GN, MEDIA_VIEWER, GtkWindow)
GnMediaViewer *gn_media_viewer_new(GtkWindow *parent);
/* URLs identify gallery slots only. Navigation never requests data. */
void gn_media_viewer_set_gallery(GnMediaViewer *self, const gchar *const *urls, guint current);
gboolean gn_media_viewer_navigate(GnMediaViewer *self, gint delta);
/* A texture carrying a GnAnimatedImage (gn_animated_image_set_for_texture())
 * is shown animated. */
void gn_media_viewer_set_texture(GnMediaViewer *self, guint index, GdkTexture *texture);
/* Any paintable: a texture, a GnAnimatedImage (plays while shown) or a
 * GtkMediaStream such as a GtkMediaFile (video: OSD controls, plays while
 * shown, pauses when navigated away or closed). */
void gn_media_viewer_set_paintable(GnMediaViewer *self, guint index, GdkPaintable *paintable);
/* What is shown now, or NULL (not loaded). */
GdkPaintable *gn_media_viewer_get_paintable(GnMediaViewer *self);
void gn_media_viewer_set_texture_for_generation(GnMediaViewer *self,
                                                 guint64 generation, guint index,
                                                 GdkTexture *texture);
guint64 gn_media_viewer_get_generation(GnMediaViewer *self);
void gn_media_viewer_request_load(GnMediaViewer *self);
void gn_media_viewer_set_zoom(GnMediaViewer *self, gdouble zoom); /* 0 = fit */
gdouble gn_media_viewer_get_zoom(GnMediaViewer *self);
guint gn_media_viewer_get_index(GnMediaViewer *self);
G_END_DECLS
#endif
