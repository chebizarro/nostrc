#include "gh-call-media.h"
#include "gh-call-capabilities.h"
#include "gh-call-settings.h"
#include "gh-call-paintable.h"
#include <gst/webrtc/webrtc.h>
#include <gst/sdp/sdp.h>
#include <nice.h>

typedef struct { gchar *candidate, *mid; guint line; } Ice;
static void ice_free(gpointer p) { Ice *i = p; g_free(i->candidate); g_free(i->mid); g_free(i); }
struct _GhCallMedia {
  GObject parent;
  GSettings *settings;
  GMainContext *context;
  gboolean configured, stopped, allow_local, relay_only, headset, muted, camera;
  gboolean remote_ready, remote_offer, test_sources, h264;
  guint video_payload;
  GstPad *video_pad;
  GstElement *video_branch;
  GstWebRTCRTPTransceiver *video_transceiver;
  GstCaps *video_preferences;
  guint audio_payload;
  gchar *stun, *turn, *username;
  GstElement *pipeline, *rtc, *audio_source, *video_source, *volume, *video_valve;
  GstElement *audio_receive, *video_receive, *audio_sink;
  GSource *bus_source, *device_source;
  GstDeviceMonitor *monitor;
  GListStore *devices;
  GstDevice *input, *output, *camera_device;
  GhCallPaintable *preview, *remote;
  GQueue ice;
  GTask *pending;
  NiceAgent *nice;
  gint received_audio, received_video;
};
static void backend_init(GhCallMediaBackendInterface *iface);
G_DEFINE_TYPE_WITH_CODE(GhCallMedia, gh_call_media, G_TYPE_OBJECT,
                       G_IMPLEMENT_INTERFACE(GH_TYPE_CALL_MEDIA_BACKEND, backend_init))

static void teardown(GhCallMediaBackend *backend);
static gboolean add_ice(GhCallMediaBackend *, const gchar *, const gchar *, guint, GError **);
static void post(GhCallMedia *self, GSourceFunc callback, gpointer data, GDestroyNotify destroy)
{
  /* invoke_full may run synchronously on a streaming thread. Always enqueue. */
  GSource *source = g_idle_source_new();
  g_source_set_callback(source, callback, data, destroy);
  g_source_attach(source, self->context);
  g_source_unref(source);
}
static gboolean fail(GError **error, GIOErrorEnum code, const gchar *message)
{
  g_set_error_literal(error, G_IO_ERROR, code, message);
  return FALSE;
}

/* Candidates are untrusted signalling, including candidates embedded in SDP. */
static gboolean candidate_allowed(GhCallMedia *self, const gchar *candidate)
{
  if (!candidate || strlen(candidate) > 4096 || strchr(candidate, '\r') || strchr(candidate, '\n')) return FALSE;
  const gchar *p = g_str_has_prefix(candidate, "a=") ? candidate + 2 : candidate;
  if (!g_str_has_prefix(p, "candidate:")) return FALSE;
  g_auto(GStrv) fields = g_strsplit(p, " ", -1);
  guint n = g_strv_length(fields);
  if (n < 8 || g_strcmp0(fields[6], "typ")) return FALSE;
  const gchar *type = fields[7];
  if (self->relay_only) return g_str_equal(type, "relay");
  if (!self->allow_local && g_str_equal(type, "host")) return FALSE;
  return g_str_equal(type, "host") || g_str_equal(type, "srflx") ||
         g_str_equal(type, "prflx") || g_str_equal(type, "relay");
}
static gboolean sdp_allowed(GhCallMedia *self, const GstSDPMessage *sdp)
{
  for (guint a = 0; a < gst_sdp_message_attributes_len(sdp); a++) {
    const GstSDPAttribute *attr = gst_sdp_message_get_attribute(sdp, a);
    if (!g_strcmp0(attr->key, "candidate")) {
      g_autofree gchar *c = g_strconcat("candidate:", attr->value, NULL);
      if (!candidate_allowed(self, c)) return FALSE;
    }
  }
  for (guint m = 0; m < gst_sdp_message_medias_len(sdp); m++) {
    const GstSDPMedia *media = gst_sdp_message_get_media(sdp, m);
    for (guint a = 0; a < gst_sdp_media_attributes_len(media); a++) {
      const GstSDPAttribute *attr = gst_sdp_media_get_attribute(media, a);
      if (!g_strcmp0(attr->key, "candidate")) {
        g_autofree gchar *c = g_strconcat("candidate:", attr->value, NULL);
        if (!candidate_allowed(self, c)) return FALSE;
      }
    }
  }
  return TRUE;
}

static gboolean configure(GhCallMediaBackend *backend, gboolean allow_local, GError **error)
{
  GhCallMedia *self = GH_CALL_MEDIA(backend);
  if (self->stopped) return fail(error, G_IO_ERROR_CLOSED, "Call media has been torn down");
  if (self->pipeline || self->pending) {
    teardown(backend);
    return fail(error, G_IO_ERROR_CLOSED, "Call policy changed; create a new media backend");
  }
  self->configured = FALSE;
  self->allow_local = allow_local;
  self->relay_only = g_settings_get_boolean(self->settings, "call-relay-only");
  /* libnice's force-relay hides local candidates but still gathers base host
   * sockets. It is NOT an implementation of no-local-network consent. */
  if (!allow_local)
    return fail(error, G_IO_ERROR_NOT_SUPPORTED,
        "Calls require local-network consent: this ICE backend cannot disable host gathering");
  g_auto(GStrv) stun = g_settings_get_strv(self->settings, "call-stun-servers");
  for (guint i = 0; stun[i]; i++)
    if (!gh_call_ice_url_validate(stun[i], FALSE, error)) return FALSE;
  g_free(self->stun); self->stun = g_strdup(stun[0]);
  g_free(self->turn); self->turn = g_settings_get_string(self->settings, "call-turn-url");
  g_free(self->username); self->username = g_settings_get_string(self->settings, "call-turn-username");
  if (*self->turn && !gh_call_ice_url_validate(self->turn, TRUE, error)) return FALSE;
  if (self->relay_only && !*self->turn)
    return fail(error, G_IO_ERROR_NOT_INITIALIZED, "Relay-only calls require a TURN server and credential");
  self->configured = TRUE;
  return TRUE;
}

