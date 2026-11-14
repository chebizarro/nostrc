/* SPDX-License-Identifier: GPL-3.0-or-later
 * The call UI (nostrc-mqrgk.8; the notification hand-off of .7): a real
 * GhWindow with the conversation list over a real GhConversationStore, the
 * real GhCallService fed signed NIP-AC inner events, and the fake media
 * backend (tests/app/fake-call-media.c) behind recording media controls.
 * Covers the header buttons and every unavailable reason, outgoing,
 * incoming answered/declined, busy, remote hang-up, answered elsewhere,
 * a message request's missed call, reconnecting, screen-reader labels of
 * Mute and Camera, the headset question, the Return to Call banner, closing
 * the window during a call, the notifier's Answer, and the 360px layout.
 * Needs a display: exits 77 without one. Nothing sleeps: the service's
 * timeouts are driven through gh_call_service_expire(). */
#include "fake-call-media.h"
#include "gh-call-ui.h"
#include "gh-call-view.h"
#include "gh-conversation-list.h"
#include "gh-conversation-view.h"
#include "gh-notifier.h"
#include "gh-test-dialog.h"
#include "gh-test-signer.h"
#include "nostrc-test-gdk-frame.h"

#include <adwaita.h>
#include <nostr-kinds.h>
#include <string.h>

void groundhog_register_resource(void);

static gchar *me, *peer, *stranger;
static const gchar *SDP = "v=0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\n";
static const gchar *VIDEO_SDP =
  "v=0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\nm=video 9 UDP/TLS/RTP/SAVPF 96\r\n";

/* ---- a device for the device menu and the headset question ---- */

#define TEST_TYPE_DEVICE (test_device_get_type())
G_DECLARE_FINAL_TYPE(TestDevice, test_device, TEST, DEVICE, GObject)
struct _TestDevice { GObject parent_instance; gchar *name, *klass; };
G_DEFINE_FINAL_TYPE(TestDevice, test_device, G_TYPE_OBJECT)
enum { DEV_PROP_0, DEV_PROP_NAME, DEV_PROP_CLASS };
static void
test_device_get_property(GObject *o, guint id, GValue *v, GParamSpec *p)
{
  TestDevice *d = TEST_DEVICE(o);
  if (id == DEV_PROP_NAME) g_value_set_string(v, d->name);
  else if (id == DEV_PROP_CLASS) g_value_set_string(v, d->klass);
  else G_OBJECT_WARN_INVALID_PROPERTY_ID(o, id, p);
}
static void
test_device_finalize(GObject *o)
{
  g_free(TEST_DEVICE(o)->name); g_free(TEST_DEVICE(o)->klass);
  G_OBJECT_CLASS(test_device_parent_class)->finalize(o);
}
static void
test_device_class_init(TestDeviceClass *k)
{
  G_OBJECT_CLASS(k)->get_property = test_device_get_property;
  G_OBJECT_CLASS(k)->finalize = test_device_finalize;
  g_object_class_install_property(G_OBJECT_CLASS(k), DEV_PROP_NAME,
    g_param_spec_string("display-name", NULL, NULL, NULL, G_PARAM_READABLE));
  g_object_class_install_property(G_OBJECT_CLASS(k), DEV_PROP_CLASS,
    g_param_spec_string("device-class", NULL, NULL, NULL, G_PARAM_READABLE));
}
static void test_device_init(TestDevice *d) { (void)d; }
static GObject *
device_new(const gchar *name, const gchar *klass)
{
  TestDevice *d = g_object_new(TEST_TYPE_DEVICE, NULL);
  d->name = g_strdup(name); d->klass = g_strdup(klass);
  return G_OBJECT(d);
}

/* ---- the fixture ---- */

typedef struct {
  GSettings *settings;
  GhNetSession *network;
  GhConversationStore *model;
  GhCallService *service;
  GhFakeCallMedia *media;
  GhWindow *window;
  GhConversation *room;
  GhCallUiMedia probe;
  gboolean keeps_running;
  GListStore *outputs, *devices;
  GObject *chosen_headset;
  gint next_camera;            /* -1: never set */
  GArray *sent;                /* kinds */
  guint muted_calls, camera_calls, selects;
  gboolean muted, camera;
} F;

static F *current;

static void op_muted(GhCallMediaBackend *m, gboolean v) { (void)m; current->muted_calls++; current->muted = v; }
static void op_camera(GhCallMediaBackend *m, gboolean v) { (void)m; current->camera_calls++; current->camera = v; }
static GListModel *op_devices(GhCallMediaBackend *m, GError **e) { (void)m; (void)e; return G_LIST_MODEL(current->devices); }
static gboolean
op_select(GhCallMediaBackend *m, const gchar *k, GObject *d, GError **e)
{ (void)m; (void)k; (void)d; (void)e; current->selects++; return TRUE; }
static const GhCallMediaOps ops = { op_muted, op_camera, NULL, NULL, op_devices, op_select };

