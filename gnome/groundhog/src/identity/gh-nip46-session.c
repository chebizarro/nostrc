#include "gh-nip46-session-private.h"

#include <nostr-keys.h>
#include <nostr-event.h>
#include <nostr-filter.h>
#include <nostr-tag.h>
#include <nostr-utils.h>
#include <nostr/nip44/nip44.h>
#include <nostr/nip46/nip46_msg.h>
#include <nostr/nip46/nip46_envelope.h>
#include <nostr/nip46/nip46_uri.h>
#include <stdlib.h>
#include <string.h>

#define MAX_INTERACTIVE 64
#define MAX_BULK 256
#define MAX_RETAINED (8 * 1024 * 1024)
#define MAX_IN_FLIGHT 4
#define PUBLISH_SECONDS 30
#define APPROVAL_SECONDS 330
#define AUTH_URL_SECONDS 600
#define PERMISSIONS "get_public_key,sign_event,nip44_encrypt,nip44_decrypt,nip04_decrypt"

typedef struct _Pending Pending;
struct _Pending {
  GhNip46Session *session;
  gchar *id;
  gchar *json;
  gchar *auth_url;
  GTask *task;
  GCancellable *cancellable;
  gulong cancel_handler;
  GhRelayPublish *publish;
  guint timer;
  gint64 started_us;
  gboolean interactive;
  gboolean in_flight;
  gboolean accepted;
  gboolean auth_opened;
};

struct _GhNip46Session {
  GObject parent_instance;
  GMainContext *context;
  gchar secret[65];
  gchar *client_pubkey;
  gchar *remote_pubkey;
  gchar *pair_secret;
  gchar *bunker_secret;
  GPtrArray *relays;
  GHashTable *ready_urls;
  GHashTable *pending;
  GPtrArray *oneway_publishes;
  GQueue interactive;
  GQueue bulk;
  GhRelayScope *scope;
  GhRelayAuthSigner *auth_signer;
  GhRelayTransport scope_transport;
  GhRelayAuthTransport scope_auth;
  GhRelayPublishTransport publish_transport;
  GhRelayPublishAuthTransport publish_auth;
  gpointer transport_data;
  GTask *pair_task;
  GCancellable *pair_cancellable;
  gulong pair_cancel_handler;
  guint pair_timer;
  GhNip46AuthUrlFunc auth_url;
  gpointer auth_url_data;
  guint64 generation;
  guint in_flight;
  guint pace_source;
  guint publish_seconds;
  guint approval_seconds;
  guint auth_url_seconds;
  guint pair_seconds;
  gsize retained;
  gboolean started;
  gboolean cancelled;
  gboolean last_rpc_timed_out;
  gboolean bunker;
  gboolean bunker_connect_started;
};

G_DEFINE_TYPE(GhNip46Session, gh_nip46_session, G_TYPE_OBJECT)
G_DEFINE_QUARK(gh-nip46-session-error-quark, gh_nip46_session_error)

enum { SIGNAL_READY, SIGNAL_OFFLINE, N_SIGNALS };
static guint signals[N_SIGNALS];
static guint64 next_generation = 1;

static void pump(GhNip46Session *self);
static void finish_pending(Pending *p, gchar *value, GError *error);

static guint
attach_source(GhNip46Session *self, GSource *source, GSourceFunc callback,
              gpointer data, GDestroyNotify destroy)
{
  g_source_set_callback(source, callback, data, destroy);
  guint id = g_source_attach(source, self->context);
  g_source_unref(source);
  return id;
}

static void
remove_source(GhNip46Session *self, guint id)
{
  if (!id) return;
  GSource *source = g_main_context_find_source_by_id(self->context, id);
  if (source) g_source_destroy(source);
}

static void
wipe(void *memory, gsize length)
{
  volatile guint8 *p = memory;
  while (length--) *p++ = 0;
}

static void
wipe_free(gchar *text)
{
  if (text) { wipe(text, strlen(text)); g_free(text); }
}

static gboolean
hex64(const gchar *text)
{
  if (!text || strlen(text) != 64) return FALSE;
  for (const gchar *p = text; *p; p++)
    if (!g_ascii_isdigit(*p) && (*p < 'a' || *p > 'f')) return FALSE;
  return TRUE;
}

static gboolean
constant_equal(const gchar *a, const gchar *b)
{
  if (!a || !b) return FALSE;
  gsize na = strlen(a), nb = strlen(b);
  guint8 difference = (guint8)(na ^ nb);
  for (gsize i = 0; i < MAX(na, nb); i++)
    difference |= (guint8)((i < na ? a[i] : 0) ^ (i < nb ? b[i] : 0));
  return difference == 0;
}

static void
pending_free(Pending *p)
{
  remove_source(p->session, p->timer);
  if (p->cancel_handler) g_cancellable_disconnect(p->cancellable, p->cancel_handler);
  if (p->publish) { gh_relay_publish_cancel(p->publish); gh_relay_publish_unref(p->publish); }
  g_clear_object(&p->cancellable);
  g_clear_object(&p->task);
  g_free(p->id);
  wipe_free(p->json);
  g_free(p->auth_url);
  g_free(p);
}

static gboolean
pump_idle(gpointer data)
{
  GhNip46Session *self = data;
  pump(self);
  g_object_unref(self);
  return G_SOURCE_REMOVE;
}

static void
schedule_pump(GhNip46Session *self)
{
  if (!self->cancelled)
    attach_source(self, g_idle_source_new(), pump_idle, g_object_ref(self), NULL);
}