typedef struct { GhCallMedia *self; gchar *candidate; guint line; } LocalIce;
static void local_ice_free(gpointer p) {
  LocalIce *i = p; g_object_unref(i->self); g_free(i->candidate); g_free(i);
}
static gboolean emit_ice(gpointer data)
{
  LocalIce *i = data;
  GhCallMedia *self = i->self;
  if (self->stopped || !candidate_allowed(self, i->candidate)) return G_SOURCE_REMOVE;
  GstWebRTCSessionDescription *sdp = NULL;
  g_object_get(self->rtc, "local-description", &sdp, NULL);
  const gchar *mid = NULL;
  if (sdp && i->line < gst_sdp_message_medias_len(sdp->sdp))
    mid = gst_sdp_media_get_attribute_val(gst_sdp_message_get_media(sdp->sdp, i->line), "mid");
  if (mid) g_signal_emit_by_name(self, "local-ice", i->candidate, mid, i->line);
  if (sdp) gst_webrtc_session_description_free(sdp);
  return G_SOURCE_REMOVE;
}
static void on_ice(GstElement *rtc, guint line, gchar *candidate, gpointer data)
{
  (void)rtc;
  GhCallMedia *self = data;
  LocalIce *i = g_new0(LocalIce, 1);
  i->self = g_object_ref(self); i->line = line; i->candidate = g_strdup(candidate);
  post(self, emit_ice, i, local_ice_free);
}
static gboolean emit_state(gpointer data)
{
  GhCallMedia *self = data;
  if (self->stopped) return G_SOURCE_REMOVE;
  GstWebRTCPeerConnectionState state;
  g_object_get(self->rtc, "connection-state", &state, NULL);
  GhCallMediaState mapped = GH_CALL_MEDIA_CONNECTING;
  if (state == GST_WEBRTC_PEER_CONNECTION_STATE_CONNECTED) mapped = GH_CALL_MEDIA_CONNECTED;
  if (state == GST_WEBRTC_PEER_CONNECTION_STATE_DISCONNECTED) mapped = GH_CALL_MEDIA_DISCONNECTED;
  if (state == GST_WEBRTC_PEER_CONNECTION_STATE_FAILED) mapped = GH_CALL_MEDIA_FAILED;
  g_signal_emit_by_name(self, "connection-state", mapped);
  if (mapped == GH_CALL_MEDIA_FAILED) teardown(GH_CALL_MEDIA_BACKEND(self));
  return G_SOURCE_REMOVE;
}
static void on_state(GObject *object, GParamSpec *pspec, gpointer data)
{
  (void)object; (void)pspec;
  GhCallMedia *self = data;
  post(self, emit_state, g_object_ref(self), g_object_unref);
}
static gboolean bus_message(GstBus *bus, GstMessage *message, gpointer data)
{
  (void)bus;
  g_autoptr(GhCallMedia) self = g_object_ref(data);
  if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR && !self->stopped) {
    /* Do not log GStreamer errors: they can contain TURN URLs/passwords/SDP. */
    g_signal_emit_by_name(self, "connection-state", GH_CALL_MEDIA_FAILED);
    teardown(GH_CALL_MEDIA_BACKEND(self));
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}
static void count_buffer(GstElement *identity, GstBuffer *buffer, gpointer data)
{
  (void)identity; (void)buffer; g_atomic_int_inc((gint *)data);
}
static GstElement *create_receiver(GhCallMedia *self, gboolean audio, gboolean vp8)
{
  GError *parse_error = NULL;
  GstElement *bin = gst_parse_bin_from_description(audio ?
      "queue name=receive_input ! rtpopusdepay ! opusdec ! audioconvert ! audioresample ! "
      "audio/x-raw,format=S16LE,rate=48000,channels=1 ! identity name=received" :
      vp8 ? "queue name=receive_input ! rtpvp8depay ! vp8dec ! videoconvert ! identity name=received" :
      "queue name=receive_input ! rtph264depay ! h264parse ! openh264dec ! videoconvert ! identity name=received",
      FALSE, &parse_error);
  if (!bin || parse_error) {
    if (bin) gst_object_unref(bin);
    g_clear_error(&parse_error); return NULL;
  }
  GstElement *receive_input = gst_bin_get_by_name(GST_BIN(bin), "receive_input");
  GstPad *receive_sink = gst_element_get_static_pad(receive_input, "sink");
  gst_element_add_pad(bin, gst_ghost_pad_new("sink", receive_sink));
  gst_object_unref(receive_sink); gst_object_unref(receive_input);
  GstElement *sink = audio ? (self->test_sources ? gst_element_factory_make("fakesink", NULL) :
      self->output ? gst_device_create_element(self->output, NULL) : gst_element_factory_make("autoaudiosink", NULL)) :
      gh_call_paintable_create_sink(self->remote);
  if (!sink) { gst_object_unref(bin); return NULL; }
  if (audio) self->audio_sink = sink;
  if (audio && self->test_sources) g_object_set(sink, "sync", TRUE, "async", FALSE, NULL);
  GstElement *last = gst_bin_get_by_name(GST_BIN(bin), "received");
  g_signal_connect(last, "handoff", G_CALLBACK(count_buffer),
                   audio ? &self->received_audio : &self->received_video);
  gst_bin_add(GST_BIN(bin), sink);
  if (audio && !self->headset) {
    GstElement *probe = gst_element_factory_make("webrtcechoprobe", "call_echo_probe");
    if (!probe) { gst_object_unref(last); gst_object_unref(bin); return NULL; }
    gst_bin_add(GST_BIN(bin), probe);
    gst_element_link_many(last, probe, sink, NULL);
  } else gst_element_link(last, sink);
  gst_object_unref(last);
  return bin;
}
static void receive_pad(GstElement *rtc, GstPad *pad, gpointer data)
{
  (void)rtc;
  GhCallMedia *self = data;
  if (GST_PAD_DIRECTION(pad) != GST_PAD_SRC) return;
  GstCaps *caps = gst_pad_get_current_caps(pad);
  if (!caps) caps = gst_pad_query_caps(pad, NULL);
  const gchar *encoding = caps && gst_caps_get_size(caps) ?
      gst_structure_get_string(gst_caps_get_structure(caps, 0), "encoding-name") : NULL;
  gboolean audio = !g_strcmp0(encoding, "OPUS");
  gboolean vp8 = !g_strcmp0(encoding, "VP8");
  gboolean h264 = !g_strcmp0(encoding, "H264");
  if (caps) gst_caps_unref(caps);
  if (!audio && !vp8 && !h264) return;
  GstElement *bin = audio ? self->audio_receive : self->video_receive;
  if (!bin) {
    bin = create_receiver(self, audio, vp8);
    if (!bin) return;
    gst_bin_add(GST_BIN(self->pipeline), bin);
    if (audio) self->audio_receive = bin; else self->video_receive = bin;
  }
  GstPad *input = gst_element_get_static_pad(bin, "sink");
  gst_pad_link(pad, input);
  gst_object_unref(input);
  gst_element_sync_state_with_parent(bin);
}