static GhCallUiMedia probe(gpointer data, gchar **reason) { (void)reason; return ((F *)data)->probe; }
static GListModel *outputs(gpointer data) { return G_LIST_MODEL(((F *)data)->outputs); }
static gboolean
choose(gpointer data, GObject *output, GError **error)
{ (void)error; g_set_object(&((F *)data)->chosen_headset, output); return TRUE; }
static void next_camera(gpointer data, gboolean on) { ((F *)data)->next_camera = on; }
static gboolean keeps_running(gpointer data) { return ((F *)data)->keeps_running; }
static void
on_send(GhCallService *s, const gchar *p, const gchar *id, int kind, int type, const gchar *t, F *f)
{ (void)s; (void)p; (void)id; (void)type; (void)t; g_array_append_val(f->sent, kind); }

static void
drain(void)
{
  for (int i = 0; i < 400 && g_main_context_iteration(NULL, FALSE); i++)
    ;
}

static gboolean deadline_hit(gpointer data) { *(gboolean *)data = TRUE; return G_SOURCE_REMOVE; }
static void
spin_until_at(gboolean (*pred)(gpointer), gpointer data, int line)
{
  gboolean expired = FALSE;
  guint timer = g_timeout_add_seconds(5, deadline_hit, &expired);
  while (!pred(data) && !expired)
    g_main_context_iteration(NULL, TRUE);
  if (expired)
    g_error("condition waited for at line %d did not hold within 5s", line);
  g_source_remove(timer);
}
#define spin_until(pred, data) spin_until_at((pred), (data), __LINE__)
static gboolean is_mapped(gpointer w) { return gtk_widget_get_mapped(GTK_WIDGET(w)); }

static GhConversation *
open_room(F *f, const gchar *const *peers)
{
  return gh_conversation_store_open_room(f->model, peers, NULL);
}

static void
fixture_up(F *f, gint width)
{
  memset(f, 0, sizeof *f);
  current = f;
  f->next_camera = -1;
  f->settings = g_settings_new("org.nostr.Groundhog");
  g_settings_reset(f->settings, "allow-calls-from-unaccepted");
  f->network = gh_net_session_new(NULL);
  f->model = gh_conversation_store_new();
  gh_conversation_store_set_account(f->model, me, NULL, NULL, NULL);
  f->service = gh_call_service_new(NULL, f->model, f->settings, f->network);
  gh_call_service_bind(f->service, me);
  gh_call_service_set_media_factory(f->service, gh_fake_call_media_factory, &f->media, NULL);
  f->sent = g_array_new(FALSE, FALSE, sizeof(int));
  g_signal_connect(f->service, "send", G_CALLBACK(on_send), f);
  f->outputs = g_list_store_new(G_TYPE_OBJECT);
  f->devices = g_list_store_new(G_TYPE_OBJECT);
  const gchar *peers[] = { peer, NULL };
  f->room = open_room(f, peers);
  f->window = gh_window_new(NULL);
  gh_conversation_list_attach(f->window, f->model, NULL);
  GhCallUiConfig config = {
    .service = f->service, .conversations = f->model, .ops = &ops,
    .probe = probe, .headset_outputs = outputs, .choose_headset = choose,
    .set_next_camera = next_camera, .keeps_running = keeps_running, .data = f,
  };
  gh_call_ui_attach(f->window, &config);
  gtk_window_set_default_size(GTK_WINDOW(f->window), width, 600);
  gtk_window_present(GTK_WINDOW(f->window));
  spin_until(is_mapped, f->window);
  drain();
}

static void
fixture_down(F *f)
{
  if (f->window)
    gtk_window_destroy(GTK_WINDOW(f->window));
  drain();
  gh_call_service_stop(f->service);
  drain();
  g_clear_object(&f->service);
  g_clear_object(&f->media);
  gh_conversation_store_set_account(f->model, NULL, NULL, NULL, NULL);
  g_clear_object(&f->model);
  g_clear_object(&f->network);
  g_clear_object(&f->outputs);
  g_clear_object(&f->devices);
  g_clear_object(&f->chosen_headset);
  g_settings_reset(f->settings, "allow-calls-from-unaccepted");
  g_clear_object(&f->settings);
  g_array_unref(f->sent);
  current = NULL;
}

static gchar *
event(guint key, const gchar *recipient, const gchar *id, int kind, GhCallType type,
      const gchar *content)
{
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  g_autofree gchar *author = gh_test_pub(key);
  NostrEvent *e = gh_call_event_new(author, recipient, id, kind, type, content, now);
  g_assert_cmpint(nostr_event_sign(e, gh_test_secret[key]), ==, 0);
  gchar *json = nostr_event_serialize_compact(e);
  nostr_event_free(e);
  return json;
}

/* key 1 is peer, 2 is me, 3 is stranger. */
static void
deliver(F *f, guint key, const gchar *recipient, const gchar *id, int kind, GhCallType type,
        const gchar *content)
{
  g_autofree gchar *json = event(key, recipient, id, kind, type, content);
  gh_call_service_receive(f->service, json, g_get_real_time() / G_USEC_PER_SEC);
  drain();
}

static GhCall *
call(F *f)
{
  return gh_call_service_get_current(f->service);
}

static GhCallView *
view(F *f)
{
  return GH_CALL_VIEW(gh_call_ui_get_view(f->window));
}

