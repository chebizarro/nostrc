/*
 * gn-video-player.h
 *
 * Video player widget with custom OSD controls (play/pause, stop, seek, time,
 * mute, volume, loop, fullscreen), auto-hiding controls, keyboard shortcuts
 * (space/k, f, m, Esc), pause when scrolled out of view or unmapped, and
 * loading/error/blocked states. Ported from Gnostr (nostrc-8xfib.4).
 *
 * The player never hands a remote URL to the media backend. Media arrives as
 * a GtkMediaStream (gn_video_player_set_stream()), a caller-vetted GFile
 * (gn_video_player_set_file()), or through a GnMediaSource's open_stream()
 * after gn_video_player_set_url(). No GtkMediaFile exists until one of these.
 */
#ifndef GN_VIDEO_PLAYER_H
#define GN_VIDEO_PLAYER_H

#include "nostr-gtk-portable-export.h"
#include <gtk/gtk.h>
#include <nostr-gtk-1.0/gn-media-source.h>

G_BEGIN_DECLS

#define GN_TYPE_VIDEO_PLAYER (gn_video_player_get_type())
NOSTR_GTK_PORTABLE_API G_DECLARE_FINAL_TYPE(GnVideoPlayer, gn_video_player, GN, VIDEO_PLAYER, GtkWidget)

NOSTR_GTK_PORTABLE_API GnVideoPlayer *gn_video_player_new(void);

/* The host's media plumbing (nullable). Its adopt() hook runs once here. */
NOSTR_GTK_PORTABLE_API void gn_video_player_set_source(GnVideoPlayer *self, GnMediaSource *source);
NOSTR_GTK_PORTABLE_API GnMediaSource *gn_video_player_get_source(GnVideoPlayer *self);

/* Identifies the video; loads nothing by itself unless the source's policy
 * is ALLOW. ASK (or no source) shows a Load action; BLOCKED says so. */
NOSTR_GTK_PORTABLE_API void gn_video_player_set_url(GnVideoPlayer *self, const char *url);
NOSTR_GTK_PORTABLE_API const char *gn_video_player_get_url(GnVideoPlayer *self);
/* The user's Load action: emits "load-requested" (url) and, with a source
 * whose policy is not BLOCKED, opens the stream through it. */
NOSTR_GTK_PORTABLE_API void gn_video_player_request_load(GnVideoPlayer *self);

/* Plays stream (nullable). The player does not tear down a stream it did
 * not create; it only pauses it when replaced or disposed. */
NOSTR_GTK_PORTABLE_API void gn_video_player_set_stream(GnVideoPlayer *self, GtkMediaStream *stream);
/* A local or otherwise vetted file, through a GtkMediaFile the player owns. */
NOSTR_GTK_PORTABLE_API void gn_video_player_set_file(GnVideoPlayer *self, GFile *file);
NOSTR_GTK_PORTABLE_API GtkMediaStream *gn_video_player_get_stream(GnVideoPlayer *self);
/* Shows the error state with message. */
NOSTR_GTK_PORTABLE_API void gn_video_player_set_error(GnVideoPlayer *self, const char *message);

NOSTR_GTK_PORTABLE_API void gn_video_player_play(GnVideoPlayer *self);
NOSTR_GTK_PORTABLE_API void gn_video_player_pause(GnVideoPlayer *self);
NOSTR_GTK_PORTABLE_API void gn_video_player_toggle_playback(GnVideoPlayer *self);
/* Pauses and seeks to the start. */
NOSTR_GTK_PORTABLE_API void gn_video_player_stop(GnVideoPlayer *self);

NOSTR_GTK_PORTABLE_API void gn_video_player_set_fullscreen(GnVideoPlayer *self, gboolean fullscreen);
NOSTR_GTK_PORTABLE_API gboolean gn_video_player_get_fullscreen(GnVideoPlayer *self);

/* GObject properties "autoplay", "loop", "muted", "volume" (bindable, e.g.
 * with g_settings_bind() in a GnMediaSource's adopt()). */
NOSTR_GTK_PORTABLE_API void gn_video_player_set_autoplay(GnVideoPlayer *self, gboolean autoplay);
NOSTR_GTK_PORTABLE_API gboolean gn_video_player_get_autoplay(GnVideoPlayer *self);
NOSTR_GTK_PORTABLE_API void gn_video_player_set_loop(GnVideoPlayer *self, gboolean loop);
NOSTR_GTK_PORTABLE_API gboolean gn_video_player_get_loop(GnVideoPlayer *self);
NOSTR_GTK_PORTABLE_API void gn_video_player_set_muted(GnVideoPlayer *self, gboolean muted);
NOSTR_GTK_PORTABLE_API gboolean gn_video_player_get_muted(GnVideoPlayer *self);
NOSTR_GTK_PORTABLE_API void gn_video_player_set_volume(GnVideoPlayer *self, double volume);
NOSTR_GTK_PORTABLE_API double gn_video_player_get_volume(GnVideoPlayer *self);

G_END_DECLS

#endif /* GN_VIDEO_PLAYER_H */
