/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gh-voice-recorder.h"
#include "gh-voice-meta.h"

#include <glib/gstdio.h>
#include <gst/gst.h>
#include <math.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

/* The level element posts messages at this interval. */
#define LEVEL_INTERVAL_NS (100 * GST_MSECOND)
/* Waveform: accumulate one sample every ~250 ms. */
#define WAVEFORM_INTERVAL_MS 250

struct _GhVoiceRecorder {
  GObject parent_instance;

  gchar *private_dir;
  gint temp_fd;
  gchar *source_factory;

  GstElement *pipeline;
  GstBus *bus;
  guint bus_watch_id;

  GhVoiceRecorderState state;
  gdouble duration;
  gdouble level;

  /* Waveform accumulation during recording. */
  GArray *waveform_levels;       /* gdouble samples accumulated */
  gdouble level_accumulator;     /* sum of squared levels in the current window */
  guint level_count;             /* number of level messages in the current window */
  gint64 last_waveform_time_ms;  /* monotonic time of last sample */

  /* Duration tick timer. */
  guint duration_timer_id;
  gint64 start_time_us;          /* g_get_monotonic_time() at recording start */
};

enum {
  PROP_0,
  PROP_STATE,
  PROP_DURATION,
  PROP_LEVEL,
  N_PROPS
};

enum {
  SIGNAL_LEVEL_UPDATED,
  SIGNAL_STATE_CHANGED,
  SIGNAL_DURATION_TICK,
  N_SIGNALS
};

static GParamSpec *props[N_PROPS];
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(GhVoiceRecorder, gh_voice_recorder, G_TYPE_OBJECT)

static void gh_voice_recorder_wipe_temp(GhVoiceRecorder *self);
static void gh_voice_recorder_set_state(GhVoiceRecorder *self, GhVoiceRecorderState state);
static void gh_voice_recorder_tear_down_pipeline(GhVoiceRecorder *self);

