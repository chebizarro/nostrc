/* SPDX-License-Identifier: GPL-3.0-or-later
 * Voice message tests (W27, nostrc-o1kl): imeta voice detection, waveform
 * computation, and recorder/player lifecycle. */
#include "gh-voice-meta.h"
#include "gh-voice-waveform.h"
#include "gh-voice-recorder.h"
#include "gh-voice-player.h"
#include "canary-scan.h"

#include <glib/gstdio.h>
#include <gst/gst.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- gh_voice_meta_is_audio ---- */

static void
test_voice_meta_is_audio(void)
{
  /* Positive cases: audio MIME types. */
  g_assert_true(gh_voice_meta_is_audio("audio/ogg"));
  g_assert_true(gh_voice_meta_is_audio("audio/mp4"));
  g_assert_true(gh_voice_meta_is_audio("audio/mpeg"));
  g_assert_true(gh_voice_meta_is_audio("audio/wav"));
  g_assert_true(gh_voice_meta_is_audio("Audio/OGG"));
  g_assert_true(gh_voice_meta_is_audio("AUDIO/MP4"));

  /* Negative cases. */
  g_assert_false(gh_voice_meta_is_audio(NULL));
  g_assert_false(gh_voice_meta_is_audio(""));
  g_assert_false(gh_voice_meta_is_audio("audio/"));
  g_assert_false(gh_voice_meta_is_audio("image/png"));
  g_assert_false(gh_voice_meta_is_audio("video/mp4"));
  g_assert_false(gh_voice_meta_is_audio("application/octet-stream"));
  g_assert_false(gh_voice_meta_is_audio("text/plain"));
}

static void
test_voice_meta_is_ogg_opus(void)
{
  g_assert_true(gh_voice_meta_is_ogg_opus("audio/ogg"));
  g_assert_true(gh_voice_meta_is_ogg_opus("Audio/OGG"));
  g_assert_true(gh_voice_meta_is_ogg_opus("audio/ogg; codecs=opus"));

  g_assert_false(gh_voice_meta_is_ogg_opus(NULL));
  g_assert_false(gh_voice_meta_is_ogg_opus("audio/mp4"));
  g_assert_false(gh_voice_meta_is_ogg_opus("audio/mpeg"));
  g_assert_false(gh_voice_meta_is_ogg_opus("audio/wav"));
}

/* ---- GhVoiceWaveform ---- */

static void
test_waveform_new_clamps(void)
{
  /* Values outside [0,1] are clamped. */
  gdouble samples[] = { -0.5, 0.0, 0.5, 1.0, 1.5 };
  g_autoptr(GhVoiceWaveform) wf = gh_voice_waveform_new(samples, 5);

  g_assert_cmpuint(gh_voice_waveform_get_n_samples(wf), ==, 5);
  const gdouble *s = gh_voice_waveform_get_samples(wf);
  g_assert_cmpfloat(s[0], ==, 0.0);   /* clamped from -0.5 */
  g_assert_cmpfloat(s[1], ==, 0.0);
  g_assert_cmpfloat(s[2], ==, 0.5);
  g_assert_cmpfloat(s[3], ==, 1.0);
  g_assert_cmpfloat(s[4], ==, 1.0);   /* clamped from 1.5 */
}

static void
test_waveform_refcount(void)
{
  gdouble samples[] = { 0.1, 0.2, 0.3 };
  GhVoiceWaveform *wf = gh_voice_waveform_new(samples, 3);
  GhVoiceWaveform *ref = gh_voice_waveform_ref(wf);
  g_assert_true(ref == wf);
  g_assert_cmpuint(gh_voice_waveform_get_n_samples(ref), ==, 3);

  gh_voice_waveform_unref(ref);
  /* Original still alive. */
  g_assert_cmpuint(gh_voice_waveform_get_n_samples(wf), ==, 3);
  gh_voice_waveform_unref(wf);
}

