/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gh-voice-player.h"
#include "gh-voice-meta.h"

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <math.h>
#include <string.h>

#define POSITION_UPDATE_MS 100

struct _GhVoicePlayer {
  GObject parent_instance;

  GstElement *pipeline;
  GstElement *appsrc;
  GstBus *bus;
  guint bus_watch_id;
  guint position_timer_id;

  GhVoicePlayerState state;
  gdouble position;
  gdouble duration;
  gdouble speed;

  GBytes *audio_bytes;
  GhVoiceWaveform *waveform;
  GCancellable *waveform_cancellable;
  gsize read_offset;
  gdouble applied_speed;
};

enum {
  PROP_0,
  PROP_STATE,
  PROP_POSITION,
  PROP_DURATION,
  PROP_SPEED,
  N_PROPS
};

enum {
  SIGNAL_POSITION_UPDATED,
  SIGNAL_STATE_CHANGED,
  SIGNAL_WAVEFORM_READY,
  SIGNAL_PLAYBACK_ERROR,
  N_SIGNALS
};

static GParamSpec *props[N_PROPS];
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(GhVoicePlayer, gh_voice_player, G_TYPE_OBJECT)

static void gh_voice_player_tear_down(GhVoicePlayer *self);
static void gh_voice_player_set_state_internal(GhVoicePlayer *self, GhVoicePlayerState st);
static gboolean gh_voice_player_build_pipeline(GhVoicePlayer *self, GError **error);

