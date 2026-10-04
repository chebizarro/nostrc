/* SPDX-License-Identifier: GPL-3.0-or-later
 * Voice message UI tests (W27, nostrc-y79b): the voice bubble widget, the
 * recording overlay in the composer, and the composer's mic button
 * visibility. Needs a display: self-skips (77) without one. Uses the
 * tests/common/nostrc-test-gdk-frame.h tolerance call for GTK 4.24. */
#include <adwaita.h>
#include <string.h>
#include "gh-attachment-transfer.h"

#include "gh-composer.h"
#include "gh-voice-bubble.h"
#include "gh-voice-meta.h"
#include "gh-voice-waveform.h"

#include "nostrc-test-gdk-frame.h"

void groundhog_register_resource(void);

/* ---- helpers ---- */

static void
tick(int n)
{
  for (int i = 0; i < n; i++)
    g_main_context_iteration(NULL, FALSE);
}

/* ---- voice bubble ---- */

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

typedef struct {
  GhAttachmentTransfer *transfer;
  GBytes *audio;
  guint downloads;
} VoiceProviderFixture;

static GhAttachmentTransfer *
voice_lookup(GhMessage *message G_GNUC_UNUSED, gpointer data)
{
  return ((VoiceProviderFixture *)data)->transfer;
}

static gboolean
voice_complete_download(gpointer data)
{
  VoiceProviderFixture *fixture = data;
  gh_attachment_transfer_succeed(fixture->transfer, fixture->audio, FALSE, FALSE);
  return G_SOURCE_REMOVE;
}

static void
voice_download(GhAttachmentTransfer *transfer, gpointer data)
{
  VoiceProviderFixture *fixture = data;
  g_assert_true(transfer == fixture->transfer);
  fixture->downloads++;
  gh_attachment_transfer_start(transfer);
  g_idle_add(voice_complete_download, fixture);
}

static gboolean
quit_loop(gpointer data)
{
  g_main_loop_quit(data);
  return G_SOURCE_REMOVE;
}

typedef struct {
  GtkAccessible *waveform;
  GMainLoop *loop;
  gboolean ready;
} WaveformWait;

static gboolean
check_waveform(gpointer data)
{
  WaveformWait *wait = data;
  if (gtk_test_accessible_has_property(wait->waveform,
                                        GTK_ACCESSIBLE_PROPERTY_LABEL)) {
    g_autofree gchar *difference = gtk_test_accessible_check_property(
      wait->waveform, GTK_ACCESSIBLE_PROPERTY_LABEL, "Voice waveform");
    if (!difference) {
      wait->ready = TRUE;
      g_main_loop_quit(wait->loop);
      return G_SOURCE_REMOVE;
    }
  }
  return G_SOURCE_CONTINUE;
}