static void
test_waveform_from_pcm_silence(void)
{
  /* Silent audio: all zeros → waveform samples should be near 0. */
  gfloat pcm[4800];  /* 0.1s at 48000 Hz */
  memset(pcm, 0, sizeof(pcm));
  g_autoptr(GhVoiceWaveform) wf = gh_voice_waveform_from_pcm(pcm, 4800, 10);

  g_assert_nonnull(wf);
  g_assert_cmpuint(gh_voice_waveform_get_n_samples(wf), ==, 10);
  const gdouble *s = gh_voice_waveform_get_samples(wf);
  for (guint i = 0; i < 10; i++)
    g_assert_cmpfloat(s[i], ==, 0.0);
}

static void
test_waveform_from_pcm_sine(void)
{
  /* A 440 Hz sine wave at 48000 Hz, amplitude 1.0. */
  gsize n_frames = 48000;
  gfloat *pcm = g_new(gfloat, n_frames);
  for (gsize i = 0; i < n_frames; i++)
    pcm[i] = sinf(2.0f * G_PI * 440.0f * (gfloat)i / 48000.0f);

  g_autoptr(GhVoiceWaveform) wf = gh_voice_waveform_from_pcm(pcm, n_frames, 20);
  g_free(pcm);

  g_assert_nonnull(wf);
  g_assert_cmpuint(gh_voice_waveform_get_n_samples(wf), ==, 20);
  const gdouble *s = gh_voice_waveform_get_samples(wf);
  /* RMS of a sine is 1/sqrt(2) ≈ 0.707; times 2 ≈ 1.0 (clamped). */
  for (guint i = 0; i < 20; i++) {
    g_assert_cmpfloat(s[i], >, 0.5);
    g_assert_cmpfloat(s[i], <=, 1.0);
  }
}

static void
test_waveform_from_pcm_empty(void)
{
  g_autoptr(GhVoiceWaveform) wf = gh_voice_waveform_from_pcm(NULL, 0, 10);
  g_assert_nonnull(wf);
  g_assert_cmpuint(gh_voice_waveform_get_n_samples(wf), ==, 1);
}

static void
test_waveform_max_samples_cap(void)
{
  /* Requesting more than the max is capped. */
  gdouble samples[10] = { 0.5 };
  g_autoptr(GhVoiceWaveform) wf = gh_voice_waveform_new(samples, 10);
  g_assert_cmpuint(gh_voice_waveform_get_n_samples(wf), ==, 10);
  g_assert_cmpuint(gh_voice_waveform_get_n_samples(wf), <=,
                   GH_VOICE_MAX_WAVEFORM_SAMPLES);
}

/* ---- constants ---- */

static void
test_voice_constants(void)
{
  g_assert_cmpstr(GH_VOICE_MIME, ==, "audio/ogg");
  g_assert_cmpstr(GH_VOICE_FILENAME, ==, "voice.ogg");
  g_assert_cmpuint(GH_VOICE_MAX_DURATION_S, ==, 300);
}

/* ---- real codec, plugin and recording lifecycle ---- */

static GBytes *
voice_fixture(const gchar *name)
{
  g_autofree gchar *path = g_build_filename(GROUNDHOG_VOICE_FIXTURE_DIR, name, NULL);
  gchar *contents = NULL;
  gsize length = 0;
  g_autoptr(GError) error = NULL;
  g_assert_true(g_file_get_contents(path, &contents, &length, &error));
  g_assert_no_error(error);
  return g_bytes_new_take(contents, length);
}

static gboolean
quit_loop(gpointer data)
{
  g_main_loop_quit(data);
  return G_SOURCE_REMOVE;
}

static void
waveform_ready(GhVoicePlayer *player G_GNUC_UNUSED, gpointer data)
{
  g_main_loop_quit(data);
}

