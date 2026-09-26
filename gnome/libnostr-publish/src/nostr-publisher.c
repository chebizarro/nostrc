/* nostr-publisher.c - Multi-relay publish engine
 *
 * SPDX-License-Identifier: MIT
 *
 * See nostr-publisher.h for the contract. The aggregation and verdict
 * rules are the ones nostr-dav's nd-publisher.c has shipped since the
 * NIP-65 outbox commit landed (all-ACK by default, numeric quorum
 * override, permanent reject short-circuits, 120 s OK deadline swept by
 * tick()); nostr-dav now delegates to this file.
 *
 * Lifetime: every request is refcounted. The `requests` map holds one
 * ref; code that fires callbacks takes a temporary ref so a callback that
 * settles or re-publishes cannot free the request under our feet. A
 * request is removed from the map before its done callback runs, which
 * is what makes re-publishing the same event id from done_cb legal.
 */

#include "nostr-publisher.h"

#include <json-glib/json-glib.h>

#include <string.h>

typedef struct {
  gchar                   *url;
  NostrPublishRelayStatus  status;
  gchar                   *reason;
} NpRelay;

struct _NostrPublishResult {
  int                    ref_count;
  gboolean               finished;

  gchar                 *event_id;
  gchar                 *signed_json;
  gchar                 *frame;        /* ["EVENT",<signed_json>] */
  NostrPublishPolicy     policy;
  gint64                 deadline;

  GPtrArray             *relays;       /* NpRelay*, target-set order */
  guint                  n_pending;

  NostrPublishVerdict    verdict;
  gchar                 *reason;
  gint64                 completed_at;

  NostrPublishRelayFunc  relay_cb;
  NostrPublishDoneFunc   done_cb;
  gpointer               user_data;
  GDestroyNotify         destroy;
};

typedef NostrPublishResult NpRequest;

/* A publisher-owned transport (factory mode). */
typedef struct {
  NostrPublisher        *publisher;   /* back-pointer, unowned */
  gchar                 *url;
  NostrPublishTransport *transport;   /* owned ref */
  GPtrArray             *queue;       /* char* event ids awaiting connect */
} NpOwned;

struct _NostrPublisher {
  NostrPublishSigner           *signer;

  GHashTable                   *bound;     /* url -> NostrPublishTransport* (ref) */
  GHashTable                   *owned;     /* url -> NpOwned* */
  NostrPublishTransportFactory  factory;
  gpointer                      factory_data;
  GDestroyNotify                factory_data_destroy;

  GHashTable                   *requests;  /* event_id -> NpRequest* (ref) */
};

static gint64
wall_now(void)
{
  return g_get_real_time() / G_USEC_PER_SEC;
}

/* ---- Requests ---- */

static void
np_relay_free(gpointer data)
{
  NpRelay *r = data;
  g_free(r->url);
  g_free(r->reason);
  g_free(r);
}

static NpRequest *
request_ref(NpRequest *req)
{
  req->ref_count++;
  return req;
}

static void
request_unref(gpointer data)
{
  NpRequest *req = data;
  if (req == NULL || --req->ref_count > 0)
    return;
  g_free(req->event_id);
  g_free(req->signed_json);
  g_free(req->frame);
  g_clear_pointer(&req->relays, g_ptr_array_unref);
  g_free(req->reason);
  g_free(req);
}

static gint
request_find_relay(NpRequest *req, const gchar *url)
{
  for (guint i = 0; i < req->relays->len; i++) {
    NpRelay *r = g_ptr_array_index(req->relays, i);
    if (g_str_equal(r->url, url))
      return (gint)i;
  }
  return -1;
}

static guint
request_count_accepted(const NpRequest *req)
{
  guint n = 0;
  for (guint i = 0; i < req->relays->len; i++) {
    const NpRelay *r = g_ptr_array_index(req->relays, i);
    if (r->status == NOSTR_PUBLISH_RELAY_ACCEPTED)
      n++;
  }
  return n;
}

/* Settles relay @idx; fires relay_cb. Caller holds a ref on @req. */
static void
settle_relay(NostrPublisher          *self,
             NpRequest               *req,
             guint                    idx,
             NostrPublishRelayStatus  status,
             const gchar             *reason)
{
  NpRelay *r = g_ptr_array_index(req->relays, idx);
  if (r->status != NOSTR_PUBLISH_RELAY_PENDING)
    return;
  r->status = status;
  g_free(r->reason);
  r->reason = g_strdup(reason);
  req->n_pending--;
  if (req->relay_cb != NULL)
    req->relay_cb(self, req->event_id, r->url, status, r->reason,
                  req->user_data);
}