static gboolean
on_call_page(F *f)
{
  return g_strcmp0(gtk_stack_get_visible_child_name(gh_window_get_root_stack(f->window)), "call") == 0;
}

static GtkWidget *
control(F *f, GhCallViewControls which)
{
  return gh_call_view_get_control(view(f), which);
}

static gboolean
visible(F *f, GhCallViewControls which)
{
  return gtk_widget_get_visible(control(f, which));
}

static gboolean
enabled(F *f, const gchar *action)
{
  return g_action_group_get_action_enabled(G_ACTION_GROUP(f->window), action);
}

static void
activate(F *f, const gchar *action)
{
  g_assert_true(enabled(f, action));
  g_action_group_activate_action(G_ACTION_GROUP(f->window), action, NULL);
  drain();
}

static void
show_room(F *f)
{
  g_assert_true(gh_window_open_item(f->window, f->room));
  drain();
}

static GtkWidget *
call_button(F *f, gboolean video)
{
  return gh_content_page_get_call_button(gh_window_get_content(f->window), video);
}

static void
assert_label(GtkWidget *widget, const gchar *label)
{
  gtk_test_accessible_assert_property(GTK_ACCESSIBLE(widget), GTK_ACCESSIBLE_PROPERTY_LABEL, label);
  g_assert_cmpstr(gtk_widget_get_tooltip_text(widget), ==, label);
}

static void
connect_media(F *f)
{
  g_signal_emit_by_name(f->media, "connection-state", GH_CALL_MEDIA_CONNECTED);
  drain();
}

/* An answered outgoing call: OUTGOING, then the peer's answer, connected. */
static GhCall *
place_and_connect(F *f, gboolean video)
{
  show_room(f);
  activate(f, video ? "call-video" : "call-voice");
  GhCall *c = call(f);
  g_assert_nonnull(c);
  deliver(f, 1, me, gh_call_get_id(c), NOSTR_KIND_CALL_ANSWER, video ? GH_CALL_VIDEO : GH_CALL_VOICE,
          video ? VIDEO_SDP : SDP);
  connect_media(f);
  g_assert_cmpint(gh_call_get_state(c), ==, GH_CALL_ACTIVE);
  return c;
}

static const gchar *
ring(F *f, guint key, GhCallType type)
{
  static const gchar *ID = "323e4567-e89b-12d3-a456-426614174000";
  static guint serial;
  static gchar id[40];
  g_snprintf(id, sizeof id, "%.30s%06u", ID, ++serial);
  deliver(f, key, me, id, NOSTR_KIND_CALL_OFFER, type, type == GH_CALL_VIDEO ? VIDEO_SDP : SDP);
  return id;
}

/* ---- header buttons ---- */

static void
assert_buttons(F *f, gboolean shown, gboolean sensitive, const gchar *tooltip)
{
  drain();
  for (guint video = 0; video <= 1; video++) {
    GtkWidget *b = call_button(f, video);
    g_assert_cmpint(gtk_widget_get_visible(b), ==, shown);
    if (!shown)
      continue;
    g_assert_cmpint(gtk_widget_is_sensitive(b), ==, sensitive);
    g_assert_cmpint(enabled(f, video ? "call-video" : "call-voice"), ==, sensitive);
    if (tooltip)
      g_assert_nonnull(strstr(gtk_widget_get_tooltip_text(b), tooltip));
    gtk_test_accessible_assert_property(GTK_ACCESSIBLE(b), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                        video ? "Start Video Call" : "Start Voice Call");
  }
}

static void
test_buttons_gating(void)
{
  F f;
  fixture_up(&f, 900);
  assert_buttons(&f, FALSE, FALSE, NULL); /* nothing shown */
  show_room(&f);
  assert_buttons(&f, TRUE, TRUE, NULL);
  g_assert_cmpstr(gtk_widget_get_tooltip_text(call_button(&f, FALSE)), ==, "Start Voice Call");
  g_assert_cmpstr(gtk_widget_get_tooltip_text(call_button(&f, TRUE)), ==, "Start Video Call");

  /* Missing GStreamer call elements. */
  f.probe = GH_CALL_UI_MEDIA_MISSING;
  g_signal_emit_by_name(f.service, "availability-changed");
  assert_buttons(&f, TRUE, FALSE, "call support is not installed");
  gtk_test_accessible_assert_property(GTK_ACCESSIBLE(call_button(&f, FALSE)),
    GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, "Calls are unavailable: call support is not installed");
  /* No echo cancellation: still possible, with a headset. */
  f.probe = GH_CALL_UI_MEDIA_HEADSET_ONLY;
  g_signal_emit_by_name(f.service, "availability-changed");
  assert_buttons(&f, TRUE, TRUE, "Use a headset");
  f.probe = GH_CALL_UI_MEDIA_READY;

  /* Tor mode (policy). */
  gh_net_session_set_mode(f.network, GH_NET_MODE_TOR, "127.0.0.1:9050");
  assert_buttons(&f, TRUE, FALSE, "Tor mode");
  gh_net_session_set_mode(f.network, GH_NET_MODE_SYSTEM, NULL);
  assert_buttons(&f, TRUE, TRUE, NULL);

  /* A build without media: no factory. */
  gh_call_service_set_media_factory(f.service, NULL, NULL, NULL);
  assert_buttons(&f, TRUE, FALSE, "call support is not installed");
  gh_call_service_set_media_factory(f.service, gh_fake_call_media_factory, &f.media, NULL);

  /* No account. */
  gh_call_service_bind(f.service, NULL);
  assert_buttons(&f, TRUE, FALSE, "without an account");
  gh_call_service_bind(f.service, me);

  /* The peer has no message inbox (gh-send-ui's banner state). */
  GhConversationView *cv = GH_CONVERSATION_VIEW(gh_content_page_get_view(gh_window_get_content(f.window)));
  gh_conversation_view_set_recipient_without_inbox(cv, "Alice");
  assert_buttons(&f, TRUE, FALSE, "Alice can’t be called");
  gh_conversation_view_set_recipient_without_inbox(cv, NULL);
  assert_buttons(&f, TRUE, TRUE, NULL);

  /* No group calls in v1. */
  const gchar *group[] = { peer, stranger, NULL };
  GhConversation *room = open_room(&f, group);
  g_assert_true(gh_window_open_item(f.window, room));
  assert_buttons(&f, FALSE, FALSE, NULL);
  fixture_down(&f);
}

