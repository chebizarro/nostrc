/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gh-voice-waveform.h"
#include "gh-voice-meta.h"

#include <math.h>
#include <string.h>

struct _GhVoiceWaveform {
  gatomicrefcount ref_count;
  guint n_samples;
  gdouble samples[];   /* flexible array */
};

GhVoiceWaveform *
gh_voice_waveform_new(const gdouble *samples, guint n_samples)
{
  if (n_samples > GH_VOICE_MAX_WAVEFORM_SAMPLES)
    n_samples = GH_VOICE_MAX_WAVEFORM_SAMPLES;

  GhVoiceWaveform *waveform =
    g_malloc0(sizeof(GhVoiceWaveform) + sizeof(gdouble) * n_samples);
  g_atomic_ref_count_init(&waveform->ref_count);
  waveform->n_samples = n_samples;

  for (guint i = 0; i < n_samples; i++)
    waveform->samples[i] = CLAMP(samples[i], 0.0, 1.0);

  return waveform;
}

GhVoiceWaveform *
gh_voice_waveform_from_pcm(const gfloat *pcm, gsize n_frames, guint target_samples)
{
  g_return_val_if_fail(pcm != NULL || n_frames == 0, NULL);

  if (target_samples == 0)
    target_samples = 64;
  if (target_samples > GH_VOICE_MAX_WAVEFORM_SAMPLES)
    target_samples = GH_VOICE_MAX_WAVEFORM_SAMPLES;

  if (n_frames == 0) {
    gdouble zero = 0.0;
    return gh_voice_waveform_new(&zero, 1);
  }

  /* Fewer frames than target samples: one sample per frame. */
  if (n_frames < target_samples)
    target_samples = (guint)n_frames;

  gdouble *samples = g_new(gdouble, target_samples);
  gsize frames_per_sample = n_frames / target_samples;

  for (guint i = 0; i < target_samples; i++) {
    gsize start = i * frames_per_sample;
    gsize end = (i + 1 == target_samples) ? n_frames : start + frames_per_sample;
    gdouble sum_sq = 0.0;
    for (gsize j = start; j < end; j++) {
      gdouble v = pcm[j];
      sum_sq += v * v;
    }
    gdouble rms = sqrt(sum_sq / (gdouble)(end - start));
    /* Normalise: RMS of a sine wave has peak 1/sqrt(2) ≈ 0.707; real
     * speech is much quieter. A factor of 2 gives visually useful
     * bars while keeping loud passages below 1.0. */
    samples[i] = CLAMP(rms * 2.0, 0.0, 1.0);
  }

  GhVoiceWaveform *waveform = gh_voice_waveform_new(samples, target_samples);
  g_free(samples);
  return waveform;
}

GhVoiceWaveform *
gh_voice_waveform_ref(GhVoiceWaveform *waveform)
{
  g_return_val_if_fail(waveform != NULL, NULL);
  g_atomic_ref_count_inc(&waveform->ref_count);
  return waveform;
}

void
gh_voice_waveform_unref(GhVoiceWaveform *waveform)
{
  if (waveform && g_atomic_ref_count_dec(&waveform->ref_count))
    g_free(waveform);
}

guint
gh_voice_waveform_get_n_samples(const GhVoiceWaveform *waveform)
{
  g_return_val_if_fail(waveform != NULL, 0);
  return waveform->n_samples;
}

const gdouble *
gh_voice_waveform_get_samples(const GhVoiceWaveform *waveform)
{
  g_return_val_if_fail(waveform != NULL, NULL);
  return waveform->samples;
}