static void
gh_voice_recorder_get_property(GObject *object, guint prop_id, GValue *value,
                                GParamSpec *pspec)
{
  GhVoiceRecorder *self = GH_VOICE_RECORDER(object);
  switch (prop_id) {
  case PROP_STATE:
    g_value_set_uint(value, self->state);
    break;
  case PROP_DURATION:
    g_value_set_double(value, self->duration);
    break;
  case PROP_LEVEL:
    g_value_set_double(value, self->level);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void
gh_voice_recorder_finalize(GObject *object)
{
  GhVoiceRecorder *self = GH_VOICE_RECORDER(object);

  gh_voice_recorder_tear_down_pipeline(self);

  /* Always wipe the temp file on finalize — the caller should have read
   * the bytes by now (finding 2: no plaintext left behind). */
  gh_voice_recorder_wipe_temp(self);

  g_clear_pointer(&self->waveform_levels, g_array_unref);
  g_free(self->private_dir);
  g_free(self->source_factory);

  G_OBJECT_CLASS(gh_voice_recorder_parent_class)->finalize(object);
}

static void
gh_voice_recorder_class_init(GhVoiceRecorderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->get_property = gh_voice_recorder_get_property;
  object_class->finalize = gh_voice_recorder_finalize;

  props[PROP_STATE] =
    g_param_spec_uint("state", NULL, NULL, 0, G_MAXUINT, GH_VOICE_RECORDER_STATE_IDLE,
                      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  props[PROP_DURATION] =
    g_param_spec_double("duration", NULL, NULL, 0.0, G_MAXDOUBLE, 0.0,
                        G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  props[PROP_LEVEL] =
    g_param_spec_double("level", NULL, NULL, 0.0, 1.0, 0.0,
                        G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties(object_class, N_PROPS, props);

  signals[SIGNAL_LEVEL_UPDATED] =
    g_signal_new("level-updated", G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 1, G_TYPE_DOUBLE);
  signals[SIGNAL_STATE_CHANGED] =
    g_signal_new("state-changed", G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 1, G_TYPE_UINT);
  signals[SIGNAL_DURATION_TICK] =
    g_signal_new("duration-tick", G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 1, G_TYPE_DOUBLE);
}

static void
gh_voice_recorder_init(GhVoiceRecorder *self)
{
  self->state = GH_VOICE_RECORDER_STATE_IDLE;
  self->temp_fd = -1;
  self->waveform_levels = g_array_new(FALSE, FALSE, sizeof(gdouble));
}

GhVoiceRecorder *
gh_voice_recorder_new_with_source(const gchar *private_dir, const gchar *source_factory)
{
  g_return_val_if_fail(private_dir != NULL, NULL);
  /* Only a fixed factory name may enter the pipeline description. The test
   * source exercises the actual recorder without requiring a microphone. */
  g_return_val_if_fail(g_strcmp0(source_factory, "autoaudiosrc") == 0 ||
                       g_strcmp0(source_factory, "audiotestsrc") == 0, NULL);

  GhVoiceRecorder *self = g_object_new(GH_TYPE_VOICE_RECORDER, NULL);
  self->private_dir = g_strdup(private_dir);
  self->source_factory = g_strdup(source_factory);
  return self;
}

GhVoiceRecorder *
gh_voice_recorder_new(const gchar *private_dir)
{
  return gh_voice_recorder_new_with_source(private_dir, "autoaudiosrc");
}

static void
gh_voice_recorder_set_state(GhVoiceRecorder *self, GhVoiceRecorderState state)
{
  if (self->state == state)
    return;
  self->state = state;
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_STATE]);
  g_signal_emit(self, signals[SIGNAL_STATE_CHANGED], 0, (guint)state);
}

static gboolean
gh_voice_recorder_wipe_fd(int fd)
{
  struct stat st;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink > 1)
    return FALSE;
  gboolean clean = TRUE;
  if (st.st_size > 0) {
    if (lseek(fd, 0, SEEK_SET) != 0) {
      clean = FALSE;
    } else {
      static const guint8 zeros[4096] = { 0 };
      off_t remaining = st.st_size;
      while (remaining > 0) {
        size_t chunk = (size_t)MIN((off_t)sizeof zeros, remaining);
        ssize_t written = write(fd, zeros, chunk);
        if (written < 0 && errno == EINTR)
          continue;
        if (written <= 0) {
          clean = FALSE;
          break;
        }
        remaining -= written;
      }
      if (clean && fsync(fd) != 0)
        clean = FALSE;
    }
  }
  if (ftruncate(fd, 0) != 0)
    clean = FALSE;
  if (fsync(fd) != 0)
    clean = FALSE;
  return clean;
}

static void
gh_voice_recorder_wipe_temp(GhVoiceRecorder *self)
{
  if (self->temp_fd < 0)
    return;
  /* The inode was unlinked before capture began. Zero/truncate the still-open
   * inode on normal handoff or cancellation; a crash closes it automatically. */
  gh_voice_recorder_wipe_fd(self->temp_fd);
  close(self->temp_fd);
  self->temp_fd = -1;
}

/* Older builds used named files. Remove those at attachment-UI startup, and
 * also cover the tiny create-to-unlink window in the current implementation. */
gboolean
gh_voice_recorder_sweep_stale(const gchar *private_dir, GError **error)
{
  g_return_val_if_fail(private_dir != NULL, FALSE);
  GStatBuf dir_stat;
  if (g_lstat(private_dir, &dir_stat) != 0) {
    if (errno == ENOENT)
      return TRUE;
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                "Cannot inspect voice recording directory: %s", g_strerror(errno));
    return FALSE;
  }
  if (!S_ISDIR(dir_stat.st_mode) || dir_stat.st_uid != geteuid() ||
      (dir_stat.st_mode & 0077) != 0) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Voice recording directory has unsafe permissions");
    return FALSE;
  }
  g_autoptr(GDir) dir = g_dir_open(private_dir, 0, error);
  if (!dir)
    return FALSE;
  const gchar *name;
  while ((name = g_dir_read_name(dir)) != NULL) {
    if (!g_str_has_prefix(name, "gh-voice-"))
      continue;
    g_autofree gchar *path = g_build_filename(private_dir, name, NULL);
    GStatBuf before;
    if (g_lstat(path, &before) != 0) {
      if (errno == ENOENT)
        continue;
      g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                  "Cannot inspect stale voice recording: %s", g_strerror(errno));
      return FALSE;
    }
    if (!S_ISREG(before.st_mode) || before.st_uid != geteuid() || before.st_nlink != 1)
      continue;
    int flags = O_WRONLY;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = g_open(path, flags, 0);
    if (fd < 0) {
      g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                  "Cannot open stale voice recording: %s", g_strerror(errno));
      return FALSE;
    }
    struct stat opened;
    gboolean same_file = fstat(fd, &opened) == 0 && S_ISREG(opened.st_mode) &&
                         opened.st_dev == before.st_dev && opened.st_ino == before.st_ino &&
                         opened.st_nlink == 1;
    if (!same_file) {
      close(fd);
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "Stale voice recording changed during cleanup");
      return FALSE;
    }
    gboolean wiped = gh_voice_recorder_wipe_fd(fd);
    close(fd);
    if (g_unlink(path) != 0) {
      g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                  "Cannot remove stale voice recording: %s", g_strerror(errno));
      return FALSE;
    }
    if (!wiped) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "Cannot reliably wipe stale voice recording");
      return FALSE;
    }
  }
  return TRUE;
}