static void prepare_video(GhCallMedia *self)
{
  if (self->video_transceiver) return;
  GstCaps *preferences = self->video_preferences ? gst_caps_ref(self->video_preferences) : gst_caps_from_string(
      "application/x-rtp,media=video,encoding-name=VP8,clock-rate=90000,payload=97;"
      "application/x-rtp,media=video,encoding-name=H264,clock-rate=90000,payload=98,packetization-mode=(string)1,profile-level-id=(string)42e01f;"
      "application/x-rtp,media=video,encoding-name=H264,clock-rate=90000,payload=99,packetization-mode=(string)1,profile-level-id=(string)42e032");
  g_signal_emit_by_name(self->rtc, "add-transceiver", GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDRECV,
                        preferences, &self->video_transceiver);
  gst_caps_unref(preferences);
}
static gboolean add_video(GhCallMedia *self, GError **error)
{
  if (self->video_source) return TRUE;
  g_autofree gchar *description = g_strdup_printf(
      "videoconvert name=capture_convert ! videoscale ! video/x-raw,width=640,height=480 ! tee name=split "
      "split. ! queue ! valve name=camera drop-mode=transform-to-gap ! %s pt=%u ! "
      "application/x-rtp,media=video,encoding-name=%s,payload=%u ! queue name=rtp_output "
      "split. ! queue leaky=downstream max-size-buffers=1 ! videoconvert name=preview_convert",
      self->h264 ? "openh264enc ! video/x-h264,profile=constrained-baseline ! h264parse ! rtph264pay config-interval=-1" :
                   "vp8enc deadline=1 ! rtpvp8pay", self->video_payload,
      self->h264 ? "H264" : "VP8", self->video_payload);
  GstElement *branch = gst_parse_bin_from_description(description, FALSE, error);
  if (!branch || (error && *error)) {
    if (branch) gst_object_unref(branch);
    return FALSE;
  }
  GstElement *source = self->test_sources ? gst_element_factory_make("videotestsrc", NULL) :
      self->camera_device ? gst_device_create_element(self->camera_device, NULL) : gst_element_factory_make("autovideosrc", NULL);
  if (!source) { gst_object_unref(branch); return fail(error, G_IO_ERROR_NOT_FOUND, "Camera capture plugin unavailable"); }
  if (self->test_sources) g_object_set(source, "is-live", TRUE, NULL);
  /* Explicit pads avoid selecting the preview branch as the RTP output. */
  GstElement *first = gst_bin_get_by_name(GST_BIN(branch), "capture_convert");
  gst_bin_add(GST_BIN(branch), source);
  if (!first || !gst_element_link(source, first)) {
    if (first) gst_object_unref(first);
    gst_object_unref(branch); return fail(error, G_IO_ERROR_FAILED, "Cannot link camera capture");
  }
  gst_object_unref(first);
  GstElement *preview = gst_bin_get_by_name(GST_BIN(branch), "preview_convert");
  GstElement *sink = gh_call_paintable_create_sink(self->preview);
  gst_bin_add(GST_BIN(branch), sink); gst_element_link(preview, sink); gst_object_unref(preview);
  GstElement *rtp_output = gst_bin_get_by_name(GST_BIN(branch), "rtp_output");
  GstPad *rtp = gst_element_get_static_pad(rtp_output, "src");
  gst_object_unref(rtp_output);
  if (!rtp) { gst_object_unref(branch); return fail(error, G_IO_ERROR_FAILED, "Cannot find video RTP output"); }
  gst_element_add_pad(branch, gst_ghost_pad_new("src", rtp)); gst_object_unref(rtp);
  gst_bin_add(GST_BIN(self->pipeline), branch);
  self->video_branch = branch;
  /* Negotiate before selecting a sender. Otherwise webrtcbin intersects the
   * offer with the currently running VP8 encoder and silently drops H.264. */
  g_object_set(self->video_transceiver, "codec-preferences", NULL, NULL);
  GstIterator *pads = gst_element_iterate_sink_pads(self->rtc);
  GValue value = G_VALUE_INIT;
  while (!self->video_pad && gst_iterator_next(pads, &value) == GST_ITERATOR_OK) {
    GstWebRTCRTPTransceiver *owner = NULL;
    g_object_get(g_value_get_object(&value), "transceiver", &owner, NULL);
    if (owner == self->video_transceiver) self->video_pad = g_value_dup_object(&value);
    g_clear_object(&owner); g_value_reset(&value);
  }
  g_value_unset(&value); gst_iterator_free(pads);
  if (!self->video_pad) {
    guint mline = 0;
    g_object_get(self->video_transceiver, "mlineindex", &mline, NULL);
    g_autofree gchar *name = g_strdup_printf("sink_%u", mline);
    self->video_pad = gst_element_request_pad_simple(self->rtc, name);
  }
  if (!self->video_pad) return fail(error, G_IO_ERROR_FAILED, "Cannot attach negotiated video sender");
  GstPad *src = gst_element_get_static_pad(branch, "src");
  GstPadLinkReturn linked = gst_pad_link(src, self->video_pad);
  gst_object_unref(src);
  if (linked != GST_PAD_LINK_OK) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Cannot link video RTP (%s)", gst_pad_link_get_name(linked));
    return FALSE;
  }
  self->video_source = source;
  self->video_valve = gst_bin_get_by_name(GST_BIN(branch), "camera");
  g_object_set(self->video_valve, "drop", !self->camera, NULL);
  gst_element_set_locked_state(source, !self->camera);
  gst_element_sync_state_with_parent(branch);
  return TRUE;
}

