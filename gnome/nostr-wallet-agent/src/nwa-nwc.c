/* nwa-nwc.c - see nwa-nwc.h
 *
 * SPDX-License-Identifier: MIT
 */
#include "nwa-nwc.h"
#include "nwa-error.h"

#include <nostr/nip47/nwc.h>
#include <nostr/nip47/nwc_client.h>
#include <nostr-event.h>
#include <nostr-tag.h>
#include <nostr-keys.h>
#include <json.h>

#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SUB_INFO "nwa-info"
#define SUB_RSP  "nwa-rsp"
#define SEEN_MAX 256

enum { SIGNAL_NOTIFICATION, SIGNAL_INFO_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

typedef struct _Pending Pending;

struct _NwaNwcClient {
  GObject parent_instance;

  gchar  *wallet_pk;
  gchar  *secret;      /* client secret key (hex) — wiped on finalize */
  gchar  *client_pk;
  gchar  *lud16;
  gchar **relays;

  NwaTransportFactory factory;
  gpointer            factory_data;
  GPtrArray          *transports;  /* NostrPublishTransport* (owned) */

  gboolean started;
  gboolean info_ready;
  gint64   info_created_at;
  NostrNwcEncryption enc;
  gchar  **methods;
  guint    info_grace_id;

  guint request_timeout_s;
  guint info_grace_ms;

