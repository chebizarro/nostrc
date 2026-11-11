#include "gh-call-service.h"
#include <nostr-kinds.h>
#include <string.h>

struct _GhCall {
  GObject parent_instance;
  gchar *id, *peer, *remote_sdp;
  GhCallType type;
  GhCallDirection direction;
  GhCallState state;
  GhCallEndReason reason;
  GhCallMediaBackend *media;
  GCancellable *cancel;
  GQueue remote_ice, local_ice;
  gboolean remote_ready, negotiating, waiting_answer, reconnecting;
  int local_kind;
  gint64 deadline, connected_time;
};
G_DEFINE_TYPE(GhCall, gh_call, G_TYPE_OBJECT)
static void call_finalize(GObject *object)
{
  GhCall *c = GH_CALL(object);
  if (c->media) GH_CALL_MEDIA_BACKEND_GET_IFACE(c->media)->teardown(c->media);
  g_clear_object(&c->media); g_clear_object(&c->cancel);
  g_free(c->id); g_free(c->peer); g_free(c->remote_sdp);
  g_queue_clear_full(&c->remote_ice, g_free); g_queue_clear_full(&c->local_ice, g_free);
  G_OBJECT_CLASS(gh_call_parent_class)->finalize(object);
}
static void gh_call_init(GhCall *c) { c->cancel = g_cancellable_new(); }
static void gh_call_class_init(GhCallClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = call_finalize;
  g_signal_new("changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}
const gchar *gh_call_get_id(GhCall *c) { return c->id; }
const gchar *gh_call_get_peer(GhCall *c) { return c->peer; }
GhCallType gh_call_get_call_type(GhCall *c) { return c->type; }
GhCallDirection gh_call_get_direction(GhCall *c) { return c->direction; }
GhCallState gh_call_get_state(GhCall *c) { return c->state; }
GhCallEndReason gh_call_get_end_reason(GhCall *c) { return c->reason; }
GhCallMediaBackend *gh_call_get_media(GhCall *c) { return c->media; }
gboolean gh_call_get_reconnecting(GhCall *c) { return c->reconnecting; }
gint64 gh_call_get_connected_time(GhCall *c) { return c->connected_time; }

struct _GhCallService {
  GObject parent_instance;
  GhCallSignalling *signalling;
  GhConversationStore *conversations;
  GSettings *settings;
  GhNetSession *network;
  GhCallMediaFactory factory;
  gpointer factory_data;
  GDestroyNotify factory_destroy;
  GHashTable *calls, *seen;
  GQueue seen_order, call_order;
  GhCall *current;
  gchar *pubkey;
  guint timer;
  guint64 generation;
  gboolean stopped;
};
G_DEFINE_TYPE(GhCallService, gh_call_service, G_TYPE_OBJECT)

static void changed(GhCallService *self, GhCall *c)
{
  g_signal_emit_by_name(c, "changed"); g_signal_emit_by_name(self, "state-changed", c);
}
static void end_call(GhCallService *self, GhCall *c, GhCallEndReason reason)
{
  if (c->state == GH_CALL_ENDED) return;
  c->state = GH_CALL_ENDED; c->reason = reason; c->reconnecting = FALSE;
  g_cancellable_cancel(c->cancel);
  if (c->media) {
    g_signal_handlers_disconnect_by_data(c->media, self);
    GH_CALL_MEDIA_BACKEND_GET_IFACE(c->media)->teardown(c->media);
  }
  g_queue_clear_full(&c->remote_ice, g_free); g_queue_clear_full(&c->local_ice, g_free);
  if (self->signalling) gh_call_signalling_cancel_call(self->signalling, c->id);
  changed(self, c);
}
static gboolean send_event(GhCallService *self, GhCall *c, int kind, const gchar *content)
{
  g_signal_emit_by_name(self, "send", c->peer, c->id, kind, c->type, content);
  gboolean ok = !self->signalling || gh_call_signalling_send(self->signalling, c->peer,
                                                          c->id, kind, c->type, content);
  if (!ok && c->state != GH_CALL_ENDED) end_call(self, c, GH_CALL_END_SIGNALLING_FAILED);
  return ok;
}
static gboolean available(GhCallService *self, GError **error)
{
  if (self->stopped || !self->pubkey || !self->network) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                         "Calls are unavailable without an account"); return FALSE;
  }
  if (gh_net_session_get_mode(self->network) == GH_NET_MODE_TOR) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                         "Calls are unavailable in Tor mode"); return FALSE;
  }
  return TRUE;
}
static GhCall *new_call(GhCallService *self, const gchar *id, const gchar *peer,
                        GhCallType type, GhCallDirection direction, gint64 now)
{
  /* Retain ended call ids through the freshness window. Bound all memory:
   * overflow drops new offers instead of forgetting a still-replayable id. */
  while (self->call_order.length >= 128) {
    GhCall *old = g_queue_peek_head(&self->call_order);
    if (old->state != GH_CALL_ENDED || old->deadline + 120 >= now) return NULL;
    g_queue_pop_head(&self->call_order);
    if (self->current == old) self->current = NULL;
    g_hash_table_remove(self->calls, old->id);
  }
  GhCall *c = g_object_new(GH_TYPE_CALL, NULL);
  c->id = g_strdup(id); c->peer = g_strdup(peer); c->type = type; c->direction = direction;
  c->deadline = now + 60;
  g_hash_table_insert(self->calls, g_strdup(id), c); g_queue_push_tail(&self->call_order, c);
  return c;
}
static gboolean accepted(GhCallService *self, const gchar *peer)
{
  if (!self->conversations) return FALSE;
  GListModel *model = G_LIST_MODEL(self->conversations);
  for (guint i = 0; i < g_list_model_get_n_items(model); i++) {
    g_autoptr(GhConversation) c = g_list_model_get_item(model, i);
    const gchar *const *peers = gh_conversation_get_peers(c);
    if (gh_conversation_get_is_direct(c) &&
        !gh_conversation_get_is_request(c) && peers && peers[0] && !peers[1] && !strcmp(peer, peers[0])) return TRUE;
  }
  return FALSE;
}
static gboolean hidden(GhCallService *self, const gchar *peer)
{
#if GROUNDHOG_HAVE_MESSAGE_POLICY
  GhMessagePolicy *p = self->conversations ? gh_conversation_store_get_policy(self->conversations) : NULL;
  return p && (!gh_message_policy_is_loaded(p) || gh_message_policy_is_updating(p) ||
               gh_message_policy_hides_author(p, peer));
#else
  (void)self; (void)peer; return FALSE;
#endif
}

