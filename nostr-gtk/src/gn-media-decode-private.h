#ifndef GN_MEDIA_DECODE_PRIVATE_H
#define GN_MEDIA_DECODE_PRIVATE_H
#include <gtk/gtk.h>
G_BEGIN_DECLS
/* The first frame of a GIF (still or animated) through GnAnimatedImage's
 * bounded decoder; never gdk-pixbuf. */
GdkTexture *gn_animated_image_decode_first_frame(GBytes *bytes, guint max_dimension,
                                                 GError **error);
/* Installs the media widgets' stylesheet (viewer and player) once per
 * process, on the default display. Moved from gnostr.css (nostrc-8xfib.4). */
void gn_media_install_css(void);
G_END_DECLS
#endif