/* ---- outgoing ---- */

static void
test_outgoing(void)
{
  F f;
  fixture_up(&f, 900);
  show_room(&f);
  activate(&f, "call-voice");
  GhCall *c = call(&f);
  g_assert_nonnull(c);
  g_assert_true(on_call_page(&f));
  g_assert_cmpint(f.next_camera, ==, FALSE);
  g_assert_cmpstr(gh_call_view_get_status(view(&f)), ==, "Calling…");
  g_assert_cmpstr(gh_call_view_get_peer(view(&f)), ==, gh_conversation_get_title(f.room));
  g_assert_true(visible(&f, GH_CALL_VIEW_MUTE));
  g_assert_true(visible(&f, GH_CALL_VIEW_HANG_UP));
  g_assert_true(visible(&f, GH_CALL_VIEW_DEVICES));
  g_assert_false(visible(&f, GH_CALL_VIEW_ANSWER));
  g_assert_false(visible(&f, GH_CALL_VIEW_CAMERA));
  g_assert_false(enabled(&f, "call-voice")); /* one call at a time */
  g_assert_cmpint(g_array_index(f.sent, int, 0), ==, NOSTR_KIND_CALL_OFFER);

  deliver(&f, 1, me, gh_call_get_id(c), NOSTR_KIND_CALL_ANSWER, GH_CALL_VOICE, SDP);
  g_assert_cmpstr(gh_call_view_get_status(view(&f)), ==, "Connecting…");
  connect_media(&f);
  g_assert_cmpstr(gh_call_view_get_status(view(&f)), ==, "0:00");
  g_assert_true(visible(&f, GH_CALL_VIEW_SWITCH_VIDEO));
  g_assert_true(enabled(&f, "call-switch-video"));

  /* Mute: the media follows and the label tells what pressing does next. */
  GtkWidget *mute = control(&f, GH_CALL_VIEW_MUTE);
  assert_label(mute, "Mute Microphone");
  activate(&f, "call-toggle-mute");
  g_assert_true(f.muted);
  assert_label(mute, "Unmute Microphone");
  g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(mute)));
  gtk_test_accessible_assert_state(GTK_ACCESSIBLE(mute), GTK_ACCESSIBLE_STATE_PRESSED, GTK_ACCESSIBLE_TRISTATE_TRUE);
  activate(&f, "call-toggle-mute");
  g_assert_false(f.muted);
  assert_label(mute, "Mute Microphone");

  /* Devices: the popover lists the classes; choosing one reaches the media. */
  g_autoptr(GObject) mic = device_new("USB Microphone", "Audio/Source");
  g_list_store_append(f.devices, mic);
  gtk_menu_button_popup(GTK_MENU_BUTTON(control(&f, GH_CALL_VIEW_DEVICES)));
  drain();
  GtkWidget *drop = gtk_widget_get_next_sibling(gtk_widget_get_first_child(
    GTK_WIDGET(gh_call_view_get_devices_box(view(&f)))));
  g_assert_true(GTK_IS_DROP_DOWN(drop));
  gtk_drop_down_set_selected(GTK_DROP_DOWN(drop), 1);
  g_assert_cmpuint(f.selects, ==, 1);
  gtk_menu_button_popdown(GTK_MENU_BUTTON(control(&f, GH_CALL_VIEW_DEVICES)));
  /* An unplugged device is said, not silent. */
  g_signal_emit_by_name(f.media, "device-removed", mic);
  g_assert_cmpstr(gh_call_ui_get_last_toast(f.window), ==, "Microphone disconnected");

  /* Switch to video renegotiates and turns the camera on. */
  activate(&f, "call-switch-video");
  g_assert_cmpint(gh_call_get_call_type(c), ==, GH_CALL_VIDEO);
  g_assert_true(f.camera);
  g_assert_true(visible(&f, GH_CALL_VIEW_CAMERA));
  GtkWidget *camera = control(&f, GH_CALL_VIEW_CAMERA);
  assert_label(camera, "Turn Off Camera");
  activate(&f, "call-toggle-camera");
  g_assert_false(f.camera);
  assert_label(camera, "Turn On Camera");

  activate(&f, "call-hang-up");
  g_assert_cmpint(gh_call_get_end_reason(c), ==, GH_CALL_END_HANGUP);
  g_assert_cmpstr(gh_call_view_get_status(view(&f)), ==, "Call ended");
  g_assert_false(visible(&f, GH_CALL_VIEW_HANG_UP));
  g_assert_true(f.media->torn_down);
  activate(&f, "call-leave-view");
  g_assert_false(on_call_page(&f));
  g_assert_true(enabled(&f, "call-voice"));
  g_assert_false(enabled(&f, "call-show")); /* the ended call is gone */
  fixture_down(&f);
}