typedef struct { GhCallService *self; GhCall *call; guint64 generation; gboolean answer; } MediaOp;
static MediaOp *op_new(GhCallService *self, GhCall *c, gboolean answer)
{
  MediaOp *op = g_new0(MediaOp, 1); op->self = g_object_ref(self); op->call = g_object_ref(c);
  op->generation = self->generation; op->answer = answer; return op;
}
static gboolean op_current(MediaOp *op)
{ return !op->self->stopped && op->generation == op->self->generation && op->call->state != GH_CALL_ENDED; }
static void op_free(MediaOp *op) { g_object_unref(op->self); g_object_unref(op->call); g_free(op); }
static void create_local(GhCallService *self, GhCall *c, int kind);
static gboolean add_ice(GhCallService *self, GhCall *c, const gchar *json)
{
  g_autofree gchar *candidate = NULL, *mid = NULL; guint index;
  if (!gh_call_ice_parse(json, &candidate, &mid, &index)) return FALSE;
  if (!GH_CALL_MEDIA_BACKEND_GET_IFACE(c->media)->add_remote_ice(c->media, candidate, mid, index, NULL)) {
    end_call(self, c, GH_CALL_END_MEDIA_FAILED); return FALSE;
  }
  return TRUE;
}
static void remote_done(GObject *source, GAsyncResult *result, gpointer data)
{
  MediaOp *op = data; GhCall *c = op->call; GhCallService *self = op->self;
  gboolean ok = GH_CALL_MEDIA_BACKEND_GET_IFACE(source)->apply_remote_finish(GH_CALL_MEDIA_BACKEND(source), result, NULL);
  if (op_current(op)) {
    if (!ok) end_call(self, c, GH_CALL_END_MEDIA_FAILED);
    else {
      c->remote_ready = TRUE;
      while (!g_queue_is_empty(&c->remote_ice) && c->state != GH_CALL_ENDED) {
        g_autofree gchar *ice = g_queue_pop_head(&c->remote_ice); add_ice(self, c, ice);
      }
      if (c->state != GH_CALL_ENDED) {
        if (op->answer) create_local(self, c, NOSTR_KIND_CALL_ANSWER);
        else { c->negotiating = FALSE; c->waiting_answer = FALSE; changed(self, c); }
      }
    }
  }
  op_free(op);
}
static void apply_remote(GhCallService *self, GhCall *c, const gchar *sdp, gboolean offer)
{
  c->remote_ready = FALSE; c->negotiating = TRUE;
  GH_CALL_MEDIA_BACKEND_GET_IFACE(c->media)->apply_remote_async(c->media, sdp, offer,
                                                      c->cancel, remote_done, op_new(self, c, offer));
}
static void local_done(GObject *source, GAsyncResult *result, gpointer data)
{
  MediaOp *op = data; GhCall *c = op->call; GhCallService *self = op->self;
  g_autofree gchar *sdp = GH_CALL_MEDIA_BACKEND_GET_IFACE(source)->create_local_finish(GH_CALL_MEDIA_BACKEND(source), result, NULL);
  if (op_current(op)) {
    if (!sdp || !g_str_has_prefix(sdp, "v=0")) end_call(self, c, GH_CALL_END_MEDIA_FAILED);
    else {
      int kind = c->local_kind; c->local_kind = 0;
      if (send_event(self, c, kind, sdp)) {
        while (!g_queue_is_empty(&c->local_ice) && c->state != GH_CALL_ENDED) {
          g_autofree gchar *ice = g_queue_pop_head(&c->local_ice);
          send_event(self, c, NOSTR_KIND_CALL_ICE_CANDIDATE, ice);
        }
        if (kind == NOSTR_KIND_CALL_ANSWER) c->negotiating = FALSE;
      }
    }
  }
  op_free(op);
}
static void create_local(GhCallService *self, GhCall *c, int kind)
{
  c->local_kind = kind; c->negotiating = TRUE; c->waiting_answer = kind != NOSTR_KIND_CALL_ANSWER;
  GH_CALL_MEDIA_BACKEND_GET_IFACE(c->media)->create_local_async(c->media, c->type,
        kind == NOSTR_KIND_CALL_ANSWER, c->cancel, local_done, op_new(self, c, FALSE));
}
static void local_ice(GhCallMediaBackend *media, const gchar *candidate,
                       const gchar *mid, guint index, gpointer data)
{
  GhCallService *self = data; GhCall *c = self->current;
  if (!c || c->media != media || c->state == GH_CALL_ENDED) return;
  g_autofree gchar *json = gh_call_ice_json(candidate, mid, index);
  if (c->local_kind) {
    if (c->local_ice.length >= GH_CALL_MAX_PENDING) end_call(self, c, GH_CALL_END_MEDIA_FAILED);
    else g_queue_push_tail(&c->local_ice, g_steal_pointer(&json));
  } else send_event(self, c, NOSTR_KIND_CALL_ICE_CANDIDATE, json);
}
static void connection_state(GhCallMediaBackend *media, int state, gpointer data)
{
  GhCallService *self = data; GhCall *c = self->current;
  if (!c || c->media != media || c->state == GH_CALL_ENDED) return;
  if (state == GH_CALL_MEDIA_CONNECTED) {
    if (!c->connected_time) c->connected_time = g_get_monotonic_time();
    c->state = GH_CALL_ACTIVE; c->reconnecting = FALSE; changed(self, c);
  } else if (state == GH_CALL_MEDIA_DISCONNECTED && c->state == GH_CALL_ACTIVE) {
    /* ICE "disconnected" is often transient; FAILED or the bound ends it. */
    if (!c->reconnecting) {
      c->reconnecting = TRUE; c->deadline = g_get_real_time()/G_USEC_PER_SEC + GH_CALL_RECONNECT_SECONDS;
      changed(self, c);
    }
  } else if (state == GH_CALL_MEDIA_FAILED || state == GH_CALL_MEDIA_DISCONNECTED) {
    end_call(self, c, GH_CALL_END_MEDIA_FAILED); send_event(self, c, NOSTR_KIND_CALL_HANGUP, "failed");
  }
}
static gboolean start_media(GhCallService *self, GhCall *c, GError **error)
{
  if (!available(self, error)) return FALSE;
  if (!self->factory) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "No call media backend is installed"); return FALSE;
  }
  c->media = self->factory(self->factory_data, error);
  if (!c->media) return FALSE;
  GhCallMediaBackendInterface *iface = GH_CALL_MEDIA_BACKEND_GET_IFACE(c->media);
  if (!iface->configure || !iface->create_local_async || !iface->create_local_finish ||
      !iface->apply_remote_async || !iface->apply_remote_finish || !iface->add_remote_ice || !iface->teardown) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Incomplete call media backend");
    g_clear_object(&c->media); return FALSE;
  }
  if (!iface->configure(c->media, g_settings_get_boolean(self->settings, "allow-local-network"), error)) {
    iface->teardown(c->media); g_clear_object(&c->media); return FALSE;
  }
  g_signal_connect(c->media, "local-ice", G_CALLBACK(local_ice), self);
  g_signal_connect(c->media, "connection-state", G_CALLBACK(connection_state), self); return TRUE;
}