  GHashTable *pending;   /* event id -> Pending* (built + dispatched) */
  GQueue     *queued;    /* Pending* waiting for info_ready */
  GHashTable *seen;      /* notification ids */
  GQueue     *seen_order;
};

struct _Pending {
  NwaNwcClient *client;       /* borrowed; pendings die before the client */
  GTask        *task;
  gchar        *method;
  gchar        *params_json;
  gchar        *event_id;
  gchar        *event_json;
  NostrNwcEncryption enc;
  guint         timeout_id;
  GPtrArray    *sent_to;      /* transports (borrowed) that took the EVENT */
  guint         rejected;
  gchar        *reject_reason;
  guint         auth_retries;
};

G_DEFINE_TYPE(NwaNwcClient, nwa_nwc_client, G_TYPE_OBJECT)

static void
wipe_free(gchar *s)
{
  if (!s) return;
  volatile gchar *p = s;
  while (*p) *p++ = 0;
  g_free(s);
}

static const gchar *
enc_label(NostrNwcEncryption e)
{
  return e == NOSTR_NWC_ENC_NIP44_V2 ? "nip44_v2" : "nip04";
}

/* ---- pending bookkeeping ---- */

static void
pending_free(Pending *p)
{
  if (p->timeout_id) g_source_remove(p->timeout_id);
  g_clear_object(&p->task);
  g_free(p->method);
  wipe_free(p->params_json);
  g_free(p->event_id);
  g_free(p->event_json);
  g_ptr_array_unref(p->sent_to);
  g_free(p->reject_reason);
  g_free(p);
}

static void
pending_detach(Pending *p)
{
  NwaNwcClient *self = p->client;
  if (p->event_id)
    g_hash_table_remove(self->pending, p->event_id);
  else
    g_queue_remove(self->queued, p);
}

static void
pending_return_node(Pending *p, JsonNode *node)
{
  pending_detach(p);
  g_task_return_pointer(p->task, node, (GDestroyNotify)json_node_unref);
  pending_free(p);
}

static void
pending_return_error(Pending *p, GError *error)
{
  pending_detach(p);
  g_task_return_error(p->task, error);
  pending_free(p);
}

gboolean
nwa_nwc_error_is_definite(const GError *error)
{
  if (!error) return FALSE;
  if (error->domain != NWA_ERROR) return FALSE;
  return error->code != NWA_ERROR_TIMEOUT;
}

static gboolean
on_pending_timeout(gpointer data)
{
  Pending *p = data;
  p->timeout_id = 0;
  if (p->sent_to->len == 0) {
    pending_return_error(p, g_error_new(NWA_ERROR, NWA_ERROR_RELAY,
                                        "could not reach any wallet relay for %s", p->method));
  } else {
    pending_return_error(p, g_error_new(NWA_ERROR, NWA_ERROR_TIMEOUT,
                                        "wallet did not answer %s in time (outcome unknown)",
                                        p->method));
  }
  return G_SOURCE_REMOVE;
}

/* ---- event building ---- */

static gchar *
build_signed_event(NwaNwcClient *self, int kind, const gchar *content, NostrTags *tags,
                   gchar **out_id)
{
  NostrEvent *ev = nostr_event_new();
  if (!ev) { nostr_tags_free(tags); return NULL; }
  nostr_event_set_kind(ev, kind);
  nostr_event_set_created_at(ev, (int64_t)time(NULL));
  nostr_event_set_content(ev, content);
  nostr_event_set_tags(ev, tags);
  gchar *json = NULL;
  if (nostr_event_sign(ev, self->secret) == 0) {
    char *id = nostr_event_get_id(ev);
    char *j = nostr_event_serialize_compact(ev);
    if (id && j) {
      json = g_strdup(j);
      if (out_id) *out_id = g_strdup(id);
    }
    free(id);
    free(j);
  }
  nostr_event_free(ev);
  return json;
}

static gboolean
encrypt_for_wallet(NwaNwcClient *self, NostrNwcEncryption enc, const gchar *plain, gchar **out)
{
  NostrNwcClientSession s = { 0 };
  s.wallet_pub_hex = self->wallet_pk;
  s.enc = enc;
  char *c = NULL;
  if (nostr_nwc_client_encrypt(&s, self->secret, self->wallet_pk, plain, &c) != 0 || !c)
    return FALSE;
  *out = g_strdup(c);
  free(c);
  return TRUE;
}

static gboolean
decrypt_from_wallet(NwaNwcClient *self, NostrNwcEncryption enc, const gchar *cipher, gchar **out)
{
  NostrNwcClientSession s = { 0 };
  s.wallet_pub_hex = self->wallet_pk;
  s.enc = enc;
  char *pl = NULL;
  if (nostr_nwc_client_decrypt(&s, self->secret, self->wallet_pk, cipher, &pl) != 0 || !pl)
    return FALSE;
  *out = g_strdup(pl);
  memset(pl, 0, strlen(pl));
  free(pl);
  return TRUE;
}

static gboolean
pending_build(NwaNwcClient *self, Pending *p, GError **error)
{
  p->enc = self->enc;
  g_autofree gchar *plain = g_strdup_printf("{\"method\":\"%s\",\"params\":%s}",
                                            p->method, p->params_json);
  gchar *cipher = NULL;
  gboolean ok = encrypt_for_wallet(self, p->enc, plain, &cipher);
  memset(plain, 0, strlen(plain));
  if (!ok) {
    g_set_error(error, NWA_ERROR, NWA_ERROR_FAILED, "%s encryption failed", enc_label(p->enc));
    return FALSE;
  }
  g_autofree gchar *expiration = g_strdup_printf("%" G_GINT64_FORMAT,
                                                 (gint64)time(NULL) + self->request_timeout_s);
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("p", self->wallet_pk, NULL));
  nostr_tags_append(tags, nostr_tag_new("encryption", enc_label(p->enc), NULL));
  /* NIP-47: wallets must not execute a request after its expiration, so a
   * request we gave up on cannot be paid later by a slow relay. */
  nostr_tags_append(tags, nostr_tag_new("expiration", expiration, NULL));
  p->event_json = build_signed_event(self, 23194, cipher, tags, &p->event_id);
  g_free(cipher);
  if (!p->event_json) {
    g_set_error_literal(error, NWA_ERROR, NWA_ERROR_FAILED, "could not sign NWC request");
    return FALSE;
  }
  return TRUE;
}

static void
pending_send_to(Pending *p, NostrPublishTransport *t)
{
  if (!nostr_publish_transport_is_connected(t)) return;
  for (guint i = 0; i < p->sent_to->len; i++)
    if (g_ptr_array_index(p->sent_to, i) == t) return;
  g_autofree gchar *frame = g_strdup_printf("[\"EVENT\",%s]", p->event_json);
  GError *err = NULL;
  if (nostr_publish_transport_send_frame(t, frame, &err))
    g_ptr_array_add(p->sent_to, t);
  else {
    g_debug("nwc: send to %s failed: %s", nostr_publish_transport_get_url(t),
            err ? err->message : "?");
    g_clear_error(&err);
  }
}

static void
pending_dispatch(NwaNwcClient *self, Pending *p)
{
  GError *err = NULL;
  if (!pending_build(self, p, &err)) {
    pending_return_error(p, err);
    return;
  }
  g_hash_table_insert(self->pending, g_strdup(p->event_id), p);
  for (guint i = 0; i < self->transports->len; i++)
    pending_send_to(p, g_ptr_array_index(self->transports, i));
}