/* ---- incoming ---- */

static void
test_incoming_answered(void)
{
  F f;
  fixture_up(&f, 900);
  ring(&f, 1, GH_CALL_VOICE);
  GhCall *c = call(&f);
  g_assert_cmpint(gh_call_get_state(c), ==, GH_CALL_RINGING);
  g_assert_true(on_call_page(&f));
  g_assert_cmpstr(gh_call_view_get_status(view(&f)), ==, "Incoming voice call");
  g_assert_true(visible(&f, GH_CALL_VIEW_ANSWER));
  g_assert_true(visible(&f, GH_CALL_VIEW_DECLINE));
  g_assert_false(visible(&f, GH_CALL_VIEW_ANSWER_VIDEO));
  g_assert_false(visible(&f, GH_CALL_VIEW_HANG_UP));
  assert_label(control(&f, GH_CALL_VIEW_ANSWER), "Answer");
  assert_label(control(&f, GH_CALL_VIEW_DECLINE), "Decline");

  /* Back to the conversations: the call rings on, behind a banner. */
  activate(&f, "call-leave-view");
  AdwBanner *banner = gh_content_page_get_call_banner(gh_window_get_content(f.window));
  g_assert_true(adw_banner_get_revealed(banner));
  g_autofree gchar *incoming = g_strdup_printf("Incoming call from %s", gh_conversation_get_title(f.room));
  g_assert_cmpstr(adw_banner_get_title(banner), ==, incoming);
  activate(&f, "call-show");
  g_assert_true(on_call_page(&f));
  g_assert_false(adw_banner_get_revealed(banner));

  activate(&f, "call-answer");
  g_assert_cmpint(gh_call_get_state(c), ==, GH_CALL_CONNECTING);
  g_assert_cmpuint(f.media->answers, ==, 1);
  g_assert_cmpstr(gh_call_view_get_status(view(&f)), ==, "Connecting…");
  connect_media(&f);
  g_assert_cmpint(gh_call_get_state(c), ==, GH_CALL_ACTIVE);
  activate(&f, "call-leave-view");
  g_autofree gchar *with = g_strdup_printf("Call with %s", gh_conversation_get_title(f.room));
  g_assert_cmpstr(adw_banner_get_title(banner), ==, with);
  g_assert_true(adw_banner_get_revealed(banner));
  /* The sidebar's banner is for the narrow layout only. */
  g_assert_false(adw_banner_get_revealed(gh_sidebar_page_get_call_banner(gh_window_get_sidebar(f.window))));

  /* Reconnecting, then the remote hang-up while the page is not shown. */
  g_signal_emit_by_name(f.media, "connection-state", GH_CALL_MEDIA_DISCONNECTED);
  drain();
  g_assert_cmpstr(gh_call_view_get_status(view(&f)), ==, "Reconnecting…");
  g_assert_false(enabled(&f, "call-switch-video"));
  connect_media(&f);
  deliver(&f, 1, me, gh_call_get_id(c), NOSTR_KIND_CALL_HANGUP, GH_CALL_VOICE, "");
  g_assert_cmpint(gh_call_get_end_reason(c), ==, GH_CALL_END_HANGUP);
  g_assert_false(adw_banner_get_revealed(banner));
  g_assert_cmpstr(gh_call_ui_get_last_toast(f.window), ==, "Call ended");
  fixture_down(&f);
}

static void
test_incoming_video_voice_answer(void)
{
  F f;
  fixture_up(&f, 900);
  ring(&f, 1, GH_CALL_VIDEO);
  g_assert_cmpstr(gh_call_view_get_status(view(&f)), ==, "Incoming video call");
  g_assert_true(visible(&f, GH_CALL_VIEW_ANSWER_VIDEO));
  assert_label(control(&f, GH_CALL_VIEW_ANSWER_VIDEO), "Answer With Video");
  /* Answer by voice: the camera is off before the media starts. */
  activate(&f, "call-answer");
  g_assert_cmpint(f.next_camera, ==, FALSE);
  g_assert_false(f.camera);
  assert_label(control(&f, GH_CALL_VIEW_CAMERA), "Turn On Camera");
  fixture_down(&f);

  fixture_up(&f, 900);
  ring(&f, 1, GH_CALL_VIDEO);
  activate(&f, "call-answer-video");
  g_assert_cmpint(f.next_camera, ==, TRUE);
  assert_label(control(&f, GH_CALL_VIEW_CAMERA), "Turn Off Camera");
  fixture_down(&f);
}