/* Removes @req from the map, fires done_cb then destroy. Caller holds a
 * ref on @req. Idempotent. */
static void
finish_request(NostrPublisher      *self,
               NpRequest           *req,
               NostrPublishVerdict  verdict,
               const gchar         *reason,
               gint64               completed_at)
{
  if (req->finished)
    return;
  req->finished     = TRUE;
  req->verdict      = verdict;
  req->reason       = g_strdup(reason);
  req->completed_at = completed_at;

  if (g_hash_table_lookup(self->requests, req->event_id) == req) {
    g_hash_table_steal(self->requests, req->event_id);
    request_unref(req);   /* the map's ref; caller's ref keeps it alive */
  }

  req->done_cb(self, req, req->user_data);
  if (req->destroy != NULL)
    req->destroy(req->user_data);
  req->destroy   = NULL;
  req->user_data = NULL;
}

/* Settles the request if every relay has answered. */
static void
maybe_complete(NostrPublisher *self, NpRequest *req, gint64 now_ts)
{
  if (req->finished || req->n_pending > 0)
    return;
  guint need = nostr_publish_policy_required_acks(&req->policy,
                                                  req->relays->len);
  NostrPublishVerdict v = request_count_accepted(req) >= need
    ? NOSTR_PUBLISH_VERDICT_PUBLISHED : NOSTR_PUBLISH_VERDICT_RETRY;
  finish_request(self, req, v, NULL, now_ts);
}

/* Snapshot of in-flight requests (each ref'd) so callbacks may mutate
 * the map while the caller iterates. */
static GPtrArray *
snapshot_requests(NostrPublisher *self)
{
  GPtrArray *out = g_ptr_array_new_with_free_func(request_unref);
  GHashTableIter it;
  gpointer v = NULL;
  g_hash_table_iter_init(&it, self->requests);
  while (g_hash_table_iter_next(&it, NULL, &v))
    g_ptr_array_add(out, request_ref(v));
  return out;
}

/* ---- Publisher-owned transports ---- */

static void
owned_free(gpointer data)
{
  NpOwned *o = data;
  if (o->transport != NULL) {
    /* Detach before disconnecting so no callback reaches a dying
     * publisher (the fixture fires state_cb from disconnect). */
    nostr_publish_transport_set_ok_callback(o->transport, NULL, NULL);
    nostr_publish_transport_set_state_callback(o->transport, NULL, NULL);
    nostr_publish_transport_set_auth_callback(o->transport, NULL, NULL);
    nostr_publish_transport_disconnect(o->transport);
    nostr_publish_transport_unref(o->transport);
  }
  g_clear_pointer(&o->queue, g_ptr_array_unref);
  g_free(o->url);
  g_free(o);
}

static void
owned_send_or_fail(NostrPublisher *self, NpOwned *o, const gchar *event_id)
{
  NpRequest *req = g_hash_table_lookup(self->requests, event_id);
  if (req == NULL)
    return;
  gint idx = request_find_relay(req, o->url);
  if (idx < 0)
    return;
  NpRelay *r = g_ptr_array_index(req->relays, idx);
  if (r->status != NOSTR_PUBLISH_RELAY_PENDING)
    return;

  GError *err = NULL;
  if (nostr_publish_transport_send_frame(o->transport, req->frame, &err))
    return;
  request_ref(req);
  settle_relay(self, req, (guint)idx, NOSTR_PUBLISH_RELAY_UNREACHABLE,
               err ? err->message : "send failed");
  g_clear_error(&err);
  maybe_complete(self, req, wall_now());
  request_unref(req);
}