static void
finish_pending(Pending *p, gchar *value, GError *error)
{
  GhNip46Session *self = g_object_ref(p->session);
  gboolean emit_offline = FALSE, emit_ready = FALSE;
  if (p->in_flight) self->in_flight--;
  else g_queue_remove(p->interactive ? &self->interactive : &self->bulk, p);
  self->retained -= p->json ? strlen(p->json) : 0;
  if (error && error->domain == GH_NIP46_SESSION_ERROR &&
      error->code == GH_NIP46_SESSION_ERROR_TIMED_OUT) {
    emit_offline = !self->last_rpc_timed_out &&
                   g_hash_table_size(self->ready_urls) > 0;
    self->last_rpc_timed_out = TRUE;
  } else if (!error && self->last_rpc_timed_out) {
    self->last_rpc_timed_out = FALSE;
    emit_ready = g_hash_table_size(self->ready_urls) > 0;
  }
  g_hash_table_steal(self->pending, p->id);
  if (error) { g_task_return_error(p->task, error); g_free(value); }
  else g_task_return_pointer(p->task, value, g_free);
  pending_free(p);
  if (emit_offline) g_signal_emit(self, signals[SIGNAL_OFFLINE], 0);
  if (emit_ready && !self->cancelled) g_signal_emit(self, signals[SIGNAL_READY], 0);
  schedule_pump(self);
  g_object_unref(self);
}

static gboolean
pending_timed_out(gpointer data)
{
  Pending *p = data;
  p->timer = 0;
  finish_pending(p, NULL, g_error_new_literal(GH_NIP46_SESSION_ERROR,
    p->accepted ? GH_NIP46_SESSION_ERROR_TIMED_OUT : GH_NIP46_SESSION_ERROR_UNAVAILABLE,
    p->accepted ? "Remote signer did not answer" : "No signer relay accepted the request"));
  return G_SOURCE_REMOVE;
}

static void
set_timer(Pending *p, guint seconds)
{
  remove_source(p->session, p->timer);
  p->timer = attach_source(p->session, g_timeout_source_new_seconds(seconds),
                           pending_timed_out, p, NULL);
}

static gboolean
valid_auth_url(const gchar *url)
{
  if (!url || strlen(url) > 2048) return FALSE;
  g_autoptr(GError) error = NULL;
  g_autoptr(GUri) uri = g_uri_parse(url, G_URI_FLAGS_NONE, &error);
  return uri && g_strcmp0(g_uri_get_scheme(uri), "https") == 0 &&
    g_uri_get_host(uri) && *g_uri_get_host(uri) && !g_uri_get_userinfo(uri);
}

static GhNip46SessionError
classify_error(const gchar *text)
{
  g_autofree gchar *lower = g_ascii_strdown(text ? text : "", -1);
  if (strstr(lower, "unsupported") || strstr(lower, "unknown method") ||
      strstr(lower, "not implemented")) return GH_NIP46_SESSION_ERROR_INVALID_RESULT;
  if (strstr(lower, "login") || strstr(lower, "authoriz") || strstr(lower, "sign in"))
    return GH_NIP46_SESSION_ERROR_UNAVAILABLE;
  return GH_NIP46_SESSION_ERROR_DENIED;
}

static const gchar *
error_message(GhNip46SessionError code)
{
  switch (code) {
    case GH_NIP46_SESSION_ERROR_INVALID_RESULT: return "Your signer does not support this action";
    case GH_NIP46_SESSION_ERROR_UNAVAILABLE: return "Sign in to your signer and try again";
    default: return "Your signer denied this action";
  }
}

static gboolean
get_event(const gchar *json, NostrEvent **out)
{
  NostrEvent *event = nostr_event_new();
  if (!event) return FALSE;
  gchar canonical[65] = { 0 };
  if (nostr_event_deserialize_signed(event, json, NULL) != NOSTR_EVENT_VALIDATION_OK ||
      nostr_event_validate(event, canonical) != NOSTR_EVENT_VALIDATION_OK ||
      nostr_event_get_kind(event) != NOSTR_EVENT_KIND_NIP46) {
    nostr_event_free(event); return FALSE;
  }
  *out = event;
  return TRUE;
}

static gboolean
has_client_p(const GhNip46Session *self, NostrEvent *event)
{
  NostrTags *tags = nostr_event_get_tags(event);
  for (size_t i = 0; tags && i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (tag && nostr_tag_size(tag) == 2 &&
        g_strcmp0(nostr_tag_get(tag, 0), "p") == 0 &&
        g_strcmp0(nostr_tag_get(tag, 1), self->client_pubkey) == 0)
      return TRUE;
  }
  return FALSE;
}

static gchar *
encrypt_json(GhNip46Session *self, const gchar *peer, const gchar *plaintext)
{
  guint8 secret[32], pubkey[32];
  gchar *ciphertext = NULL;
  if (nostr_hex2bin(secret, self->secret, sizeof secret) &&
      nostr_hex2bin(pubkey, peer, sizeof pubkey))
    nostr_nip44_encrypt_v2(secret, pubkey, (const guint8 *)plaintext,
                            strlen(plaintext), &ciphertext);
  wipe(secret, sizeof secret);
  return ciphertext;
}

static gchar *
decrypt_json(GhNip46Session *self, const gchar *peer, const gchar *ciphertext)
{
  guint8 secret[32], pubkey[32], *plaintext = NULL;
  size_t length = 0;
  gchar *result = NULL;
  if (nostr_hex2bin(secret, self->secret, sizeof secret) &&
      nostr_hex2bin(pubkey, peer, sizeof pubkey) &&
      nostr_nip44_decrypt_v2(secret, pubkey, ciphertext, &plaintext, &length) == 0 &&
      length <= 1024 * 1024 && g_utf8_validate((const gchar *)plaintext, length, NULL))
    result = g_strndup((const gchar *)plaintext, length);
  wipe(secret, sizeof secret);
  if (plaintext) { wipe(plaintext, length); free(plaintext); }
  return result;
}