static void
gh_voice_player_get_property(GObject *object, guint prop_id, GValue *value,
                              GParamSpec *pspec)
{
  GhVoicePlayer *self = GH_VOICE_PLAYER(object);
  switch (prop_id) {
  case PROP_STATE:
    g_value_set_uint(value, self->state);
    break;
  case PROP_POSITION:
    g_value_set_double(value, self->position);
    break;
  case PROP_DURATION:
    g_value_set_double(value, self->duration);
    break;
  case PROP_SPEED:
    g_value_set_double(value, self->speed);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void
gh_voice_player_set_property(GObject *object, guint prop_id, const GValue *value,
                              GParamSpec *pspec)
{
  GhVoicePlayer *self = GH_VOICE_PLAYER(object);
  switch (prop_id) {
  case PROP_SPEED:
    gh_voice_player_set_speed(self, g_value_get_double(value));
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void
gh_voice_player_finalize(GObject *object)
{
  GhVoicePlayer *self = GH_VOICE_PLAYER(object);
  gh_voice_player_tear_down(self);
  g_clear_object(&self->waveform_cancellable);
  g_clear_pointer(&self->waveform, gh_voice_waveform_unref);
  g_clear_pointer(&self->audio_bytes, g_bytes_unref);
  G_OBJECT_CLASS(gh_voice_player_parent_class)->finalize(object);
}

static void
gh_voice_player_class_init(GhVoicePlayerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->get_property = gh_voice_player_get_property;
  object_class->set_property = gh_voice_player_set_property;
  object_class->finalize = gh_voice_player_finalize;

  props[PROP_STATE] =
    g_param_spec_uint("state", NULL, NULL, 0, G_MAXUINT, GH_VOICE_PLAYER_STATE_STOPPED,
                      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  props[PROP_POSITION] =
    g_param_spec_double("position", NULL, NULL, 0.0, G_MAXDOUBLE, 0.0,
                        G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  props[PROP_DURATION] =
    g_param_spec_double("duration", NULL, NULL, 0.0, G_MAXDOUBLE, 0.0,
                        G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  props[PROP_SPEED] =
    g_param_spec_double("speed", NULL, NULL, 0.5, 3.0, 1.0,
                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties(object_class, N_PROPS, props);

  signals[SIGNAL_POSITION_UPDATED] =
    g_signal_new("position-updated", G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 1, G_TYPE_DOUBLE);
  signals[SIGNAL_STATE_CHANGED] =
    g_signal_new("state-changed", G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 1, G_TYPE_UINT);
  signals[SIGNAL_WAVEFORM_READY] =
    g_signal_new("waveform-ready", G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 0);
  signals[SIGNAL_PLAYBACK_ERROR] =
    g_signal_new("playback-error", G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
gh_voice_player_init(GhVoicePlayer *self)
{
  self->state = GH_VOICE_PLAYER_STATE_STOPPED;
  self->speed = 1.0;
}

GhVoicePlayer *
gh_voice_player_new(void)
{
  return g_object_new(GH_TYPE_VOICE_PLAYER, NULL);
}

static void
gh_voice_player_set_state_internal(GhVoicePlayer *self, GhVoicePlayerState st)
{
  if (self->state == st)
    return;
  self->state = st;
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_STATE]);
  g_signal_emit(self, signals[SIGNAL_STATE_CHANGED], 0, (guint)st);
}

static void
gh_voice_player_tear_down(GhVoicePlayer *self)
{
  if (self->position_timer_id) {
    g_source_remove(self->position_timer_id);
    self->position_timer_id = 0;
  }
  if (self->bus_watch_id) {
    g_source_remove(self->bus_watch_id);
    self->bus_watch_id = 0;
  }
  g_clear_object(&self->bus);
  if (self->pipeline) {
    gst_element_set_state(self->pipeline, GST_STATE_NULL);
    g_clear_pointer(&self->pipeline, gst_object_unref);
  }
  self->appsrc = NULL;   /* owned by pipeline */
}

static void
gh_voice_player_query_duration(GhVoicePlayer *self)
{
  if (!self->pipeline)
    return;
  gint64 dur_ns = 0;
  if (gst_element_query_duration(self->pipeline, GST_FORMAT_TIME, &dur_ns) && dur_ns > 0) {
    self->duration = (gdouble)dur_ns / GST_SECOND;
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_DURATION]);
  }
}

static gboolean
gh_voice_player_position_tick(gpointer user_data)
{
  GhVoicePlayer *self = GH_VOICE_PLAYER(user_data);
  if (self->state != GH_VOICE_PLAYER_STATE_PLAYING) {
    self->position_timer_id = 0;
    return G_SOURCE_REMOVE;
  }

  gint64 pos_ns = 0;
  if (self->pipeline &&
      gst_element_query_position(self->pipeline, GST_FORMAT_TIME, &pos_ns) && pos_ns >= 0) {
    self->position = (gdouble)pos_ns / GST_SECOND;
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_POSITION]);
    g_signal_emit(self, signals[SIGNAL_POSITION_UPDATED], 0, self->position);
  }

  /* Try to learn the duration once. */
  if (self->duration <= 0.0)
    gh_voice_player_query_duration(self);

  return G_SOURCE_CONTINUE;
}

static gboolean
gh_voice_player_bus_callback(GstBus *bus G_GNUC_UNUSED, GstMessage *message,
                              gpointer user_data)
{
  GhVoicePlayer *self = GH_VOICE_PLAYER(user_data);

  switch (GST_MESSAGE_TYPE(message)) {
  case GST_MESSAGE_EOS:
    /* A stream source has reached EOS. Rebuild from the retained bytes on
     * the next Play; seeking the old EOS pipeline is not reliable. */
    gh_voice_player_tear_down(self);
    self->position = 0.0;
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_POSITION]);
    gh_voice_player_set_state_internal(self, GH_VOICE_PLAYER_STATE_STOPPED);
    break;
  case GST_MESSAGE_ERROR: {
    GError *error = NULL;
    gst_message_parse_error(message, &error, NULL);
    g_warning("GhVoicePlayer pipeline error: %s", error ? error->message : "(unknown)");
    const gchar *user_error = error && error->domain == GST_STREAM_ERROR &&
      (error->code == GST_STREAM_ERROR_CODEC_NOT_FOUND ||
       error->code == GST_STREAM_ERROR_TYPE_NOT_FOUND)
      ? "Voice messages aren't available: missing audio support"
      : "Voice message couldn't be played";
    g_signal_emit(self, signals[SIGNAL_PLAYBACK_ERROR], 0, user_error);
    g_clear_error(&error);
    gh_voice_player_tear_down(self);
    gh_voice_player_set_state_internal(self, GH_VOICE_PLAYER_STATE_STOPPED);
    break;
  }
  case GST_MESSAGE_DURATION_CHANGED:
    gh_voice_player_query_duration(self);
    break;
  case GST_MESSAGE_ASYNC_DONE:
    if (self->pipeline && self->state == GH_VOICE_PLAYER_STATE_PLAYING &&
        self->applied_speed != self->speed) {
      gint64 pos_ns = 0;
      gst_element_query_position(self->pipeline, GST_FORMAT_TIME, &pos_ns);
      if (gst_element_seek(self->pipeline, self->speed, GST_FORMAT_TIME,
                           GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE,
                           GST_SEEK_TYPE_SET, MAX(pos_ns, 0),
                           GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE))
        self->applied_speed = self->speed;
    }
    break;
  default:
    break;
  }

  return G_SOURCE_CONTINUE;
}

/* A seekable in-memory source lets GStreamer re-read compressed bytes when
 * the user changes playback rate or seeks. Keep each push bounded. */
static gboolean
on_seek_data(GstAppSrc *src G_GNUC_UNUSED, guint64 offset, gpointer user_data)
{
  GhVoicePlayer *self = GH_VOICE_PLAYER(user_data);
  if (!self->audio_bytes || offset > g_bytes_get_size(self->audio_bytes))
    return FALSE;
  self->read_offset = (gsize)offset;
  return TRUE;
}

static void
on_need_data(GstAppSrc *src, guint length, gpointer user_data)
{
  GhVoicePlayer *self = GH_VOICE_PLAYER(user_data);
  if (!self->audio_bytes)
    return;

  gsize size = 0;
  const guint8 *data = g_bytes_get_data(self->audio_bytes, &size);
  if (self->read_offset >= size) {
    gst_app_src_end_of_stream(src);
    return;
  }
  gsize chunk = MIN(size - self->read_offset, (gsize)65536);
  if (length > 0)
    chunk = MIN(chunk, (gsize)length);
  GstBuffer *buf = gst_buffer_new_memdup(data + self->read_offset, chunk);
  self->read_offset += chunk;
  gst_app_src_push_buffer(src, buf);
}

/* Check that all required GStreamer element factories are available before
 * building the pipeline. Returns TRUE if all are present, or sets error
 * and returns FALSE with a user-facing message (finding 5). */
static gboolean
gh_voice_player_check_factories(GError **error)
{
  static const gchar *required[] = {
    "appsrc", "decodebin", "audioconvert", "audioresample",
    "scaletempo", "autoaudiosink", NULL
  };
  for (const gchar **name = required; *name; name++) {
    GstElementFactory *f = gst_element_factory_find(*name);
    if (!f) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                  "Voice messages aren't available: missing audio support (%s)", *name);
      return FALSE;
    }
    gst_object_unref(f);
  }
  return TRUE;
}