static void
test_player_decodes_waveform(void)
{
  const gchar *fixtures[] = { "white-noise-aac.m4a", "groundhog-opus.ogg" };
  for (guint i = 0; i < G_N_ELEMENTS(fixtures); i++) {
    g_autoptr(GBytes) audio = voice_fixture(fixtures[i]);
    g_autoptr(GhVoicePlayer) player = gh_voice_player_new();
    g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
    g_autoptr(GError) error = NULL;
    g_signal_connect(player, "waveform-ready", G_CALLBACK(waveform_ready), loop);
    g_assert_true(gh_voice_player_load(player, audio, NULL, &error));
    g_assert_no_error(error);
    guint timeout = g_timeout_add_seconds(5, quit_loop, loop);
    g_main_loop_run(loop);
    g_source_remove(timeout);
    g_autoptr(GhVoiceWaveform) waveform = gh_voice_player_dup_waveform(player);
    g_assert_nonnull(waveform);
    g_assert_cmpuint(gh_voice_waveform_get_n_samples(waveform), >, 0);
    gdouble peak = 0.0;
    const gdouble *samples = gh_voice_waveform_get_samples(waveform);
    for (guint j = 0; j < gh_voice_waveform_get_n_samples(waveform); j++)
      peak = MAX(peak, samples[j]);
    g_assert_cmpfloat(peak, >, 0.05);
    gh_voice_player_stop(player);
  }
}

typedef struct {
  GMainLoop *loop;
  guint errors;
} PlaybackWait;

static void
playback_state(GhVoicePlayer *player G_GNUC_UNUSED, guint state, gpointer data)
{
  PlaybackWait *wait = data;
  if (state == GH_VOICE_PLAYER_STATE_STOPPED)
    g_main_loop_quit(wait->loop);
}

static void
playback_error(GhVoicePlayer *player G_GNUC_UNUSED,
               const gchar *message G_GNUC_UNUSED, gpointer data)
{
  ((PlaybackWait *)data)->errors++;
}

static void
test_player_replay_speed(void)
{
  g_autoptr(GBytes) audio = voice_fixture("groundhog-opus.ogg");
  g_autoptr(GhVoicePlayer) player = gh_voice_player_new();
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_voice_player_load(player, audio, NULL, &error));
  g_assert_no_error(error);
  g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
  PlaybackWait wait = { .loop = loop };
  g_signal_connect(player, "state-changed", G_CALLBACK(playback_state), &wait);
  g_signal_connect(player, "playback-error", G_CALLBACK(playback_error), &wait);
  gh_voice_player_set_speed(player, 2.0);
  for (guint run = 0; run < 2; run++) {
    gh_voice_player_play(player);
    g_assert_cmpint(gh_voice_player_get_state(player), ==,
                    GH_VOICE_PLAYER_STATE_PLAYING);
    guint timeout = g_timeout_add_seconds(5, quit_loop, loop);
    g_main_loop_run(loop);
    if (g_main_context_find_source_by_id(NULL, timeout))
      g_source_remove(timeout);
    g_assert_cmpuint(wait.errors, ==, 0);
    g_assert_cmpint(gh_voice_player_get_state(player), ==,
                    GH_VOICE_PLAYER_STATE_STOPPED);
    g_assert_cmpfloat(gh_voice_player_get_active_speed(player), ==, 2.0);
  }
}