static void
gh_voice_recorder_tear_down_pipeline(GhVoiceRecorder *self)
{
  if (self->duration_timer_id) {
    g_source_remove(self->duration_timer_id);
    self->duration_timer_id = 0;
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
}

static void
gh_voice_recorder_accumulate_level(GhVoiceRecorder *self, gdouble rms_db)
{
  /* Convert dB to linear (clamp floor). */
  gdouble linear = (rms_db > -60.0) ? pow(10.0, rms_db / 20.0) : 0.0;
  linear = CLAMP(linear, 0.0, 1.0);

  self->level = linear;
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_LEVEL]);
  g_signal_emit(self, signals[SIGNAL_LEVEL_UPDATED], 0, linear);

  /* Accumulate for waveform. */
  self->level_accumulator += linear * linear;
  self->level_count++;

  gint64 now = g_get_monotonic_time() / 1000;
  if (now - self->last_waveform_time_ms >= WAVEFORM_INTERVAL_MS) {
    gdouble sample = 0.0;
    if (self->level_count > 0)
      sample = sqrt(self->level_accumulator / self->level_count);
    sample = CLAMP(sample, 0.0, 1.0);
    g_array_append_val(self->waveform_levels, sample);
    self->level_accumulator = 0.0;
    self->level_count = 0;
    self->last_waveform_time_ms = now;
  }
}