GhCall *gh_call_service_place(GhCallService *self, const gchar *peer, GhCallType type, GError **error)
{
  if (!available(self, error)) return NULL;
  if (self->current && self->current->state != GH_CALL_ENDED) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, "A call is already in progress"); return NULL;
  }
  if (!peer || strlen(peer) != 64 || !g_strcmp0(peer, self->pubkey) || hidden(self, peer)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid call recipient"); return NULL;
  }
  for (const gchar *p = peer; *p; p++) if (!g_ascii_isxdigit(*p) || g_ascii_isupper(*p)) return NULL;
  g_autofree gchar *id = g_uuid_string_random();
  GhCall *c = new_call(self, id, peer, type, GH_CALL_OUTBOUND, g_get_real_time()/G_USEC_PER_SEC);
  if (!c) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "Call history capacity reached"); return NULL; }
  self->current = c;
  if (!start_media(self, c, error)) { end_call(self, c, GH_CALL_END_MEDIA_FAILED); return NULL; }
  c->state = GH_CALL_OUTGOING; changed(self, c); create_local(self, c, NOSTR_KIND_CALL_OFFER); return c;
}
gboolean gh_call_service_answer(GhCallService *self, GError **error)
{
  GhCall *c = self->current;
  if (!c || c->state != GH_CALL_RINGING) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "No incoming call is ringing"); return FALSE;
  }
  if (hidden(self, c->peer)) {
    end_call(self, c, GH_CALL_END_PRIVACY);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "The caller is hidden by this account");
    return FALSE;
  }
  if (!start_media(self, c, error)) return FALSE;
  c->state = GH_CALL_CONNECTING; changed(self, c); apply_remote(self, c, c->remote_sdp, TRUE); return TRUE;
}
void gh_call_service_decline(GhCallService *self)
{
  GhCall *c = self->current;
  if (c && c->state == GH_CALL_RINGING) {
    end_call(self, c, GH_CALL_END_DECLINED); send_event(self, c, NOSTR_KIND_CALL_REJECT, "");
  }
}
void gh_call_service_hang_up(GhCallService *self)
{
  GhCall *c = self->current;
  if (c && c->state != GH_CALL_ENDED) {
    end_call(self, c, GH_CALL_END_HANGUP); send_event(self, c, NOSTR_KIND_CALL_HANGUP, "");
  }
}
gboolean gh_call_service_upgrade_video(GhCallService *self, GError **error)
{
  GhCall *c = self->current;
  if (!c || c->state != GH_CALL_ACTIVE || c->negotiating || c->type == GH_CALL_VIDEO) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, "Call cannot upgrade now"); return FALSE;
  }
  c->type = GH_CALL_VIDEO; changed(self, c); create_local(self, c, NOSTR_KIND_CALL_RENEGOTIATE); return TRUE;
}