/* Build (or rebuild) the playback pipeline from the stored audio_bytes.
 * Returns FALSE and sets error on failure (finding 5: validates factories
 * and pipeline structure). */
static gboolean
gh_voice_player_build_pipeline(GhVoicePlayer *self, GError **error)
{
  gh_voice_player_tear_down(self);

  if (!gh_voice_player_check_factories(error))
    return FALSE;

  GError *parse_error = NULL;
  self->pipeline = gst_parse_launch(
    "appsrc name=src ! decodebin ! audioconvert ! audioresample ! "
    "scaletempo ! autoaudiosink", &parse_error);
  if (!self->pipeline || parse_error || !GST_IS_BIN(self->pipeline)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                "Voice messages aren't available: missing audio support");
    g_clear_error(&parse_error);
    if (self->pipeline)
      g_clear_pointer(&self->pipeline, gst_object_unref);
    return FALSE;
  }

  self->appsrc = gst_bin_get_by_name(GST_BIN(self->pipeline), "src");
  if (!self->appsrc) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Voice messages aren't available: missing audio support");
    g_clear_pointer(&self->pipeline, gst_object_unref);
    return FALSE;
  }

  self->read_offset = 0;
  self->applied_speed = 1.0;
  g_object_set(self->appsrc, "stream-type", GST_APP_STREAM_TYPE_SEEKABLE,
               "format", GST_FORMAT_BYTES, NULL);

  gsize total_size = g_bytes_get_size(self->audio_bytes);
  g_object_set(self->appsrc, "size", (gint64)total_size, NULL);
  gst_object_unref(self->appsrc);   /* pipeline keeps it */

  g_signal_connect(self->appsrc, "need-data", G_CALLBACK(on_need_data), self);
  g_signal_connect(self->appsrc, "seek-data", G_CALLBACK(on_seek_data), self);

  self->bus = gst_element_get_bus(self->pipeline);
  self->bus_watch_id = gst_bus_add_watch(self->bus, gh_voice_player_bus_callback, self);

  /* Pre-roll to get the duration. */
  gst_element_set_state(self->pipeline, GST_STATE_PAUSED);

  return TRUE;
}