static void
set_info_ready(NwaNwcClient *self)
{
  if (self->info_grace_id) {
    g_source_remove(self->info_grace_id);
    self->info_grace_id = 0;
  }
  if (self->info_ready) return;
  self->info_ready = TRUE;
  Pending *p;
  while ((p = g_queue_pop_head(self->queued)))
    pending_dispatch(self, p);
}

static gboolean
on_info_grace(gpointer data)
{
  NwaNwcClient *self = data;
  self->info_grace_id = 0;
  if (!self->info_ready)
    g_debug("nwc: no wallet info event yet; assuming %s", enc_label(self->enc));
  set_info_ready(self);
  return G_SOURCE_REMOVE;
}

/* ---- inbound ---- */

static const gchar *
find_tag(NostrEvent *ev, const gchar *key)
{
  NostrTags *tags = nostr_event_get_tags(ev);
  if (!tags) return NULL;
  for (size_t i = 0; i < nostr_tags_size(tags); i++) {
    NostrTag *t = nostr_tags_get(tags, i);
    if (t && nostr_tag_size(t) >= 2 && g_strcmp0(nostr_tag_get_key(t), key) == 0)
      return nostr_tag_get_value(t);
  }
  return NULL;
}

static void
handle_info(NwaNwcClient *self, NostrEvent *ev)
{
  gint64 created = nostr_event_get_created_at(ev);
  if (created < self->info_created_at) return;
  self->info_created_at = created;

  const gchar *content = nostr_event_get_content(ev);
  g_strfreev(self->methods);
  self->methods = g_strsplit_set(content ? content : "", " \t\n,", -1);
  /* drop empty entries */
  GPtrArray *m = g_ptr_array_new();
  for (guint i = 0; self->methods[i]; i++)
    if (*self->methods[i]) g_ptr_array_add(m, g_strdup(self->methods[i]));
  g_ptr_array_add(m, NULL);
  g_strfreev(self->methods);
  self->methods = (gchar **)g_ptr_array_free(m, FALSE);

  const gchar *enc = find_tag(ev, "encryption");
  NostrNwcEncryption chosen = NOSTR_NWC_ENC_NIP04; /* absent tag => nip04 only */
  if (enc) {
    g_auto(GStrv) schemes = g_strsplit_set(enc, " ,", -1);
    for (guint i = 0; schemes[i]; i++)
      if (g_str_equal(schemes[i], "nip44_v2") || g_str_equal(schemes[i], "nip44-v2"))
        chosen = NOSTR_NWC_ENC_NIP44_V2;
  }
  self->enc = chosen;
  g_signal_emit(self, signals[SIGNAL_INFO_CHANGED], 0);
  set_info_ready(self);
}

static void
handle_response(NwaNwcClient *self, NostrEvent *ev)
{
  const gchar *req_id = find_tag(ev, "e");
  if (!req_id) return;
  Pending *p = g_hash_table_lookup(self->pending, req_id);
  if (!p) return; /* not ours, or a duplicate from another relay */
  const gchar *ptag = find_tag(ev, "p");
  if (ptag && g_strcmp0(ptag, self->client_pk) != 0) return;

  /* NIP-47: the response uses the request's scheme. No fallback, so a
   * NIP-44 exchange can never be downgraded to NIP-04. */
  const gchar *cipher = nostr_event_get_content(ev);
  gchar *plain = NULL;
  if (!cipher || !decrypt_from_wallet(self, p->enc, cipher, &plain)) {
    g_debug("nwc: cannot decrypt %s response to %s", enc_label(p->enc), req_id);
    return; /* keep waiting; a forged/garbled copy must not fail the request */
  }

  g_autoptr(JsonParser) parser = json_parser_new();
  gboolean parsed = json_parser_load_from_data(parser, plain, -1, NULL);
  wipe_free(plain);
  JsonNode *root = parsed ? json_parser_get_root(parser) : NULL;
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
    pending_return_error(p, g_error_new(NWA_ERROR, NWA_ERROR_TIMEOUT,
                                        "wallet sent an unreadable %s response (outcome unknown)",
                                        p->method));
    return;
  }
  JsonObject *o = json_node_get_object(root);
  JsonNode *errn = json_object_get_member(o, "error");
  if (errn && JSON_NODE_HOLDS_OBJECT(errn)) {
    JsonObject *eo = json_node_get_object(errn);
    const gchar *code = json_object_get_string_member_with_default(eo, "code", "OTHER");
    const gchar *msg = json_object_get_string_member_with_default(eo, "message", "");
    pending_return_error(p, g_error_new(NWA_ERROR, NWA_ERROR_WALLET, "[%s] %s", code, msg));
    return;
  }
  JsonNode *res = json_object_get_member(o, "result");
  const gchar *rtype = json_object_get_string_member_with_default(o, "result_type", NULL);
  if (rtype && g_strcmp0(rtype, p->method) != 0)
    g_debug("nwc: result_type %s for %s request", rtype, p->method);
  pending_return_node(p, res ? json_node_copy(res) : json_node_new(JSON_NODE_NULL));
}

