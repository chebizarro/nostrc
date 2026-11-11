#include "gh-call-media.h"
#include "gh-call-capabilities.h"
#include "gh-call-settings.h"
#include <gst/sdp/sdp.h>
#include <stdio.h>
static gboolean force_h264;
static gboolean use_cancellable;
static gboolean initial_video;
typedef struct { GstDevice parent; } TestMicrophone;
typedef struct { GstDeviceClass parent; } TestMicrophoneClass;
G_DEFINE_TYPE(TestMicrophone, test_microphone, GST_TYPE_DEVICE)
static GstElement *test_capture(GstDevice *device, const gchar *name) {
  (void)device;
  GstElement *source = gst_element_factory_make("audiotestsrc", name);
  g_object_set(source, "is-live", TRUE, NULL); return source;
}
static void test_microphone_class_init(TestMicrophoneClass *klass) { GST_DEVICE_CLASS(klass)->create_element = test_capture; }
static void test_microphone_init(TestMicrophone *self) { (void)self; }
static GstDevice *microphone(void) {
  return g_object_new(test_microphone_get_type(), "display-name", "Disposable test microphone", "device-class", "Audio/Source", NULL);
}

/* Internal harness, never used by the application. */
GhCallMedia *gh_call_media_new_test(GSettings *settings);
void gh_call_media_test_received(GhCallMedia *, gint *, gint *);
void gh_call_media_test_remove_device(GhCallMedia *, GstDevice *);
static GhCallMediaBackendInterface *iface(GhCallMedia *m) {
  return GH_CALL_MEDIA_BACKEND_GET_IFACE(m);
}
static gboolean deadline(gpointer data) { (void)data; g_error("Media test deadline exceeded"); return G_SOURCE_REMOVE; }
static void done(GObject *object, GAsyncResult *result, gpointer data) {
  (void)object; *(GAsyncResult **)data = g_object_ref(result);
}
static gchar *local(GhCallMedia *m, GhCallType type, gboolean answer)
{
  GAsyncResult *result = NULL; GError *error = NULL;
  iface(m)->create_local_async(GH_CALL_MEDIA_BACKEND(m), type, answer, NULL, done, &result);
  while (!result) g_main_context_iteration(NULL, TRUE);
  gchar *sdp = iface(m)->create_local_finish(GH_CALL_MEDIA_BACKEND(m), result, &error);
  g_assert_no_error(error); g_assert_nonnull(sdp); g_object_unref(result); return sdp;
}
static void remote(GhCallMedia *m, const gchar *sdp, gboolean offer)
{
  GAsyncResult *result = NULL; GError *error = NULL;
  iface(m)->apply_remote_async(GH_CALL_MEDIA_BACKEND(m), sdp, offer, NULL, done, &result);
  while (!result) g_main_context_iteration(NULL, TRUE);
  gboolean ok = iface(m)->apply_remote_finish(GH_CALL_MEDIA_BACKEND(m), result, &error);
  g_assert_no_error(error); g_assert_true(ok); g_object_unref(result);
}
static void forward(GhCallMedia *from, const gchar *candidate, const gchar *mid, guint line, gpointer peer)
{
  (void)from; GError *error = NULL;
  g_assert_true(iface(peer)->add_remote_ice(GH_CALL_MEDIA_BACKEND(peer), candidate, mid, line, &error));
  g_assert_no_error(error);
}
static void state(GhCallMedia *m, gint state, gpointer data)
{
  (void)m; (void)data; g_assert_cmpint(state, !=, GH_CALL_MEDIA_FAILED);
}
static GSettings *settings(void)
{
  GSettings *s = g_settings_new("org.nostr.Groundhog");
  const gchar *empty[] = {NULL};
  g_settings_set_strv(s, "call-stun-servers", empty);
  g_settings_set_string(s, "call-turn-url", "");
  g_settings_set_boolean(s, "call-relay-only", FALSE);
  return s;
}
static void exchange(GhCallMedia *a, GhCallMedia *b, GhCallType type)
{
  g_autofree gchar *offer = local(a, type, FALSE);
  if (type == GH_CALL_VIDEO && force_h264) {
    GstSDPMessage *message = NULL;
    g_assert_cmpint(gst_sdp_message_new_from_text(offer, &message), ==, GST_SDP_OK);
    for (guint i = 0; i < gst_sdp_message_medias_len(message); i++) {
      GstSDPMedia *media = (GstSDPMedia *)gst_sdp_message_get_media(message, i);
      if (g_strcmp0(gst_sdp_media_get_media(media), "video")) continue;
      for (guint f = gst_sdp_media_formats_len(media); f > 0; f--) {
        const gchar *format = gst_sdp_media_get_format(media, f - 1);
        GstCaps *caps = gst_sdp_media_get_caps_from_media(media, atoi(format));
        const gchar *encoding = gst_structure_get_string(gst_caps_get_structure(caps, 0), "encoding-name");
        if (g_strcmp0(encoding, "H264")) gst_sdp_media_remove_format(media, f - 1);
        gst_caps_unref(caps);
      }
      g_assert_cmpuint(gst_sdp_media_formats_len(media), >, 0);
    }
    g_free(offer); offer = gst_sdp_message_as_text(message); gst_sdp_message_free(message);
  }
  remote(b, offer, TRUE);
  g_autofree gchar *answer = local(b, type, TRUE);
  remote(a, answer, FALSE);
  g_assert_nonnull(strstr(offer, "OPUS/48000"));
  if (type == GH_CALL_VIDEO) g_assert_nonnull(strstr(answer, force_h264 ? "H264/90000" : "VP8/90000"));
  if (force_h264 && type == GH_CALL_VIDEO) g_assert_nonnull(strstr(answer, "profile-level-id=42e01f"));
}
static void loopback(void)
{
  g_autoptr(GSettings) s = settings();
  GhCallMedia *a = gh_call_media_new_test(s), *b = gh_call_media_new_test(s);
  GError *error = NULL;
  GstDevice *capture = microphone();
  g_assert_true(gh_call_media_select_device(a, "Audio/Source", capture, &error)); g_assert_no_error(error);
  g_assert_true(iface(a)->configure(GH_CALL_MEDIA_BACKEND(a), TRUE, &error)); g_assert_no_error(error);
  g_assert_true(iface(b)->configure(GH_CALL_MEDIA_BACKEND(b), TRUE, &error)); g_assert_no_error(error);
  g_signal_connect(a, "local-ice", G_CALLBACK(forward), b);
  g_signal_connect(b, "local-ice", G_CALLBACK(forward), a);
  g_signal_connect(a, "connection-state", G_CALLBACK(state), NULL);
  g_signal_connect(b, "connection-state", G_CALLBACK(state), NULL);
  if (!initial_video) exchange(a, b, GH_CALL_VOICE);
  exchange(a, b, GH_CALL_VIDEO);
  gint aa, av, ba, bv;
  do {
    g_main_context_iteration(NULL, TRUE);
    gh_call_media_test_received(a, &aa, &av); gh_call_media_test_received(b, &ba, &bv);
  } while (aa < 10 || av < 10 || ba < 10 || bv < 10);
  g_assert_cmpint(gdk_paintable_get_intrinsic_width(gh_call_media_get_remote_video(a)), >, 0);
  g_print("decoded A audio=%d video=%d; B audio=%d video=%d\n", aa, av, ba, bv);
  gh_call_media_set_muted(a, TRUE); gh_call_media_set_muted(a, FALSE);
  gh_call_media_set_camera_enabled(a, FALSE); gh_call_media_set_camera_enabled(a, TRUE);
  gh_call_media_test_remove_device(a, capture);
  GstDevice *replacement = microphone();
  g_assert_true(gh_call_media_select_device(a, "Audio/Source", replacement, &error)); g_assert_no_error(error);
  gint before_audio = ba;
  do {
    g_main_context_iteration(NULL, TRUE);
    gh_call_media_test_received(b, &ba, &bv);
  } while (ba < before_audio + 10);
  gst_object_unref(capture); gst_object_unref(replacement);
  g_signal_handlers_disconnect_by_data(a, b); g_signal_handlers_disconnect_by_data(b, a);
  g_assert_false(iface(a)->configure(GH_CALL_MEDIA_BACKEND(a), FALSE, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CLOSED); g_clear_error(&error);
  iface(a)->teardown(GH_CALL_MEDIA_BACKEND(a)); iface(a)->teardown(GH_CALL_MEDIA_BACKEND(a));
  iface(b)->teardown(GH_CALL_MEDIA_BACKEND(b));
  gboolean a_dead = FALSE, b_dead = FALSE;
  g_object_add_weak_pointer(G_OBJECT(a), (gpointer *)&a);
  g_object_add_weak_pointer(G_OBJECT(b), (gpointer *)&b);
  g_object_unref(a); g_object_unref(b);
  while (g_main_context_pending(NULL)) g_main_context_iteration(NULL, FALSE);
  a_dead = a == NULL; b_dead = b == NULL;
  g_assert_true(a_dead); g_assert_true(b_dead);
}
static void h264_loopback(void) { force_h264 = TRUE; loopback(); force_h264 = FALSE; }
static void video_establish(void) { initial_video = TRUE; loopback(); initial_video = FALSE; }
static void reject_high_profile(void)
{
  g_autoptr(GSettings) s = settings();
  g_autoptr(GhCallMedia) m = gh_call_media_new_test(s);
  GError *error = NULL; GAsyncResult *result = NULL;
  g_assert_true(iface(m)->configure(GH_CALL_MEDIA_BACKEND(m), TRUE, &error)); g_assert_no_error(error);
  iface(m)->apply_remote_async(GH_CALL_MEDIA_BACKEND(m),
      "v=0\r\no=- 1 1 IN IP4 0.0.0.0\r\ns=-\r\nt=0 0\r\n"
      "m=audio 9 UDP/TLS/RTP/SAVPF 96\r\na=rtpmap:96 OPUS/48000/2\r\n"
      "m=video 9 UDP/TLS/RTP/SAVPF 98\r\na=rtpmap:98 H264/90000\r\n"
      "a=fmtp:98 packetization-mode=1;profile-level-id=640c1f\r\n", TRUE, NULL, done, &result);
  while (!result) g_main_context_iteration(NULL, TRUE);
  g_assert_false(iface(m)->apply_remote_finish(GH_CALL_MEDIA_BACKEND(m), result, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
  g_clear_error(&error); g_object_unref(result);
}
static void policy(void)
{
  g_autoptr(GSettings) s = settings();
  g_autoptr(GhCallMedia) m = gh_call_media_new_test(s);
  GError *error = NULL;
  g_assert_false(iface(m)->configure(GH_CALL_MEDIA_BACKEND(m), FALSE, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED); g_clear_error(&error);
  GAsyncResult *blocked = NULL;
  iface(m)->create_local_async(GH_CALL_MEDIA_BACKEND(m), GH_CALL_VIDEO, FALSE, NULL, done, &blocked);
  while (!blocked) g_main_context_iteration(NULL, TRUE);
  g_assert_null(iface(m)->create_local_finish(GH_CALL_MEDIA_BACKEND(m), blocked, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CLOSED); g_clear_error(&error); g_object_unref(blocked);
  g_assert_false(iface(m)->add_remote_ice(GH_CALL_MEDIA_BACKEND(m),
      "candidate:1 1 UDP 1 127.0.0.1 1234 typ host", "audio0", 0, &error));
  g_clear_error(&error);
  g_settings_set_boolean(s, "call-relay-only", TRUE);
  g_assert_false(iface(m)->configure(GH_CALL_MEDIA_BACKEND(m), TRUE, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED); g_clear_error(&error);
  g_settings_set_string(s, "call-turn-url", "turn://127.0.0.1:3478");
  g_assert_true(iface(m)->configure(GH_CALL_MEDIA_BACKEND(m), TRUE, &error)); g_assert_no_error(error);
  g_assert_false(iface(m)->add_remote_ice(GH_CALL_MEDIA_BACKEND(m),
      "candidate:1 1 UDP 1 127.0.0.1 1234 typ host", "audio0", 0, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT); g_clear_error(&error);
  g_assert_true(iface(m)->add_remote_ice(GH_CALL_MEDIA_BACKEND(m),
      "candidate:1 1 UDP 1 127.0.0.1 1234 typ relay", "audio0", 0, &error));
  g_assert_no_error(error);
  GAsyncResult *result = NULL;
  iface(m)->apply_remote_async(GH_CALL_MEDIA_BACKEND(m),
      "v=0\r\no=- 1 1 IN IP4 0.0.0.0\r\ns=-\r\nt=0 0\r\n"
      "m=audio 9 UDP/TLS/RTP/SAVPF 96\r\na=rtpmap:96 OPUS/48000/2\r\n"
      "a=candidate:1 1 UDP 1 127.0.0.1 1234 typ host\r\n", TRUE, NULL, done, &result);
  while (!result) g_main_context_iteration(NULL, TRUE);
  g_assert_false(iface(m)->apply_remote_finish(GH_CALL_MEDIA_BACKEND(m), result, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT); g_clear_error(&error);
  g_object_unref(result);
  g_assert_false(gh_call_ice_url_validate("turn://user:password@example.org", TRUE, &error));
  g_clear_error(&error);
  g_assert_true(gh_call_ice_url_validate("turns://example.org:5349?transport=tcp", TRUE, &error));
  g_assert_no_error(error);
}
static void cancel_pending(void)
{
  g_autoptr(GSettings) s = settings();
  g_autoptr(GhCallMedia) m = gh_call_media_new_test(s);
  GError *error = NULL;
  g_assert_true(iface(m)->configure(GH_CALL_MEDIA_BACKEND(m), TRUE, &error));
  g_assert_no_error(error);
  GAsyncResult *result = NULL;
  g_autoptr(GCancellable) cancel = g_cancellable_new();
  iface(m)->create_local_async(GH_CALL_MEDIA_BACKEND(m), GH_CALL_VOICE, FALSE, cancel, done, &result);
  if (use_cancellable) g_cancellable_cancel(cancel);
  else iface(m)->teardown(GH_CALL_MEDIA_BACKEND(m));
  while (!result) g_main_context_iteration(NULL, TRUE);
  gchar *sdp = iface(m)->create_local_finish(GH_CALL_MEDIA_BACKEND(m), result, &error);
  g_assert_null(sdp); g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error(&error); g_object_unref(result);
}
static void cancelled_operation(void) { use_cancellable = TRUE; cancel_pending(); use_cancellable = FALSE; }
typedef struct {
  GhCallMedia *media;
  GMainLoop *loop;
  gboolean caller, connected, ready;
  guint phase;
  gboolean answering;
  GhCallType type;
} ProcessPeer;
static void process_create(ProcessPeer *peer, GhCallType type, gboolean answer);
static void maybe_upgrade(ProcessPeer *peer) {
  if (peer->caller && peer->connected && peer->phase == 1) {
    peer->phase = 2; process_create(peer, GH_CALL_VIDEO, FALSE);
  }
}
static void process_state(GhCallMedia *m, gint value, gpointer data) {
  ProcessPeer *peer = data; state(m, value, NULL);
  if (value == GH_CALL_MEDIA_CONNECTED) { peer->connected = TRUE; maybe_upgrade(peer); }
}
static void process_ice(GhCallMedia *m, const gchar *candidate, const gchar *mid, guint line, gpointer data) {
  (void)m; (void)data;
  if (g_getenv("GH_CALL_TEST_TURN")) g_assert_nonnull(strstr(candidate, " typ relay"));
  g_autofree gchar *c = g_base64_encode((const guchar *)candidate, strlen(candidate));
  g_autofree gchar *id = g_base64_encode((const guchar *)mid, strlen(mid));
  g_print("MSG ICE %u %s %s\n", line, id, c); fflush(stdout);
}
static void process_local_done(GObject *source, GAsyncResult *result, gpointer data) {
  ProcessPeer *peer = data; GError *error = NULL;
  g_autofree gchar *sdp = iface(peer->media)->create_local_finish(GH_CALL_MEDIA_BACKEND(source), result, &error);
  g_assert_no_error(error);
  g_autofree gchar *encoded = g_base64_encode((const guchar *)sdp, strlen(sdp));
  g_print("MSG %s %u %s\n", peer->answering ? "ANSWER" : "OFFER", peer->type, encoded); fflush(stdout);
}
static void process_create(ProcessPeer *peer, GhCallType type, gboolean answer) {
  peer->answering = answer; peer->type = type;
  iface(peer->media)->create_local_async(GH_CALL_MEDIA_BACKEND(peer->media), type, answer, NULL, process_local_done, peer);
}
static void process_remote_done(GObject *source, GAsyncResult *result, gpointer data) {
  ProcessPeer *peer = data; GError *error = NULL;
  gboolean ok = iface(peer->media)->apply_remote_finish(GH_CALL_MEDIA_BACKEND(source), result, &error);
  g_assert_no_error(error); g_assert_true(ok);
  if (peer->answering) process_create(peer, peer->type, TRUE);
  else if (peer->phase == 0) { peer->phase = 1; maybe_upgrade(peer); }
}
static gboolean process_input(GIOChannel *channel, GIOCondition condition, gpointer data) {
  ProcessPeer *peer = data;
  gchar *line = NULL; gsize length = 0;
  GIOStatus status = g_io_channel_read_line(channel, &line, &length, NULL, NULL);
  if (status == G_IO_STATUS_AGAIN) return G_SOURCE_CONTINUE;
  if (status != G_IO_STATUS_NORMAL || !line || g_str_has_prefix(line, "STOP")) {
    g_free(line); g_main_loop_quit(peer->loop); return G_SOURCE_CONTINUE;
  }
  (void)condition; g_strchomp(line);
  g_auto(GStrv) f = g_strsplit(line, " ", -1);
  GError *error = NULL;
  if (!g_strcmp0(f[0], "MSG") && !g_strcmp0(f[1], "ICE")) {
    gsize n, id_n;
    g_autofree guchar *raw = g_base64_decode(f[4], &n);
    g_autofree guchar *id_raw = g_base64_decode(f[3], &id_n);
    g_autofree gchar *candidate = g_strndup((const gchar *)raw, n);
    g_autofree gchar *mid = g_strndup((const gchar *)id_raw, id_n);
    g_assert_true(iface(peer->media)->add_remote_ice(GH_CALL_MEDIA_BACKEND(peer->media), candidate, mid, atoi(f[2]), &error));
    g_assert_no_error(error);
  } else if (!g_strcmp0(f[0], "MSG")) {
    gsize n; g_autofree guchar *raw = g_base64_decode(f[3], &n);
    g_autofree gchar *sdp = g_strndup((const gchar *)raw, n);
    peer->answering = !g_strcmp0(f[1], "OFFER"); peer->type = atoi(f[2]);
    iface(peer->media)->apply_remote_async(GH_CALL_MEDIA_BACKEND(peer->media), sdp, peer->answering, NULL, process_remote_done, peer);
  }
  g_free(line); return G_SOURCE_CONTINUE;
}
static void process_frame(GdkPaintable *paintable, gpointer data) {
  (void)paintable; ProcessPeer *peer = data;
  gint audio, video; gh_call_media_test_received(peer->media, &audio, &video);
  if (!peer->ready && audio >= 10 && video >= 10) {
    peer->ready = TRUE; g_print("READY audio=%d video=%d\n", audio, video); fflush(stdout);
  }
}
static int process_peer(gboolean caller)
{
  g_autoptr(GSettings) s = settings();
  if (g_getenv("GH_CALL_TEST_TURN")) {
    GAsyncResult *result = NULL; GError *error = NULL;
    const gchar *url = "turn://127.0.0.1:3478";
    gh_call_turn_password_store_async(url, "gh-voice-media", "disposable-test-only", NULL, done, &result);
    while (!result) g_main_context_iteration(NULL, TRUE);
    g_assert_true(gh_call_turn_password_store_finish(result, &error)); g_assert_no_error(error); g_object_unref(result);
    g_settings_set_string(s, "call-turn-url", url);
    g_settings_set_string(s, "call-turn-username", "gh-voice-media");
    g_settings_set_boolean(s, "call-relay-only", TRUE);
  }
  ProcessPeer peer = { .caller = caller, .loop = g_main_loop_new(NULL, FALSE), .media = gh_call_media_new_test(s) };
  GError *error = NULL;
  g_assert_true(iface(peer.media)->configure(GH_CALL_MEDIA_BACKEND(peer.media), TRUE, &error)); g_assert_no_error(error);
  g_signal_connect(peer.media, "local-ice", G_CALLBACK(process_ice), &peer);
  g_signal_connect(peer.media, "connection-state", G_CALLBACK(process_state), &peer);
  g_signal_connect(gh_call_media_get_remote_video(peer.media), "invalidate-contents", G_CALLBACK(process_frame), &peer);
  GIOChannel *channel = g_io_channel_unix_new(0);
  g_io_channel_set_encoding(channel, NULL, NULL);
  g_io_channel_set_flags(channel, G_IO_FLAG_NONBLOCK, NULL);
  guint input = g_io_add_watch(channel, G_IO_IN | G_IO_HUP | G_IO_ERR, process_input, &peer);
  guint timeout = g_timeout_add_seconds(40, deadline, NULL);
  if (caller) process_create(&peer, GH_CALL_VOICE, FALSE);
  g_main_loop_run(peer.loop);
  iface(peer.media)->teardown(GH_CALL_MEDIA_BACKEND(peer.media));
  g_source_remove(input); g_source_remove(timeout);
  g_io_channel_unref(channel); g_object_unref(peer.media); g_main_loop_unref(peer.loop);
  while (g_main_context_pending(NULL)) g_main_context_iteration(NULL, FALSE);
  return peer.ready ? 0 : 1;
}
int main(int argc, char **argv)
{
  gst_init(&argc, &argv);
  if (argc == 2 && g_str_has_prefix(argv[1], "--peer-")) {
    int result = process_peer(!g_strcmp0(argv[1], "--peer-offer")); gst_deinit(); return result;
  }
  g_test_init(&argc, &argv, NULL);
  GError *error = NULL;
  if (!gh_call_capabilities_check(FALSE, &error)) {
    g_printerr("UNAVAILABLE: %s\n", error->message); g_clear_error(&error); gst_deinit(); return 77;
  }
  guint timeout = g_timeout_add_seconds(45, deadline, NULL);
  g_test_add_func("/call-media/policy", policy);
  g_test_add_func("/call-media/loopback-upgrade", loopback);
  g_test_add_func("/call-media/teardown-pending", cancel_pending);
  g_test_add_func("/call-media/h264-fallback", h264_loopback);
  g_test_add_func("/call-media/cancellable", cancelled_operation);
  g_test_add_func("/call-media/video-establish", video_establish);
  g_test_add_func("/call-media/reject-high-profile", reject_high_profile);
  int result = g_test_run();
  while (g_main_context_pending(NULL)) g_main_context_iteration(NULL, FALSE);
  g_source_remove(timeout); gst_deinit(); return result;
}