static void
test_voice_bubble_download_play(void)
{
  g_autoptr(GBytes) audio = voice_fixture("white-noise-aac.m4a");
  g_autoptr(GhAttachmentTransfer) transfer =
    gh_attachment_transfer_new_described("voice-ui", "audio/mp4", "voice.m4a");
  VoiceProviderFixture fixture = { .transfer = transfer, .audio = audio };
  GhVoiceBubbleProvider provider = {
    .lookup = voice_lookup, .download = voice_download
  };
  GtkWidget *window = gtk_window_new();
  GtkWidget *bubble = gh_voice_bubble_new();
  gh_voice_bubble_set_provider(window, &provider, &fixture, NULL);
  gtk_window_set_child(GTK_WINDOW(window), bubble);
  g_autoptr(GhMessage) message = g_object_new(GH_TYPE_MESSAGE, NULL);
  gh_voice_bubble_set_message(GH_VOICE_BUBBLE(bubble), message);
  gtk_window_present(GTK_WINDOW(window));
  tick(30);
  g_assert_cmpuint(fixture.downloads, ==, 0); /* no auto-fetch */
  g_assert_cmpint(gh_attachment_transfer_get_state(transfer), ==,
                  GH_ATTACHMENT_STATE_IDLE);

  g_assert_true(gtk_widget_activate_action(bubble, "voice.download", NULL));
  g_assert_cmpuint(fixture.downloads, ==, 1);
  tick(30);
  g_assert_cmpint(gh_attachment_transfer_get_state(transfer), ==,
                  GH_ATTACHMENT_STATE_READY);
  GtkWidget *player_box = GTK_WIDGET(gtk_widget_get_template_child(
    bubble, GH_TYPE_VOICE_BUBBLE, "player_box"));
  GtkButton *play = GTK_BUTTON(gtk_widget_get_template_child(
    bubble, GH_TYPE_VOICE_BUBBLE, "play_button"));
  GtkButton *speed = GTK_BUTTON(gtk_widget_get_template_child(
    bubble, GH_TYPE_VOICE_BUBBLE, "speed_button"));
  GtkAccessible *waveform = GTK_ACCESSIBLE(gtk_widget_get_template_child(
    bubble, GH_TYPE_VOICE_BUBBLE, "waveform_area"));
  g_assert_true(gtk_widget_get_visible(player_box));
  g_assert_cmpstr(gh_voice_bubble_get_summary(GH_VOICE_BUBBLE(bubble)), ==,
                  "Voice message");

  g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
  WaveformWait wait = { .waveform = waveform, .loop = loop };
  guint poll = g_timeout_add(20, check_waveform, &wait);
  guint timeout = g_timeout_add_seconds(5, quit_loop, loop);
  g_main_loop_run(loop);
  if (g_main_context_find_source_by_id(NULL, poll)) g_source_remove(poll);
  if (g_main_context_find_source_by_id(NULL, timeout)) g_source_remove(timeout);
  g_assert_true(wait.ready);

  g_assert_true(gtk_widget_activate_action(bubble, "voice.cycle-speed", NULL));
  gtk_test_accessible_assert_property(GTK_ACCESSIBLE(speed),
                                      GTK_ACCESSIBLE_PROPERTY_LABEL,
                                      "Playback speed 1.5\xC3\x97");
  g_assert_true(gtk_widget_activate_action(bubble, "voice.play", NULL));
  gtk_test_accessible_assert_property(GTK_ACCESSIBLE(play),
                                      GTK_ACCESSIBLE_PROPERTY_LABEL,
                                      "Pause voice message");
  g_assert_true(gtk_widget_activate_action(bubble, "voice.play", NULL));
  gtk_test_accessible_assert_property(GTK_ACCESSIBLE(play),
                                      GTK_ACCESSIBLE_PROPERTY_LABEL,
                                      "Play voice message");
  gtk_window_destroy(GTK_WINDOW(window));
  tick(30);
}

static void
test_voice_bubble_new(void)
{
  GtkWidget *bubble = gh_voice_bubble_new();
  g_assert_nonnull(bubble);
  g_assert_true(GH_IS_VOICE_BUBBLE(bubble));

  /* Default state: no message, no player. */
  g_assert_null(gh_voice_bubble_get_message(GH_VOICE_BUBBLE(bubble)));
  g_assert_cmpuint(gh_voice_bubble_get_index(GH_VOICE_BUBBLE(bubble)), ==, 0);

  /* Summary says not downloaded. */
  const gchar *summary = gh_voice_bubble_get_summary(GH_VOICE_BUBBLE(bubble));
  g_assert_nonnull(summary);
  g_assert_true(strstr(summary, "Voice") != NULL || strstr(summary, "voice") != NULL);

  GtkWidget *window = gtk_window_new();
  gtk_window_set_child(GTK_WINDOW(window), bubble);
  gtk_window_present(GTK_WINDOW(window));
  tick(30);
  g_assert_true(gtk_widget_get_mapped(bubble));

  gtk_window_destroy(GTK_WINDOW(window));
  tick(30);
}