static void
handle_notification(NwaNwcClient *self, NostrEvent *ev, const gchar *id)
{
  if (g_hash_table_contains(self->seen, id)) return;
  gchar *key = g_strdup(id);
  g_hash_table_add(self->seen, key);
  g_queue_push_tail(self->seen_order, key);
  while (g_queue_get_length(self->seen_order) > SEEN_MAX)
    g_hash_table_remove(self->seen, g_queue_pop_head(self->seen_order));

  NostrNwcEncryption enc = nostr_event_get_kind(ev) == 23197 ? NOSTR_NWC_ENC_NIP44_V2
                                                             : NOSTR_NWC_ENC_NIP04;
  gchar *plain = NULL;
  const gchar *cipher = nostr_event_get_content(ev);
  if (!cipher || !decrypt_from_wallet(self, enc, cipher, &plain)) return;
  g_autoptr(JsonParser) parser = json_parser_new();
  gboolean parsed = json_parser_load_from_data(parser, plain, -1, NULL);
  wipe_free(plain);
  JsonNode *root = parsed ? json_parser_get_root(parser) : NULL;
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) return;
  JsonObject *o = json_node_get_object(root);
  const gchar *type = json_object_get_string_member_with_default(o, "notification_type", NULL);
  JsonNode *n = json_object_get_member(o, "notification");
  if (!type || !n || !JSON_NODE_HOLDS_OBJECT(n)) return;
  g_autofree gchar *nj = json_to_string(n, FALSE);
  g_signal_emit(self, signals[SIGNAL_NOTIFICATION], 0, type, nj);
}

static void
handle_event_json(NwaNwcClient *self, const gchar *event_json)
{
  NostrEvent *ev = nostr_event_new();
  if (!ev) return;
  if (nostr_event_deserialize(ev, event_json) != 0 ||
      !nostr_event_check_signature(ev) ||
      g_strcmp0(nostr_event_get_pubkey(ev), self->wallet_pk) != 0) {
    nostr_event_free(ev);
    return; /* only authentic wallet events are considered */
  }
  switch (nostr_event_get_kind(ev)) {
    case 13194: handle_info(self, ev); break;
    case 23195: handle_response(self, ev); break;
    case 23196:
    case 23197: {
      char *id = nostr_event_get_id(ev);
      if (id) handle_notification(self, ev, id);
      free(id);
      break;
    }
    default: break;
  }
  nostr_event_free(ev);
}

static void
send_subscriptions(NwaNwcClient *self, NostrPublishTransport *t)
{
  g_autofree gchar *info = g_strdup_printf(
    "[\"REQ\",\"" SUB_INFO "\",{\"kinds\":[13194],\"authors\":[\"%s\"],\"limit\":1}]",
    self->wallet_pk);
  g_autofree gchar *rsp = g_strdup_printf(
    "[\"REQ\",\"" SUB_RSP "\",{\"kinds\":[23195,23196,23197],\"authors\":[\"%s\"],"
    "\"#p\":[\"%s\"],\"since\":%" G_GINT64_FORMAT "}]",
    self->wallet_pk, self->client_pk, (gint64)time(NULL) - 60);
  (void)nostr_publish_transport_send_frame(t, info, NULL);
  (void)nostr_publish_transport_send_frame(t, rsp, NULL);
}

typedef struct {
  NwaNwcClient *self;           /* weak */
  NostrPublishTransport *t;     /* ref */
  gchar *event_id;              /* NULL = re-subscribe */
} Retry;