static gboolean build_pipeline(GhCallMedia *self, const gchar *password, GError **error)
{
  if (self->pipeline) return TRUE;
  if (!gh_call_capabilities_check(!self->headset, error)) return FALSE;
  const gchar *required[] = { "appsink", "videoscale", "tee", "valve", "volume", "identity",
    "openh264enc", "h264parse", "rtph264pay", "rtph264depay", "openh264dec", NULL };
  for (guint i = 0; required[i]; i++) {
    GstElementFactory *factory = gst_element_factory_find(required[i]);
    GstPluginFeature *loaded = factory ? gst_plugin_feature_load(GST_PLUGIN_FEATURE(factory)) : NULL;
    if (factory) gst_object_unref(factory);
    if (!loaded) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                  "Calls unavailable: install the GStreamer runtime plugin providing %s", required[i]);
      return FALSE;
    }
    gst_object_unref(loaded);
  }
  self->pipeline = gst_pipeline_new(NULL);
  self->rtc = gst_element_factory_make("webrtcbin", "rtc");
  gst_bin_add(GST_BIN(self->pipeline), self->rtc);
  GstWebRTCICE *ice = NULL;
  if (!g_object_class_find_property(G_OBJECT_GET_CLASS(self->rtc), "ice-agent"))
    return fail(error, G_IO_ERROR_NOT_SUPPORTED, "This webrtcbin cannot expose its ICE agent for clean shutdown");
  g_object_get(self->rtc, "ice-agent", &ice, NULL);
  if (ice && g_object_class_find_property(G_OBJECT_GET_CLASS(ice), "agent"))
    g_object_get(ice, "agent", &self->nice, NULL);
  g_clear_object(&ice);
  if (!self->nice) return fail(error, G_IO_ERROR_NOT_SUPPORTED, "This ICE implementation cannot release TURN allocations safely");
  g_object_set(self->nice, "upnp", FALSE, NULL);
  g_object_set(self->rtc, "bundle-policy", GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE,
      "ice-transport-policy", self->relay_only ? GST_WEBRTC_ICE_TRANSPORT_POLICY_RELAY : GST_WEBRTC_ICE_TRANSPORT_POLICY_ALL, NULL);
  if (self->stun && !self->relay_only) g_object_set(self->rtc, "stun-server", self->stun, NULL);
  if (self->turn && *self->turn) {
    g_autoptr(GUri) uri = g_uri_parse(self->turn, G_URI_FLAGS_NONE, NULL);
    g_autofree gchar *user = g_uri_escape_string(self->username, NULL, FALSE);
    g_autofree gchar *pass = g_uri_escape_string(password, NULL, FALSE);
    g_autofree gchar *credentials = g_strconcat(user, ":", pass, NULL);
    g_autofree gchar *url = g_uri_join(G_URI_FLAGS_ENCODED, g_uri_get_scheme(uri), credentials,
        g_uri_get_host(uri), g_uri_get_port(uri), "", g_uri_get_query(uri), NULL);
    gboolean accepted = FALSE;
    g_signal_emit_by_name(self->rtc, "add-turn-server", url, &accepted);
    /* GStreamer owns its copy until teardown. Never log either copy. */
    memset(pass, 0, strlen(pass)); memset(credentials, 0, strlen(credentials)); memset(url, 0, strlen(url));
    if (!accepted) return fail(error, G_IO_ERROR_INVALID_ARGUMENT, "TURN server rejected by media runtime");
  }
  /* Register the echo probe before the capture DSP enters PAUSED. It cannot
   * first be created by pad-added after negotiation has already started. */
  self->audio_receive = create_receiver(self, TRUE, FALSE);
  if (!self->audio_receive) return fail(error, G_IO_ERROR_FAILED, "Cannot create audio playback route");
  gst_bin_add(GST_BIN(self->pipeline), self->audio_receive);
  GstElement *audio = gst_parse_bin_from_description(self->headset ?
      "audioconvert ! audioresample ! audio/x-raw,format=S16LE,rate=48000,channels=1 ! volume name=mute ! opusenc ! "
      "rtpopuspay name=audio_pay pt=96 ! capsfilter name=audio_caps caps=\"application/x-rtp,media=audio,encoding-name=OPUS,payload=96\" ! queue" :
      "audioconvert ! audioresample ! audio/x-raw,format=S16LE,rate=48000,channels=1 ! "
      "webrtcdsp probe=call_echo_probe ! volume name=mute ! opusenc ! rtpopuspay name=audio_pay pt=96 ! "
      "capsfilter name=audio_caps caps=\"application/x-rtp,media=audio,encoding-name=OPUS,payload=96\" ! queue", TRUE, error);
  if (!audio || (error && *error)) {
    if (audio) gst_object_unref(audio);
    return FALSE;
  }
  self->audio_source = self->test_sources ? gst_element_factory_make("audiotestsrc", NULL) :
      self->input ? gst_device_create_element(self->input, NULL) : gst_element_factory_make("autoaudiosrc", NULL);
  if (!self->audio_source) { gst_object_unref(audio); return fail(error, G_IO_ERROR_NOT_FOUND, "Audio capture plugin unavailable"); }
  if (self->test_sources) g_object_set(self->audio_source, "is-live", TRUE, NULL);
  gst_bin_add_many(GST_BIN(self->pipeline), self->audio_source, audio, NULL);
  if (!gst_element_link_many(self->audio_source, audio, self->rtc, NULL)) return fail(error, G_IO_ERROR_FAILED, "Cannot link Opus capture");
  self->volume = gst_bin_get_by_name(GST_BIN(audio), "mute");
  g_object_set(self->volume, "mute", self->muted, NULL);
  g_signal_connect(self->rtc, "on-ice-candidate", G_CALLBACK(on_ice), self);
  g_signal_connect(self->rtc, "notify::connection-state", G_CALLBACK(on_state), self);
  g_signal_connect(self->rtc, "pad-added", G_CALLBACK(receive_pad), self);
  GstBus *bus = gst_element_get_bus(self->pipeline);
  self->bus_source = gst_bus_create_watch(bus);
  g_source_set_callback(self->bus_source, G_SOURCE_FUNC(bus_message), self, NULL);
  g_source_attach(self->bus_source, self->context);
  gst_object_unref(bus);
  return TRUE;
}

typedef enum { OP_CREATE, OP_LOCAL, OP_REMOTE } Stage;
typedef struct {
  Stage stage;
  GhCallType type;
  gboolean answer;
  GstWebRTCSessionDescription *description;
  gulong cancellation;
} Operation;
static void operation_free(gpointer data)
{
  Operation *op = data;
  if (op->description) gst_webrtc_session_description_free(op->description);
  g_free(op);
}
static void disconnect_cancel(GTask *task)
{
  Operation *op = g_task_get_task_data(task);
  if (op->cancellation) {
    g_cancellable_disconnect(g_task_get_cancellable(task), op->cancellation);
    op->cancellation = 0;
  }
}
static void complete_error(GTask *task, GError *error)
{
  GhCallMedia *self = g_task_get_source_object(task);
  disconnect_cancel(task);
  g_clear_object(&self->pending);
  g_task_return_error(task, error);
  teardown(GH_CALL_MEDIA_BACKEND(self));
}
static gboolean cancelled(gpointer data)
{
  GTask *task = data;
  GhCallMedia *self = g_task_get_source_object(task);
  if (self->pending == task) teardown(GH_CALL_MEDIA_BACKEND(self));
  return G_SOURCE_REMOVE;
}
static void on_cancel(GCancellable *cancel, gpointer data)
{
  (void)cancel;
  GTask *task = data;
  post(g_task_get_source_object(task), cancelled, g_object_ref(task), g_object_unref);
}