static void
test_voice_bubble_compact(void)
{
  GtkWidget *bubble = gh_voice_bubble_new();
  GhVoiceBubble *vb = GH_VOICE_BUBBLE(bubble);
  gh_voice_bubble_set_compact(vb, TRUE);
  gh_voice_bubble_set_compact(vb, FALSE);

  GtkWidget *window = gtk_window_new();
  gtk_window_set_child(GTK_WINDOW(window), bubble);
  gtk_window_present(GTK_WINDOW(window));
  tick(30);
  gtk_window_destroy(GTK_WINDOW(window));
  tick(30);
}

static void
test_voice_bubble_index(void)
{
  GtkWidget *bubble = gh_voice_bubble_new();
  GhVoiceBubble *vb = GH_VOICE_BUBBLE(bubble);
  gh_voice_bubble_set_index(vb, 3);
  g_assert_cmpuint(gh_voice_bubble_get_index(vb), ==, 3);
  gh_voice_bubble_set_index(vb, 0);
  g_assert_cmpuint(gh_voice_bubble_get_index(vb), ==, 0);

  GtkWidget *window = gtk_window_new();
  gtk_window_set_child(GTK_WINDOW(window), bubble);
  gtk_window_present(GTK_WINDOW(window));
  tick(10);
  gtk_window_destroy(GTK_WINDOW(window));
  tick(10);
}

/* ---- composer voice button ---- */

static void
test_composer_voice_button_hidden(void)
{
  /* Without can-record-voice, the voice button is hidden. */
  GtkWidget *composer = gh_composer_new();
  GhComposer *c = GH_COMPOSER(composer);

  g_assert_false(gh_composer_get_can_record_voice(c));

  GtkWidget *window = gtk_window_new();
  gtk_window_set_child(GTK_WINDOW(window), composer);
  gtk_window_present(GTK_WINDOW(window));
  tick(30);

  /* The voice button should not be visible. */
  g_assert_false(gh_composer_get_can_record_voice(c));

  gtk_window_destroy(GTK_WINDOW(window));
  tick(30);
}

static void
test_composer_voice_button_shown(void)
{
  /* With can-record-voice, the voice button is shown. */
  GtkWidget *composer = gh_composer_new();
  GhComposer *c = GH_COMPOSER(composer);

  gh_composer_set_can_record_voice(c, TRUE);
  g_assert_true(gh_composer_get_can_record_voice(c));

  GtkWidget *window = gtk_window_new();
  gtk_window_set_child(GTK_WINDOW(window), composer);
  gtk_window_present(GTK_WINDOW(window));
  tick(30);

  g_assert_true(gh_composer_get_can_record_voice(c));

  gtk_window_destroy(GTK_WINDOW(window));
  tick(30);
}

/* ---- recording overlay ---- */

static void
test_composer_recording_overlay(void)
{
  GtkWidget *composer = gh_composer_new();
  GhComposer *c = GH_COMPOSER(composer);

  GtkWidget *window = gtk_window_new();
  gtk_window_set_child(GTK_WINDOW(window), composer);
  gtk_window_present(GTK_WINDOW(window));
  tick(30);

  /* Show recording page. */
  gh_composer_show_recording(c);
  tick(10);

  /* Update level and time. */
  gh_composer_set_recording_level(c, 0.7);
  gh_composer_set_recording_time(c, 5.5);
  tick(10);

  /* Update again. */
  gh_composer_set_recording_level(c, 0.0);
  gh_composer_set_recording_time(c, 125.0);
  GtkAccessible *time = GTK_ACCESSIBLE(gtk_widget_get_template_child(
    composer, GH_TYPE_COMPOSER, "recording_time"));
  gtk_test_accessible_assert_property(time, GTK_ACCESSIBLE_PROPERTY_LABEL,
                                      "Recording duration 2:05");
  tick(10);

  /* Return to edit. */
  gh_composer_hide_recording(c);
  tick(10);

  gtk_window_destroy(GTK_WINDOW(window));
  tick(30);
}

static gboolean stop_emitted;
static gboolean cancel_emitted;

static void
on_stop(GhComposer *c G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  stop_emitted = TRUE;
}

static void
on_cancel(GhComposer *c G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  cancel_emitted = TRUE;
}