/* Decode the explicitly downloaded audio in a worker. MDK 0.11 does not
 * transmit waveform samples in imeta, so the UI derives an 80-bar RMS
 * envelope from real PCM. Never interpret compressed container bytes as PCM.
 * The 8 kHz/5-minute and 10-second wall limits bound untrusted input. */
static GhVoiceWaveform *
voice_decode_waveform(GBytes *bytes, GCancellable *cancellable)
{
  GstElement *pipeline = NULL;
  GstElement *source = NULL;
  GstElement *sink = NULL;
  GstBus *bus = NULL;
  GArray *windows = NULL;
  GhVoiceWaveform *result = NULL;
  GError *parse_error = NULL;
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
  const gchar *format = "F32LE";
#else
  const gchar *format = "F32BE";
#endif
  g_autofree gchar *description = g_strdup_printf(
    "appsrc name=src ! decodebin ! audioconvert ! audioresample ! "
    "audio/x-raw,format=%s,channels=1,rate=8000 ! appsink name=sink "
    "sync=false max-buffers=4", format);
  pipeline = gst_parse_launch(description, &parse_error);
  if (!pipeline || parse_error || !GST_IS_BIN(pipeline))
    goto out;
  source = gst_bin_get_by_name(GST_BIN(pipeline), "src");
  sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
  if (!GST_IS_APP_SRC(source) || !GST_IS_APP_SINK(sink))
    goto out;

  gsize size = 0;
  gconstpointer data = g_bytes_get_data(bytes, &size);
  g_object_set(source, "stream-type", GST_APP_STREAM_TYPE_STREAM,
               "format", GST_FORMAT_BYTES, "size", (gint64)size, NULL);
  bus = gst_element_get_bus(pipeline);
  windows = g_array_new(FALSE, FALSE, sizeof(gdouble));
  if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
    goto out;
  GstBuffer *input = gst_buffer_new_memdup(data, size);
  if (gst_app_src_push_buffer(GST_APP_SRC(source), input) != GST_FLOW_OK)
    goto out;
  gst_app_src_end_of_stream(GST_APP_SRC(source));

  gdouble sum_sq = 0.0;
  guint window_frames = 0;
  guint total_frames = 0;
  gint64 deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
  while (!g_cancellable_is_cancelled(cancellable) &&
         g_get_monotonic_time() < deadline && total_frames < 8000 * 300) {
    GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink),
                                                      100 * GST_MSECOND);
    if (!sample) {
      if (gst_app_sink_is_eos(GST_APP_SINK(sink)))
        break;
      GstMessage *message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
      if (message) {
        gst_message_unref(message);
        break;
      }
      continue;
    }
    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstMapInfo map;
    if (buffer && gst_buffer_map(buffer, &map, GST_MAP_READ)) {
      for (gsize offset = 0; offset + sizeof(gfloat) <= map.size &&
           total_frames < 8000 * 300; offset += sizeof(gfloat)) {
        gfloat value;
        memcpy(&value, map.data + offset, sizeof value);
        if (!isfinite(value))
          value = 0.0f;
        sum_sq += (gdouble)value * value;
        window_frames++;
        total_frames++;
        if (window_frames == 800) {
          gdouble rms = sqrt(sum_sq / window_frames);
          g_array_append_val(windows, rms);
          sum_sq = 0.0;
          window_frames = 0;
        }
      }
      gst_buffer_unmap(buffer, &map);
    }
    gst_sample_unref(sample);
  }
  if (window_frames > 0) {
    gdouble rms = sqrt(sum_sq / window_frames);
    g_array_append_val(windows, rms);
  }
  if (windows->len > 0 && !g_cancellable_is_cancelled(cancellable)) {
    guint count = MIN(windows->len, 80u);
    gdouble bars[80] = { 0 };
    for (guint i = 0; i < count; i++) {
      guint first = i * windows->len / count;
      guint end = (i + 1) * windows->len / count;
      gdouble sum = 0.0;
      for (guint j = first; j < end; j++) {
        gdouble rms = g_array_index(windows, gdouble, j);
        sum += rms * rms;
      }
      bars[i] = CLAMP(2.0 * sqrt(sum / (end - first)), 0.0, 1.0);
    }
    result = gh_voice_waveform_new(bars, count);
  }