static gboolean
gh_voice_recorder_bus_callback(GstBus *bus G_GNUC_UNUSED, GstMessage *message,
                               gpointer user_data)
{
  GhVoiceRecorder *self = GH_VOICE_RECORDER(user_data);

  switch (GST_MESSAGE_TYPE(message)) {
  case GST_MESSAGE_ELEMENT: {
    const GstStructure *s = gst_message_get_structure(message);
    if (s && gst_structure_has_name(s, "level")) {
      const GValue *rms_list = gst_structure_get_value(s, "rms");
      if (rms_list && GST_VALUE_HOLDS_LIST(rms_list) &&
          gst_value_list_get_size(rms_list) > 0) {
        const GValue *rms_val = gst_value_list_get_value(rms_list, 0);
        gdouble rms_db = g_value_get_double(rms_val);
        gh_voice_recorder_accumulate_level(self, rms_db);
      }
    }
    break;
  }
  case GST_MESSAGE_EOS:
    gh_voice_recorder_tear_down_pipeline(self);
    gh_voice_recorder_set_state(self, GH_VOICE_RECORDER_STATE_DONE);
    break;
  case GST_MESSAGE_ERROR: {
    GError *error = NULL;
    gst_message_parse_error(message, &error, NULL);
    g_warning("GhVoiceRecorder pipeline error: %s", error ? error->message : "(unknown)");
    g_clear_error(&error);
    gh_voice_recorder_tear_down_pipeline(self);
    gh_voice_recorder_wipe_temp(self);
    gh_voice_recorder_set_state(self, GH_VOICE_RECORDER_STATE_ERROR);
    break;
  }
  default:
    break;
  }

  return G_SOURCE_CONTINUE;
}

static gboolean
gh_voice_recorder_duration_tick(gpointer user_data)
{
  GhVoiceRecorder *self = GH_VOICE_RECORDER(user_data);
  if (self->state != GH_VOICE_RECORDER_STATE_RECORDING)
    return G_SOURCE_REMOVE;

  gint64 now = g_get_monotonic_time();
  self->duration = (gdouble)(now - self->start_time_us) / G_USEC_PER_SEC;
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_DURATION]);
  g_signal_emit(self, signals[SIGNAL_DURATION_TICK], 0, self->duration);

  /* Auto-stop at max duration. */
  if (self->duration >= (gdouble)GH_VOICE_MAX_DURATION_S) {
    gh_voice_recorder_stop(self);
    return G_SOURCE_REMOVE;
  }

  return G_SOURCE_CONTINUE;
}

gboolean
gh_voice_recorder_start(GhVoiceRecorder *self, GError **error)
{
  g_return_val_if_fail(GH_IS_VOICE_RECORDER(self), FALSE);
  g_return_val_if_fail(self->state == GH_VOICE_RECORDER_STATE_IDLE ||
                       self->state == GH_VOICE_RECORDER_STATE_DONE ||
                       self->state == GH_VOICE_RECORDER_STATE_CANCELLED ||
                       self->state == GH_VOICE_RECORDER_STATE_ERROR, FALSE);

  /* Initialize GStreamer if not already. */
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

  static const gchar *required[] = {
    "audioconvert", "audioresample", "level", "opusenc", "oggmux", "fdsink", NULL
  };
  for (const gchar **name = required; *name; name++) {
    GstElementFactory *factory = gst_element_factory_find(*name);
    if (!factory) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                  "Voice messages aren't available: missing audio support (%s)", *name);
      return FALSE;
    }
    gst_object_unref(factory);
  }
  GstElementFactory *source_factory = gst_element_factory_find(self->source_factory);
  if (!source_factory) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                "Voice messages aren't available: missing audio support (%s)",
                self->source_factory);
    return FALSE;
  }
  gst_object_unref(source_factory);

  /* Clean up any previous recording. */
  gh_voice_recorder_tear_down_pipeline(self);
  gh_voice_recorder_wipe_temp(self);
  g_array_set_size(self->waveform_levels, 0);
  self->level_accumulator = 0.0;
  self->level_count = 0;
  self->duration = 0.0;
  self->level = 0.0;

  /* Create the private directory with restricted permissions and validate
   * it (finding 2: check directory). */
  if (g_mkdir_with_parents(self->private_dir, 0700) != 0) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Cannot create voice recording directory");
    return FALSE;
  }
  GStatBuf dir_stat;
  if (g_lstat(self->private_dir, &dir_stat) != 0 ||
      !S_ISDIR(dir_stat.st_mode) || dir_stat.st_uid != geteuid() ||
      (dir_stat.st_mode & 0077) != 0) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Voice recording directory has unsafe permissions");
    return FALSE;
  }

  /* Create a unique 0600 inode, then remove its name BEFORE capture starts.
   * fdsink writes to the open descriptor. SIGKILL closes the last descriptor,
   * so no named plaintext recording remains in the persistent cache. */
  g_autofree gchar *temp_path =
    g_build_filename(self->private_dir, "gh-voice-XXXXXX", NULL);
  int flags = O_CREAT | O_EXCL | O_RDWR;
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
  int tmpfd = g_mkstemp_full(temp_path, flags, 0600);
  if (tmpfd < 0) {
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                "Cannot create voice recording file: %s", g_strerror(errno));
    return FALSE;
  }
