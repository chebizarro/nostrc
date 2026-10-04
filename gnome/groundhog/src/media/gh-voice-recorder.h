/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef GH_VOICE_RECORDER_H
#define GH_VOICE_RECORDER_H

#include <gio/gio.h>

#include "gh-voice-waveform.h"

G_BEGIN_DECLS

/*
 * GhVoiceRecorder (W27, nostrc-o1kl): a GStreamer-based voice recorder that
 * produces an OGG/Opus file matching MDK v0.11.0's wire format.
 *
 * Pipeline: autoaudiosrc → audioconvert → audioresample → level → opusenc →
 *           oggmux → fdsink (an already-unlinked 0600 inode in private_dir).
 * The fd is wiped and closed after dup_bytes() or on failure; process death
 * closes it without leaving a reachable plaintext cache file.
 *
 * States: IDLE → RECORDING → DONE | CANCELLED | ERROR.
 *
 * Level updates arrive through the "level-updated" signal at ~10 Hz
 * while recording; the accumulated waveform is available after a
 * successful stop.
 *
 * Privacy (charter): the desktop audio service mediates microphone access;
 * there is no app-callable XDG microphone portal. Recording state is visible
 * in the UI; there is no transcription. The unlinked inode is wiped on cancel,
 * error and finalize. Named files left by older builds are swept at startup.
 *
 * Main context only.
 */

typedef enum {
  GH_VOICE_RECORDER_STATE_IDLE,
  GH_VOICE_RECORDER_STATE_RECORDING,
  GH_VOICE_RECORDER_STATE_DONE,
  GH_VOICE_RECORDER_STATE_CANCELLED,
  GH_VOICE_RECORDER_STATE_ERROR
} GhVoiceRecorderState;

#define GH_TYPE_VOICE_RECORDER (gh_voice_recorder_get_type())
G_DECLARE_FINAL_TYPE(GhVoiceRecorder, gh_voice_recorder, GH, VOICE_RECORDER, GObject)

/* private_dir is created/verified as a user-owned 0700 directory. */
GhVoiceRecorder *gh_voice_recorder_new(const gchar *private_dir);
/* Remove legacy/stale gh-voice-* files from private_dir. A missing directory
 * is success; an unsafe directory or failed wipe is an error. */
gboolean gh_voice_recorder_sweep_stale(const gchar *private_dir, GError **error);
/* Deterministic capture seam for tests: source_factory is either
 * "autoaudiosrc" or "audiotestsrc"; the rest of the pipeline is identical. */
GhVoiceRecorder *gh_voice_recorder_new_with_source(const gchar *private_dir,
                                                   const gchar *source_factory);

/* Start recording. Returns FALSE with error if the pipeline cannot be
 * built (no microphone, GStreamer not available). */
gboolean gh_voice_recorder_start(GhVoiceRecorder *self, GError **error);

/* Stop recording successfully; the unlinked fd is retained until dup_bytes(). */
void gh_voice_recorder_stop(GhVoiceRecorder *self);

/* Cancel: the unlinked fd is wiped and closed. */
void gh_voice_recorder_cancel(GhVoiceRecorder *self);

GhVoiceRecorderState gh_voice_recorder_get_state(GhVoiceRecorder *self);

/* The recording duration in seconds (updated live while recording). */
gdouble gh_voice_recorder_get_duration(GhVoiceRecorder *self);

/* The current audio level in [0,1] (updated ~10 Hz while recording). */
gdouble gh_voice_recorder_get_level(GhVoiceRecorder *self);

/* The waveform computed during recording (available after DONE; transfer
 * full). */
GhVoiceWaveform *gh_voice_recorder_dup_waveform(GhVoiceRecorder *self);

/* The recording as in-memory bytes (available after DONE; transfer full).
 * Reads, zeroes and closes the unlinked fd even when the read fails. */
GBytes *gh_voice_recorder_dup_bytes(GhVoiceRecorder *self, GError **error);

/* Signals:
 *   "level-updated" (gdouble level) — emitted ~10 Hz while recording.
 *   "state-changed" (GhVoiceRecorderState state) — emitted on state transitions.
 *   "duration-tick" (gdouble seconds) — emitted ~1 Hz while recording.
 */

G_END_DECLS
#endif