out:
  if (pipeline)
    gst_element_set_state(pipeline, GST_STATE_NULL);
  g_clear_pointer(&bus, gst_object_unref);
  g_clear_pointer(&source, gst_object_unref);
  g_clear_pointer(&sink, gst_object_unref);
  g_clear_pointer(&pipeline, gst_object_unref);
  g_clear_pointer(&windows, g_array_unref);
  g_clear_error(&parse_error);
  return result;
}

static void
voice_waveform_worker(GTask *task, gpointer source_object G_GNUC_UNUSED,
                      gpointer task_data, GCancellable *cancellable)
{
  GhVoiceWaveform *waveform = voice_decode_waveform(task_data, cancellable);
  g_task_return_pointer(task, waveform, (GDestroyNotify)gh_voice_waveform_unref);
}

static void
voice_waveform_done(GObject *source, GAsyncResult *async_result,
                    gpointer user_data G_GNUC_UNUSED)
{
  GhVoicePlayer *self = GH_VOICE_PLAYER(source);
  GTask *task = G_TASK(async_result);
  g_autoptr(GError) error = NULL;
  GhVoiceWaveform *waveform = g_task_propagate_pointer(task, &error);
  if (g_task_get_cancellable(task) == self->waveform_cancellable && waveform)
    gh_voice_player_set_waveform(self, waveform);
  g_clear_pointer(&waveform, gh_voice_waveform_unref);
}

gboolean
gh_voice_player_load(GhVoicePlayer *self, GBytes *audio_bytes,
                     GhVoiceWaveform *waveform, GError **error)
{
  g_return_val_if_fail(GH_IS_VOICE_PLAYER(self), FALSE);
  g_return_val_if_fail(audio_bytes != NULL, FALSE);

  gh_voice_player_tear_down(self);
  if (self->waveform_cancellable)
    g_cancellable_cancel(self->waveform_cancellable);
  g_clear_object(&self->waveform_cancellable);
  gh_voice_player_set_state_internal(self, GH_VOICE_PLAYER_STATE_STOPPED);

  g_clear_pointer(&self->audio_bytes, g_bytes_unref);
  self->audio_bytes = g_bytes_ref(audio_bytes);
  g_clear_pointer(&self->waveform, gh_voice_waveform_unref);
  if (waveform)
    self->waveform = gh_voice_waveform_ref(waveform);

  self->position = 0.0;
  self->duration = 0.0;

  if (!gst_is_initialized()) {
    GError *gst_error = NULL;
    if (!gst_init_check(NULL, NULL, &gst_error)) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "GStreamer initialisation failed: %s",
                  gst_error ? gst_error->message : "(unknown)");
      g_clear_error(&gst_error);
      return FALSE;
    }
  }

  /* Build the pipeline. Privacy: no MPRIS element, no media-keys, no
   * application-name on the sink — the audio never appears in system
   * "now playing" notifications. */
  if (!gh_voice_player_build_pipeline(self, error))
    return FALSE;

  if (!waveform) {
    self->waveform_cancellable = g_cancellable_new();
    GTask *task = g_task_new(self, self->waveform_cancellable,
                             voice_waveform_done, NULL);
    g_task_set_task_data(task, g_bytes_ref(audio_bytes),
                         (GDestroyNotify)g_bytes_unref);
    g_task_run_in_thread(task, voice_waveform_worker);
    g_object_unref(task);
  }
  return TRUE;
}

void
gh_voice_player_play(GhVoicePlayer *self)
{
  g_return_if_fail(GH_IS_VOICE_PLAYER(self));

  /* If the pipeline was torn down (error) but we have bytes, rebuild it
   * for replay (finding 9). */
  if (!self->pipeline && self->audio_bytes) {
    GError *error = NULL;
    if (!gh_voice_player_build_pipeline(self, &error)) {
      g_warning("GhVoicePlayer: cannot rebuild pipeline for replay: %s",
                error ? error->message : "(unknown)");
      g_clear_error(&error);
      return;
    }
  }
  if (!self->pipeline)
    return;

  /* ASYNC_DONE applies the selected rate after decoder preroll. A seek on
   * an unprerolled decodebin can fail silently. */
  gst_element_set_state(self->pipeline, GST_STATE_PLAYING);
  gh_voice_player_set_state_internal(self, GH_VOICE_PLAYER_STATE_PLAYING);

  if (!self->position_timer_id)
    self->position_timer_id =
      g_timeout_add(POSITION_UPDATE_MS, gh_voice_player_position_tick, self);
}