void gh_call_service_receive(GhCallService *self, const gchar *json, gint64 now)
{
  if (!available(self, NULL)) return;
  NostrEvent *e = gh_call_event_parse(json, NULL, now, NULL); if (!e) return;
  const gchar *id = gh_call_event_tag(e, "call-id"), *p = gh_call_event_tag(e, "p");
  const gchar *author = nostr_event_get_pubkey(e), *content = nostr_event_get_content(e);
  int kind = nostr_event_get_kind(e); gboolean own = !strcmp(author, self->pubkey);
  g_autofree gchar *event_id = nostr_event_get_id(e);
  GhCall *c = g_hash_table_lookup(self->calls, id);
  if ((own && (!c || strcmp(p, c->peer))) || (!own && strcmp(p, self->pubkey))) goto out;
  if (g_hash_table_contains(self->seen, event_id)) goto out;
  if (self->seen_order.length >= 4096) {
    gchar *old = g_queue_pop_head(&self->seen_order); g_hash_table_remove(self->seen, old); g_free(old);
  }
  g_queue_push_tail(&self->seen_order, g_strdup(event_id)); g_hash_table_add(self->seen, g_strdup(event_id));
  if (c && c->state == GH_CALL_ENDED) goto out;
  if (own) {
    if (c && c->direction == GH_CALL_INCOMING && c->state == GH_CALL_RINGING &&
        (kind == NOSTR_KIND_CALL_ANSWER || kind == NOSTR_KIND_CALL_REJECT))
      end_call(self, c, GH_CALL_END_ELSEWHERE);
    goto out;
  }
  if (kind == NOSTR_KIND_CALL_OFFER) {
    if (c || hidden(self, author)) goto out;
    GhCallType type = !g_strcmp0(gh_call_event_tag(e, "call-type"), "video") ? GH_CALL_VIDEO : GH_CALL_VOICE;
    c = new_call(self, id, author, type, GH_CALL_INCOMING, now); if (!c) goto out;
    c->remote_sdp = g_strdup(content);
    if (!accepted(self, author) && !g_settings_get_boolean(self->settings, "allow-calls-from-unaccepted")) {
      end_call(self, c, GH_CALL_END_REQUEST); g_signal_emit_by_name(self, "missed-call", c); goto out;
    }
    if (self->current && self->current->state != GH_CALL_ENDED) {
      end_call(self, c, GH_CALL_END_BUSY); send_event(self, c, NOSTR_KIND_CALL_REJECT, "busy"); goto out;
    }
    self->current = c; c->state = GH_CALL_RINGING; changed(self, c);
    g_signal_emit_by_name(self, "incoming-ring", c); goto out;
  }
  if (!c || strcmp(c->peer, author) || self->current != c) goto out;
  switch (kind) {
    case NOSTR_KIND_CALL_ANSWER:
      if (c->media && c->waiting_answer) {
        if (c->state == GH_CALL_OUTGOING) c->state = GH_CALL_CONNECTING;
        c->waiting_answer = FALSE; changed(self, c); apply_remote(self, c, content, FALSE);
      }
      break;
    case NOSTR_KIND_CALL_ICE_CANDIDATE:
      if (c->remote_ready && c->media) add_ice(self, c, content);
      else if (c->remote_ice.length < GH_CALL_MAX_PENDING) g_queue_push_tail(&c->remote_ice, g_strdup(content));
      else end_call(self, c, GH_CALL_END_MEDIA_FAILED);
      break;
    case NOSTR_KIND_CALL_RENEGOTIATE:
      if (c->state == GH_CALL_ACTIVE && !c->negotiating) {
        if (strstr(content, "m=video")) c->type = GH_CALL_VIDEO;
        changed(self, c); apply_remote(self, c, content, TRUE);
      }
      break;
    case NOSTR_KIND_CALL_HANGUP: {
      gboolean missed = c->state == GH_CALL_RINGING;
      end_call(self, c, GH_CALL_END_HANGUP);
      if (missed) g_signal_emit_by_name(self, "missed-call", c);
      break;
    }
    case NOSTR_KIND_CALL_REJECT:
      if (c->state == GH_CALL_OUTGOING || c->state == GH_CALL_CONNECTING)
        end_call(self, c, !strcmp(content, "busy") ? GH_CALL_END_BUSY : GH_CALL_END_DECLINED);
      break;
    default: break;
  }
out:
  nostr_event_free(e);
}
void gh_call_service_expire(GhCallService *self, gint64 now)
{
  GhCall *c = self->current;
  if (c && (c->state == GH_CALL_RINGING || c->state == GH_CALL_OUTGOING || c->state == GH_CALL_CONNECTING) && now >= c->deadline) {
    gboolean incoming = c->direction == GH_CALL_INCOMING;
    end_call(self, c, GH_CALL_END_TIMEOUT);
    send_event(self, c, NOSTR_KIND_CALL_HANGUP, "timeout");
    if (incoming) g_signal_emit_by_name(self, "missed-call", c);
  } else if (c && c->state == GH_CALL_ACTIVE && c->reconnecting && now >= c->deadline) {
    end_call(self, c, GH_CALL_END_MEDIA_FAILED);
    send_event(self, c, NOSTR_KIND_CALL_HANGUP, "failed");
  }
}
static gboolean tick(gpointer data)
{ gh_call_service_expire(data, g_get_real_time()/G_USEC_PER_SEC); return G_SOURCE_CONTINUE; }
void gh_call_service_bind(GhCallService *self, const gchar *pubkey)
{
  self->generation++;
  if (self->current) end_call(self, self->current, GH_CALL_END_ACCOUNT_CHANGED);
  self->current = NULL;
  g_queue_clear(&self->call_order); g_hash_table_remove_all(self->calls);
  g_queue_clear_full(&self->seen_order, g_free); g_hash_table_remove_all(self->seen);
  g_free(self->pubkey); self->pubkey = g_strdup(pubkey);
  g_signal_emit_by_name(self, "availability-changed");
}
static void on_event(GhCallSignalling *s, const gchar *json, gpointer data)
{ (void)s; gh_call_service_receive(data, json, g_get_real_time()/G_USEC_PER_SEC); }
static void on_reset(GhCallSignalling *s, gpointer data)
{ gh_call_service_bind(data, gh_call_signalling_get_pubkey(s)); }
static void on_failure(GhCallSignalling *s, const gchar *id, gpointer data)
{
  (void)s; GhCallService *self = data;
  GhCall *c = g_hash_table_lookup(self->calls, id);
  if (c && c->state != GH_CALL_ENDED) end_call(self, c, GH_CALL_END_SIGNALLING_FAILED);
}
static void privacy_changed(GObject *source, gpointer data)
{
  (void)source; GhCallService *self = data; GhCall *c = self->current;
  g_signal_emit_by_name(self, "availability-changed");
  if (!c || c->state == GH_CALL_ENDED) return;
  if (!available(self, NULL) || hidden(self, c->peer)) end_call(self, c, GH_CALL_END_PRIVACY);
  else if (c->media && !GH_CALL_MEDIA_BACKEND_GET_IFACE(c->media)->configure(c->media,
                      g_settings_get_boolean(self->settings, "allow-local-network"), NULL))
    end_call(self, c, GH_CALL_END_PRIVACY);
}
static void settings_changed(GSettings *settings, const gchar *key, gpointer data)
{ (void)key; privacy_changed(G_OBJECT(settings), data); }
void gh_call_service_stop(GhCallService *self)
{
  if (self->stopped) return;
  self->stopped = TRUE; gh_call_service_bind(self, NULL);
  if (self->timer) { g_source_remove(self->timer); self->timer = 0; }
  if (self->signalling) { g_signal_handlers_disconnect_by_data(self->signalling, self); gh_call_signalling_stop(self->signalling); }
  if (self->settings) g_signal_handlers_disconnect_by_data(self->settings, self);
  if (self->network) g_signal_handlers_disconnect_by_data(self->network, self);
}
static void dispose(GObject *object)
{
  GhCallService *self = GH_CALL_SERVICE(object); gh_call_service_stop(self);
  g_clear_object(&self->signalling); g_clear_object(&self->conversations);
  g_clear_object(&self->settings); g_clear_object(&self->network);
  G_OBJECT_CLASS(gh_call_service_parent_class)->dispose(object);
}
static void finalize(GObject *object)
{
  GhCallService *self = GH_CALL_SERVICE(object);
  if (self->factory_destroy) self->factory_destroy(self->factory_data);
  g_hash_table_unref(self->calls); g_hash_table_unref(self->seen); g_free(self->pubkey);
  G_OBJECT_CLASS(gh_call_service_parent_class)->finalize(object);
}
static void gh_call_service_init(GhCallService *self)
{
  self->calls = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
  self->seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
}
static void gh_call_service_class_init(GhCallServiceClass *klass)
{
  GObjectClass *object = G_OBJECT_CLASS(klass); object->dispose = dispose; object->finalize = finalize;
  const gchar *signals[] = { "incoming-ring", "missed-call", "state-changed" };
  for (guint i = 0; i < G_N_ELEMENTS(signals); i++)
    g_signal_new(signals[i], G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, GH_TYPE_CALL);
  g_signal_new("availability-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  g_signal_new("send", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                G_TYPE_NONE, 5, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT, G_TYPE_INT, G_TYPE_STRING);
}
GhCallService *gh_call_service_new(GhCallSignalling *signalling, GhConversationStore *conversations,
                                  GSettings *settings, GhNetSession *network)
{
  GhCallService *self = g_object_new(GH_TYPE_CALL_SERVICE, NULL);
  if (conversations) self->conversations = g_object_ref(conversations);
  self->settings = g_object_ref(settings);
  if (network) { self->network = g_object_ref(network); g_signal_connect(network, "changed", G_CALLBACK(privacy_changed), self); }
  if (signalling) {
    self->signalling = g_object_ref(signalling);
    g_signal_connect(signalling, "event", G_CALLBACK(on_event), self);
    g_signal_connect(signalling, "reset", G_CALLBACK(on_reset), self);
    g_signal_connect(signalling, "failed", G_CALLBACK(on_failure), self);
    gh_call_service_bind(self, gh_call_signalling_get_pubkey(signalling));
  }
  g_signal_connect(settings, "changed", G_CALLBACK(settings_changed), self);
  self->timer = g_timeout_add_seconds(1, tick, self); return self;
}
void gh_call_service_set_media_factory(GhCallService *self, GhCallMediaFactory factory, gpointer data, GDestroyNotify destroy)
{
  g_return_if_fail(!self->current || self->current->state == GH_CALL_ENDED);
  if (self->factory_destroy) self->factory_destroy(self->factory_data);
  self->factory = factory; self->factory_data = data; self->factory_destroy = destroy;
  g_signal_emit_by_name(self, "availability-changed");
}
GhCall *gh_call_service_get_current(GhCallService *self) { return self->current; }
gboolean gh_call_service_check_available(GhCallService *self, GError **error)
{
  if (!available(self, error)) return FALSE;
  if (!self->factory) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "No call media backend is installed"); return FALSE;
  }
  return TRUE;
}
