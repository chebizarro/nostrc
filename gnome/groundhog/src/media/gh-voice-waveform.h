/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef GH_VOICE_WAVEFORM_H
#define GH_VOICE_WAVEFORM_H

#include <glib.h>

G_BEGIN_DECLS

/*
 * Waveform samples for voice messages (W27, nostrc-o1kl).
 *
 * A waveform is a fixed-length array of normalised amplitude samples
 * (0.0 .. 1.0) that represents the loudness envelope of an audio clip,
 * used for the voice bubble visualisation. It is computed locally — MDK
 * v0.11.0 stores waveform samples in the draft database but does not
 * transmit them in the imeta tag.
 *
 * GhVoiceWaveform is an immutable, reference-counted value.
 *
 * Network-free, GTK-free, GStreamer-free (works on raw float samples).
 */

typedef struct _GhVoiceWaveform GhVoiceWaveform;

/* Create a waveform from n_samples float values in [0,1], clamped and
 * copied. The result is reference-counted (initial refcount 1). */
GhVoiceWaveform *gh_voice_waveform_new(const gdouble *samples, guint n_samples);

/* Create a waveform by computing RMS amplitudes from interleaved PCM
 * float samples (one channel, values in [-1,1]). The result has at most
 * target_samples entries (≤ GH_VOICE_MAX_WAVEFORM_SAMPLES). */
GhVoiceWaveform *gh_voice_waveform_from_pcm(const gfloat *pcm, gsize n_frames,
                                             guint target_samples);

GhVoiceWaveform *gh_voice_waveform_ref(GhVoiceWaveform *waveform);
void gh_voice_waveform_unref(GhVoiceWaveform *waveform);

/* The number of samples. */
guint gh_voice_waveform_get_n_samples(const GhVoiceWaveform *waveform);
/* The sample array (borrowed, valid while the waveform lives). */
const gdouble *gh_voice_waveform_get_samples(const GhVoiceWaveform *waveform);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GhVoiceWaveform, gh_voice_waveform_unref)

G_END_DECLS
#endif