#ifndef O_CLOEXEC
  fcntl(tmpfd, F_SETFD, FD_CLOEXEC);
#endif
  if (g_unlink(temp_path) != 0) {
    int saved_errno = errno;
    gh_voice_recorder_wipe_fd(tmpfd);
    close(tmpfd);
    g_unlink(temp_path);
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved_errno),
                "Cannot unlink voice recording file: %s", g_strerror(saved_errno));
    return FALSE;
  }
  self->temp_fd = tmpfd;

  /* Build the pipeline:
   * autoaudiosrc → audioconvert → audioresample → level → opusenc → oggmux → fdsink
   *
   * The level element gives us amplitude updates for the meter and waveform.
   * audioconvert and audioresample ensure the audio is in a format opusenc
   * accepts (mono float at 48000 Hz). */
  g_autofree gchar *pipeline_desc = g_strdup_printf(
    "%s ! audioconvert ! audioresample ! audio/x-raw,rate=48000,channels=1 ! "
    "level post-messages=true interval=%" G_GUINT64_FORMAT " ! "
    "opusenc bitrate=32000 ! oggmux ! fdsink name=voice_sink sync=false",
    self->source_factory, (guint64)LEVEL_INTERVAL_NS);

  GError *parse_error = NULL;
  self->pipeline = gst_parse_launch(pipeline_desc, &parse_error);
  if (!self->pipeline || parse_error || !GST_IS_BIN(self->pipeline)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                "Voice messages aren't available: missing audio support%s%s",
                parse_error ? ": " : "", parse_error ? parse_error->message : "");
    g_clear_error(&parse_error);
    gh_voice_recorder_tear_down_pipeline(self);
    gh_voice_recorder_wipe_temp(self);
    return FALSE;
  }

  GstElement *sink = gst_bin_get_by_name(GST_BIN(self->pipeline), "voice_sink");
  if (!sink) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                "Voice messages aren't available: missing audio support (fdsink)");
    gh_voice_recorder_tear_down_pipeline(self);
    gh_voice_recorder_wipe_temp(self);
    return FALSE;
  }
  g_object_set(sink, "fd", self->temp_fd, NULL);
  gst_object_unref(sink);

  /* Watch the bus for level messages, EOS and errors. */
  self->bus = gst_element_get_bus(self->pipeline);
  self->bus_watch_id = gst_bus_add_watch(self->bus, gh_voice_recorder_bus_callback, self);

  /* Start recording. */
  GstStateChangeReturn ret = gst_element_set_state(self->pipeline, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Cannot start recording: pipeline failed to enter playing state");
    gh_voice_recorder_tear_down_pipeline(self);
    gh_voice_recorder_wipe_temp(self);
    return FALSE;
  }

  self->start_time_us = g_get_monotonic_time();
  self->last_waveform_time_ms = self->start_time_us / 1000;

  /* Duration tick every second. */
  self->duration_timer_id = g_timeout_add(1000, gh_voice_recorder_duration_tick, self);

  gh_voice_recorder_set_state(self, GH_VOICE_RECORDER_STATE_RECORDING);
  return TRUE;
}