static void
test_player_partial_plugins(void)
{
  if (!g_test_subprocess()) {
    g_test_trap_subprocess(NULL, 0, 0);
    g_test_trap_assert_passed();
    return;
  }
  gst_init(NULL, NULL);
  GstRegistry *registry = gst_registry_get();
  GstPluginFeature *feature = gst_registry_find_feature(registry, "decodebin",
                                                         GST_TYPE_ELEMENT_FACTORY);
  g_assert_nonnull(feature);
  gst_registry_remove_feature(registry, feature);
  gst_object_unref(feature);
  g_autoptr(GBytes) audio = voice_fixture("white-noise-aac.m4a");
  g_autoptr(GhVoicePlayer) player = gh_voice_player_new();
  g_autoptr(GError) error = NULL;
  g_assert_false(gh_voice_player_load(player, audio, NULL, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
  g_assert_nonnull(strstr(error->message, "missing audio support"));
}

static void
recorder_state_changed(GhVoiceRecorder *recorder G_GNUC_UNUSED, guint state,
                       gpointer data)
{
  if (state == GH_VOICE_RECORDER_STATE_DONE || state == GH_VOICE_RECORDER_STATE_ERROR)
    g_main_loop_quit(data);
}

static gboolean
stop_recorder(gpointer data)
{
  gh_voice_recorder_stop(data);
  return G_SOURCE_REMOVE;
}

static void
test_recorder_private_wipe(void)
{
  g_autoptr(GError) error = NULL;
  g_autofree gchar *root = g_dir_make_tmp("gh-voice-privacy-XXXXXX", &error);
  g_assert_no_error(error);
  g_autofree gchar *private_dir = g_build_filename(root, "voice", NULL);
  g_autoptr(GhVoiceRecorder) recorder =
    gh_voice_recorder_new_with_source(private_dir, "audiotestsrc");
  g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
  g_signal_connect(recorder, "state-changed", G_CALLBACK(recorder_state_changed), loop);
  g_assert_true(gh_voice_recorder_start(recorder, &error));
  g_assert_no_error(error);

  /* There is no named plaintext file even while the recorder is active. */
  guint n_files = 0;
  CanaryScan *scan = canary_scan_new();
  g_assert_cmpuint(canary_scan_tree(scan, root, &n_files), ==, 0);
  g_assert_cmpuint(n_files, ==, 0);
  guint stop_id = g_timeout_add(350, stop_recorder, recorder);
  guint timeout = g_timeout_add_seconds(5, quit_loop, loop);
  g_main_loop_run(loop);
  if (g_main_context_find_source_by_id(NULL, stop_id))
    g_source_remove(stop_id);
  if (g_main_context_find_source_by_id(NULL, timeout))
    g_source_remove(timeout);
  g_assert_cmpint(gh_voice_recorder_get_state(recorder), ==,
                  GH_VOICE_RECORDER_STATE_DONE);

  g_autoptr(GBytes) bytes = gh_voice_recorder_dup_bytes(recorder, &error);
  g_assert_no_error(error);
  g_assert_nonnull(bytes);
  g_assert_cmpuint(g_bytes_get_size(bytes), >, 100);
  g_assert_cmpmem(g_bytes_get_data(bytes, NULL), 4, "OggS", 4);
  n_files = 0;
  g_assert_cmpuint(canary_scan_tree(scan, root, &n_files), ==, 0);
  g_assert_cmpuint(n_files, ==, 0);
  canary_scan_free(scan);
  g_assert_cmpint(g_rmdir(private_dir), ==, 0);
  g_assert_cmpint(g_rmdir(root), ==, 0);
}

static void
test_recorder_missing_audio_support(void)
{
  if (!g_test_subprocess()) {
    g_test_trap_subprocess(NULL, 0, 0);
    g_test_trap_assert_passed();
    return;
  }
  gst_init(NULL, NULL);
  GstRegistry *registry = gst_registry_get();
  GstPluginFeature *feature = gst_registry_find_feature(registry, "opusenc",
                                                         GST_TYPE_ELEMENT_FACTORY);
  g_assert_nonnull(feature);
  gst_registry_remove_feature(registry, feature);
  gst_object_unref(feature);
  g_autoptr(GError) error = NULL;
  g_autofree gchar *root = g_dir_make_tmp("gh-voice-unavailable-XXXXXX", &error);
  g_assert_no_error(error);
  g_autofree gchar *private_dir = g_build_filename(root, "voice", NULL);
  g_autoptr(GhVoiceRecorder) recorder = gh_voice_recorder_new_with_source(private_dir,
                                                                          "audiotestsrc");
  g_assert_false(gh_voice_recorder_start(recorder, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
  g_assert_nonnull(strstr(error->message, "missing audio support"));
  g_assert_false(g_file_test(private_dir, G_FILE_TEST_EXISTS));
  g_assert_cmpint(g_rmdir(root), ==, 0);
}

static void
test_recorder_crash_recovery(void)
{
  if (g_test_subprocess()) {
    const gchar *private_dir = g_getenv("GH_VOICE_CRASH_TEST_DIR");
    g_assert_nonnull(private_dir);
    g_autoptr(GhVoiceRecorder) recorder =
      gh_voice_recorder_new_with_source(private_dir, "audiotestsrc");
    g_autoptr(GError) child_error = NULL;
    g_assert_true(gh_voice_recorder_start(recorder, &child_error));
    g_assert_no_error(child_error);
    g_usleep(250 * 1000);
    static const char marker[] = "VOICE_CAPTURE_STARTED\n";
    g_assert_cmpint(write(STDOUT_FILENO, marker, sizeof marker - 1), ==,
                    (ssize_t)(sizeof marker - 1));
    raise(SIGKILL); /* no GObject finalization, EOS or cleanup callback */
    _exit(1);
  }

  g_autoptr(GError) error = NULL;
  g_autofree gchar *root = g_dir_make_tmp("gh-voice-crash-XXXXXX", &error);
  g_assert_no_error(error);
  g_autofree gchar *private_dir = g_build_filename(root, "voice", NULL);
  g_assert_true(g_setenv("GH_VOICE_CRASH_TEST_DIR", private_dir, TRUE));
  g_test_trap_subprocess(NULL, 30 * G_USEC_PER_SEC, 0);
  g_test_trap_assert_failed();
  g_test_trap_assert_stdout("*VOICE_CAPTURE_STARTED*");

  /* The child died while writing; no plaintext name may survive it. */
  CanaryScan *scan = canary_scan_new();
  guint n_files = 0;
  g_assert_cmpuint(canary_scan_tree(scan, root, &n_files), ==, 0);
  g_assert_cmpuint(n_files, ==, 0);

  /* Simulate a file from an older build (or death during create/unlink).
   * The next attachment-UI startup calls the same sweep function. */
  g_autofree gchar *stale = g_build_filename(private_dir, "gh-voice-stale", NULL);
  const gchar *canary = "stale-voice-plaintext-canary-10e53bc6";
  int fd = g_open(stale, O_CREAT | O_EXCL | O_RDWR, 0600);
  g_assert_cmpint(fd, >=, 0);
  g_assert_cmpint(write(fd, canary, strlen(canary)), ==, (ssize_t)strlen(canary));
  canary_scan_add(scan, "stale voice", canary);
  g_assert_cmpuint(canary_scan_file(scan, stale), >, 0);
  canary_scan_clear_hits(scan);
  g_assert_cmpint(g_chmod(stale, 0200), ==, 0); /* still wipe write-only legacy files */
  g_assert_true(gh_voice_recorder_sweep_stale(private_dir, &error));
  g_assert_no_error(error);
  g_assert_false(g_file_test(stale, G_FILE_TEST_EXISTS));
  GStatBuf retained;
  g_assert_cmpint(fstat(fd, &retained), ==, 0);
  g_assert_cmpint(retained.st_size, ==, 0);
  close(fd);
  n_files = 0;
  g_assert_cmpuint(canary_scan_tree(scan, root, &n_files), ==, 0);
  g_assert_cmpuint(n_files, ==, 0);
  canary_scan_free(scan);
  g_assert_cmpint(g_rmdir(private_dir), ==, 0);
  g_assert_cmpint(g_rmdir(root), ==, 0);
}

/* ---- main ---- */

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/voice/meta/is-audio", test_voice_meta_is_audio);
  g_test_add_func("/voice/meta/is-ogg-opus", test_voice_meta_is_ogg_opus);
  g_test_add_func("/voice/meta/constants", test_voice_constants);
  g_test_add_func("/voice/waveform/new-clamps", test_waveform_new_clamps);
  g_test_add_func("/voice/waveform/refcount", test_waveform_refcount);
  g_test_add_func("/voice/waveform/pcm-silence", test_waveform_from_pcm_silence);
  g_test_add_func("/voice/waveform/pcm-sine", test_waveform_from_pcm_sine);
  g_test_add_func("/voice/waveform/pcm-empty", test_waveform_from_pcm_empty);
  g_test_add_func("/voice/waveform/max-samples-cap", test_waveform_max_samples_cap);
  g_test_add_func("/voice/player/decoded-waveform", test_player_decodes_waveform);
  g_test_add_func("/voice/player/replay-speed", test_player_replay_speed);
  g_test_add_func("/voice/player/partial-plugins", test_player_partial_plugins);
  g_test_add_func("/voice/recorder/private-wipe", test_recorder_private_wipe);
  g_test_add_func("/voice/recorder/crash-recovery", test_recorder_crash_recovery);
  g_test_add_func("/voice/recorder/missing-audio-support", test_recorder_missing_audio_support);

  return g_test_run();
}