static gchar *
signed_envelope(GhNip46Session *self, const gchar *peer,
                const gchar *plaintext, gboolean response)
{
  char *ciphertext = encrypt_json(self, peer, plaintext);
  NostrEvent *event = NULL;
  gchar *json = NULL;
  if (ciphertext && (response ?
      nostr_nip46_build_response_event(self->client_pubkey, peer, ciphertext, &event) :
      nostr_nip46_build_request_event(self->client_pubkey, peer, ciphertext, &event)) == 0) {
    if (nostr_event_sign(event, self->secret) == 0)
      json = nostr_event_serialize_compact(event);
  }
  nostr_event_free(event);
  free(ciphertext);
  return json;
}

static gboolean
pace_done(gpointer data)
{
  GhNip46Session *self = data;
  self->pace_source = 0;
  pump(self);
  return G_SOURCE_REMOVE;
}

typedef struct { GhNip46Session *session; gchar *id; } PublishFailure;

static gboolean
publish_failure_idle(gpointer data)
{
  PublishFailure *failure = data;
  Pending *p = g_hash_table_lookup(failure->session->pending, failure->id);
  if (p && !p->accepted)
    finish_pending(p, NULL, g_error_new_literal(GH_NIP46_SESSION_ERROR,
      GH_NIP46_SESSION_ERROR_UNAVAILABLE, "No signer relay accepted the request"));
  g_object_unref(failure->session);
  g_free(failure->id);
  g_free(failure);
  return G_SOURCE_REMOVE;
}

static void
publish_done(GhRelayPublish *publish, const GhRelayPublishSummary *summary,
             gpointer data)
{
  Pending *p = data;
  GhNip46Session *self = p->session;
  (void)publish;
  if (g_hash_table_lookup(self->pending, p->id) != p) return;
  if (!summary->any_accepted) {
    /* The publisher must unwind before its owner can cancel/unref it. */
    PublishFailure *failure = g_new0(PublishFailure, 1);
    failure->session = g_object_ref(self);
    failure->id = g_strdup(p->id);
    attach_source(self, g_idle_source_new(), publish_failure_idle, failure, NULL);
    return;
  }
  p->accepted = TRUE;
  if (!p->auth_opened) set_timer(p, self->approval_seconds);
}

static void
publish_update(GhRelayPublish *publish, const GhRelayPublishResult *result,
               gpointer data)
{
  Pending *p = data;
  (void)publish;
  if (result->outcome == GH_RELAY_PUBLISH_ACCEPTED && !p->accepted) {
    p->accepted = TRUE;
    set_timer(p, p->session->approval_seconds);
  }
}

static void
pump(GhNip46Session *self)
{
  if (self->cancelled || g_hash_table_size(self->ready_urls) == 0 ||
      !self->remote_pubkey || self->pace_source) return;
  while (self->in_flight < MAX_IN_FLIGHT) {
    Pending *p = g_queue_pop_head(!g_queue_is_empty(&self->interactive) ?
                                    &self->interactive : &self->bulk);
    if (!p) return;
    gchar *event = signed_envelope(self, self->remote_pubkey, p->json, FALSE);
    GError *error = NULL;
    GhRelayPublish *publish = event ?
      (self->publish_transport.open ? gh_relay_publish_new_with_transport(
         self->generation, event, &self->publish_transport, self->transport_data,
         publish_update, publish_done, p, &error) :
       gh_relay_publish_new(self->generation, event, publish_update, publish_done, p, &error)) : NULL;
    free(event);
    if (!publish) {
      g_clear_error(&error);
      finish_pending(p, NULL, g_error_new_literal(GH_NIP46_SESSION_ERROR,
        GH_NIP46_SESSION_ERROR_UNAVAILABLE, "Could not prepare signer request"));
      continue;
    }
    if (self->publish_auth.send_auth)
      gh_relay_publish_set_auth_transport(publish, &self->publish_auth);
    gh_relay_publish_set_account_signer(publish, self->auth_signer, NULL);
    for (guint i = 0; i < self->relays->len; i++) {
      const gchar *url = g_ptr_array_index(self->relays, i);
      gh_relay_publish_add_url(publish, url, NULL);
      gh_relay_publish_set_url_auth(publish, url, GH_RELAY_AUTH_ACCOUNT, NULL);
    }
    gh_relay_publish_set_deadline(publish, self->publish_seconds);
    p->publish = publish;
    p->in_flight = TRUE;
    self->in_flight++;
    set_timer(p, self->publish_seconds + 1);
    if (!gh_relay_publish_start(publish, &error)) {
      g_clear_error(&error);
      finish_pending(p, NULL, g_error_new_literal(GH_NIP46_SESSION_ERROR,
        GH_NIP46_SESSION_ERROR_UNAVAILABLE, "No signer relay accepted the request"));
    }
    self->pace_source = attach_source(self, g_timeout_source_new(150),
                                      pace_done, self, NULL);
    return;
  }
}

static void
pair_cleanup(GhNip46Session *self)
{
  if (self->pair_timer) { remove_source(self, self->pair_timer); self->pair_timer = 0; }
  if (self->pair_cancel_handler) {
    g_cancellable_disconnect(self->pair_cancellable, self->pair_cancel_handler);
    self->pair_cancel_handler = 0;
  }
  g_clear_object(&self->pair_cancellable);
}

