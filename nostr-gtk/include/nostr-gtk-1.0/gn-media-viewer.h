#ifndef GN_MEDIA_VIEWER_H
#define GN_MEDIA_VIEWER_H
#include <gtk/gtk.h>
#include <nostr-gtk-1.0/gn-media-source.h>
#include <nostr-gtk-1.0/gn-media-decode.h>
G_BEGIN_DECLS
/*
 * GnMediaViewer: a modal, undecorated panel over its parent window showing a
 * gallery of images, animated GIFs and videos (nostrc-8xfib.4, the Gnostr
 * viewer ported into nostr-gtk).
 *
 * Images: fit to the window without upscaling (the label shows the real
 * scale), zoom in 25% steps from what is shown (+, -, wheel, pinch, Zoom
 * buttons), 1 for 100%, 0 or f to fit, double-click toggles fit and 100%,
 * drag pans and arrow keys pan above 100%. Left/Right otherwise move through
 * the gallery. A click outside the image or Esc closes. Save (Ctrl+S) and
 * Copy link (Ctrl+C, web URLs only) are offered. Videos (a GtkMediaStream)
 * play with GnVideoPlayer controls while shown and pause when navigated away
 * from or closed.
 *
 * Loading: URLs identify gallery slots; the widget never fetches by itself.
 * Without a source an empty slot offers "Load image", which emits
 * "load-requested" (index, url); the host answers with set_texture /
 * set_paintable (optionally guarded by the generation). With a
 * GnMediaSource, policy ALLOW loads the shown slot at once, ASK waits for the
 * Load action, BLOCKED never loads; fetched bytes are decoded within the
 * decode limits off the main thread. Each slot shown starts a new generation:
 * earlier results are dropped and the pending request is cancelled, also on
 * close.
 *
 * Signals: "load-requested" (guint index, const char *url); "save-requested"
 * (guint index, const char *url, GdkPaintable *shown) -> gboolean, a handler
 * returning TRUE replaces the default native Save dialog (PNG of the shown
 * frame); "link-copied" (const char *url) after Copy link wrote the
 * clipboard, for the host to confirm (for example a toast).
 */
#define GN_TYPE_MEDIA_VIEWER (gn_media_viewer_get_type())
G_DECLARE_FINAL_TYPE(GnMediaViewer, gn_media_viewer, GN, MEDIA_VIEWER, GtkWindow)
GnMediaViewer *gn_media_viewer_new(GtkWindow *parent);
/* Sizes the panel to its parent (at least 400x300) and presents it. */
void gn_media_viewer_present(GnMediaViewer *self);

/* URLs identify gallery slots only (at most 256). Navigation never requests
 * data without a source whose policy allows it. */
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
void gn_media_viewer_set_paintable_for_generation(GnMediaViewer *self, guint64 generation,
                                                  guint index, GdkPaintable *paintable);
guint64 gn_media_viewer_get_generation(GnMediaViewer *self);
/* The Load action: emits "load-requested" and, with a source whose policy is
 * not BLOCKED, fetches. Nothing when the slot is loaded or loading. */
void gn_media_viewer_request_load(GnMediaViewer *self);

/* Host-driven states of the shown slot (ignored for another generation or
 * slot, or once loaded): a spinner; a message without Load; blocked with
 * Load (reason NULL: "Remote media is blocked"). */
void gn_media_viewer_set_loading(GnMediaViewer *self, guint64 generation, guint index);
void gn_media_viewer_set_error(GnMediaViewer *self, guint64 generation, guint index,
                               const char *message);
void gn_media_viewer_set_blocked(GnMediaViewer *self, gboolean blocked, const char *reason);
gboolean gn_media_viewer_get_loading(GnMediaViewer *self);
/* TRUE while the Load action is shown. */
gboolean gn_media_viewer_get_load_offered(GnMediaViewer *self);
/* The blocked or error text shown, or NULL. */
const char *gn_media_viewer_get_message(GnMediaViewer *self);
/* The shown slot's pending request, or NULL. */
GCancellable *gn_media_viewer_get_cancellable(GnMediaViewer *self);

/* The host's media plumbing (nullable); see gn-media-source.h. */
void gn_media_viewer_set_source(GnMediaViewer *self, GnMediaSource *source);
GnMediaSource *gn_media_viewer_get_source(GnMediaViewer *self);
/* Limits for bytes fetched through the source (NULL: the defaults). */
void gn_media_viewer_set_decode_limits(GnMediaViewer *self, const GnMediaDecodeLimits *limits);

void gn_media_viewer_set_zoom(GnMediaViewer *self, gdouble zoom); /* 0 = fit */
gdouble gn_media_viewer_get_zoom(GnMediaViewer *self);            /* 0 = fit */
/* The scale on screen, also when fitting. */
gdouble gn_media_viewer_get_shown_zoom(GnMediaViewer *self);
guint gn_media_viewer_get_index(GnMediaViewer *self);

/* "can-save" (default TRUE): offer Save for still images. */
void gn_media_viewer_set_can_save(GnMediaViewer *self, gboolean can_save);
gboolean gn_media_viewer_get_can_save(GnMediaViewer *self);
/* The Save and Copy link actions. */
void gn_media_viewer_save(GnMediaViewer *self);
void gn_media_viewer_copy_link(GnMediaViewer *self);
G_END_DECLS
#endif
