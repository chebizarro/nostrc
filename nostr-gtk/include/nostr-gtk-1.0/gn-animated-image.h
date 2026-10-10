#ifndef GN_ANIMATED_IMAGE_H
#define GN_ANIMATED_IMAGE_H
#include <gtk/gtk.h>
G_BEGIN_DECLS
/*
 * GnAnimatedImage: an animated GIF as a GdkPaintable (nostrc-p15n5.8).
 *
 * Decoded by a small bounded decoder in this library, never by gdk-pixbuf:
 * the header's logical screen size is checked before any pixel is decoded
 * (max_dimension per side), and decoding stops at GN_ANIMATED_IMAGE_MAX_FRAMES
 * frames or GN_ANIMATED_IMAGE_MAX_BYTES of decoded pixels, keeping the frames
 * already decoded. Frame delays under 20 ms play at 100 ms, as browsers do.
 *
 * It animates only while playing. gn_animated_image_attach() ties playback to
 * the widgets that show it: it plays while at least one of them is mapped and
 * pauses when all are unmapped (scrolled away, recycled, window closed).
 * Any other paintable passed to attach() just detaches what the widget had.
 */
#define GN_ANIMATED_IMAGE_MAX_FRAMES 1000
#define GN_ANIMATED_IMAGE_MAX_BYTES (192u * 1024u * 1024u)
#define GN_TYPE_ANIMATED_IMAGE (gn_animated_image_get_type())
G_DECLARE_FINAL_TYPE(GnAnimatedImage, gn_animated_image, GN, ANIMATED_IMAGE, GObject)

/* TRUE when bytes start with a GIF signature; the screen size when non-NULL. */
gboolean gn_animated_image_probe(GBytes *bytes, guint *out_width, guint *out_height);
/* A GIF with more than one frame, or NULL: G_IO_ERROR_NOT_SUPPORTED for
 * another format or a still GIF, G_IO_ERROR_INVALID_DATA when damaged or over
 * max_dimension. */
GnAnimatedImage *gn_animated_image_new_from_bytes(GBytes *bytes, guint max_dimension,
                                                  GError **error);
guint gn_animated_image_get_n_frames(GnAnimatedImage *self);
guint gn_animated_image_get_frame(GnAnimatedImage *self);
/* The frame shown now (borrowed). */
GdkTexture *gn_animated_image_get_current_texture(GnAnimatedImage *self);
/* Shows the next frame now (tests; playback calls it on its timer). */
void gn_animated_image_advance(GnAnimatedImage *self);
gboolean gn_animated_image_get_playing(GnAnimatedImage *self);

/* widget shows paintable (nullable): plays a GnAnimatedImage while widget is
 * mapped; replaces what was attached to widget before. */
void gn_animated_image_attach(GtkWidget *widget, GdkPaintable *paintable);

/* An animation decoded alongside a still texture (e.g. by a loader that
 * hands out GdkTexture), kept on the texture; NULL when none. */
void gn_animated_image_set_for_texture(GdkTexture *texture, GnAnimatedImage *animation);
GnAnimatedImage *gn_animated_image_get_for_texture(GdkTexture *texture);
/* The animation for texture when there is one, else texture (borrowed). */
GdkPaintable *gn_animated_image_paintable_for_texture(GdkTexture *texture);
G_END_DECLS
#endif
