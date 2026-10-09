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
void gn_media_viewer_set_texture(GnMediaViewer *self, guint index, GdkTexture *texture);
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