static void
test_incoming_declined(void)
{
  F f;
  fixture_up(&f, 900);
  ring(&f, 1, GH_CALL_VOICE);
  GhCall *c = call(&f);
  activate(&f, "call-decline");
  g_assert_cmpint(gh_call_get_end_reason(c), ==, GH_CALL_END_DECLINED);
  g_assert_cmpint(g_array_index(f.sent, int, f.sent->len - 1), ==, NOSTR_KIND_CALL_REJECT);
  g_assert_cmpstr(gh_call_view_get_status(view(&f)), ==, "Call declined");
  g_assert_false(enabled(&f, "call-answer"));
  fixture_down(&f);
}

static void
test_busy(void)
{
  F f;
  fixture_up(&f, 900);
  show_room(&f);
  activate(&f, "call-voice");
  GhCall *c = call(&f);
  deliver(&f, 1, me, gh_call_get_id(c), NOSTR_KIND_CALL_REJECT, GH_CALL_VOICE, "busy");
  g_assert_cmpint(gh_call_get_end_reason(c), ==, GH_CALL_END_BUSY);
  g_assert_cmpstr(gh_call_view_get_status(view(&f)), ==, "Busy");
  fixture_down(&f);
}

static void
test_elsewhere_and_timeout(void)
{
  F f;
  fixture_up(&f, 900);
  const gchar *id = ring(&f, 1, GH_CALL_VOICE);
  GhCall *c = call(&f);
  /* This account answered on another device (its self-addressed answer). */
  deliver(&f, 2, peer, id, NOSTR_KIND_CALL_ANSWER, GH_CALL_VOICE, SDP);
  g_assert_cmpint(gh_call_get_end_reason(c), ==, GH_CALL_END_ELSEWHERE);
  g_assert_cmpstr(gh_call_view_get_status(view(&f)), ==, "Answered or declined on another device");
  activate(&f, "call-leave-view");

  ring(&f, 1, GH_CALL_VOICE);
  c = call(&f);
  gh_call_service_expire(f.service, g_get_real_time() / G_USEC_PER_SEC + 61);
  drain();
  g_assert_cmpstr(gh_call_view_get_status(view(&f)), ==, "Missed call");
  fixture_down(&f);
}

static void
test_request_missed(void)
{
  F f;
  fixture_up(&f, 900);
  /* A stranger (a message request) never rings: only a missed call. */
  ring(&f, 3, GH_CALL_VOICE);
  g_assert_null(call(&f));
  g_assert_false(on_call_page(&f));
  g_assert_false(adw_banner_get_revealed(gh_content_page_get_call_banner(gh_window_get_content(f.window))));
  g_assert_false(enabled(&f, "call-answer"));
  g_assert_cmpuint(f.sent->len, ==, 0);
  fixture_down(&f);
}

/* ---- the headset question ---- */

static void
test_headset(void)
{
  F f;
  fixture_up(&f, 900);
  f.probe = GH_CALL_UI_MEDIA_HEADSET_ONLY;
  g_autoptr(GObject) headset = device_new("Headset", "Audio/Sink");
  g_list_store_append(f.outputs, headset);
  show_room(&f);
  activate(&f, "call-voice");
  g_assert_null(call(&f)); /* not before a headset is chosen */
  AdwDialog *dialog = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(f.window));
  g_assert_true(ADW_IS_ALERT_DIALOG(dialog));
  g_assert_cmpstr(adw_alert_dialog_get_heading(ADW_ALERT_DIALOG(dialog)), ==, "Use a Headset");
  g_assert_true(adw_alert_dialog_get_response_enabled(ADW_ALERT_DIALOG(dialog), "continue"));
  spin_until(gh_test_dialog_shown, dialog);
  g_signal_emit_by_name(dialog, "response", "continue");
  adw_dialog_force_close(dialog);
  drain();
  g_assert_true(f.chosen_headset == headset);
  g_assert_nonnull(call(&f));
  g_assert_true(on_call_page(&f));
  gh_call_service_hang_up(f.service);
  fixture_down(&f);
}

/* ---- closing the window ---- */

static gboolean destroyed_flag;
static void on_destroyed(GtkWidget *w, gpointer d) { (void)w; (void)d; destroyed_flag = TRUE; }
static gboolean is_destroyed(gpointer d) { (void)d; return destroyed_flag; }