static void
retry_free(gpointer data)
{
  Retry *r = data;
  if (r->self) g_object_remove_weak_pointer(G_OBJECT(r->self), (gpointer *)&r->self);
  nostr_publish_transport_unref(r->t);
  g_free(r->event_id);
  g_free(r);
}

static gboolean
on_retry(gpointer data)
{
  Retry *r = data;
  if (!r->self || !r->self->started) return G_SOURCE_REMOVE;
  if (!r->event_id) {
    send_subscriptions(r->self, r->t);
  } else {
    Pending *p = g_hash_table_lookup(r->self->pending, r->event_id);
    if (p) pending_send_to(p, r->t);
  }
  return G_SOURCE_REMOVE;
}

static void
schedule_retry(NwaNwcClient *self, NostrPublishTransport *t, const gchar *event_id)
{
  Retry *r = g_new0(Retry, 1);
  r->self = self;
  g_object_add_weak_pointer(G_OBJECT(self), (gpointer *)&r->self);
  r->t = nostr_publish_transport_ref(t);
  r->event_id = g_strdup(event_id);
  g_timeout_add_full(G_PRIORITY_DEFAULT, 1000, on_retry, r, retry_free);
}

static void
on_frame(NostrPublishTransport *t, const gchar *kind_hint, const gchar *envelope, gpointer data)
{
  NwaNwcClient *self = data;
  if (!kind_hint) return;
  if (g_str_equal(kind_hint, "EVENT") || g_str_equal(kind_hint, "EOSE") ||
      g_str_equal(kind_hint, "CLOSED")) {
    g_autoptr(JsonParser) parser = json_parser_new();
    if (!json_parser_load_from_data(parser, envelope, -1, NULL)) return;
    JsonNode *root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_ARRAY(root)) return;
    JsonArray *a = json_node_get_array(root);
    guint len = json_array_get_length(a);
    if (len < 2) return;
    const gchar *sub = json_node_get_string(json_array_get_element(a, 1));

    if (g_str_equal(kind_hint, "EVENT") && len >= 3) {
      JsonNode *evn = json_array_get_element(a, 2);
      if (!JSON_NODE_HOLDS_OBJECT(evn)) return;
      g_autofree gchar *ej = json_to_string(evn, FALSE);
      handle_event_json(self, ej);
    } else if (g_str_equal(kind_hint, "EOSE")) {
      if (g_strcmp0(sub, SUB_INFO) == 0 && !self->info_ready)
        set_info_ready(self); /* stored info absent on this relay: assume nip04 */
    } else if (g_str_equal(kind_hint, "CLOSED")) {
      const gchar *reason = len >= 3 ? json_node_get_string(json_array_get_element(a, 2)) : NULL;
      if (reason && g_str_has_prefix(reason, "auth-required"))
        schedule_retry(self, t, NULL);
      else
        g_debug("nwc: %s closed %s: %s", nostr_publish_transport_get_url(t),
                sub ? sub : "?", reason ? reason : "");
    }
  }
}

static void
on_ok(NostrPublishTransport *t, const gchar *event_id, gboolean accepted,
      const gchar *reason, gpointer data)
{
  NwaNwcClient *self = data;
  Pending *p = g_hash_table_lookup(self->pending, event_id);
  if (!p || accepted) return;
  if (reason && g_str_has_prefix(reason, "auth-required") && p->auth_retries < 1) {
    p->auth_retries++;
    g_ptr_array_remove(p->sent_to, t);
    schedule_retry(self, t, event_id);
    return;
  }
  p->rejected++;
  g_free(p->reject_reason);
  p->reject_reason = g_strdup(reason ? reason : "rejected");
  if (p->rejected >= self->transports->len) {
    pending_return_error(p, g_error_new(NWA_ERROR, NWA_ERROR_RELAY,
                                        "relay refused the %s request: %s",
                                        p->method, p->reject_reason));
  }
}

static gchar *
on_auth(NostrPublishTransport *t, const gchar *challenge, gpointer data)
{
  NwaNwcClient *self = data;
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("relay", nostr_publish_transport_get_url(t), NULL));
  nostr_tags_append(tags, nostr_tag_new("challenge", challenge, NULL));
  return build_signed_event(self, 22242, "", tags, NULL);
}

