/* SPDX-License-Identifier: MIT */
/* Device-free packaging proof, not the application's call pipeline.
 * Two full-duplex peers, in-process SDP/trickle ICE, no STUN/TURN/signalling.
 * Success requires decoded Opus and VP8 buffers at BOTH peers. */
#include "call_test_runtime.h"
#include <gst/gst.h>
#include <gst/webrtc/webrtc.h>

#define N_PEERS 2
#define N_MEDIA 2
#define REQUIRED_BUFFERS 10

typedef struct {
  GstElement *pipeline;
  GstElement *rtc;
  gint received[N_MEDIA];
} Peer;

typedef struct {
  Peer peers[N_PEERS];
  GMainLoop *loop;
  gboolean passed;
} Call;

static void
on_ice(GstElement *rtc, guint mline, gchar *candidate, gpointer other)
{
  (void)rtc;
  g_signal_emit_by_name(other, "add-ice-candidate", mline, candidate);
}

static void
on_buffer(GstElement *sink, GstBuffer *buffer, GstPad *pad, gpointer count)
{
  (void)buffer;
  (void)pad;
  if (g_atomic_int_add((gint *)count, 1) + 1 == REQUIRED_BUFFERS)
    gst_element_post_message(sink, gst_message_new_application(GST_OBJECT(sink),
        gst_structure_new_empty("decoded-media")));
}

static void
on_pad(GstElement *rtc, GstPad *pad, gpointer data)
{
  (void)rtc;
  if (GST_PAD_DIRECTION(pad) != GST_PAD_SRC)
    return;
  Peer *peer = data;
  GstCaps *caps = gst_pad_get_current_caps(pad);
  g_assert_nonnull(caps);
  const gchar *media = gst_structure_get_string(gst_caps_get_structure(caps, 0), "media");
  gboolean audio = g_strcmp0(media, "audio") == 0;
  g_assert_true(audio || g_strcmp0(media, "video") == 0);
  gst_caps_unref(caps);
  GError *error = NULL;
  GstElement *receiver = gst_parse_bin_from_description(audio
      ? "queue ! rtpopusdepay ! opusdec ! audioconvert ! audio/x-raw ! fakesink name=decoded sync=false async=false signal-handoffs=true"
      : "queue ! rtpvp8depay ! vp8dec ! videoconvert ! video/x-raw ! fakesink name=decoded sync=false async=false signal-handoffs=true",
      TRUE, &error);
  g_assert_no_error(error);
  g_assert_nonnull(receiver);
  GstElement *sink = gst_bin_get_by_name(GST_BIN(receiver), "decoded");
  g_signal_connect(sink, "handoff", G_CALLBACK(on_buffer), &peer->received[audio ? 0 : 1]);
  gst_object_unref(sink);
  g_assert_true(gst_bin_add(GST_BIN(peer->pipeline), receiver));
  GstPad *sinkpad = gst_element_get_static_pad(receiver, "sink");
  g_assert_cmpint(gst_pad_link(pad, sinkpad), ==, GST_PAD_LINK_OK);
  gst_object_unref(sinkpad);
  g_assert_true(gst_element_sync_state_with_parent(receiver));
}

static gboolean
on_bus(GstBus *bus, GstMessage *message, gpointer data)
{
  (void)bus;
  Call *call = data;
  if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
    GError *error = NULL;
    gchar *debug = NULL;
    gst_message_parse_error(message, &error, &debug);
    g_printerr("%s: %s (%s)\n", GST_OBJECT_NAME(message->src), error->message,
               debug ? debug : "");
    g_clear_error(&error);
    g_free(debug);
    g_main_loop_quit(call->loop);
  } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_APPLICATION) {
    for (guint p = 0; p < N_PEERS; p++)
      for (guint m = 0; m < N_MEDIA; m++)
        if (g_atomic_int_get(&call->peers[p].received[m]) < REQUIRED_BUFFERS)
          return G_SOURCE_CONTINUE;
    call->passed = TRUE;
    g_main_loop_quit(call->loop);
  }
  return G_SOURCE_CONTINUE;
}

static gboolean
on_timeout(gpointer data)
{
  Call *call = data;
  g_printerr("Loopback timed out before both peers decoded audio and video\n");
  g_main_loop_quit(call->loop);
  return G_SOURCE_CONTINUE;
}