static void
test_close_window(void)
{
  F f;
  fixture_up(&f, 900);
  GhCall *c = place_and_connect(&f, FALSE);
  destroyed_flag = FALSE;
  g_signal_connect(f.window, "destroy", G_CALLBACK(on_destroyed), NULL);
  gtk_window_close(GTK_WINDOW(f.window));
  drain();
  g_assert_false(destroyed_flag);
  AdwDialog *dialog = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(f.window));
  g_assert_true(ADW_IS_ALERT_DIALOG(dialog));
  g_assert_cmpstr(adw_alert_dialog_get_heading(ADW_ALERT_DIALOG(dialog)), ==, "End Call?");
  g_assert_cmpstr(adw_alert_dialog_get_default_response(ADW_ALERT_DIALOG(dialog)), ==, "cancel");
  spin_until(gh_test_dialog_shown, dialog);
  g_signal_emit_by_name(dialog, "response", "end");
  adw_dialog_force_close(dialog);
  spin_until(is_destroyed, NULL);
  g_assert_cmpint(gh_call_get_end_reason(c), ==, GH_CALL_END_HANGUP);
  g_assert_cmpint(g_array_index(f.sent, int, f.sent->len - 1), ==, NOSTR_KIND_CALL_HANGUP);
  f.window = NULL;
  fixture_down(&f);

  /* With background delivery the call outlives the window, and a new
   * window comes up on it. */
  fixture_up(&f, 900);
  f.keeps_running = TRUE;
  c = place_and_connect(&f, FALSE);
  destroyed_flag = FALSE;
  g_signal_connect(f.window, "destroy", G_CALLBACK(on_destroyed), NULL);
  gtk_window_close(GTK_WINDOW(f.window));
  spin_until(is_destroyed, NULL);
  g_assert_cmpint(gh_call_get_state(c), ==, GH_CALL_ACTIVE);
  f.window = gh_window_new(NULL);
  GhCallUiConfig config = { .service = f.service, .conversations = f.model, .ops = &ops,
                            .probe = probe, .keeps_running = keeps_running, .data = &f };
  gh_call_ui_attach(f.window, &config);
  gtk_window_present(GTK_WINDOW(f.window));
  drain();
  g_assert_true(on_call_page(&f));
  g_assert_cmpstr(gh_call_view_get_status(view(&f)), !=, "");
  gh_call_service_hang_up(f.service);
  fixture_down(&f);
}

/* ---- the notification's Answer ---- */

static void
test_notifier_answer(void)
{
  F f;
  fixture_up(&f, 900);
  gtk_window_destroy(GTK_WINDOW(f.window));
  drain();
  g_autoptr(GApplication) app = g_application_new("org.nostr.GroundhogTest.CallUi", G_APPLICATION_DEFAULT_FLAGS);
  GhNotifierConfig nconfig = { .settings = f.settings, .conversations = f.model };
  g_autoptr(GhNotifier) notifier = gh_notifier_new(app, &nconfig);
  f.window = gh_window_new(NULL);
  gh_conversation_list_attach(f.window, f.model, NULL);
  GhCallUiConfig config = { .service = f.service, .conversations = f.model, .ops = &ops,
                            .probe = probe, .set_next_camera = next_camera,
                            .notifier = G_OBJECT(notifier), .data = &f };
  gh_call_ui_attach(f.window, &config);
  gtk_window_present(GTK_WINDOW(f.window));
  drain();
  ring(&f, 1, GH_CALL_VIDEO);
  activate(&f, "call-leave-view");
  gboolean handled = FALSE;
  g_signal_emit_by_name(notifier, "answer-call", call(&f), FALSE, &handled);
  drain();
  g_assert_true(handled);
  g_assert_cmpint(gh_call_get_state(call(&f)), ==, GH_CALL_CONNECTING);
  g_assert_cmpint(f.next_camera, ==, FALSE); /* Answer, not Answer With Video */
  g_assert_true(on_call_page(&f));
  gh_call_service_hang_up(f.service);
  gtk_window_destroy(GTK_WINDOW(f.window));
  f.window = NULL;
  drain();
  g_object_run_dispose(G_OBJECT(notifier));
  fixture_down(&f);
}

/* ---- 360px ---- */

static gboolean
count_tick(GtkWidget *w, GdkFrameClock *clock, gpointer data)
{
  (void)w; (void)clock;
  return ++*(guint *)data < 3 ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}
static gboolean ticked(gpointer data) { return *(guint *)data >= 3; }

/* Opt-in visual evidence: GH_CALL_UI_SNAPSHOT_DIR=<dir> renders the window. */
static void
maybe_snapshot(F *f, const gchar *name)
{
  const gchar *dir = g_getenv("GH_CALL_UI_SNAPSHOT_DIR");
  if (!dir || !*dir)
    return;
  guint ticks = 0;
  gtk_widget_add_tick_callback(GTK_WIDGET(f->window), count_tick, &ticks, NULL);
  gtk_widget_queue_draw(GTK_WIDGET(f->window));
  spin_until(ticked, &ticks);
  GtkWidget *widget = adw_application_window_get_content(ADW_APPLICATION_WINDOW(f->window));
  g_autoptr(GdkPaintable) paintable = gtk_widget_paintable_new(widget);
  gint w = gtk_widget_get_width(widget), h = gtk_widget_get_height(widget);
  g_autoptr(GtkSnapshot) snapshot = gtk_snapshot_new();
  gdk_paintable_snapshot(paintable, snapshot, w, h);
  g_autoptr(GskRenderNode) node = gtk_snapshot_free_to_node(g_steal_pointer(&snapshot));
  GskRenderer *renderer = gtk_native_get_renderer(GTK_NATIVE(f->window));
  g_assert_nonnull(node);
  g_autoptr(GdkTexture) texture = gsk_renderer_render_texture(renderer, node,
    &GRAPHENE_RECT_INIT(0, 0, w, h));
  g_autofree gchar *path = g_build_filename(dir, name, NULL);
  g_assert_true(gdk_texture_save_to_png(texture, path));
}