static void
on_state(NostrPublishTransport *t, gboolean connected, const GError *error, gpointer data)
{
  NwaNwcClient *self = data;
  if (!connected) {
    g_debug("nwc: %s disconnected%s%s", nostr_publish_transport_get_url(t),
            error ? ": " : "", error ? error->message : "");
    return;
  }
  send_subscriptions(self, t);
  GHashTableIter it;
  gpointer k, v;
  g_hash_table_iter_init(&it, self->pending);
  while (g_hash_table_iter_next(&it, &k, &v))
    pending_send_to(v, t);
}

/* ---- public API ---- */

NwaNwcClient *
nwa_nwc_client_new(const gchar *nwc_uri, NwaTransportFactory factory, gpointer factory_data,
                   GError **error)
{
  NostrNwcConnection c = { 0 };
  if (!nwc_uri || nostr_nwc_uri_parse(nwc_uri, &c) != 0) {
    g_set_error_literal(error, NWA_ERROR, NWA_ERROR_INVALID_ARGS, "invalid nostr+walletconnect URI");
    return NULL;
  }
  if (!c.relays || !c.relays[0]) {
    nostr_nwc_connection_clear(&c);
    g_set_error_literal(error, NWA_ERROR, NWA_ERROR_INVALID_ARGS, "pairing has no relay");
    return NULL;
  }
  char *pk = nostr_key_get_public(c.secret_hex);
  if (!pk) {
    nostr_nwc_connection_clear(&c);
    g_set_error_literal(error, NWA_ERROR, NWA_ERROR_INVALID_ARGS, "invalid pairing secret");
    return NULL;
  }
  NwaNwcClient *self = g_object_new(NWA_TYPE_NWC_CLIENT, NULL);
  self->wallet_pk = g_ascii_strdown(c.wallet_pubkey_hex, -1);
  self->secret = g_strdup(c.secret_hex);
  self->client_pk = g_strdup(pk);
  free(pk);
  self->lud16 = g_strdup(c.lud16);
  self->relays = g_strdupv(c.relays);
  nostr_nwc_connection_clear(&c);
  self->factory = factory;
  self->factory_data = factory_data;
  return self;
}

void
nwa_nwc_client_set_timeouts(NwaNwcClient *self, guint request_timeout_s, guint info_grace_ms)
{
  if (request_timeout_s) self->request_timeout_s = request_timeout_s;
  self->info_grace_ms = info_grace_ms;
}

void
nwa_nwc_client_start(NwaNwcClient *self)
{
  if (self->started) return;
  self->started = TRUE;
  for (guint i = 0; self->relays[i]; i++) {
    NostrPublishTransport *t = self->factory
      ? self->factory(self->relays[i], self->factory_data)
      : nostr_publish_transport_new_websocket(self->relays[i]);
    if (!t) continue;
    nostr_publish_transport_set_listener(t, on_frame, self);
    nostr_publish_transport_set_state_callback(t, on_state, self);
    nostr_publish_transport_set_ok_callback(t, on_ok, self);
    nostr_publish_transport_set_auth_callback(t, on_auth, self);
    g_ptr_array_add(self->transports, t);
  }
  if (self->info_grace_ms)
    self->info_grace_id = g_timeout_add(self->info_grace_ms, on_info_grace, self);
  /* connect after registration so a synchronous connect sees every transport */
  for (guint i = 0; i < self->transports->len; i++)
    nostr_publish_transport_connect_async(g_ptr_array_index(self->transports, i));
}

static void
fail_all(NwaNwcClient *self, const gchar *why)
{
  Pending *p;
  while ((p = g_queue_peek_head(self->queued)))
    pending_return_error(p, g_error_new(NWA_ERROR, NWA_ERROR_FAILED, "%s", why));
  GList *vals = g_hash_table_get_values(self->pending);
  for (GList *l = vals; l; l = l->next) {
    p = l->data;
    pending_return_error(p, g_error_new(NWA_ERROR, NWA_ERROR_TIMEOUT,
                                        "%s before the wallet answered %s (outcome unknown)",
                                        why, p->method));
  }
  g_list_free(vals);
}

void
nwa_nwc_client_stop(NwaNwcClient *self)
{
  if (!self->started) return;
  self->started = FALSE;
  if (self->info_grace_id) {
    g_source_remove(self->info_grace_id);
    self->info_grace_id = 0;
  }
  fail_all(self, "wallet connection closed");
  for (guint i = 0; i < self->transports->len; i++) {
    NostrPublishTransport *t = g_ptr_array_index(self->transports, i);
    nostr_publish_transport_set_listener(t, NULL, NULL);
    nostr_publish_transport_set_state_callback(t, NULL, NULL);
    nostr_publish_transport_set_ok_callback(t, NULL, NULL);
    nostr_publish_transport_set_auth_callback(t, NULL, NULL);
    nostr_publish_transport_disconnect(t);
  }
  g_ptr_array_set_size(self->transports, 0);
}

