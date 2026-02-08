#ifndef GNOSTR_YOUTUBE_EMBED_H
#define GNOSTR_YOUTUBE_EMBED_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#ifdef HAVE_WEBKITGTK

#define GNOSTR_TYPE_YOUTUBE_EMBED (gnostr_youtube_embed_get_type())
G_DECLARE_FINAL_TYPE(GnostrYoutubeEmbed, gnostr_youtube_embed, GNOSTR, YOUTUBE_EMBED, GtkWidget)

/**
 * gnostr_youtube_embed_new:
 * @video_id: YouTube video ID (11 chars)
 *
 * Creates a new YouTube embed widget that loads the video in a WebKitWebView.
 *
 * Returns: (transfer full): A new #GnostrYoutubeEmbed
 */
GtkWidget *gnostr_youtube_embed_new(const char *video_id);

/**
 * gnostr_youtube_embed_stop:
 * @self: A #GnostrYoutubeEmbed
 *
 * Stops video playback and releases WebKit resources.
 */
void gnostr_youtube_embed_stop(GnostrYoutubeEmbed *self);

#endif /* HAVE_WEBKITGTK */

/**
 * gnostr_youtube_embed_is_available:
 *
 * Returns: %TRUE if inline YouTube playback is supported (WebKit available).
 */
gboolean gnostr_youtube_embed_is_available(void);

G_END_DECLS

#endif /* GNOSTR_YOUTUBE_EMBED_H */