static void
check_promise(GstPromise *promise)
{
  g_assert_cmpint(gst_promise_wait(promise), ==, GST_PROMISE_RESULT_REPLIED);
  const GstStructure *reply = gst_promise_get_reply(promise);
  if (reply && gst_structure_has_field(reply, "error"))
    g_error("WebRTC promise failed: %s", gst_structure_to_string(reply));
}

static GstWebRTCSessionDescription *
create_description(GstElement *rtc, const gchar *action, const gchar *field)
{
  GstPromise *promise = gst_promise_new();
  g_signal_emit_by_name(rtc, action, NULL, promise);
  check_promise(promise);
  GstWebRTCSessionDescription *description = NULL;
  g_assert_true(gst_structure_get(gst_promise_get_reply(promise), field,
      GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &description, NULL));
  gst_promise_unref(promise);
  return description;
}

static void
set_description(GstElement *rtc, const gchar *action, GstWebRTCSessionDescription *sdp)
{
  GstPromise *promise = gst_promise_new();
  g_signal_emit_by_name(rtc, action, sdp, promise);
  check_promise(promise);
  gst_promise_unref(promise);
}

int main(int argc, char **argv)
{
  gst_init(&argc, &argv);
  GError *error = NULL;
  int status = gh_call_test_runtime_status();
  if (status) { gst_deinit(); return status; }
  Call call = {0};
  guint watches[N_PEERS];
  call.loop = g_main_loop_new(NULL, FALSE);
  for (guint p = 0; p < N_PEERS; p++) {
    Peer *peer = &call.peers[p];
    peer->pipeline = gst_parse_launch(
      "webrtcbin name=rtc bundle-policy=max-bundle "
      "audiotestsrc is-live=true ! audioconvert ! audioresample ! "
      "audio/x-raw,rate=48000,channels=1 ! opusenc ! rtpopuspay pt=96 ! "
      "application/x-rtp,media=audio,encoding-name=OPUS,payload=96 ! queue ! rtc. "
      "videotestsrc is-live=true ! video/x-raw,width=160,height=120,framerate=15/1 ! "
      "vp8enc deadline=1 ! rtpvp8pay pt=97 ! "
      "application/x-rtp,media=video,encoding-name=VP8,payload=97 ! queue ! rtc.", &error);
    g_assert_no_error(error);
    g_assert_nonnull(peer->pipeline);
    peer->rtc = gst_bin_get_by_name(GST_BIN(peer->pipeline), "rtc");
    g_signal_connect(peer->rtc, "pad-added", G_CALLBACK(on_pad), peer);
    GstBus *bus = gst_element_get_bus(peer->pipeline);
    watches[p] = gst_bus_add_watch(bus, on_bus, &call);
    gst_object_unref(bus);
  }
  for (guint p = 0; p < N_PEERS; p++) {
    g_signal_connect(call.peers[p].rtc, "on-ice-candidate", G_CALLBACK(on_ice),
                     call.peers[1-p].rtc);
    g_assert_cmpint(gst_element_set_state(call.peers[p].pipeline, GST_STATE_PLAYING),
                    !=, GST_STATE_CHANGE_FAILURE);
  }
  GstElement *a = call.peers[0].rtc, *b = call.peers[1].rtc;
  GstWebRTCSessionDescription *offer = create_description(a, "create-offer", "offer");
  /* Install each remote description before its opposite starts gathering ICE. */
  set_description(b, "set-remote-description", offer);
  set_description(a, "set-local-description", offer);
  GstWebRTCSessionDescription *answer = create_description(b, "create-answer", "answer");
  set_description(a, "set-remote-description", answer);
  set_description(b, "set-local-description", answer);
  gst_webrtc_session_description_free(offer);
  gst_webrtc_session_description_free(answer);

  guint timeout = g_timeout_add_seconds(30, on_timeout, &call);
  g_main_loop_run(call.loop);
  g_source_remove(timeout);
  /* Keep both peer objects alive until streaming and ICE callbacks stop. */
  for (guint p = 0; p < N_PEERS; p++)
    gst_element_set_state(call.peers[p].pipeline, GST_STATE_NULL);
  for (guint p = 0; p < N_PEERS; p++) {
    g_print("peer %u: decoded Opus=%d VP8=%d\n", p,
            g_atomic_int_get(&call.peers[p].received[0]),
            g_atomic_int_get(&call.peers[p].received[1]));
    g_source_remove(watches[p]);
    gst_object_unref(call.peers[p].rtc);
    gst_object_unref(call.peers[p].pipeline);
  }
  g_main_loop_unref(call.loop);
  gst_deinit();
  return call.passed ? 0 : 1;
}