static void
on_owned_state(NostrPublishTransport *transport,
               gboolean               connected,
               const GError          *error,
               gpointer               user_data)
{
  (void)transport;
  NpOwned *o = user_data;
  NostrPublisher *self = o->publisher;

  if (connected) {
    GPtrArray *queue = o->queue;
    o->queue = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < queue->len; i++)
      owned_send_or_fail(self, o, g_ptr_array_index(queue, i));
    g_ptr_array_unref(queue);
    return;
  }

  /* A clean close (no error) reconnects on its own; frames already sent
   * on it fall to the OK deadline. A failed connection ends this attempt
   * for every request still waiting on the relay. */
  if (error == NULL)
    return;
  g_ptr_array_set_size(o->queue, 0);

  g_autoptr(GPtrArray) reqs = snapshot_requests(self);
  gint64 now = wall_now();
  for (guint i = 0; i < reqs->len; i++) {
    NpRequest *req = g_ptr_array_index(reqs, i);
    if (req->finished)
      continue;
    gint idx = request_find_relay(req, o->url);
    if (idx < 0)
      continue;
    settle_relay(self, req, (guint)idx, NOSTR_PUBLISH_RELAY_UNREACHABLE,
                 error->message);
    maybe_complete(self, req, now);
  }
}

static void
on_owned_ok(NostrPublishTransport *transport,
            const gchar           *event_id,
            gboolean               accepted,
            const gchar           *reason,
            gpointer               user_data)
{
  (void)transport;
  NpOwned *o = user_data;
  (void)nostr_publisher_record_ok(o->publisher, o->url, event_id, accepted,
                                  reason);
}

static gchar *
on_owned_auth(NostrPublishTransport *transport,
              const gchar           *challenge,
              gpointer               user_data)
{
  (void)transport;
  NpOwned *o = user_data;
  if (o->publisher->signer == NULL)
    return NULL;
  GError *err = NULL;
  gchar *signed_auth =
    nostr_publish_signer_sign_auth_event(o->publisher->signer, o->url,
                                         challenge, wall_now(), &err);
  if (signed_auth == NULL) {
    g_debug("nostr-publisher: NIP-42 AUTH sign failed for %s: %s",
            o->url, err ? err->message : "unknown");
    g_clear_error(&err);
  }
  return signed_auth;
}

static NpOwned *
owned_get_or_create(NostrPublisher *self, const gchar *url)
{
  NpOwned *o = g_hash_table_lookup(self->owned, url);
  if (o != NULL)
    return o;
  NostrPublishTransport *t = self->factory(url, self->factory_data);
  if (t == NULL)
    return NULL;

  o = g_new0(NpOwned, 1);
  o->publisher = self;
  o->url       = g_strdup(url);
  o->transport = t;
  o->queue     = g_ptr_array_new_with_free_func(g_free);
  nostr_publish_transport_set_ok_callback(t, on_owned_ok, o);
  nostr_publish_transport_set_state_callback(t, on_owned_state, o);
  nostr_publish_transport_set_auth_callback(t, on_owned_auth, o);
  g_hash_table_insert(self->owned, o->url, o);
  return o;
}

/* ---- Dispatch ---- */

/* Sends (or queues) @req's frame to relay @idx, settling it UNREACHABLE
 * when that is impossible. Caller holds a ref on @req. */
static void
dispatch_to_relay(NostrPublisher *self, NpRequest *req, guint idx)
{
  NpRelay *r = g_ptr_array_index(req->relays, idx);
  GError *err = NULL;

  NostrPublishTransport *bound = g_hash_table_lookup(self->bound, r->url);
  if (bound != NULL) {
    if (!nostr_publish_transport_send_frame(bound, req->frame, &err)) {
      settle_relay(self, req, idx, NOSTR_PUBLISH_RELAY_UNREACHABLE,
                   err ? err->message : "send failed");
      g_clear_error(&err);
    }
    return;
  }

  NpOwned *o = self->factory != NULL ? owned_get_or_create(self, r->url)
                                     : NULL;
  if (o == NULL) {
    settle_relay(self, req, idx, NOSTR_PUBLISH_RELAY_UNREACHABLE,
                 "no transport");
    return;
  }
  if (nostr_publish_transport_is_connected(o->transport)) {
    if (!nostr_publish_transport_send_frame(o->transport, req->frame, &err)) {
      settle_relay(self, req, idx, NOSTR_PUBLISH_RELAY_UNREACHABLE,
                   err ? err->message : "send failed");
      g_clear_error(&err);
    }
    return;
  }
  /* Queue first: a transport that connects synchronously flushes from
   * inside connect_async(). */
  g_ptr_array_add(o->queue, g_strdup(req->event_id));
  nostr_publish_transport_connect_async(o->transport);
}