void
gh_voice_recorder_stop(GhVoiceRecorder *self)
{
  g_return_if_fail(GH_IS_VOICE_RECORDER(self));
  if (self->state != GH_VOICE_RECORDER_STATE_RECORDING)
    return;

  /* Flush the last waveform window. */
  if (self->level_count > 0) {
    gdouble sample = sqrt(self->level_accumulator / self->level_count);
    sample = CLAMP(sample, 0.0, 1.0);
    g_array_append_val(self->waveform_levels, sample);
    self->level_accumulator = 0.0;
    self->level_count = 0;
  }

  /* Update final duration. */
  gint64 now = g_get_monotonic_time();
  self->duration = (gdouble)(now - self->start_time_us) / G_USEC_PER_SEC;
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_DURATION]);

  /* Send EOS to flush the pipeline, which moves to DONE in the bus callback. */
  if (self->pipeline)
    gst_element_send_event(self->pipeline, gst_event_new_eos());
}

void
gh_voice_recorder_cancel(GhVoiceRecorder *self)
{
  g_return_if_fail(GH_IS_VOICE_RECORDER(self));
  if (self->state != GH_VOICE_RECORDER_STATE_RECORDING)
    return;

  gh_voice_recorder_tear_down_pipeline(self);
  gh_voice_recorder_wipe_temp(self);
  gh_voice_recorder_set_state(self, GH_VOICE_RECORDER_STATE_CANCELLED);
}

GhVoiceRecorderState
gh_voice_recorder_get_state(GhVoiceRecorder *self)
{
  g_return_val_if_fail(GH_IS_VOICE_RECORDER(self), GH_VOICE_RECORDER_STATE_IDLE);
  return self->state;
}

gdouble
gh_voice_recorder_get_duration(GhVoiceRecorder *self)
{
  g_return_val_if_fail(GH_IS_VOICE_RECORDER(self), 0.0);
  return self->duration;
}

gdouble
gh_voice_recorder_get_level(GhVoiceRecorder *self)
{
  g_return_val_if_fail(GH_IS_VOICE_RECORDER(self), 0.0);
  return self->level;
}

GhVoiceWaveform *
gh_voice_recorder_dup_waveform(GhVoiceRecorder *self)
{
  g_return_val_if_fail(GH_IS_VOICE_RECORDER(self), NULL);
  if (self->waveform_levels->len == 0)
    return NULL;
  return gh_voice_waveform_new(
    (const gdouble *)self->waveform_levels->data,
    self->waveform_levels->len);
}

GBytes *
gh_voice_recorder_dup_bytes(GhVoiceRecorder *self, GError **error)
{
  g_return_val_if_fail(GH_IS_VOICE_RECORDER(self), NULL);
  g_return_val_if_fail(self->state == GH_VOICE_RECORDER_STATE_DONE, NULL);
  g_return_val_if_fail(self->temp_fd >= 0, NULL);

  struct stat st;
  if (fstat(self->temp_fd, &st) != 0 || !S_ISREG(st.st_mode) ||
      st.st_size <= 0 || st.st_size > 32 * 1024 * 1024 ||
      lseek(self->temp_fd, 0, SEEK_SET) != 0) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Cannot read voice recording");
    gh_voice_recorder_wipe_temp(self);
    return NULL;
  }
  gsize length = (gsize)st.st_size;
  gchar *contents = g_try_malloc(length);
  if (!contents) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
                "Cannot allocate memory for voice recording");
    gh_voice_recorder_wipe_temp(self);
    return NULL;
  }
  gsize read_total = 0;
  while (read_total < length) {
    ssize_t n = read(self->temp_fd, contents + read_total, length - read_total);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "Cannot read voice recording");
      memset(contents, 0, length);
      g_free(contents);
      gh_voice_recorder_wipe_temp(self);
      return NULL;
    }
    read_total += (gsize)n;
  }
  GBytes *bytes = g_bytes_new_take(contents, length);
  /* The copy is owned by the send path; close the unlinked inode now. */
  gh_voice_recorder_wipe_temp(self);
  return bytes;
}