void
gh_voice_player_pause(GhVoicePlayer *self)
{
  g_return_if_fail(GH_IS_VOICE_PLAYER(self));
  if (!self->pipeline)
    return;
  if (self->state != GH_VOICE_PLAYER_STATE_PLAYING)
    return;

  gst_element_set_state(self->pipeline, GST_STATE_PAUSED);
  gh_voice_player_set_state_internal(self, GH_VOICE_PLAYER_STATE_PAUSED);
}

void
gh_voice_player_stop(GhVoicePlayer *self)
{
  g_return_if_fail(GH_IS_VOICE_PLAYER(self));

  self->position = 0.0;
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_POSITION]);

  gh_voice_player_tear_down(self);
  if (self->waveform_cancellable)
    g_cancellable_cancel(self->waveform_cancellable);
  g_clear_object(&self->waveform_cancellable);
  gh_voice_player_set_state_internal(self, GH_VOICE_PLAYER_STATE_STOPPED);
}

void
gh_voice_player_seek(GhVoicePlayer *self, gdouble position_s)
{
  g_return_if_fail(GH_IS_VOICE_PLAYER(self));
  if (!self->pipeline)
    return;

  gint64 pos_ns = (gint64)(position_s * GST_SECOND);
  gst_element_seek_simple(self->pipeline, GST_FORMAT_TIME,
                          GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT, pos_ns);
  self->position = position_s;
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_POSITION]);
}

GhVoicePlayerState
gh_voice_player_get_state(GhVoicePlayer *self)
{
  g_return_val_if_fail(GH_IS_VOICE_PLAYER(self), GH_VOICE_PLAYER_STATE_STOPPED);
  return self->state;
}

gdouble
gh_voice_player_get_position(GhVoicePlayer *self)
{
  g_return_val_if_fail(GH_IS_VOICE_PLAYER(self), 0.0);
  return self->position;
}

gdouble
gh_voice_player_get_duration(GhVoicePlayer *self)
{
  g_return_val_if_fail(GH_IS_VOICE_PLAYER(self), 0.0);
  return self->duration;
}

void
gh_voice_player_set_speed(GhVoicePlayer *self, gdouble speed)
{
  g_return_if_fail(GH_IS_VOICE_PLAYER(self));
  speed = CLAMP(speed, 0.5, 3.0);
  if (self->speed == speed)
    return;
  self->speed = speed;
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_SPEED]);

  /* Apply the rate change to the running pipeline; if not playing the rate
   * is applied at the next play() call (finding 9). */
  if (self->pipeline && self->state == GH_VOICE_PLAYER_STATE_PLAYING) {
    gint64 pos_ns = 0;
    gst_element_query_position(self->pipeline, GST_FORMAT_TIME, &pos_ns);
    if (gst_element_seek(self->pipeline, speed, GST_FORMAT_TIME,
                         GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE,
                         GST_SEEK_TYPE_SET, pos_ns,
                         GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE))
      self->applied_speed = speed;
  }
}

gdouble
gh_voice_player_get_speed(GhVoicePlayer *self)
{
  g_return_val_if_fail(GH_IS_VOICE_PLAYER(self), 1.0);
  return self->speed;
}

gdouble
gh_voice_player_get_active_speed(GhVoicePlayer *self)
{
  g_return_val_if_fail(GH_IS_VOICE_PLAYER(self), 1.0);
  return self->applied_speed;
}

GhVoiceWaveform *
gh_voice_player_dup_waveform(GhVoicePlayer *self)
{
  g_return_val_if_fail(GH_IS_VOICE_PLAYER(self), NULL);
  if (!self->waveform)
    return NULL;
  return gh_voice_waveform_ref(self->waveform);
}

void
gh_voice_player_set_waveform(GhVoicePlayer *self, GhVoiceWaveform *waveform)
{
  g_return_if_fail(GH_IS_VOICE_PLAYER(self));
  g_clear_pointer(&self->waveform, gh_voice_waveform_unref);
  if (waveform)
    self->waveform = gh_voice_waveform_ref(waveform);
  g_signal_emit(self, signals[SIGNAL_WAVEFORM_READY], 0);
}