static gchar *
extract_event_id(const gchar *signed_json, GError **error)
{
  g_autoptr(JsonParser) parser = json_parser_new();
  GError *parse_err = NULL;
  if (!json_parser_load_from_data(parser, signed_json, -1, &parse_err)) {
    g_set_error(error, NOSTR_PUBLISH_ERROR, NOSTR_PUBLISH_ERROR_INVALID_EVENT,
                "signed event is not valid JSON: %s", parse_err->message);
    g_clear_error(&parse_err);
    return NULL;
  }
  JsonNode *root = json_parser_get_root(parser);
  if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root)) {
    g_set_error_literal(error, NOSTR_PUBLISH_ERROR,
                        NOSTR_PUBLISH_ERROR_INVALID_EVENT,
                        "signed event is not a JSON object");
    return NULL;
  }
  JsonNode *id = json_object_get_member(json_node_get_object(root), "id");
  if (id == NULL || !JSON_NODE_HOLDS_VALUE(id) ||
      json_node_get_value_type(id) != G_TYPE_STRING ||
      *json_node_get_string(id) == '\0') {
    g_set_error_literal(error, NOSTR_PUBLISH_ERROR,
                        NOSTR_PUBLISH_ERROR_INVALID_EVENT,
                        "signed event has no id field");
    return NULL;
  }
  return g_strdup(json_node_get_string(id));
}

/* ---- Public API ---- */

NostrPublisher *
nostr_publisher_new(NostrPublishSigner *signer)
{
  NostrPublisher *self = g_new0(NostrPublisher, 1);
  self->signer   = signer ? nostr_publish_signer_ref(signer) : NULL;
  self->bound    = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                         (GDestroyNotify)nostr_publish_transport_unref);
  self->owned    = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
                                         owned_free);
  self->requests = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
                                         request_unref);
  return self;
}

void
nostr_publisher_free(NostrPublisher *self)
{
  if (self == NULL)
    return;

  /* Drop in-flight requests without a verdict; destroy still runs. */
  GHashTableIter it;
  gpointer v = NULL;
  g_hash_table_iter_init(&it, self->requests);
  while (g_hash_table_iter_next(&it, NULL, &v)) {
    NpRequest *req = v;
    req->finished = TRUE;
    if (req->destroy != NULL)
      req->destroy(req->user_data);
    req->destroy = NULL;
  }
  g_clear_pointer(&self->requests, g_hash_table_destroy);
  g_clear_pointer(&self->owned, g_hash_table_destroy);
  g_clear_pointer(&self->bound, g_hash_table_destroy);
  if (self->factory_data_destroy != NULL && self->factory_data != NULL)
    self->factory_data_destroy(self->factory_data);
  g_clear_pointer(&self->signer, nostr_publish_signer_unref);
  g_free(self);
}

void
nostr_publisher_bind_transport(NostrPublisher        *self,
                               const gchar           *relay_url,
                               NostrPublishTransport *transport)
{
  g_return_if_fail(self != NULL);
  g_return_if_fail(relay_url != NULL);
  if (transport == NULL)
    g_hash_table_remove(self->bound, relay_url);
  else
    g_hash_table_replace(self->bound, g_strdup(relay_url),
                         nostr_publish_transport_ref(transport));
}

void
nostr_publisher_set_transport_factory(NostrPublisher               *self,
                                      NostrPublishTransportFactory  factory,
                                      gpointer                      factory_data,
                                      GDestroyNotify                factory_data_destroy)
{
  g_return_if_fail(self != NULL);
  if (self->factory_data_destroy != NULL && self->factory_data != NULL)
    self->factory_data_destroy(self->factory_data);
  self->factory              = factory;
  self->factory_data         = factory_data;
  self->factory_data_destroy = factory_data_destroy;
}

