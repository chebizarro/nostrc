#ifndef GNOSTR_YOUTUBE_URL_H
#define GNOSTR_YOUTUBE_URL_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * gnostr_youtube_url_is_youtube:
 * @url: A URL string
 *
 * Checks whether the given URL is a YouTube video URL.
 *
 * Returns: %TRUE if the URL points to a YouTube video
 */
gboolean gnostr_youtube_url_is_youtube(const char *url);

/**
 * gnostr_youtube_url_extract_video_id:
 * @url: A YouTube URL string
 *
 * Extracts the video ID from a YouTube URL.
 * Handles watch?v=, youtu.be/, shorts/, embed/, music.youtube.com.
 *
 * Returns: (transfer full) (nullable): The video ID string, or %NULL.
 *          Free with g_free().
 */
char *gnostr_youtube_url_extract_video_id(const char *url);

/**
 * gnostr_youtube_url_build_embed:
 * @video_id: A YouTube video ID
 *
 * Builds a YouTube embed URL for the given video ID.
 *
 * Returns: (transfer full): The embed URL. Free with g_free().
 */
char *gnostr_youtube_url_build_embed(const char *video_id);

G_END_DECLS

#endif /* GNOSTR_YOUTUBE_URL_H */