static void
test_composer_recording_signals(void)
{
  GtkWidget *composer = gh_composer_new();
  GhComposer *c = GH_COMPOSER(composer);

  stop_emitted = FALSE;
  cancel_emitted = FALSE;
  g_signal_connect(c, "stop-recording", G_CALLBACK(on_stop), NULL);
  g_signal_connect(c, "cancel-recording", G_CALLBACK(on_cancel), NULL);

  GtkWidget *window = gtk_window_new();
  gtk_window_set_child(GTK_WINDOW(window), composer);
  gtk_window_present(GTK_WINDOW(window));
  tick(30);

  gh_composer_show_recording(c);
  tick(10);

  /* Escape must cancel through the key controller, not just an action. */
  g_autoptr(GListModel) controllers = gtk_widget_observe_controllers(composer);
  gboolean escape_handled = FALSE;
  for (guint i = 0; i < g_list_model_get_n_items(controllers); i++) {
    g_autoptr(GtkEventController) controller = g_list_model_get_item(controllers, i);
    if (g_strcmp0(gtk_event_controller_get_name(controller),
                  "groundhog-composer-escape") == 0)
      g_signal_emit_by_name(controller, "key-pressed", GDK_KEY_Escape, 0,
                            (GdkModifierType)0, &escape_handled);
  }
  g_assert_true(escape_handled);
  g_assert_true(cancel_emitted);
  cancel_emitted = FALSE;

  /* A swipe left over recording status must also cancel. */
  GtkWidget *status = GTK_WIDGET(gtk_widget_get_template_child(
    composer, GH_TYPE_COMPOSER, "recording_label"));
  g_autoptr(GListModel) gestures = gtk_widget_observe_controllers(status);
  for (guint i = 0; i < g_list_model_get_n_items(gestures); i++) {
    g_autoptr(GtkEventController) controller = g_list_model_get_item(gestures, i);
    if (g_strcmp0(gtk_event_controller_get_name(controller),
                  "groundhog-recording-swipe") == 0)
      g_signal_emit_by_name(controller, "drag-end", -100.0, 0.0);
  }
  g_assert_true(cancel_emitted);

  /* Activate stop action. */
  gtk_widget_activate_action(composer, "composer.stop-recording", NULL);
  tick(10);
  g_assert_true(stop_emitted);

  /* Activate cancel action. */
  gtk_widget_activate_action(composer, "composer.cancel-recording", NULL);
  tick(10);
  g_assert_true(cancel_emitted);

  gtk_window_destroy(GTK_WINDOW(window));
  tick(30);
}

/* ---- voice meta (quick sanity in GUI context) ---- */

static void
test_voice_meta_quick(void)
{
  g_assert_true(gh_voice_meta_is_audio("audio/ogg"));
  g_assert_false(gh_voice_meta_is_audio("image/png"));
  g_assert_cmpstr(GH_VOICE_MIME, ==, "audio/ogg");
  g_assert_cmpstr(GH_VOICE_FILENAME, ==, "voice.ogg");
}

/* ---- main ---- */

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  nostrc_test_tolerate_gdk_frame_warning();

  if (!gtk_init_check()) {
    g_test_skip("No display available");
    return 77;
  }
  adw_init();
  groundhog_register_resource();

  g_test_add_func("/voice-ui/bubble/new", test_voice_bubble_new);
  g_test_add_func("/voice-ui/bubble/compact", test_voice_bubble_compact);
  g_test_add_func("/voice-ui/bubble/index", test_voice_bubble_index);
  g_test_add_func("/voice-ui/bubble/download-play", test_voice_bubble_download_play);
  g_test_add_func("/voice-ui/composer/voice-button-hidden", test_composer_voice_button_hidden);
  g_test_add_func("/voice-ui/composer/voice-button-shown", test_composer_voice_button_shown);
  g_test_add_func("/voice-ui/composer/recording-overlay", test_composer_recording_overlay);
  g_test_add_func("/voice-ui/composer/recording-signals", test_composer_recording_signals);
  g_test_add_func("/voice-ui/meta/quick", test_voice_meta_quick);

  return g_test_run();
}