typedef struct { GTask *task; GError *error; GstWebRTCSessionDescription *description; } Completion;
static void completion_free(gpointer data) {
  Completion *c = data;
  g_clear_error(&c->error);
  if (c->description) gst_webrtc_session_description_free(c->description);
  g_object_unref(c->task); g_free(c);
}
static void promise_changed(GstPromise *promise, gpointer data);
static void set_description(GTask *task, const gchar *action)
{
  GhCallMedia *self = g_task_get_source_object(task);
  Operation *op = g_task_get_task_data(task);
  GstPromise *promise = gst_promise_new_with_change_func(promise_changed, g_object_ref(task), g_object_unref);
  g_signal_emit_by_name(self->rtc, action, op->description, promise);
}
static gboolean promise_complete(gpointer data)
{
  Completion *c = data;
  GTask *task = c->task;
  GhCallMedia *self = g_task_get_source_object(task);
  Operation *op = g_task_get_task_data(task);
  if (self->pending != task || self->stopped) return G_SOURCE_REMOVE;
  if (g_task_get_cancellable(task) && g_cancellable_is_cancelled(g_task_get_cancellable(task))) {
    teardown(GH_CALL_MEDIA_BACKEND(self)); return G_SOURCE_REMOVE;
  }
  if (c->error) { complete_error(task, g_steal_pointer(&c->error)); return G_SOURCE_REMOVE; }
  if (op->stage == OP_CREATE) {
    if (!c->description || !sdp_allowed(self, c->description->sdp)) {
      complete_error(task, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, "Cannot create policy-compliant SDP"));
      return G_SOURCE_REMOVE;
    }
    op->description = g_steal_pointer(&c->description);
    op->stage = OP_LOCAL;
    set_description(task, "set-local-description");
    return G_SOURCE_REMOVE;
  }
  if (self->video_transceiver && ((op->stage == OP_REMOTE && op->description->type == GST_WEBRTC_SDP_TYPE_ANSWER) ||
      (op->stage == OP_LOCAL && op->answer))) {
    GError *error = NULL;
    if (!add_video(self, &error)) { complete_error(task, error); return G_SOURCE_REMOVE; }
  }
  disconnect_cancel(task);
  g_clear_object(&self->pending);
  if (op->stage == OP_REMOTE) {
    self->remote_ready = TRUE;
    self->remote_offer = op->description->type == GST_WEBRTC_SDP_TYPE_OFFER;
    while (!g_queue_is_empty(&self->ice)) {
      Ice *ice = g_queue_pop_head(&self->ice);
      GError *error = NULL;
      gboolean ok = add_ice(GH_CALL_MEDIA_BACKEND(self), ice->candidate, ice->mid, ice->line, &error);
      ice_free(ice);
      if (!ok) { g_task_return_error(task, error); teardown(GH_CALL_MEDIA_BACKEND(self)); return G_SOURCE_REMOVE; }
    }
    g_task_return_boolean(task, TRUE);
  } else {
    if (op->answer) self->remote_offer = FALSE;
    g_task_return_pointer(task, gst_sdp_message_as_text(op->description->sdp), g_free);
  }
  return G_SOURCE_REMOVE;
}
static void promise_changed(GstPromise *promise, gpointer data)
{
  GTask *task = data;
  Operation *op = g_task_get_task_data(task);
  Completion *c = g_new0(Completion, 1);
  c->task = g_object_ref(task);
  if (gst_promise_wait(promise) != GST_PROMISE_RESULT_REPLIED)
    c->error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, "Media negotiation interrupted");
  else {
    const GstStructure *reply = gst_promise_get_reply(promise);
    if (reply && gst_structure_has_field(reply, "error"))
      c->error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, "Media negotiation failed");
    else if (op->stage == OP_CREATE && reply)
      gst_structure_get(reply, op->answer ? "answer" : "offer",
          GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &c->description, NULL);
  }
  post(g_task_get_source_object(task), promise_complete, c, completion_free);
  gst_promise_unref(promise);
}
static gboolean has_video(const GstSDPMessage *sdp)
{
  for (guint i = 0; i < gst_sdp_message_medias_len(sdp); i++)
    if (!g_strcmp0(gst_sdp_media_get_media(gst_sdp_message_get_media(sdp, i)), "video")) return TRUE;
  return FALSE;
}
static gboolean select_remote_codecs(GhCallMedia *self, const GstSDPMessage *sdp, GError **error)
{
  for (guint m = 0; m < gst_sdp_message_medias_len(sdp); m++) {
    const GstSDPMedia *media = gst_sdp_message_get_media(sdp, m);
    gboolean video = !g_strcmp0(gst_sdp_media_get_media(media), "video");
    gboolean audio = !g_strcmp0(gst_sdp_media_get_media(media), "audio");
    if ((!audio && !video) || (gst_sdp_media_get_port(media) == 0 &&
        !gst_sdp_media_get_attribute_val(media, "bundle-only"))) continue;
    GstCaps *preferred = NULL, *fallback = NULL;
    for (guint f = 0; f < gst_sdp_media_formats_len(media); f++) {
      const gchar *format = gst_sdp_media_get_format(media, f);
      gchar *end = NULL;
      guint64 payload = g_ascii_strtoull(format, &end, 10);
      if (!end || *end || payload > 127) continue;
      GstCaps *caps = gst_sdp_media_get_caps_from_media(media, payload);
      if (!caps || gst_caps_is_empty(caps)) { if (caps) gst_caps_unref(caps); continue; }
      GstStructure *s = gst_caps_get_structure(caps, 0);
      gst_structure_set_name(s, "application/x-rtp");
      const gchar *encoding = gst_structure_get_string(s, "encoding-name");
      const gchar *profile = gst_structure_get_string(s, "profile-level-id");
      const gchar *packetization = gst_structure_get_string(s, "packetization-mode");
      if (!preferred && ((audio && !g_strcmp0(encoding, "OPUS")) || (video && !g_strcmp0(encoding, "VP8"))))
        preferred = gst_caps_ref(caps);
      if (video && !fallback && !g_strcmp0(encoding, "H264") && !g_strcmp0(packetization, "1") &&
          (!g_strcmp0(profile, "42e01f") || !g_strcmp0(profile, "42e032"))) fallback = gst_caps_ref(caps);
      gst_caps_unref(caps);
    }
    if (!preferred) preferred = g_steal_pointer(&fallback);
    if (fallback) gst_caps_unref(fallback);
    if (!preferred) return fail(error, G_IO_ERROR_NOT_SUPPORTED, "Peer offers no compatible Opus, VP8 or constrained-baseline H.264 codec");
    GstStructure *s = gst_caps_get_structure(preferred, 0);
    gint payload = 0; gst_structure_get_int(s, "payload", &payload);
    if (audio) {
      self->audio_payload = payload;
      GstElement *pay = gst_bin_get_by_name(GST_BIN(self->pipeline), "audio_pay");
      GstElement *filter = gst_bin_get_by_name(GST_BIN(self->pipeline), "audio_caps");
      g_object_set(pay, "pt", (guint)payload, NULL);
      GstCaps *rtp = gst_caps_new_simple("application/x-rtp", "media", G_TYPE_STRING, "audio",
          "encoding-name", G_TYPE_STRING, "OPUS", "payload", G_TYPE_INT, payload, NULL);
      g_object_set(filter, "caps", rtp, NULL);
      gst_caps_unref(rtp); gst_object_unref(pay); gst_object_unref(filter);
    } else {
      gboolean h264 = !g_strcmp0(gst_structure_get_string(s, "encoding-name"), "H264");
      if (self->video_branch && (h264 != self->h264 || (guint)payload != self->video_payload)) {
        gst_element_set_state(self->video_branch, GST_STATE_NULL);
        GstPad *src = gst_element_get_static_pad(self->video_branch, "src");
        gst_pad_unlink(src, self->video_pad); gst_object_unref(src);
        gst_bin_remove(GST_BIN(self->pipeline), self->video_branch);
        self->video_branch = self->video_source = NULL;
        g_clear_pointer(&self->video_valve, gst_object_unref);
      }
      self->h264 = h264; self->video_payload = payload;
      g_clear_pointer(&self->video_preferences, gst_caps_unref);
      self->video_preferences = gst_caps_ref(preferred);
    }
    gst_caps_unref(preferred);
  }
  return TRUE;
}
static void start_operation(GTask *task, const gchar *password)
{
  GhCallMedia *self = g_task_get_source_object(task);
  Operation *op = g_task_get_task_data(task);
  if (self->pending != task || self->stopped) return;
  GError *error = NULL;
  if ((g_task_get_cancellable(task) && g_cancellable_set_error_if_cancelled(g_task_get_cancellable(task), &error)) ||
      !build_pipeline(self, password, &error) ||
      (op->description && !select_remote_codecs(self, op->description->sdp, &error))) {
    complete_error(task, error); return;
  }
  if (op->type == GH_CALL_VIDEO || (op->description && has_video(op->description->sdp))) prepare_video(self);
  if (gst_element_set_state(self->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    complete_error(task, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, "Cannot start media devices")); return;
  }
  if (op->stage == OP_REMOTE) {
    self->remote_ready = FALSE;
    set_description(task, "set-remote-description");
  }
  else {
    GstPromise *promise = gst_promise_new_with_change_func(promise_changed, g_object_ref(task), g_object_unref);
    g_signal_emit_by_name(self->rtc, op->answer ? "create-answer" : "create-offer", NULL, promise);
  }
}
static void password_ready(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  GTask *task = data;
  GhCallMedia *self = g_task_get_source_object(task);
  GError *error = NULL;
  gchar *password = gh_call_turn_password_lookup_finish(result, &error);
  if (self->pending == task && !self->stopped) {
    if (error) complete_error(task, g_steal_pointer(&error));
    else start_operation(task, password);
  }
  g_clear_error(&error);
  gh_call_turn_password_free(password);
  g_object_unref(task);
}
static void begin(GhCallMedia *self, Operation *op, GCancellable *cancel,
                  GAsyncReadyCallback callback, gpointer data, GError *error)
{
  GTask *task = g_task_new(self, cancel, callback, data);
  g_task_set_task_data(task, op, operation_free);
  if (!error && (self->stopped || !self->configured))
    error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CLOSED, "Call media is not configured or has been torn down");
  if (!error && self->pending)
    error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_PENDING, "A media negotiation is already pending");
  if (!error && cancel && g_cancellable_is_cancelled(cancel))
    error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED, "Call operation cancelled");
  if (error) { g_task_return_error(task, error); g_object_unref(task); return; }
  self->pending = g_object_ref(task);
  if (cancel) op->cancellation = g_cancellable_connect(cancel, G_CALLBACK(on_cancel), task, NULL);
  if (!self->pipeline && self->turn && *self->turn)
    gh_call_turn_password_lookup_async(self->turn, self->username, cancel, password_ready, g_object_ref(task));
  else start_operation(task, NULL);
  g_object_unref(task);
}
static void create_local(GhCallMediaBackend *backend, GhCallType type, gboolean answer,
                         GCancellable *cancel, GAsyncReadyCallback callback, gpointer data)
{
  GhCallMedia *self = GH_CALL_MEDIA(backend);
  Operation *op = g_new0(Operation, 1);
  op->stage = OP_CREATE; op->type = type; op->answer = answer;
  GError *error = NULL;
  if (answer && !self->remote_offer)
    error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "An answer requires an applied remote offer");
  else if (type != GH_CALL_VOICE && type != GH_CALL_VIDEO)
    error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Unknown call media type");
  begin(self, op, cancel, callback, data, error);
}
static gchar *create_finish(GhCallMediaBackend *backend, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, backend), NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}
static void apply_remote(GhCallMediaBackend *backend, const gchar *sdp, gboolean offer,
                         GCancellable *cancel, GAsyncReadyCallback callback, gpointer data)
{
  GhCallMedia *self = GH_CALL_MEDIA(backend);
  Operation *op = g_new0(Operation, 1); op->stage = OP_REMOTE;
  GstSDPMessage *message = NULL;
  GError *error = NULL;
  if (!sdp || strlen(sdp) > 65536 || gst_sdp_message_new_from_text(sdp, &message) != GST_SDP_OK ||
      !gst_sdp_message_medias_len(message) || !sdp_allowed(self, message)) {
    if (message) gst_sdp_message_free(message);
    error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid or policy-forbidden remote SDP");
  } else op->description = gst_webrtc_session_description_new(
        offer ? GST_WEBRTC_SDP_TYPE_OFFER : GST_WEBRTC_SDP_TYPE_ANSWER, message);
  begin(self, op, cancel, callback, data, error);
}
static gboolean apply_finish(GhCallMediaBackend *backend, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, backend), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}
static gboolean add_ice(GhCallMediaBackend *backend, const gchar *candidate,
                        const gchar *mid, guint line, GError **error)
{
  GhCallMedia *self = GH_CALL_MEDIA(backend);
  if (self->stopped || !self->configured) return fail(error, G_IO_ERROR_CLOSED, "Call media is not available");
  if (!candidate_allowed(self, candidate) || !mid || strlen(mid) > 256 || line > 16)
    return fail(error, G_IO_ERROR_INVALID_ARGUMENT, "Invalid or policy-forbidden ICE candidate");
  if (!self->remote_ready) {
    if (self->ice.length >= 256) return fail(error, G_IO_ERROR_NO_SPACE, "Too many queued ICE candidates");
    Ice *i = g_new0(Ice, 1); i->candidate = g_strdup(candidate); i->mid = g_strdup(mid); i->line = line;
    g_queue_push_tail(&self->ice, i); return TRUE;
  }
  GstWebRTCSessionDescription *sdp = NULL;
  g_object_get(self->rtc, "remote-description", &sdp, NULL);
  gboolean valid = sdp && line < gst_sdp_message_medias_len(sdp->sdp) &&
      !g_strcmp0(mid, gst_sdp_media_get_attribute_val(gst_sdp_message_get_media(sdp->sdp, line), "mid"));
  if (sdp) gst_webrtc_session_description_free(sdp);
  if (!valid) return fail(error, G_IO_ERROR_INVALID_ARGUMENT, "ICE candidate does not identify a remote media section");
  g_signal_emit_by_name(self->rtc, "add-ice-candidate", line, candidate, NULL);
  return TRUE;
}
static void clear_source(GSource **source)
{
  if (*source) g_source_destroy(*source);
  g_clear_pointer(source, g_source_unref);
}
static void nice_closed(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source; (void)result; *(gboolean *)data = TRUE;
}
static void teardown(GhCallMediaBackend *backend)
{
  GhCallMedia *self = GH_CALL_MEDIA(backend);
  if (self->stopped) return;
  self->stopped = TRUE;
  if (self->rtc) g_signal_handlers_disconnect_by_data(self->rtc, self);
  clear_source(&self->bus_source);
  if (self->pipeline) gst_element_set_state(self->pipeline, GST_STATE_NULL);
  if (self->nice) {
    /* Complete TURN deallocation before releasing the agent. A private context
     * avoids dispatching UI/signalling while honoring synchronous teardown. */
    GMainContext *closing = g_main_context_new();
    g_main_context_push_thread_default(closing);
    gboolean closed = FALSE;
    nice_agent_close_async(self->nice, nice_closed, &closed);
    while (!closed) g_main_context_iteration(closing, TRUE);
    g_main_context_pop_thread_default(closing);
    g_main_context_unref(closing);
    g_clear_object(&self->nice);
  }
  g_clear_pointer(&self->volume, gst_object_unref);
  g_clear_pointer(&self->video_valve, gst_object_unref);
  if (self->video_pad) {
    gst_element_release_request_pad(self->rtc, self->video_pad);
    g_clear_pointer(&self->video_pad, gst_object_unref);
  }
  self->video_branch = NULL;
  g_clear_object(&self->video_transceiver);
  g_clear_pointer(&self->pipeline, gst_object_unref);
  self->rtc = self->audio_source = self->video_source = NULL;
  self->audio_receive = self->video_receive = self->audio_sink = NULL;
  g_queue_clear_full(&self->ice, ice_free);
  gh_call_paintable_stop(self->preview); gh_call_paintable_stop(self->remote);
  clear_source(&self->device_source);
  if (self->monitor) gst_device_monitor_stop(self->monitor);
  g_clear_object(&self->monitor);
  if (self->pending) {
    GTask *task = g_steal_pointer(&self->pending);
    disconnect_cancel(task);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Call media was torn down");
    g_object_unref(task);
  }
}
static void backend_init(GhCallMediaBackendInterface *iface)
{
  iface->configure = configure; iface->create_local_async = create_local;
  iface->create_local_finish = create_finish; iface->apply_remote_async = apply_remote;
  iface->apply_remote_finish = apply_finish; iface->add_remote_ice = add_ice; iface->teardown = teardown;
}