gboolean
nostr_publisher_publish_signed(NostrPublisher           *self,
                               const gchar              *signed_json,
                               const gchar *const       *relays,
                               const NostrPublishPolicy *policy,
                               gint64                    now_ts,
                               NostrPublishRelayFunc     relay_cb,
                               NostrPublishDoneFunc      done_cb,
                               gpointer                  user_data,
                               GDestroyNotify            destroy,
                               GError                  **error)
{
  g_return_val_if_fail(self != NULL, FALSE);
  g_return_val_if_fail(signed_json != NULL, FALSE);
  g_return_val_if_fail(done_cb != NULL, FALSE);

  g_autofree gchar *event_id = extract_event_id(signed_json, error);
  if (event_id == NULL)
    goto reject;

  if (g_hash_table_contains(self->requests, event_id)) {
    g_set_error(error, NOSTR_PUBLISH_ERROR,
                NOSTR_PUBLISH_ERROR_ALREADY_IN_FLIGHT,
                "event %s is already being published", event_id);
    goto reject;
  }

  GPtrArray *targets = g_ptr_array_new_with_free_func(np_relay_free);
  if (relays != NULL) {
    for (guint i = 0; relays[i] != NULL; i++) {
      if (*relays[i] == '\0')
        continue;
      gboolean dup = FALSE;
      for (guint j = 0; j < targets->len && !dup; j++)
        dup = g_str_equal(((NpRelay *)g_ptr_array_index(targets, j))->url,
                          relays[i]);
      if (dup)
        continue;
      NpRelay *r = g_new0(NpRelay, 1);
      r->url    = g_strdup(relays[i]);
      r->status = NOSTR_PUBLISH_RELAY_PENDING;
      g_ptr_array_add(targets, r);
    }
  }
  if (targets->len == 0) {
    g_ptr_array_unref(targets);
    g_set_error_literal(error, NOSTR_PUBLISH_ERROR,
                        NOSTR_PUBLISH_ERROR_NO_RELAYS,
                        "no relays to publish to");
    goto reject;
  }

  if (now_ts <= 0)
    now_ts = wall_now();

  NpRequest *req = g_new0(NpRequest, 1);
  req->ref_count   = 1;   /* the map's ref */
  req->event_id    = g_steal_pointer(&event_id);
  req->signed_json = g_strdup(signed_json);
  req->frame       = g_strdup_printf("[\"EVENT\",%s]", signed_json);
  if (policy != NULL)
    req->policy = *policy;
  guint wait = req->policy.ok_wait_sec > 0 ? req->policy.ok_wait_sec
                                           : NOSTR_PUBLISH_DEFAULT_OK_WAIT_SEC;
  req->deadline  = now_ts + wait;
  req->relays    = targets;
  req->n_pending = targets->len;
  req->relay_cb  = relay_cb;
  req->done_cb   = done_cb;
  req->user_data = user_data;
  req->destroy   = destroy;
  g_hash_table_insert(self->requests, req->event_id, req);

  request_ref(req);
  for (guint i = 0; i < req->relays->len && !req->finished; i++)
    dispatch_to_relay(self, req, i);
  maybe_complete(self, req, now_ts);
  request_unref(req);
  return TRUE;

reject:
  if (destroy != NULL)
    destroy(user_data);
  return FALSE;
}

gboolean
nostr_publisher_publish(NostrPublisher           *self,
                        const gchar              *unsigned_json,
                        const gchar *const       *relays,
                        const NostrPublishPolicy *policy,
                        gint64                    now_ts,
                        NostrPublishRelayFunc     relay_cb,
                        NostrPublishDoneFunc      done_cb,
                        gpointer                  user_data,
                        GDestroyNotify            destroy,
                        GError                  **error)
{
  g_return_val_if_fail(self != NULL, FALSE);
  g_return_val_if_fail(unsigned_json != NULL, FALSE);
  g_return_val_if_fail(done_cb != NULL, FALSE);

  if (self->signer == NULL) {
    g_set_error_literal(error, NOSTR_PUBLISH_ERROR,
                        NOSTR_PUBLISH_ERROR_NO_SIGNER,
                        "publisher has no signer");
    if (destroy != NULL)
      destroy(user_data);
    return FALSE;
  }
  g_autofree gchar *signed_json =
    nostr_publish_signer_sign_event_json(self->signer, unsigned_json, NULL,
                                         error);
  if (signed_json == NULL) {
    if (destroy != NULL)
      destroy(user_data);
    return FALSE;
  }
  return nostr_publisher_publish_signed(self, signed_json, relays, policy,
                                        now_ts, relay_cb, done_cb,
                                        user_data, destroy, error);
}