const gchar *nwa_nwc_client_get_wallet_pubkey(NwaNwcClient *self) { return self->wallet_pk; }
const gchar *nwa_nwc_client_get_client_pubkey(NwaNwcClient *self) { return self->client_pk; }
const gchar *nwa_nwc_client_get_lud16(NwaNwcClient *self) { return self->lud16; }
const gchar *const *nwa_nwc_client_get_relays(NwaNwcClient *self) { return (const gchar *const *)self->relays; }
const gchar *nwa_nwc_client_get_encryption(NwaNwcClient *self) { return enc_label(self->enc); }
const gchar *const *nwa_nwc_client_get_methods(NwaNwcClient *self) { return (const gchar *const *)self->methods; }

gboolean
nwa_nwc_client_supports(NwaNwcClient *self, const gchar *method)
{
  if (!self->methods) return TRUE; /* unknown: let the wallet answer NOT_IMPLEMENTED */
  return g_strv_contains((const gchar *const *)self->methods, method);
}

void
nwa_nwc_client_request_async(NwaNwcClient *self, const gchar *method, JsonObject *params,
                             GCancellable *cancellable,
                             GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail(NWA_IS_NWC_CLIENT(self));
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, nwa_nwc_client_request_async);
  if (!self->started) {
    g_task_return_new_error(task, NWA_ERROR, NWA_ERROR_NOT_PAIRED, "wallet connection not started");
    g_object_unref(task);
    return;
  }
  Pending *p = g_new0(Pending, 1);
  p->client = self;
  p->task = task;
  p->method = g_strdup(method);
  if (params) {
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(n, params);
    p->params_json = json_to_string(n, FALSE);
    json_node_unref(n);
  } else {
    p->params_json = g_strdup("{}");
  }
  p->sent_to = g_ptr_array_new();
  p->timeout_id = g_timeout_add_seconds(self->request_timeout_s, on_pending_timeout, p);

  if (self->info_ready) {
    pending_dispatch(self, p);
  } else {
    g_queue_push_tail(self->queued, p);
  }
}

JsonNode *
nwa_nwc_client_request_finish(NwaNwcClient *self, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, self), NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

/* ---- GObject ---- */

static void
nwa_nwc_client_dispose(GObject *obj)
{
  NwaNwcClient *self = NWA_NWC_CLIENT(obj);
  nwa_nwc_client_stop(self);
  G_OBJECT_CLASS(nwa_nwc_client_parent_class)->dispose(obj);
}

static void
nwa_nwc_client_finalize(GObject *obj)
{
  NwaNwcClient *self = NWA_NWC_CLIENT(obj);
  wipe_free(self->secret);
  g_free(self->wallet_pk);
  g_free(self->client_pk);
  g_free(self->lud16);
  g_strfreev(self->relays);
  g_strfreev(self->methods);
  g_ptr_array_unref(self->transports);
  g_hash_table_unref(self->pending);
  g_queue_free(self->queued);
  g_hash_table_unref(self->seen);
  g_queue_free(self->seen_order);
  G_OBJECT_CLASS(nwa_nwc_client_parent_class)->finalize(obj);
}

static void
nwa_nwc_client_class_init(NwaNwcClientClass *klass)
{
  GObjectClass *oc = G_OBJECT_CLASS(klass);
  oc->dispose = nwa_nwc_client_dispose;
  oc->finalize = nwa_nwc_client_finalize;
  signals[SIGNAL_NOTIFICATION] =
    g_signal_new("notification", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
                 NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);
  signals[SIGNAL_INFO_CHANGED] =
    g_signal_new("info-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
                 NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
nwa_nwc_client_init(NwaNwcClient *self)
{
  self->transports = g_ptr_array_new_with_free_func((GDestroyNotify)nostr_publish_transport_unref);
  self->pending = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->queued = g_queue_new();
  self->seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->seen_order = g_queue_new();
  self->enc = NOSTR_NWC_ENC_NIP04;
  self->request_timeout_s = 60;
  self->info_grace_ms = 5000;
}
