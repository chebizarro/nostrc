/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef GH_VOICE_PLAYER_H
#define GH_VOICE_PLAYER_H

#include <gio/gio.h>

#include "gh-voice-waveform.h"

G_BEGIN_DECLS

/*
 * GhVoicePlayer (W27, nostrc-o1kl): a GStreamer-based audio player for
 * voice messages.
 *
 * Plays from in-memory bytes (never from disk outside the encrypted store).
 * Pipeline: appsrc → decodebin → audioconvert → audioresample →
 *           scaletempo → autoaudiosink
 *
 * States: STOPPED → PLAYING ↔ PAUSED → STOPPED.
 *
 * Playback speed: 1.0×, 1.5×, 2.0× (scaletempo preserves pitch).
 *
 * Privacy (charter): no MPRIS integration — the audio never surfaces in
 * system "now playing" metadata. The pipeline's application-name is left
 * unset and no media-keys or MPRIS element is inserted.
 *
 * Main context only.
 */

typedef enum {
  GH_VOICE_PLAYER_STATE_STOPPED,
  GH_VOICE_PLAYER_STATE_PLAYING,
  GH_VOICE_PLAYER_STATE_PAUSED
} GhVoicePlayerState;

#define GH_TYPE_VOICE_PLAYER (gh_voice_player_get_type())
G_DECLARE_FINAL_TYPE(GhVoicePlayer, gh_voice_player, GH, VOICE_PLAYER, GObject)

GhVoicePlayer *gh_voice_player_new(void);

/* Load audio from in-memory bytes and optionally a pre-computed waveform
 * (NULL: compute one after decoding). */
gboolean gh_voice_player_load(GhVoicePlayer *self, GBytes *audio_bytes,
                              GhVoiceWaveform *waveform, GError **error);

/* Playback controls. */
void gh_voice_player_play(GhVoicePlayer *self);
void gh_voice_player_pause(GhVoicePlayer *self);
void gh_voice_player_stop(GhVoicePlayer *self);

/* Seek to a position in seconds. */
void gh_voice_player_seek(GhVoicePlayer *self, gdouble position_s);

GhVoicePlayerState gh_voice_player_get_state(GhVoicePlayer *self);

/* Current playback position in seconds (updated ~10 Hz while playing). */
gdouble gh_voice_player_get_position(GhVoicePlayer *self);

/* Total duration in seconds (available after load). */
gdouble gh_voice_player_get_duration(GhVoicePlayer *self);

/* Playback speed. */
void gh_voice_player_set_speed(GhVoicePlayer *self, gdouble speed);
gdouble gh_voice_player_get_speed(GhVoicePlayer *self);
/* The rate accepted by the GStreamer pipeline (1.0 before a seek succeeds). */
gdouble gh_voice_player_get_active_speed(GhVoicePlayer *self);

/* The waveform (from load or computed; transfer full). */
GhVoiceWaveform *gh_voice_player_dup_waveform(GhVoicePlayer *self);

/* Set the waveform after load (e.g. computed from decoded audio). Emits
 * "waveform-ready". */
void gh_voice_player_set_waveform(GhVoicePlayer *self, GhVoiceWaveform *waveform);

/* Signals:
 *   "position-updated" (gdouble position_s)
 *   "state-changed" (GhVoicePlayerState state)
 *   "waveform-ready" ()
 *   "playback-error" (const gchar *message)
 */

G_END_DECLS
#endif