static void
pair_public_key_done(GObject *source, GAsyncResult *result, gpointer data)
{
  GhNip46Session *self = GH_NIP46_SESSION(source);
  (void)data;
  GError *error = NULL;
  gchar *pubkey = gh_nip46_session_call_finish(self, result, &error);
  if (!self->pair_task) { g_free(pubkey); g_clear_error(&error); return; }
  GTask *task = g_steal_pointer(&self->pair_task);
  pair_cleanup(self);
  if (error) g_task_return_error(task, error);
  else if (!hex64(pubkey)) {
    g_free(pubkey);
    g_task_return_new_error(task, GH_NIP46_SESSION_ERROR,
      GH_NIP46_SESSION_ERROR_INVALID_RESULT, "Signer returned an invalid account key");
  } else g_task_return_pointer(task, pubkey, g_free);
  g_object_unref(task);
}

static void
pair_connect_done(GObject *source, GAsyncResult *result, gpointer data)
{
  GhNip46Session *self = GH_NIP46_SESSION(source);
  (void)data;
  GError *error = NULL;
  g_autofree gchar *reply = gh_nip46_session_call_finish(self, result, &error);
  if (!self->pair_task) { g_clear_error(&error); return; }
  if (error || g_strcmp0(reply, "ack") != 0) {
    GTask *task = g_steal_pointer(&self->pair_task);
    pair_cleanup(self);
    if (error) g_task_return_error(task, error);
    else g_task_return_new_error(task, GH_NIP46_SESSION_ERROR,
      GH_NIP46_SESSION_ERROR_INVALID_RESULT, "Signer did not acknowledge connect");
    g_object_unref(task);
    return;
  }
  wipe_free(g_steal_pointer(&self->bunker_secret));
  gh_nip46_session_call_async(self, "get_public_key", NULL, 0,
                               g_task_get_cancellable(self->pair_task),
                               pair_public_key_done, NULL);
}

static void
begin_bunker_connect(GhNip46Session *self)
{
  if (!self->bunker || !self->pair_task || self->bunker_connect_started ||
      !gh_nip46_session_is_ready(self)) return;
  self->bunker_connect_started = TRUE;
  const gchar *params[] = { self->remote_pubkey,
                            self->bunker_secret ? self->bunker_secret : "",
                            PERMISSIONS };
  gh_nip46_session_call_async(self, "connect", params, G_N_ELEMENTS(params),
                               g_task_get_cancellable(self->pair_task),
                               pair_connect_done, NULL);
}

static void
send_oneway(GhNip46Session *self, const gchar *peer, const gchar *plaintext)
{
  gchar *event = signed_envelope(self, peer, plaintext, TRUE);
  if (!event) return;
  GhRelayPublish *publish = self->publish_transport.open ?
    gh_relay_publish_new_with_transport(self->generation, event,
      &self->publish_transport, self->transport_data, NULL, NULL, NULL, NULL) :
    gh_relay_publish_new(self->generation, event, NULL, NULL, NULL, NULL);
  free(event);
  if (!publish) return;
  if (self->publish_auth.send_auth)
    gh_relay_publish_set_auth_transport(publish, &self->publish_auth);
  gh_relay_publish_set_account_signer(publish, self->auth_signer, NULL);
  for (guint i = 0; i < self->relays->len; i++) {
    const gchar *url = g_ptr_array_index(self->relays, i);
    gh_relay_publish_add_url(publish, url, NULL);
    gh_relay_publish_set_url_auth(publish, url, GH_RELAY_AUTH_ACCOUNT, NULL);
  }
  gh_relay_publish_set_deadline(publish, self->publish_seconds);
  g_ptr_array_add(self->oneway_publishes, publish);
  gh_relay_publish_start(publish, NULL);
}

static void
handle_pair_response(GhNip46Session *self, const gchar *author, const gchar *json)
{
  if (!self->pair_task || !self->pair_secret || self->remote_pubkey) return;
  NostrNip46Response response = { 0 };
  gboolean connect_request = FALSE;
  gchar *connect_id = NULL;
  gboolean match = FALSE;
  if (nostr_nip46_response_parse(json, &response) == 0) {
    match = constant_equal(response.result, self->pair_secret);
    nostr_nip46_response_free(&response);
  } else {
    NostrNip46Request request = { 0 };
    if (nostr_nip46_request_parse(json, &request) == 0) {
      if (g_strcmp0(request.method, "connect") == 0) {
        for (size_t i = 0; i < request.n_params; i++)
          match |= constant_equal(request.params[i], self->pair_secret);
        if (match) {
          connect_request = TRUE;
          connect_id = g_strdup(request.id);
        }
      }
      nostr_nip46_request_free(&request);
    }
  }
  if (!match) return;
  self->remote_pubkey = g_strdup(author);
  wipe_free(g_steal_pointer(&self->pair_secret));
  if (connect_request) {
    char *ack = nostr_nip46_response_build_ok(connect_id, "\"ack\"");
    if (ack) { send_oneway(self, author, ack); free(ack); }
  }
  g_free(connect_id);
  gh_nip46_session_call_async(self, "get_public_key", NULL, 0,
                               g_task_get_cancellable(self->pair_task),
                               pair_public_key_done, NULL);
}