void gh_call_media_set_muted(GhCallMedia *self, gboolean muted) {
  self->muted = muted;
  if (self->volume) g_object_set(self->volume, "mute", muted, NULL);
}
void gh_call_media_set_camera_enabled(GhCallMedia *self, gboolean enabled) {
  self->camera = enabled;
  if (self->video_valve) g_object_set(self->video_valve, "drop", !enabled, NULL);
  if (self->video_source) {
    gst_element_set_locked_state(self->video_source, !enabled);
    if (enabled) gst_element_sync_state_with_parent(self->video_source);
    else gst_element_set_state(self->video_source, GST_STATE_NULL);
  }
}
gboolean gh_call_media_set_headset(GhCallMedia *self, gboolean headset, GError **error) {
  if (self->pipeline || self->stopped) return fail(error, G_IO_ERROR_BUSY, "Choose the audio route before starting a call");
  if (headset && !self->output && !self->test_sources)
    return fail(error, G_IO_ERROR_INVALID_ARGUMENT, "Select an explicit headset output; the system default route can change to speakers");
  self->headset = headset; return TRUE;
}
static gboolean device_message(GstBus *bus, GstMessage *message, gpointer data)
{
  (void)bus;
  g_autoptr(GhCallMedia) self = g_object_ref(data);
  GstDevice *device = NULL;
  if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_DEVICE_ADDED) {
    gst_message_parse_device_added(message, &device);
    g_list_store_append(self->devices, device);
  } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_DEVICE_REMOVED) {
    gst_message_parse_device_removed(message, &device);
    guint position;
    if (g_list_store_find(self->devices, device, &position)) g_list_store_remove(self->devices, position);
    if (device == self->input && self->audio_source) gst_element_set_state(self->audio_source, GST_STATE_NULL);
    if (device == self->camera_device && self->video_source) gst_element_set_state(self->video_source, GST_STATE_NULL);
    if (device == self->output) {
      /* Do not silently switch a verified headset onto speakers without AEC. */
      g_signal_emit_by_name(self, "connection-state", GH_CALL_MEDIA_FAILED);
      teardown(GH_CALL_MEDIA_BACKEND(self));
    }
    g_signal_emit_by_name(self, "device-removed", device);
  }
  if (device) gst_object_unref(device);
  return G_SOURCE_CONTINUE;
}
gboolean gh_call_media_start_devices(GhCallMedia *self, GError **error)
{
  if (self->stopped) return fail(error, G_IO_ERROR_CLOSED, "Call media has been torn down");
  if (self->monitor) return TRUE;
  self->monitor = gst_device_monitor_new();
  gst_device_monitor_add_filter(self->monitor, "Audio/Source", NULL);
  gst_device_monitor_add_filter(self->monitor, "Audio/Sink", NULL);
  gst_device_monitor_add_filter(self->monitor, "Video/Source", NULL);
  if (!gst_device_monitor_start(self->monitor)) {
    g_clear_object(&self->monitor);
    return fail(error, G_IO_ERROR_NOT_SUPPORTED, "No media device provider is available");
  }
  GstBus *bus = gst_device_monitor_get_bus(self->monitor);
  self->device_source = gst_bus_create_watch(bus);
  g_source_set_callback(self->device_source, G_SOURCE_FUNC(device_message), self, NULL);
  g_source_attach(self->device_source, self->context); gst_object_unref(bus);
  return TRUE;
}
GListModel *gh_call_media_get_devices(GhCallMedia *self) { return G_LIST_MODEL(self->devices); }
static gboolean replace_device(GhCallMedia *self, GstElement **old, GstDevice *device,
                               const gchar *fallback, gboolean source, gboolean active, GError **error)
{
  if (!*old) return TRUE;
  GstElement *replacement = device ? gst_device_create_element(device, NULL) : gst_element_factory_make(fallback, NULL);
  if (!replacement) return fail(error, G_IO_ERROR_NOT_FOUND, "Selected media device is unavailable");
  GstElement *parent = GST_ELEMENT(gst_object_get_parent(GST_OBJECT(*old)));
  GstPad *pad = gst_element_get_static_pad(*old, source ? "src" : "sink");
  GstPad *peer = gst_pad_get_peer(pad);
  gst_element_set_state(*old, GST_STATE_NULL);
  if (source) gst_pad_unlink(pad, peer); else gst_pad_unlink(peer, pad);
  gst_object_unref(pad);
  gst_bin_remove(GST_BIN(parent), *old);
  gst_bin_add(GST_BIN(parent), replacement); *old = replacement;
  pad = gst_element_get_static_pad(replacement, source ? "src" : "sink");
  GstPadLinkReturn linked = source ? gst_pad_link(pad, peer) : gst_pad_link(peer, pad);
  gst_object_unref(pad); gst_object_unref(peer); gst_object_unref(parent);
  gst_element_set_locked_state(replacement, !active);
  if (linked != GST_PAD_LINK_OK || (active && !gst_element_sync_state_with_parent(replacement))) {
    teardown(GH_CALL_MEDIA_BACKEND(self));
    return fail(error, G_IO_ERROR_FAILED, "Cannot activate selected media device");
  }
  return TRUE;
}
gboolean gh_call_media_select_device(GhCallMedia *self, const gchar *klass, GstDevice *device, GError **error)
{
  if (self->stopped) return fail(error, G_IO_ERROR_CLOSED, "Call media has been torn down");
  if (self->pending) return fail(error, G_IO_ERROR_BUSY, "Wait for media negotiation before changing devices");
  if (device && !gst_device_has_classes(device, klass)) return fail(error, G_IO_ERROR_INVALID_ARGUMENT, "Wrong device class");
  if (!g_strcmp0(klass, "Audio/Source")) {
    if (!replace_device(self, &self->audio_source, device, "autoaudiosrc", TRUE, TRUE, error)) return FALSE;
    g_set_object(&self->input, device);
  } else if (!g_strcmp0(klass, "Audio/Sink")) {
    if (self->pipeline && self->headset) return fail(error, G_IO_ERROR_NOT_SUPPORTED,
        "End this headset call before changing output: the new route needs echo cancellation");
    if (!replace_device(self, &self->audio_sink, device, "autoaudiosink", FALSE, TRUE, error)) return FALSE;
    g_set_object(&self->output, device); self->headset = FALSE;
  } else if (!g_strcmp0(klass, "Video/Source")) {
    if (!replace_device(self, &self->video_source, device, "autovideosrc", TRUE, self->camera, error)) return FALSE;
    g_set_object(&self->camera_device, device);
    gh_call_media_set_camera_enabled(self, self->camera);
  }
  else return fail(error, G_IO_ERROR_INVALID_ARGUMENT, "Unknown device class");
  return TRUE;
}
GdkPaintable *gh_call_media_get_remote_video(GhCallMedia *self) { return GDK_PAINTABLE(self->remote); }
GdkPaintable *gh_call_media_get_local_preview(GhCallMedia *self) { return GDK_PAINTABLE(self->preview); }
static void dispose(GObject *object)
{
  GhCallMedia *self = GH_CALL_MEDIA(object);
  teardown(GH_CALL_MEDIA_BACKEND(self));
  g_clear_object(&self->settings); g_clear_object(&self->devices);
  g_clear_object(&self->input); g_clear_object(&self->output); g_clear_object(&self->camera_device);
  g_clear_object(&self->remote); g_clear_object(&self->preview);
  G_OBJECT_CLASS(gh_call_media_parent_class)->dispose(object);
}
static void finalize(GObject *object)
{
  GhCallMedia *self = GH_CALL_MEDIA(object);
  g_main_context_unref(self->context);
  g_free(self->stun); g_free(self->turn); g_free(self->username);
  g_clear_pointer(&self->video_preferences, gst_caps_unref);
  G_OBJECT_CLASS(gh_call_media_parent_class)->finalize(object);
}
static void gh_call_media_class_init(GhCallMediaClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = dispose; G_OBJECT_CLASS(klass)->finalize = finalize;
  g_signal_new("device-removed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
      NULL, NULL, NULL, G_TYPE_NONE, 1, GST_TYPE_DEVICE);
}
static void gh_call_media_init(GhCallMedia *self)
{
  gst_init(NULL, NULL);
  self->context = g_main_context_ref_thread_default();
  self->devices = g_list_store_new(GST_TYPE_DEVICE);
  self->preview = gh_call_paintable_new(self->context);
  self->remote = gh_call_paintable_new(self->context);
  self->camera = TRUE;
  self->video_payload = 97;
  self->audio_payload = 96;
}
GhCallMedia *gh_call_media_new(GSettings *settings)
{
  g_return_val_if_fail(G_IS_SETTINGS(settings), NULL);
  GhCallMedia *self = g_object_new(GH_TYPE_CALL_MEDIA, NULL);
  self->settings = g_object_ref(settings); return self;
}

/* Private device-free harness: production negotiation, codecs, transport and
 * receive path, replacing only physical capture/playback devices. */
GhCallMedia *gh_call_media_new_test(GSettings *settings)
{
  GhCallMedia *self = gh_call_media_new(settings);
  self->test_sources = TRUE; self->headset = TRUE; return self;
}
void gh_call_media_test_remove_device(GhCallMedia *self, GstDevice *device)
{
  GstMessage *message = gst_message_new_device_removed(GST_OBJECT(self->pipeline), device);
  device_message(NULL, message, self); gst_message_unref(message);
}
void gh_call_media_test_received(GhCallMedia *self, gint *audio, gint *video)
{
  *audio = g_atomic_int_get(&self->received_audio);
  *video = g_atomic_int_get(&self->received_video);
}