static void
assert_inside(GtkWidget *widget, GtkWidget *window, gint width)
{
  graphene_rect_t bounds;
  g_assert_true(gtk_widget_compute_bounds(widget, window, &bounds));
  g_assert_cmpfloat(bounds.origin.x, >=, 0);
  g_assert_cmpfloat(bounds.origin.x + bounds.size.width, <=, width);
}

/* As test_shell_layout.c: wait for the narrow breakpoint, then measure the
 * window's content (the toplevel may add decoration margins). */
static gboolean
collapsed(gpointer data)
{
  F *f = data;
  return adw_navigation_split_view_get_collapsed(gh_window_get_split(f->window));
}

static GtkWidget *
content_of(F *f)
{
  return adw_application_window_get_content(ADW_APPLICATION_WINDOW(f->window));
}

static void
test_narrow(void)
{
  F f;
  fixture_up(&f, 360);
  spin_until(collapsed, &f);
  if (gtk_widget_get_width(content_of(&f)) > 360)
    g_error("the 360px window's content is %d px wide", gtk_widget_get_width(content_of(&f)));
  GhCall *c = place_and_connect(&f, TRUE);
  (void)c;
  drain();
  GtkWidget *page = gh_call_ui_get_view(f.window);
  g_assert_cmpint(gtk_widget_get_width(page), <=, 360);
  gint minimum = 0;
  gtk_widget_measure(page, GTK_ORIENTATION_HORIZONTAL, -1, &minimum, NULL, NULL, NULL);
  g_assert_cmpint(minimum, <=, 360);
  GhCallViewControls shown[] = { GH_CALL_VIEW_MUTE, GH_CALL_VIEW_CAMERA, GH_CALL_VIEW_DEVICES,
                                 GH_CALL_VIEW_HANG_UP };
  for (guint i = 0; i < G_N_ELEMENTS(shown); i++) {
    GtkWidget *w = control(&f, shown[i]);
    g_assert_true(gtk_widget_get_mapped(w));
    assert_inside(w, content_of(&f), 360);
  }
  maybe_snapshot(&f, "call-ui-360-video-active.png");
  /* Narrow: the list shows the sidebar's banner when the call is left. */
  activate(&f, "call-leave-view");
  adw_navigation_split_view_set_show_content(gh_window_get_split(f.window), FALSE);
  drain();
  g_assert_true(adw_banner_get_revealed(gh_sidebar_page_get_call_banner(gh_window_get_sidebar(f.window))));
  maybe_snapshot(&f, "call-ui-360-banner.png");
  gh_call_service_hang_up(f.service);
  drain();
  ring(&f, 1, GH_CALL_VIDEO);
  g_assert_true(on_call_page(&f));
  drain();
  for (GhCallViewControls b = GH_CALL_VIEW_DECLINE; b <= GH_CALL_VIEW_ANSWER; b <<= 1) {
    GtkWidget *w = control(&f, b);
    GtkWidget *content = content_of(&f);
    assert_inside(w, content, 360);
  }
  maybe_snapshot(&f, "call-ui-360-ringing.png");
  gh_call_service_decline(f.service);
  fixture_down(&f);
}

int
main(int argc, char **argv)
{
  g_setenv("GTK_A11Y", "test", TRUE);
  g_test_init(&argc, &argv, NULL);
  nostrc_test_tolerate_gdk_frame_warning();
  if (!gtk_init_check()) {
    g_printerr("groundhog-call-ui skipped: no graphical display\n");
    return 77;
  }
  adw_init();
  groundhog_register_resource();
  g_object_set(gtk_settings_get_default(), "gtk-enable-animations", FALSE, NULL);
  me = gh_test_pub(2);
  peer = gh_test_pub(1);
  stranger = gh_test_pub(3);
  g_test_add_func("/call-ui/buttons-gating", test_buttons_gating);
  g_test_add_func("/call-ui/outgoing", test_outgoing);
  g_test_add_func("/call-ui/incoming-answered", test_incoming_answered);
  g_test_add_func("/call-ui/incoming-video-voice-answer", test_incoming_video_voice_answer);
  g_test_add_func("/call-ui/incoming-declined", test_incoming_declined);
  g_test_add_func("/call-ui/busy", test_busy);
  g_test_add_func("/call-ui/elsewhere-and-timeout", test_elsewhere_and_timeout);
  g_test_add_func("/call-ui/request-missed", test_request_missed);
  g_test_add_func("/call-ui/headset", test_headset);
  g_test_add_func("/call-ui/close-window", test_close_window);
  g_test_add_func("/call-ui/notifier-answer", test_notifier_answer);
  g_test_add_func("/call-ui/narrow-360", test_narrow);
  int status = g_test_run();
  g_free(me); g_free(peer); g_free(stranger);
  return status;
}