static void
scope_update_inner(GhRelayScope *scope, const GhRelayUpdate *update, gpointer data)
{
  GhNip46Session *self = data;
  (void)scope;
  if (self->cancelled) return;
  if (update->notice == GH_RELAY_NOTICE_EOSE) {
    gboolean first = g_hash_table_size(self->ready_urls) == 0;
    g_hash_table_add(self->ready_urls, g_strdup(update->url));
    if (first) {
      if (!self->last_rpc_timed_out)
        g_signal_emit(self, signals[SIGNAL_READY], 0);
      begin_bunker_connect(self);
    }
    schedule_pump(self);
    return;
  }
  if (update->notice == GH_RELAY_NOTICE_DISCONNECTED ||
      update->notice == GH_RELAY_NOTICE_CLOSED ||
      update->notice == GH_RELAY_NOTICE_ERROR) {
    g_hash_table_remove(self->ready_urls, update->url);
    if (g_hash_table_size(self->ready_urls) == 0)
      g_signal_emit(self, signals[SIGNAL_OFFLINE], 0);
    return;
  }
  if (update->notice != GH_RELAY_NOTICE_EVENT || !update->event_json) return;
  NostrEvent *event = NULL;
  if (!get_event(update->event_json, &event)) return;
  g_autofree gchar *author = g_strdup(nostr_event_get_pubkey(event));
  if (!has_client_p(self, event) || !hex64(author) ||
      (self->remote_pubkey && g_strcmp0(self->remote_pubkey, author) != 0)) {
    nostr_event_free(event); return;
  }
  g_autofree gchar *plaintext = decrypt_json(self, author, nostr_event_get_content(event));
  nostr_event_free(event);
  if (!plaintext) return;
  if (!self->remote_pubkey) { handle_pair_response(self, author, plaintext); return; }
  NostrNip46Response response = { 0 };
  if (nostr_nip46_response_parse(plaintext, &response) != 0) return;
  Pending *p = g_hash_table_lookup(self->pending, response.id);
  if (!p) { nostr_nip46_response_free(&response); return; }
  if (g_strcmp0(response.result, "auth_url") == 0) {
    if (!valid_auth_url(response.error) ||
        (p->auth_url && g_strcmp0(p->auth_url, response.error) != 0)) {
      finish_pending(p, NULL, g_error_new_literal(GH_NIP46_SESSION_ERROR,
        GH_NIP46_SESSION_ERROR_INVALID_RESULT, "Signer supplied an invalid approval URL"));
    } else {
      if (!p->auth_opened) {
        g_autofree gchar *id = g_strdup(p->id);
        p->auth_url = g_strdup(response.error);
        p->auth_opened = TRUE;
        gboolean launched = self->auth_url &&
          self->auth_url(self, response.error, self->auth_url_data);
        p = g_hash_table_lookup(self->pending, id);
        if (!p) { nostr_nip46_response_free(&response); return; }
        if (!launched) {
          finish_pending(p, NULL, g_error_new_literal(GH_NIP46_SESSION_ERROR,
            GH_NIP46_SESSION_ERROR_UNAVAILABLE, "Could not open signer approval page"));
          nostr_nip46_response_free(&response);
          return;
        }
      }
      gint64 remaining = p->started_us + (gint64)self->auth_url_seconds * G_USEC_PER_SEC -
                         g_get_monotonic_time();
      if (remaining <= 0)
        finish_pending(p, NULL, g_error_new_literal(GH_NIP46_SESSION_ERROR,
          GH_NIP46_SESSION_ERROR_TIMED_OUT, "Signer approval timed out"));
      else
        set_timer(p, (guint)((remaining + G_USEC_PER_SEC - 1) / G_USEC_PER_SEC));
    }
  } else if (response.error && *response.error) {
    GhNip46SessionError code = classify_error(response.error);
    finish_pending(p, NULL, g_error_new_literal(GH_NIP46_SESSION_ERROR,
      code, error_message(code)));
  } else if (response.result) {
    finish_pending(p, g_strdup(response.result), NULL);
  } else {
    finish_pending(p, NULL, g_error_new_literal(GH_NIP46_SESSION_ERROR,
      GH_NIP46_SESSION_ERROR_INVALID_RESULT, "Signer returned no result"));
  }
  nostr_nip46_response_free(&response);
}

static void
scope_update(GhRelayScope *scope, const GhRelayUpdate *update, gpointer data)
{
  GhNip46Session *self = g_object_ref(data);
  scope_update_inner(scope, update, self);
  g_object_unref(self);
}

static void
sign_auth_async(gpointer data, const gchar *unsigned_json, GCancellable *cancellable,
                GAsyncReadyCallback callback, gpointer callback_data)
{
  GhNip46Session *self = data;
  GTask *task = g_task_new(self, cancellable, callback, callback_data);
  NostrEvent *event = nostr_event_new();
  gchar *json = NULL;
  if (!self->cancelled && event &&
      nostr_event_deserialize_compact(event, unsigned_json, NULL) == 1 &&
      nostr_event_sign(event, self->secret) == 0)
    json = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  if (json) g_task_return_pointer(task, json, free);
  else g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Could not authenticate signer relay");
  g_object_unref(task);
}

static gchar *
sign_auth_finish(GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer(G_TASK(result), error);
}

static void
gh_nip46_session_dispose(GObject *object)
{
  GhNip46Session *self = GH_NIP46_SESSION(object);
  gh_nip46_session_cancel(self);
  G_OBJECT_CLASS(gh_nip46_session_parent_class)->dispose(object);
}

static void
gh_nip46_session_finalize(GObject *object)
{
  GhNip46Session *self = GH_NIP46_SESSION(object);
  wipe(self->secret, sizeof self->secret);
  g_free(self->client_pubkey);
  g_free(self->remote_pubkey);
  wipe_free(self->pair_secret);
  wipe_free(self->bunker_secret);
  g_ptr_array_unref(self->relays);
  g_ptr_array_unref(self->oneway_publishes);
  g_main_context_unref(self->context);
  g_hash_table_unref(self->ready_urls);
  g_hash_table_unref(self->pending);
  G_OBJECT_CLASS(gh_nip46_session_parent_class)->finalize(object);
}