gboolean
nostr_publisher_record_ok(NostrPublisher *self,
                          const gchar    *relay_url,
                          const gchar    *event_id,
                          gboolean        accepted,
                          const gchar    *reason)
{
  g_return_val_if_fail(self != NULL, FALSE);
  g_return_val_if_fail(relay_url != NULL, FALSE);
  g_return_val_if_fail(event_id != NULL, FALSE);

  NpRequest *req = g_hash_table_lookup(self->requests, event_id);
  if (req == NULL)
    return FALSE;
  gint idx = request_find_relay(req, relay_url);
  if (idx < 0)
    return FALSE;
  NpRelay *r = g_ptr_array_index(req->relays, idx);
  if (r->status != NOSTR_PUBLISH_RELAY_PENDING)
    return FALSE;

  NostrPublishRelayStatus status;
  switch (nostr_publish_classify_ok(accepted, reason)) {
  case NOSTR_PUBLISH_OK_ACCEPT:
    status = NOSTR_PUBLISH_RELAY_ACCEPTED;
    break;
  case NOSTR_PUBLISH_OK_PERMANENT:
    status = NOSTR_PUBLISH_RELAY_REJECTED_PERMANENT;
    break;
  case NOSTR_PUBLISH_OK_TRANSIENT:
  default:
    status = NOSTR_PUBLISH_RELAY_REJECTED_TRANSIENT;
    break;
  }

  request_ref(req);
  settle_relay(self, req, (guint)idx, status, reason);
  if (status == NOSTR_PUBLISH_RELAY_REJECTED_PERMANENT)
    finish_request(self, req, NOSTR_PUBLISH_VERDICT_FAILED_PERMANENT,
                   reason ? reason : "relay rejected event", wall_now());
  else
    maybe_complete(self, req, wall_now());
  request_unref(req);
  return TRUE;
}

gboolean
nostr_publisher_tick(NostrPublisher *self, gint64 now_ts)
{
  g_return_val_if_fail(self != NULL, FALSE);

  g_autoptr(GPtrArray) reqs = snapshot_requests(self);
  for (guint i = 0; i < reqs->len; i++) {
    NpRequest *req = g_ptr_array_index(reqs, i);
    if (req->finished || now_ts < req->deadline)
      continue;
    for (guint j = 0; j < req->relays->len && !req->finished; j++)
      settle_relay(self, req, j, NOSTR_PUBLISH_RELAY_TIMED_OUT, "timed out");
    maybe_complete(self, req, now_ts);
  }
  return g_hash_table_size(self->requests) > 0;
}

gboolean
nostr_publisher_is_in_flight(NostrPublisher *self, const gchar *event_id)
{
  g_return_val_if_fail(self != NULL, FALSE);
  g_return_val_if_fail(event_id != NULL, FALSE);
  return g_hash_table_contains(self->requests, event_id);
}

guint
nostr_publisher_get_n_in_flight(NostrPublisher *self)
{
  g_return_val_if_fail(self != NULL, 0);
  return g_hash_table_size(self->requests);
}

/* ---- Result accessors ---- */

NostrPublishVerdict
nostr_publish_result_get_verdict(const NostrPublishResult *result)
{
  g_return_val_if_fail(result != NULL, NOSTR_PUBLISH_VERDICT_RETRY);
  return result->verdict;
}

const gchar *
nostr_publish_result_get_event_id(const NostrPublishResult *result)
{
  g_return_val_if_fail(result != NULL, NULL);
  return result->event_id;
}

const gchar *
nostr_publish_result_get_signed_json(const NostrPublishResult *result)
{
  g_return_val_if_fail(result != NULL, NULL);
  return result->signed_json;
}

const gchar *
nostr_publish_result_get_reason(const NostrPublishResult *result)
{
  g_return_val_if_fail(result != NULL, NULL);
  return result->reason;
}

gint64
nostr_publish_result_get_completed_at(const NostrPublishResult *result)
{
  g_return_val_if_fail(result != NULL, 0);
  return result->completed_at;
}

guint
nostr_publish_result_get_n_relays(const NostrPublishResult *result)
{
  g_return_val_if_fail(result != NULL, 0);
  return result->relays->len;
}

guint
nostr_publish_result_get_n_accepted(const NostrPublishResult *result)
{
  g_return_val_if_fail(result != NULL, 0);
  return request_count_accepted(result);
}

const gchar *
nostr_publish_result_get_relay(const NostrPublishResult  *result,
                               guint                      index,
                               NostrPublishRelayStatus   *out_status,
                               const gchar              **out_reason)
{
  g_return_val_if_fail(result != NULL, NULL);
  g_return_val_if_fail(index < result->relays->len, NULL);
  const NpRelay *r = g_ptr_array_index(result->relays, index);
  if (out_status != NULL)
    *out_status = r->status;
  if (out_reason != NULL)
    *out_reason = r->reason;
  return r->url;
}
