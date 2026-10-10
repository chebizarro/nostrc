/*
 * gnostr-video-player.h
 *
 * Enhanced video player widget with custom controls overlay.
 * Features: fullscreen support, GSettings-based autoplay/loop config,
 * and a controls overlay with play/pause, seek, volume, and fullscreen buttons.
 */

#ifndef GNOSTR_VIDEO_PLAYER_H
#define GNOSTR_VIDEO_PLAYER_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_VIDEO_PLAYER (gnostr_video_player_get_type())

G_DECLARE_FINAL_TYPE(GnostrVideoPlayer, gnostr_video_player, GNOSTR, VIDEO_PLAYER, GtkWidget)

/**
 * gnostr_video_player_new:
 *
 * Creates a new video player widget.
 * Reads autoplay and loop settings from GSettings (org.gnostr.Client).
 *
 * Returns: (transfer full): A new #GnostrVideoPlayer
 */
GnostrVideoPlayer *gnostr_video_player_new(void);

/**
 * gnostr_video_player_set_uri:
 * @self: A #GnostrVideoPlayer
 * @uri: The URI of the video to play
 *
 * Sets the video source URI. This can be a remote URL (http/https)
 * or a local file URI (file://).
 */
void gnostr_video_player_set_uri(GnostrVideoPlayer *self, const char *uri);

/**
 * gnostr_video_player_get_uri:
 * @self: A #GnostrVideoPlayer
 *
 * Returns: (transfer none) (nullable): The current video URI, or NULL
 */
const char *gnostr_video_player_get_uri(GnostrVideoPlayer *self);

/**
 * gnostr_video_player_play:
 * @self: A #GnostrVideoPlayer
 *
 * Starts or resumes video playback.
 */
void gnostr_video_player_play(GnostrVideoPlayer *self);

/**
 * gnostr_video_player_pause:
 * @self: A #GnostrVideoPlayer
 *
 * Pauses video playback.
 */
void gnostr_video_player_pause(GnostrVideoPlayer *self);

/**
 * gnostr_video_player_toggle_playback:
 * @self: A #GnostrVideoPlayer
 *
 * Toggles between play and pause states.
 */
void gnostr_video_player_toggle_playback(GnostrVideoPlayer *self);

/**
 * gnostr_video_player_stop:
 * @self: A #GnostrVideoPlayer
 *
 * Stops video playback, resets position to beginning, and shows
 * the first frame as a thumbnail/poster.
 */
void gnostr_video_player_stop(GnostrVideoPlayer *self);

/**
 * gnostr_video_player_set_fullscreen:
 * @self: A #GnostrVideoPlayer
 * @fullscreen: Whether to enter fullscreen mode
 *
 * Enters or exits fullscreen mode. In fullscreen mode, the video
 * is displayed in a separate window with auto-hiding controls.
 */
void gnostr_video_player_set_fullscreen(GnostrVideoPlayer *self, gboolean fullscreen);

/**
 * gnostr_video_player_get_fullscreen:
 * @self: A #GnostrVideoPlayer
 *
 * Returns: TRUE if the player is in fullscreen mode
 */
gboolean gnostr_video_player_get_fullscreen(GnostrVideoPlayer *self);

/**
 * gnostr_video_player_set_autoplay:
 * @self: A #GnostrVideoPlayer
 * @autoplay: Whether to autoplay videos
 *
 * Sets whether videos should start playing automatically.
 * This overrides the GSettings value for this instance.
 */
void gnostr_video_player_set_autoplay(GnostrVideoPlayer *self, gboolean autoplay);

/**
 * gnostr_video_player_get_autoplay:
 * @self: A #GnostrVideoPlayer
 *
 * Returns: TRUE if autoplay is enabled
 */
gboolean gnostr_video_player_get_autoplay(GnostrVideoPlayer *self);

/**
 * gnostr_video_player_set_loop:
 * @self: A #GnostrVideoPlayer
 * @loop: Whether to loop videos
 *
 * Sets whether videos should loop when finished.
 * This overrides the GSettings value for this instance.
 */
void gnostr_video_player_set_loop(GnostrVideoPlayer *self, gboolean loop);

/**
 * gnostr_video_player_get_loop:
 * @self: A #GnostrVideoPlayer
 *
 * Returns: TRUE if loop is enabled
 */
gboolean gnostr_video_player_get_loop(GnostrVideoPlayer *self);

/**
 * gnostr_video_player_set_muted:
 * @self: A #GnostrVideoPlayer
 * @muted: Whether audio should be muted
 *
 * Sets the mute state of the video player.
 */
void gnostr_video_player_set_muted(GnostrVideoPlayer *self, gboolean muted);

/**
 * gnostr_video_player_get_muted:
 * @self: A #GnostrVideoPlayer
 *
 * Returns: TRUE if audio is muted
 */
gboolean gnostr_video_player_get_muted(GnostrVideoPlayer *self);

/**
 * gnostr_video_player_set_volume:
 * @self: A #GnostrVideoPlayer
 * @volume: Volume level (0.0 to 1.0)
 *
 * Sets the audio volume level.
 */
void gnostr_video_player_set_volume(GnostrVideoPlayer *self, double volume);

/**
 * gnostr_video_player_get_volume:
 * @self: A #GnostrVideoPlayer
 *
 * Returns: The current volume level (0.0 to 1.0)
 */
double gnostr_video_player_get_volume(GnostrVideoPlayer *self);

G_END_DECLS

#endif /* GNOSTR_VIDEO_PLAYER_H */