static void
gh_nip46_session_class_init(GhNip46SessionClass *klass)
{
  GObjectClass *objects = G_OBJECT_CLASS(klass);
  objects->dispose = gh_nip46_session_dispose;
  objects->finalize = gh_nip46_session_finalize;
  signals[SIGNAL_READY] = g_signal_new("ready", G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIGNAL_OFFLINE] = g_signal_new("offline", G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
gh_nip46_session_init(GhNip46Session *self)
{
  self->context = g_main_context_ref_thread_default();
  self->generation = next_generation++;
  self->publish_seconds = PUBLISH_SECONDS;
  self->approval_seconds = APPROVAL_SECONDS;
  self->auth_url_seconds = AUTH_URL_SECONDS;
  self->pair_seconds = 300;
  self->relays = g_ptr_array_new_with_free_func(g_free);
  self->ready_urls = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->pending = g_hash_table_new(g_str_hash, g_str_equal);
  self->oneway_publishes = g_ptr_array_new_with_free_func((GDestroyNotify)gh_relay_publish_unref);
  g_queue_init(&self->interactive);
  g_queue_init(&self->bulk);
}

void
gh_nip46_session_set_test_deadlines(GhNip46Session *self,
                                        guint publish_seconds,
                                        guint approval_seconds,
                                        guint auth_url_seconds,
                                        guint pair_seconds)
{
  g_return_if_fail(GH_IS_NIP46_SESSION(self));
  g_return_if_fail(!self->started && !self->pair_task &&
                   g_hash_table_size(self->pending) == 0);
  g_return_if_fail(publish_seconds && approval_seconds &&
                   auth_url_seconds && pair_seconds);
  self->publish_seconds = publish_seconds;
  self->approval_seconds = approval_seconds;
  self->auth_url_seconds = auth_url_seconds;
  self->pair_seconds = pair_seconds;
}

GhNip46Session *
gh_nip46_session_new(const gchar *secret, const gchar *remote,
                     const gchar *const *relays,
                     const GhRelayTransport *scope_transport,
                     const GhRelayAuthTransport *scope_auth,
                     const GhRelayPublishTransport *publish_transport,
                     const GhRelayPublishAuthTransport *publish_auth,
                     gpointer transport_data, GError **error)
{
  if (!hex64(secret) || (remote && !hex64(remote)) || !relays || !relays[0]) {
    g_set_error_literal(error, GH_NIP46_SESSION_ERROR,
      GH_NIP46_SESSION_ERROR_INVALID_INPUT, "Invalid signer key or relay list");
    return NULL;
  }
  GhNip46Session *self = g_object_new(GH_TYPE_NIP46_SESSION, NULL);
  g_strlcpy(self->secret, secret, sizeof self->secret);
  self->client_pubkey = nostr_key_get_public(secret);
  self->remote_pubkey = g_strdup(remote);
  if (!self->client_pubkey) goto invalid;
  for (guint i = 0; relays[i]; i++) {
    if (i == 4 || !gh_relay_url_validate(relays[i], error)) goto invalid;
    if (!g_ptr_array_find_with_equal_func(self->relays, relays[i],
                                           (GEqualFunc)g_str_equal, NULL))
      g_ptr_array_add(self->relays, g_strdup(relays[i]));
  }
  if (scope_transport) self->scope_transport = *scope_transport;
  if (scope_auth) self->scope_auth = *scope_auth;
  if (publish_transport) self->publish_transport = *publish_transport;
  if (publish_auth) self->publish_auth = *publish_auth;
  self->transport_data = transport_data;
  return self;
invalid:
  if (error && !*error)
    g_set_error_literal(error, GH_NIP46_SESSION_ERROR,
      GH_NIP46_SESSION_ERROR_INVALID_INPUT, "Invalid signer key or relay list");
  g_object_unref(self);
  return NULL;
}

GhNip46Session *
gh_nip46_session_new_qr(const gchar *const *relays,
                        const GhRelayTransport *scope_transport,
                        const GhRelayAuthTransport *scope_auth,
                        const GhRelayPublishTransport *publish_transport,
                        const GhRelayPublishAuthTransport *publish_auth,
                        gpointer transport_data, gchar **out_uri, GError **error)
{
  g_return_val_if_fail(out_uri != NULL, NULL);
  *out_uri = NULL;
  char *key = nostr_key_generate_private();
  GhNip46Session *self = key ? gh_nip46_session_new(key, NULL, relays,
    scope_transport, scope_auth, publish_transport, publish_auth, transport_data, error) : NULL;
  if (key) { wipe(key, strlen(key)); free(key); }
  if (!self) return NULL;
  char *token_key = nostr_key_generate_private();
  if (!token_key) {
    g_set_error_literal(error, GH_NIP46_SESSION_ERROR,
      GH_NIP46_SESSION_ERROR_UNAVAILABLE, "Could not create pairing secret");
    g_object_unref(self); return NULL;
  }
  self->pair_secret = g_strndup(token_key, 32);
  wipe(token_key, strlen(token_key));
  free(token_key);
  NostrNip46ConnectURI uri = { .client_pubkey_hex = self->client_pubkey,
    .relays = (char **)self->relays->pdata, .n_relays = self->relays->len,
    .secret = self->pair_secret, .perms_csv = PERMISSIONS, .name = "Groundhog" };
  char *built = NULL;
  if (nostr_nip46_uri_build_connect(&uri, &built) != 0) {
    g_set_error_literal(error, GH_NIP46_SESSION_ERROR,
      GH_NIP46_SESSION_ERROR_UNAVAILABLE, "Could not build pairing link");
    g_object_unref(self); return NULL;
  }
  *out_uri = g_strdup(built);
  free(built);
  return self;
}

GhNip46Session *
gh_nip46_session_new_bunker(const gchar *bunker_uri,
                        const GhRelayTransport *scope_transport,
                        const GhRelayAuthTransport *scope_auth,
                        const GhRelayPublishTransport *publish_transport,
                        const GhRelayPublishAuthTransport *publish_auth,
                        gpointer transport_data, GError **error)
{
  NostrNip46BunkerURI uri = { 0 };
  if (!bunker_uri || nostr_nip46_uri_parse_bunker(bunker_uri, &uri) != 0 ||
      uri.n_relays == 0) {
    nostr_nip46_uri_bunker_free(&uri);
    g_set_error_literal(error, GH_NIP46_SESSION_ERROR,
      GH_NIP46_SESSION_ERROR_INVALID_INPUT, "Invalid bunker link");
    return NULL;
  }
  g_autofree gchar **relays = g_new0(gchar *, uri.n_relays + 1);
  for (gsize i = 0; i < uri.n_relays; i++) relays[i] = uri.relays[i];
  char *key = nostr_key_generate_private();
  GhNip46Session *self = key ? gh_nip46_session_new(key, uri.remote_signer_pubkey_hex,
    (const gchar *const *)relays, scope_transport, scope_auth,
    publish_transport, publish_auth, transport_data, error) : NULL;
  if (key) { wipe(key, strlen(key)); free(key); }
  if (self) { self->bunker = TRUE; self->bunker_secret = g_strdup(uri.secret); }
  nostr_nip46_uri_bunker_free(&uri);
  return self;
}

const gchar *gh_nip46_session_get_client_pubkey(GhNip46Session *self)
{ return self->client_pubkey; }
gchar *gh_nip46_session_dup_client_secret(GhNip46Session *self)
{ return self->cancelled ? NULL : g_strdup(self->secret); }
const gchar *gh_nip46_session_get_remote_pubkey(GhNip46Session *self)
{ return self->remote_pubkey; }
gboolean gh_nip46_session_is_ready(GhNip46Session *self)
{ return !self->cancelled && !self->last_rpc_timed_out &&
         g_hash_table_size(self->ready_urls) > 0; }

void
gh_nip46_session_start(GhNip46Session *self)
{
  g_return_if_fail(GH_IS_NIP46_SESSION(self));
  if (self->started || self->cancelled) return;
  self->started = TRUE;
  NostrFilters *filters = nostr_filters_new();
  NostrFilter *filter = nostr_filter_new();
  const int kinds[] = { NOSTR_EVENT_KIND_NIP46 };
  nostr_filter_set_kinds(filter, kinds, 1);
  nostr_filter_tags_append(filter, "p", self->client_pubkey, NULL);
  nostr_filter_set_since_i64(filter, g_get_real_time() / G_USEC_PER_SEC - 60);
  nostr_filters_add(filters, filter);
  nostr_filter_free(filter); /* contents moved into the vector */
  self->scope = self->scope_transport.open ? gh_relay_scope_new_with_transport(
    self->generation, filters, &self->scope_transport, self->transport_data,
    scope_update, self) : gh_relay_scope_new(self->generation, filters, scope_update, self);
  if (self->scope_auth.send_auth) gh_relay_scope_set_auth_transport(self->scope, &self->scope_auth);
  self->auth_signer = gh_relay_auth_signer_new(self->generation, NULL,
    self->client_pubkey, sign_auth_async, sign_auth_finish, self, NULL);
  gh_relay_scope_set_account_signer(self->scope, self->auth_signer, NULL);
  for (guint i = 0; i < self->relays->len; i++) {
    const gchar *url = g_ptr_array_index(self->relays, i);
    gh_relay_scope_add_url(self->scope, url, NULL);
    gh_relay_scope_set_url_auth(self->scope, url, GH_RELAY_AUTH_ACCOUNT, NULL);
  }
  gh_relay_scope_start(self->scope);
}

void
gh_nip46_session_cancel(GhNip46Session *self)
{
  g_return_if_fail(GH_IS_NIP46_SESSION(self));
  if (self->cancelled) return;
  self->cancelled = TRUE;
  if (self->pace_source) { remove_source(self, self->pace_source); self->pace_source = 0; }
  if (self->auth_signer) gh_relay_auth_signer_revoke(self->auth_signer);
  for (guint i = 0; i < self->oneway_publishes->len; i++)
    gh_relay_publish_cancel(g_ptr_array_index(self->oneway_publishes, i));
  g_ptr_array_set_size(self->oneway_publishes, 0);
  if (self->scope) { gh_relay_scope_cancel(self->scope); gh_relay_scope_unref(self->scope); self->scope = NULL; }
  while (g_hash_table_size(self->pending)) {
    GHashTableIter iter; gpointer key, value;
    g_hash_table_iter_init(&iter, self->pending);
    g_hash_table_iter_next(&iter, &key, &value);
    finish_pending(value, NULL, g_error_new_literal(GH_NIP46_SESSION_ERROR,
      GH_NIP46_SESSION_ERROR_CANCELLED, "Signer session was cancelled"));
  }
  if (self->pair_task) {
    GTask *task = g_steal_pointer(&self->pair_task);
    pair_cleanup(self);
    g_task_return_new_error(task, GH_NIP46_SESSION_ERROR,
      GH_NIP46_SESSION_ERROR_CANCELLED, "Pairing was cancelled");
    g_object_unref(task);
  }
  pair_cleanup(self);
  g_clear_pointer(&self->auth_signer, gh_relay_auth_signer_unref);
  wipe_free(g_steal_pointer(&self->pair_secret));
  wipe_free(g_steal_pointer(&self->bunker_secret));
  wipe(self->secret, sizeof self->secret);
}

void
gh_nip46_session_set_auth_url_handler(GhNip46Session *self,
                                            GhNip46AuthUrlFunc callback,
                                            gpointer user_data)
{ self->auth_url = callback; self->auth_url_data = user_data; }

static gboolean
interactive_method(const gchar *method)
{
  return g_str_equal(method, "connect") || g_str_equal(method, "sign_event") ||
    g_str_equal(method, "get_public_key") || g_str_equal(method, "ping");
}

typedef struct { GhNip46Session *session; gchar *id; } CancelRequest;

static gboolean
cancel_idle(gpointer data)
{
  CancelRequest *request = data;
  Pending *p = g_hash_table_lookup(request->session->pending, request->id);
  if (p) finish_pending(p, NULL, g_error_new_literal(GH_NIP46_SESSION_ERROR,
    GH_NIP46_SESSION_ERROR_CANCELLED, "Signer request was cancelled"));
  g_object_unref(request->session);
  g_free(request->id);
  g_free(request);
  return G_SOURCE_REMOVE;
}

static void
pending_cancelled(GCancellable *cancellable, gpointer data)
{
  Pending *p = data;
  (void)cancellable;
  CancelRequest *request = g_new0(CancelRequest, 1);
  request->session = g_object_ref(p->session);
  request->id = g_strdup(p->id);
  attach_source(p->session, g_idle_source_new(), cancel_idle, request, NULL);
}

void
gh_nip46_session_call_async(GhNip46Session *self, const gchar *method,
                                 const gchar *const *params, gsize n_params,
                                 GCancellable *cancellable,
                                 GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail(GH_IS_NIP46_SESSION(self));
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_check_cancellable(task, FALSE);
  if (self->cancelled || !self->remote_pubkey) {
    g_task_return_new_error(task, GH_NIP46_SESSION_ERROR,
      GH_NIP46_SESSION_ERROR_UNAVAILABLE, "Signer session is unavailable");
    g_object_unref(task); return;
  }
  if (!method || !*method || n_params > 16) {
    g_task_return_new_error(task, GH_NIP46_SESSION_ERROR,
      GH_NIP46_SESSION_ERROR_INVALID_INPUT, "Invalid signer request");
    g_object_unref(task); return;
  }
  gboolean interactive = interactive_method(method);
  if (g_queue_get_length(interactive ? &self->interactive : &self->bulk) >=
      (interactive ? MAX_INTERACTIVE : MAX_BULK)) {
    g_task_return_new_error(task, GH_NIP46_SESSION_ERROR,
      GH_NIP46_SESSION_ERROR_UNAVAILABLE, "Your signer is busy");
    g_object_unref(task); return;
  }
  char *id = nostr_nip46_request_id_generate();
  char *json = id ? nostr_nip46_request_build(id, method, params, n_params) : NULL;
  if (!json || strlen(json) > MAX_RETAINED - self->retained) {
    free(id); free(json);
    g_task_return_new_error(task, GH_NIP46_SESSION_ERROR,
      GH_NIP46_SESSION_ERROR_UNAVAILABLE, "Your signer is busy");
    g_object_unref(task); return;
  }
  Pending *p = g_new0(Pending, 1);
  p->session = self;
  p->id = g_strdup(id);
  p->json = g_strdup(json);
  p->task = task;
  p->interactive = interactive;
  p->started_us = g_get_monotonic_time();
  free(id); free(json);
  self->retained += strlen(p->json);
  g_hash_table_insert(self->pending, p->id, p);
  g_queue_push_tail(interactive ? &self->interactive : &self->bulk, p);
  set_timer(p, self->approval_seconds);
  if (cancellable) {
    p->cancellable = g_object_ref(cancellable);
    p->cancel_handler = g_cancellable_connect(cancellable,
      G_CALLBACK(pending_cancelled), p, NULL);
  }
  schedule_pump(self);
}

gchar *
gh_nip46_session_call_finish(GhNip46Session *self, GAsyncResult *result,
                                    GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, self), NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

static gboolean
pair_timeout(gpointer data)
{
  GhNip46Session *self = data;
  self->pair_timer = 0;
  if (self->pair_task) {
    GTask *task = g_steal_pointer(&self->pair_task);
    pair_cleanup(self);
    g_task_return_new_error(task, GH_NIP46_SESSION_ERROR,
      GH_NIP46_SESSION_ERROR_TIMED_OUT, "Signer pairing timed out");
    g_object_unref(task);
  }
  gh_nip46_session_cancel(self);
  return G_SOURCE_REMOVE;
}

static gboolean
pair_cancel_idle(gpointer data)
{
  gh_nip46_session_cancel(data);
  return G_SOURCE_REMOVE;
}

static void
pair_cancelled(GCancellable *cancellable, gpointer data)
{
  (void)cancellable;
  GhNip46Session *self = data;
  attach_source(self, g_idle_source_new(), pair_cancel_idle,
                g_object_ref(self), g_object_unref);
}

void
gh_nip46_session_pair_async(GhNip46Session *self, GCancellable *cancellable,
                                 GAsyncReadyCallback callback, gpointer user_data)
{
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_check_cancellable(task, FALSE);
  if (self->cancelled || self->pair_task || (!self->bunker && !self->pair_secret)) {
    g_task_return_new_error(task, GH_NIP46_SESSION_ERROR,
      GH_NIP46_SESSION_ERROR_INVALID_INPUT, "Pairing attempt is unavailable");
    g_object_unref(task); return;
  }
  self->pair_task = task;
  self->pair_timer = attach_source(self,
    g_timeout_source_new_seconds(self->pair_seconds), pair_timeout, self, NULL);
  if (cancellable) {
    self->pair_cancellable = g_object_ref(cancellable);
    self->pair_cancel_handler = g_cancellable_connect(cancellable,
      G_CALLBACK(pair_cancelled), self, NULL);
  }
  gh_nip46_session_start(self);
  if (self->bunker && gh_nip46_session_is_ready(self)) begin_bunker_connect(self);
}

gchar *
gh_nip46_session_pair_finish(GhNip46Session *self, GAsyncResult *result,
                                    GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, self), NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}
