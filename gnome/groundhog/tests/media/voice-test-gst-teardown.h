#pragma once

#include <gst/gst.h>

/* Stopping a pipeline joins its task, not the default pool's reusable worker.
 * Observe one worker's TLS destructor so suite teardown cannot silently revert
 * to leaving that pool alive at process exit (nostrc-csn2w). */
typedef struct {
  GstTask *task;
  GMutex mutex;
  GCond cond;
  gboolean armed;
  gboolean exited;
} VoiceTestGstTeardown;

static void
voice_test_gst_thread_exited(gpointer data)
{
  VoiceTestGstTeardown *state = data;
  g_mutex_lock(&state->mutex);
  state->exited = TRUE;
  g_cond_signal(&state->cond);
  g_mutex_unlock(&state->mutex);
}

static GPrivate voice_test_gst_worker = G_PRIVATE_INIT(voice_test_gst_thread_exited);

static void
voice_test_gst_arm_worker(gpointer data)
{
  VoiceTestGstTeardown *state = data;
  g_assert_null(g_private_get(&voice_test_gst_worker));
  g_private_set(&voice_test_gst_worker, state);
  gst_task_pause(state->task);
  g_mutex_lock(&state->mutex);
  state->armed = TRUE;
  g_cond_signal(&state->cond);
  g_mutex_unlock(&state->mutex);
}

static void
voice_test_gst_wait(VoiceTestGstTeardown *state, const gboolean *predicate)
{
  gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
  g_mutex_lock(&state->mutex);
  while (!*predicate) {
    if (!g_cond_wait_until(&state->cond, &state->mutex, deadline))
      break;
  }
  g_assert_true(*predicate);
  g_mutex_unlock(&state->mutex);
}

/* Call only after all media work has completed; no GStreamer calls may follow.
 * libav legitimately loads libsrt transitively for AAC. Its TLS-key destructor
 * at process exit only frees the current thread's value before deleting the key.
 * Deinitialize GStreamer first, while worker TLS destructors can still run. */
static void
voice_test_gst_deinit(void)
{
  if (!gst_is_initialized())
    return;

  /* Metadata lookup must not load the unrelated GStreamer SRT plugin. Requiring
   * libsrt itself to be absent would incorrectly reject real AAC decoding. */
  GstPlugin *srt = gst_registry_find_plugin(gst_registry_get(), "srt");
  if (srt) {
    g_assert_false(gst_plugin_is_loaded(srt));
    gst_object_unref(srt);
  }

  VoiceTestGstTeardown state = {0};
  GRecMutex task_lock;
  g_mutex_init(&state.mutex);
  g_cond_init(&state.cond);
  g_rec_mutex_init(&task_lock);
  state.task = gst_task_new(voice_test_gst_arm_worker, &state, NULL);
  gst_task_set_lock(state.task, &task_lock);
  g_assert_true(gst_task_start(state.task));
  voice_test_gst_wait(&state, &state.armed);
  g_assert_true(gst_task_stop(state.task));
  g_assert_true(gst_task_join(state.task));
  gst_object_unref(state.task);
  g_rec_mutex_clear(&task_lock);

  gst_deinit();
  voice_test_gst_wait(&state, &state.exited);
  g_cond_clear(&state.cond);
  g_mutex_clear(&state.mutex);
}
