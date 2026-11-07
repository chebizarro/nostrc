/* SPDX-License-Identifier: MIT
 *
 * relay_pool.c - Relay connectivity using libnostr NostrSimplePool.
 *
 * Wraps NostrSimplePool for multi-relay management, subscriptions,
 * publishing, and event dispatch. No dlsym, no hand-rolled reconnect
 * logic — all handled by libnostr.
 */

#include "signet/relay_pool.h"
#include "relay_pool_private.h"

#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <json-glib/json-glib.h>

/* libnostr */
#include <nostr-simple-pool.h>
#include <nostr-relay.h>
#include <nostr-event.h>
#include <nostr-envelope.h>
#include <nostr-filter.h>
#include <nostr-keys.h>
#include <nostr-tag.h>
#include <nostr-kinds.h>
#include <nostr-subscription.h>
#include <nostr-connection.h>

#include <time.h>
#include <sodium.h>

typedef struct RelayState RelayState;
static GPrivate callback_pool = G_PRIVATE_INIT(NULL);
static void pool_release(SignetRelayPool *rp);
static void relay_state_unref(gpointer data);
static void signet_relay_pool_register_auth(SignetRelayPool *rp);

/* ------------------------------ relay pool ------------------------------- */

struct SignetRelayPool {
  NostrSimplePool *pool;
  gint refs;
  gboolean stopping;
  GSource *stop_source;
  GHashTable *relay_states; /* relay -> RelayState, owns references */
  GHashTable *sources;      /* source ID -> GSource, owns references */
  uint64_t watch_serial;
  SignetReplyAckStats reply_acks;
  SignetSubscriptionCallback on_subscription;
  void *subscription_data;

  /* Relay URLs (kept for subscribe/publish iteration) */
  char **urls;
  size_t n_urls;

  /* Event callback */
  SignetRelayEventCallback on_event;
  void *user_data;

  /* NIP-42: hex private key for signing AUTH responses (may be empty) */
  char auth_sk_hex[65];

  /* NIP-42: override relay URL for AUTH event tag (optional, may be empty) */
  char auth_relay_tag_url[256];

  /* Last subscribed kinds — stored so post-auth re-subscribe can replay them */
  int   *active_kinds;
  size_t n_active_kinds;

  /* NPA-04: Scoped filter parameters for post-AUTH re-subscribe replay */
  char  *filter_pubkey_hex;  /* #p tag value (owned, may be NULL) */
  int64_t filter_since;      /* since timestamp (0 = unset) */

  /* NPA-03: Track latest event timestamp for since-based backfill on reconnect.
   * Use 64-bit timestamp storage to avoid Y2038 overflow. */
  gint64 last_event_ts;

  GMutex mu;
  gboolean started;

  /* fp-e08y: relay reconfiguration runs on a background thread.
   * libnostr's connect path blocks for up to NOSTR_CONNECT_RESULT_TIMEOUT_MS
   * (30s, not configurable) per relay, so doing it inline meant a SIGHUP whose
   * candidate config named an unreachable relay froze the GLib main loop --
   * no 25910 management, no NIP-46, no NIP-5L -- for the whole timeout. */
  GMutex   reconfig_mu;       /* serializes reconfigure workers; not rp->mu */
  GCond    reconfig_cond;     /* broadcast when a worker retires */
  guint    reconfig_gen;      /* bumped per set_relays  [rp->mu] */
  guint    reconfig_pending;  /* workers in flight      [rp->mu] */
  gboolean disposing;         /* set by _free()         [rp->mu] */
};

/* One relay-set reconfiguration, handed to a background thread. Owns
 * everything it points at except @rp and @fresh (which rp owns). */
typedef struct {
  SignetRelayPool *rp;
  NostrSimplePool *fresh;             /* pool to bring up (rp->pool at spawn) */
  NostrSimplePool *old_pool;          /* superseded pool, to stop and free */
  char           **urls;              /* private copy of the target URL set */
  size_t           n_urls;
  guint            gen;
  gboolean         old_started;       /* @old_pool needs stopping */
  gboolean         restart_after;     /* pool was running: start @fresh too */
} SignetRelayReconfigure;

/* NPA-09: g_active_pool global eliminated.
 * libnostr now supports event_middleware_ex with user_data, so we pass
 * the SignetRelayPool pointer directly through the callback context. */

/* Forward declarations — defined after public API section */
static NostrFilters *signet_relay_pool_build_filters_locked(SignetRelayPool *rp);

/* ----------------------- event middleware bridge -------------------------- */

static void signet_pool_event_middleware(NostrIncomingEvent *incoming, void *user_data) {
  SignetRelayPool *rp = (SignetRelayPool *)user_data;
  if (!rp || !incoming || !incoming->event || !rp->on_event) return;

  /* NPA-01: Verify Schnorr signature before dispatching.
   * Relays are untrusted — they can inject or modify event JSON.
   * Only events with valid signatures are forwarded to handlers. */
  if (!nostr_event_check_signature(incoming->event)) {
    char *bad_id = nostr_event_get_id(incoming->event);
    g_warning("[signetd] dropping event %s: invalid signature", bad_id ? bad_id : "(null)");
    free(bad_id);
    return;
  }

  char *event_id = nostr_event_get_id(incoming->event);

  SignetRelayEventView ev;
  ev.kind = nostr_event_get_kind(incoming->event);
  ev.created_at = nostr_event_get_created_at(incoming->event);
  ev.event_id_hex = event_id;
  ev.pubkey_hex = nostr_event_get_pubkey(incoming->event);
  ev.content = nostr_event_get_content(incoming->event);
  ev.event_json = nostr_event_serialize_compact(incoming->event);

  /* NPA-03: Track latest event timestamp for since-based backfill.
   * After reconnect, filter_since is updated to this value so we
   * don't reprocess events the replay cache has already evicted. */
  if (ev.created_at > 0) {
    g_mutex_lock(&rp->mu);
    if ((gint64)ev.created_at > rp->last_event_ts) {
      rp->last_event_ts = (gint64)ev.created_at;
    }
    g_mutex_unlock(&rp->mu);
  }

  gpointer previous_pool = g_private_get(&callback_pool);
  g_private_set(&callback_pool, rp);
  rp->on_event(&ev, rp->user_data);
  g_private_set(&callback_pool, previous_pool);

  free((void *)ev.event_json);
  free(event_id);
}

/* ----------------------- NIP-42 auth callback ----------------------------- */

/* The relay owns callback references; the pool and each deferred source own
 * separate references. A detached callback may finish after stop(), but cannot
 * outlive either the pool allocation or its relay allocation. */
typedef struct OkWatch {
  struct OkWatch *next;
  uint64_t serial;
  char *event_id;
  char *key;
  SignetRelayPool *rp;
  uint64_t generation;
  gboolean auth;
  SignetPublishOkCallback cb;
  void *data;
  GDestroyNotify destroy;
} OkWatch;

struct RelayState {
  gint refs;
  SignetRelayPool *rp;
  NostrRelay *relay;
  NostrSimplePool *owner_pool;
  GMutex mu;
  GCond retired;
  GHashTable *watches;
  uint64_t generation;
  char *challenge;
  char *closed_reason;
  gboolean closed_unreported;
  NostrSubscription *required;
  gboolean ready;
  gboolean auth_pending;
  uint64_t subscription_revision;
  gboolean attached;
};

typedef struct {
  RelayState *state;
  uint64_t generation;
  uint64_t subscription_revision;
  uint64_t watch_serial;
  guint id;
  GSource *source;
  char *challenge;
  char sk_hex[65];
  char *event_id;
  char *notification;
  char *reason;
  gboolean resubscribe;
  gboolean auth_complete;
} Deferred;

static RelayState *relay_state_ref(RelayState *s) {
  g_atomic_int_inc(&s->refs);
  return s;
}

static void watch_free(gpointer data) {
  OkWatch *w = data;
  while (w) {
    OkWatch *next = w->next;
    if (w->destroy) w->destroy(w->data);
    g_free(w->event_id);
    g_free(w->key);
    g_free(w);
    w = next;
  }
}

/* One wire OK resolves all callers watching the same signed event. */
static gboolean insert_watch_locked(RelayState *s, OkWatch *w) {
  w->rp = s->rp;
  w->key = g_strdup_printf("%" G_GUINT64_FORMAT ":%s", w->generation, w->event_id);
  OkWatch *old = g_hash_table_lookup(s->watches, w->key);
  w->serial = old ? old->serial : ++s->rp->watch_serial;
  w->next = old;
  if (old) g_hash_table_steal(s->watches, old->key);
  g_hash_table_insert(s->watches, w->key, w);
  return old == NULL;
}

static void relay_state_unref(gpointer data) {
  RelayState *s = data;
  g_mutex_lock(&s->mu);
  gboolean last = g_atomic_int_dec_and_test(&s->refs);
  g_cond_broadcast(&s->retired);
  g_mutex_unlock(&s->mu);
  if (!last) return;
  if (s->required) nostr_subscription_unref(s->required);
  g_hash_table_destroy(s->watches);
  g_free(s->challenge);
  g_free(s->closed_reason);
  g_cond_clear(&s->retired);
  g_mutex_clear(&s->mu);
  nostr_relay_unref(s->relay);
  SignetRelayPool *rp = s->rp;
  g_free(s);
  pool_release(rp);
}

static uint64_t relay_generation(NostrRelay *relay) {
  uint64_t generation = 0;
  char *challenge = nostr_relay_get_auth_challenge(relay, &generation);
  free(challenge);
  return generation;
}

static void finish_watch(OkWatch *w, gboolean accepted, const char *reason) {
  while (w) {
    OkWatch *next = w->next;
    w->next = NULL;
    if (w->cb) {
      gpointer previous = g_private_get(&callback_pool);
      g_private_set(&callback_pool, w->rp);
      w->cb(w->event_id, accepted, reason, w->data);
      g_private_set(&callback_pool, previous);
    }
    watch_free(w);
    w = next;
  }
}

/* Callbacks are never invoked while holding rp->mu or the dispatcher mutex. */
static GPtrArray *take_aborted_watches(RelayState *s) {
  GPtrArray *pending = g_ptr_array_new();
  g_mutex_lock(&s->mu);
  GHashTableIter it;
  gpointer value;
  g_hash_table_iter_init(&it, s->watches);
  while (g_hash_table_iter_next(&it, NULL, &value)) {
    g_ptr_array_add(pending, value);
    g_hash_table_iter_steal(&it);
  }
  s->ready = FALSE;
  s->auth_pending = FALSE;
  g_clear_pointer(&s->challenge, g_free);
  if (s->required) nostr_subscription_unref(s->required);
  s->required = NULL;
  g_mutex_unlock(&s->mu);
  return pending;
}
static void finish_aborted_watches(GPtrArray *pending) {
  for (guint i = 0; i < pending->len; i++)
    finish_watch(g_ptr_array_index(pending, i), FALSE, "transport-aborted:");
  g_ptr_array_free(pending, TRUE);
}

static void abort_watches(RelayState *s) {
  finish_aborted_watches(take_aborted_watches(s));
}

static void deferred_free(gpointer data) {
  Deferred *d = data;
  SignetRelayPool *rp = d->state->rp;
  g_mutex_lock(&rp->mu);
  Deferred *registered = g_hash_table_lookup(rp->sources, GUINT_TO_POINTER(d->id));
  GSource *source = registered ? registered->source : NULL;
  if (source) g_hash_table_steal(rp->sources, GUINT_TO_POINTER(d->id));
  g_mutex_unlock(&rp->mu);
  if (source) g_source_unref(source);
  sodium_memzero(d->sk_hex, sizeof(d->sk_hex));
  g_free(d->challenge);
  g_free(d->event_id);
  g_free(d->notification);
  g_free(d->reason);
  relay_state_unref(d->state);
  g_free(d);
}

static void capture_required_locked(SignetRelayPool *rp) {
  NostrSimplePool *pool = rp->pool;
  pthread_mutex_lock(&pool->pool_mutex);
  GHashTableIter it;
  gpointer value;
  g_hash_table_iter_init(&it, rp->relay_states);
  while (g_hash_table_iter_next(&it, NULL, &value)) {
    RelayState *s = value;
    NostrSubscription *latest = NULL;
    for (size_t i = 0; i < pool->subs_count; i++)
      if (pool->subs[i] && nostr_subscription_get_relay(pool->subs[i]) == s->relay)
        latest = pool->subs[i];
    g_mutex_lock(&s->mu);
    if (s->required) nostr_subscription_unref(s->required);
    s->required = latest ? nostr_subscription_ref(latest) : NULL;
    s->subscription_revision++;
    s->ready = FALSE;
    g_mutex_unlock(&s->mu);
  }
  pthread_mutex_unlock(&pool->pool_mutex);
}

static gboolean deferred_run(gpointer data);

/* nostr_relay_write copies the frame. Close before draining so the writer
 * either already supplied its Error or must release it on failed send. */
static void release_write_answer(GoChannel *answer) {
  if (!answer) return;
  go_channel_close(answer);
  void *result = NULL;
  while (go_channel_try_receive(answer, &result) == 0) {
    if (result) free_error(result);
    result = NULL;
  }
  go_channel_unref(answer);
}

/* Unlike nostr_relay_publish(), this does not wait up to five seconds for
 * writer completion on the GLib main thread. OK watches own relay outcomes. */
static gboolean enqueue_event(NostrRelay *relay, NostrEvent *event) {
  NostrEventEnvelope event_env = {.base.type = NOSTR_ENVELOPE_EVENT, .event = event};
  NostrAuthEnvelope auth_env = {.base.type = NOSTR_ENVELOPE_AUTH, .event = event};
  const NostrEnvelope *envelope = nostr_event_get_kind(event) == NOSTR_KIND_CLIENT_AUTHENTICATION
    ? (const NostrEnvelope *)&auth_env : (const NostrEnvelope *)&event_env;
  char *frame = nostr_envelope_serialize_compact(envelope);
  if (!frame) return FALSE;
  GoChannel *answer = nostr_relay_write(relay, frame);
  free(frame); /* nostr_relay_write copied it */
  gboolean enqueued = answer != NULL;
  release_write_answer(answer);
  return enqueued;
}

/* Caller holds rp->mu. Attaching only after publishing the ID closes the
 * attach/dispatch race when a different thread owns the GLib main context. */
static void defer_locked(RelayState *s, Deferred *d, guint timeout_ms) {
  SignetRelayPool *rp = s->rp;
  d->state = relay_state_ref(s);
  d->generation = relay_generation(s->relay);
  g_mutex_lock(&s->mu);
  d->subscription_revision = s->subscription_revision;
  g_mutex_unlock(&s->mu);
  GSource *source = timeout_ms ? g_timeout_source_new(timeout_ms) : g_idle_source_new();
  g_source_set_callback(source, deferred_run, d, deferred_free);
  d->source = source;
  d->id = g_source_attach(source, NULL);
  g_hash_table_insert(rp->sources, GUINT_TO_POINTER(d->id), d);
}

static gboolean deferred_run(gpointer data) {
  Deferred *d = data;
  RelayState *s = d->state;
  SignetRelayPool *rp = s->rp;
  OkWatch *expired = NULL;
  SignetSubscriptionCallback cb = NULL;
  void *cb_data = NULL;
  g_mutex_lock(&rp->mu);
  if (rp->stopping || rp->disposing || !s->attached ||
      (!d->event_id && d->generation != relay_generation(s->relay))) {
    g_mutex_unlock(&rp->mu);
    return G_SOURCE_REMOVE;
  }
  if (d->event_id) {
    g_mutex_lock(&s->mu);
    expired = g_hash_table_lookup(s->watches, d->event_id);
    if (expired && expired->serial == d->watch_serial)
      g_hash_table_steal(s->watches, d->event_id);
    else expired = NULL;
    g_mutex_unlock(&s->mu);
  } else if (d->notification) {
    gboolean current = TRUE;
    g_mutex_lock(&s->mu);
    if (!strcmp(d->notification, "EOSE"))
      current = d->subscription_revision == s->subscription_revision && s->ready &&
        s->required && !nostr_subscription_is_closed(s->required) &&
        nostr_subscription_is_eosed(s->required) && nostr_relay_is_connected(s->relay);
    else if (!strcmp(d->notification, "CLOSED") && g_strcmp0(d->reason, "auth-rejected:"))
      current = d->subscription_revision == s->subscription_revision && s->required &&
        nostr_subscription_is_closed(s->required);
    g_mutex_unlock(&s->mu);
    cb = current ? rp->on_subscription : NULL;
    cb_data = rp->subscription_data;
  } else if (d->resubscribe) {
    if (d->auth_complete) {
      g_mutex_lock(&s->mu);
      s->auth_pending = FALSE;
      g_mutex_unlock(&s->mu);
    }
    NostrFilters *filters = signet_relay_pool_build_filters_locked(rp);
    if (filters) {
      nostr_simple_pool_subscribe_async(rp->pool, (const char **)rp->urls,
                                        rp->n_urls, *filters, true);
      capture_required_locked(rp);
      nostr_filters_free(filters);
    }
  } else if (d->challenge && d->sk_hex[0]) {
    NostrEvent *evt = nostr_event_new();
    nostr_event_set_kind(evt, NOSTR_KIND_CLIENT_AUTHENTICATION);
    nostr_event_set_created_at(evt, time(NULL));
    nostr_event_set_content(evt, "");
    NostrTags *tags = nostr_tags_new(0);
    const char *url = rp->auth_relay_tag_url[0] ? rp->auth_relay_tag_url
                                               : nostr_relay_get_url_const(s->relay);
    nostr_tags_append(tags, nostr_tag_new("relay", url, NULL));
    nostr_tags_append(tags, nostr_tag_new("challenge", d->challenge, NULL));
    nostr_event_set_tags(evt, tags);
    if (nostr_event_sign(evt, d->sk_hex) == 0) {
      char *id = nostr_event_get_id(evt);
      OkWatch *w = g_new0(OkWatch, 1);
      w->event_id = id;
      w->generation = d->generation;
      w->auth = TRUE;
      g_mutex_lock(&s->mu);
      insert_watch_locked(s, w);
      g_mutex_unlock(&s->mu);
      enqueue_event(s->relay, evt);
    }
    nostr_event_free(evt);
  }
  g_mutex_unlock(&rp->mu);
  if (expired) finish_watch(expired, FALSE,
    d->generation == relay_generation(s->relay) ? "timeout:" : "transport-aborted:");
  if (cb) {
    gpointer previous = g_private_get(&callback_pool);
    g_private_set(&callback_pool, rp);
    cb(nostr_relay_get_url_const(s->relay), d->notification, d->reason, cb_data);
    g_private_set(&callback_pool, previous);
  }
  return G_SOURCE_REMOVE;
}

static void notify_locked(RelayState *s, const char *state, const char *reason) {
  Deferred *d = g_new0(Deferred, 1);
  d->notification = g_strdup(state);
  d->reason = g_strdup(reason);
  defer_locked(s, d, 0);
}

static void ok_dispatch(const char *id, bool accepted, const char *reason, void *data) {
  RelayState *s = data;
  OkWatch *w = NULL;
  g_mutex_lock(&s->mu);
  char *key = g_strdup_printf("%" G_GUINT64_FORMAT ":%s", relay_generation(s->relay), id);
  w = g_hash_table_lookup(s->watches, key);
  if (!w) {
    GHashTableIter it;
    gpointer value;
    g_hash_table_iter_init(&it, s->watches);
    while (g_hash_table_iter_next(&it, NULL, &value)) {
      OkWatch *old = value;
      if (!strcmp(old->event_id, id)) { w = old; break; }
    }
  }
  if (w) g_hash_table_steal(s->watches, w->key); /* terminal exactly once */
  g_free(key);
  g_mutex_unlock(&s->mu);
  if (!w) return;
  while (w) {
    OkWatch *next = w->next;
    w->next = NULL;
    if (w->generation != relay_generation(s->relay)) {
      finish_watch(w, FALSE, "transport-aborted:");
      w = next;
      continue;
    }
    if (w->auth) {
      SignetRelayPool *rp = s->rp;
      g_mutex_lock(&rp->mu);
      if (!rp->stopping && !rp->disposing && s->attached &&
          w->generation == relay_generation(s->relay)) {
        if (accepted) {
          Deferred *d = g_new0(Deferred, 1);
          d->resubscribe = TRUE;
          d->auth_complete = TRUE;
          defer_locked(s, d, 0);
        } else {
          /* Do not log arbitrary relay reason text: it may echo secrets. */
          notify_locked(s, "CLOSED", "auth-rejected:");
        }
      }
      g_mutex_unlock(&rp->mu);
    }
    /* Keep local terminal statuses distinct even if an untrusted relay uses
     * our reserved timeout/transport-aborted text as its rejection reason. */
    char *relay_reason = accepted ? NULL : g_strdup_printf("relay-rejected: %s", reason ? reason : "");
    finish_watch(w, accepted, accepted ? reason : relay_reason);
    g_free(relay_reason);
    w = next;
  }
}

static void auth_challenge(NostrRelay *relay, const char *challenge, void *data) {
  RelayState *s = data;
  SignetRelayPool *rp = s->rp;
  g_mutex_lock(&rp->mu);
  if (rp->stopping || rp->disposing || !s->attached || !rp->auth_sk_hex[0]) {
    g_mutex_unlock(&rp->mu);
    return;
  }
  uint64_t generation = relay_generation(relay);
  g_mutex_lock(&s->mu);
  gboolean duplicate = s->generation == generation && !g_strcmp0(s->challenge, challenge);
  if (!duplicate) {
    s->generation = generation;
    g_free(s->challenge);
    s->challenge = g_strdup(challenge); /* challenges are not truncated */
    s->ready = FALSE;
    s->auth_pending = TRUE;
  }
  g_mutex_unlock(&s->mu);
  if (!duplicate) {
    Deferred *d = g_new0(Deferred, 1);
    d->challenge = g_strdup(challenge);
    memcpy(d->sk_hex, rp->auth_sk_hex, sizeof(d->sk_hex));
    defer_locked(s, d, 0);
    /* A proactive AUTH can change the retained challenge generation before
     * the queued CONNECTED notification runs. Notify the current generation. */
    if (nostr_relay_is_connected(relay)) notify_locked(s, "CONNECTED", NULL);
  }
  g_mutex_unlock(&rp->mu);
}

static void relay_changed(NostrRelay *relay, NostrRelayConnectionState old_state,
                           NostrRelayConnectionState state, void *data) {
  (void)old_state;
  RelayState *s = data;
  SignetRelayPool *rp = s->rp;
  GPtrArray *aborted = NULL;
  g_mutex_lock(&rp->mu);
  if (!rp->stopping && !rp->disposing && s->attached &&
      nostr_relay_get_connection_state(relay) == state) {
    if (state != NOSTR_RELAY_STATE_CONNECTED) aborted = take_aborted_watches(s);
    notify_locked(s, state == NOSTR_RELAY_STATE_CONNECTED ? "CONNECTED" : "DISCONNECTED", NULL);
    if (state == NOSTR_RELAY_STATE_CONNECTED) {
      Deferred *d = g_new0(Deferred, 1);
      d->resubscribe = TRUE;
      defer_locked(s, d, 0);
    }
  }
  g_mutex_unlock(&rp->mu);
  if (aborted) finish_aborted_watches(aborted);
}

static RelayState *ensure_state_locked(SignetRelayPool *rp, NostrRelay *relay) {
  RelayState *s = g_hash_table_lookup(rp->relay_states, relay);
  if (s) return s;
  s = g_new0(RelayState, 1);
  s->refs = 1;
  s->rp = rp;
  g_atomic_int_inc(&rp->refs);
  s->relay = nostr_relay_ref(relay);
  s->owner_pool = rp->pool;
  s->generation = relay_generation(relay);
  s->attached = TRUE;
  g_mutex_init(&s->mu);
  g_cond_init(&s->retired);
  s->watches = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, watch_free);
  g_hash_table_insert(rp->relay_states, relay, s);
  nostr_relay_set_ok_callback_full(relay, ok_dispatch, relay_state_ref(s), relay_state_unref);
  nostr_relay_set_auth_callback_full(relay, auth_challenge, relay_state_ref(s), relay_state_unref);
  nostr_relay_set_state_callback_full(relay, relay_changed, relay_state_ref(s), relay_state_unref);
  char *retained = nostr_relay_get_auth_challenge(relay, NULL);
  if (retained && rp->auth_sk_hex[0]) {
    s->auth_pending = TRUE;
    s->challenge = g_strdup(retained);
    Deferred *d = g_new0(Deferred, 1);
    d->challenge = g_strdup(retained);
    memcpy(d->sk_hex, rp->auth_sk_hex, sizeof(d->sk_hex));
    defer_locked(s, d, 0);
  }
  free(retained);
  if (nostr_relay_is_connected(relay)) notify_locked(s, "CONNECTED", NULL);
  return s;
}

void signet_relay_pool_watch_auth_ok(SignetRelayPool *rp, NostrRelay *relay,
                                     const char *id, const char *url) {
  (void)url;
  g_mutex_lock(&rp->mu);
  RelayState *s = ensure_state_locked(rp, relay);
  OkWatch *w = g_new0(OkWatch, 1);
  w->event_id = g_strdup(id);
  w->auth = TRUE;
  w->generation = relay_generation(relay);
  g_mutex_lock(&s->mu);
  insert_watch_locked(s, w);
  g_mutex_unlock(&s->mu);
  g_mutex_unlock(&rp->mu);
}

static void watch_publish_locked(SignetRelayPool *rp, NostrRelay *relay, const char *id,
                                 SignetPublishOkCallback cb, void *data, GDestroyNotify destroy) {
  RelayState *s = ensure_state_locked(rp, relay);
  OkWatch *w = g_new0(OkWatch, 1);
  w->event_id = g_strdup(id);
  w->generation = relay_generation(relay);
  w->cb = cb;
  w->data = data;
  w->destroy = destroy;
  g_mutex_lock(&s->mu);
  gboolean first = insert_watch_locked(s, w);
  uint64_t serial = w->serial;
  char *key = first ? g_strdup(w->key) : NULL;
  g_mutex_unlock(&s->mu);
  if (first) {
    Deferred *d = g_new0(Deferred, 1);
    d->event_id = key; /* generation + event ID, not another generation's watch */
    d->watch_serial = serial;
    defer_locked(s, d, 10000); /* operation deadline, not a catch-up timer */
  }
}

void signet_relay_pool_watch_publish_ok(SignetRelayPool *rp, NostrRelay *relay,
                                        const char *id, SignetPublishOkCallback cb, void *data) {
  g_mutex_lock(&rp->mu);
  watch_publish_locked(rp, relay, id, cb, data, NULL);
  g_mutex_unlock(&rp->mu);
}

static void subscription_changed(NostrRelay *relay, const char *state,
                                  const char *reason, void *data) {
  SignetRelayPool *rp = data;
  g_mutex_lock(&rp->mu);
  RelayState *s = g_hash_table_lookup(rp->relay_states, relay);
  if (s && !rp->stopping && !rp->disposing) {
    g_mutex_lock(&s->mu);
    gboolean closed = s->required && nostr_subscription_is_closed(s->required);
    if (closed) {
      g_free(s->closed_reason);
      s->closed_reason = g_strdup(reason);
      s->closed_unreported = TRUE;
    }
    gboolean ready = !s->auth_pending && s->required && !closed && nostr_relay_is_connected(relay) &&
                       nostr_subscription_is_eosed(s->required);
    gboolean newly_ready = ready && !s->ready;
    s->ready = ready;
    g_mutex_unlock(&s->mu);
    if ((!strcmp(state, "EOSE") && newly_ready) || (!strcmp(state, "CLOSED") && closed))
      notify_locked(s, state, reason);
  }
  g_mutex_unlock(&rp->mu);
}

static void signet_relay_pool_register_auth(SignetRelayPool *rp) {
  NostrSimplePool *pool = rp->pool;
  pthread_mutex_lock(&pool->pool_mutex);
  for (size_t i = 0; i < pool->relay_count; i++)
    if (pool->relays[i]) ensure_state_locked(rp, pool->relays[i]);
  pthread_mutex_unlock(&pool->pool_mutex);
}

/* ------------------------------ public API -------------------------------- */

SignetRelayPool *signet_relay_pool_new(const SignetRelayPoolConfig *cfg) {
  if (!cfg) return NULL;

  SignetRelayPool *rp = (SignetRelayPool *)calloc(1, sizeof(*rp));
  if (!rp) return NULL;

  rp->refs = 1;
  rp->relay_states = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, relay_state_unref);
  rp->sources = g_hash_table_new(g_direct_hash, g_direct_equal);
  g_mutex_init(&rp->mu);
  g_mutex_init(&rp->reconfig_mu);
  g_cond_init(&rp->reconfig_cond);

  rp->on_event = cfg->on_event;
  rp->user_data = cfg->user_data;

  /* Copy NIP-42 auth key (optional) */
  if (cfg->auth_sk_hex && cfg->auth_sk_hex[0]) {
    g_strlcpy(rp->auth_sk_hex, cfg->auth_sk_hex, sizeof(rp->auth_sk_hex));
  }

  /* Copy NIP-42 relay tag URL override (optional) */
  if (cfg->auth_relay_tag_url && cfg->auth_relay_tag_url[0]) {
    g_strlcpy(rp->auth_relay_tag_url, cfg->auth_relay_tag_url, sizeof(rp->auth_relay_tag_url));
  }

  /* Create the underlying NostrSimplePool */
  rp->pool = nostr_simple_pool_new();
  if (!rp->pool) {
    g_cond_clear(&rp->reconfig_cond);
    g_mutex_clear(&rp->reconfig_mu);
    g_mutex_clear(&rp->mu);
    g_hash_table_destroy(rp->relay_states);
    g_hash_table_destroy(rp->sources);
    free(rp);
    return NULL;
  }

  nostr_simple_pool_set_subscription_callback(rp->pool, subscription_changed, rp);

  /* Wire live incoming-event dispatch through libnostr SimplePool.
   * NPA-09: Use _ex variant to pass relay pool as user_data instead of
   * relying on a process-global pointer. */
  nostr_simple_pool_set_event_middleware_ex(rp->pool, signet_pool_event_middleware, rp);
  nostr_simple_pool_set_auto_unsub_on_eose(rp->pool, false);

  /* Store URLs and add relays to the pool */
  if (cfg->n_relays > 0 && cfg->relays) {
    rp->urls = (char **)calloc(cfg->n_relays, sizeof(char *));
    if (!rp->urls) {
      nostr_simple_pool_free(rp->pool);
      g_cond_clear(&rp->reconfig_cond);
      g_mutex_clear(&rp->reconfig_mu);
      g_mutex_clear(&rp->mu);
      free(rp);
      return NULL;
    }
    rp->n_urls = cfg->n_relays;

    for (size_t i = 0; i < cfg->n_relays; i++) {
      rp->urls[i] = g_strdup(cfg->relays[i] ? cfg->relays[i] : "");
      /* fp-1r0k: register, do not dial. This runs on signetd's startup path
       * before the main loop exists, and the blocking variant spent up to 30s
       * per unreachable relay here -- delaying every listener the daemon is
       * supposed to be bringing up. libnostr's redial worker connects them in
       * the background and keeps retrying (fp-ieg8). */
      nostr_simple_pool_ensure_relay_async(rp->pool, rp->urls[i]);
    }
  }

  /* Register NIP-42 auth callback on each relay */
  signet_relay_pool_register_auth(rp);

  rp->started = FALSE;
  return rp;
}

void signet_relay_pool_free(SignetRelayPool *rp) {
  if (!rp) return;

  /* fp-e08y: a reconfigure worker may still be dialling relays and will touch
   * rp->pool and the filter cache. Refuse new reconfigures and wait the
   * in-flight ones out before tearing any of that down. */
  g_mutex_lock(&rp->mu);
  rp->disposing = TRUE;
  while (rp->reconfig_pending > 0)
    g_cond_wait(&rp->reconfig_cond, &rp->mu);
  g_mutex_unlock(&rp->mu);

  signet_relay_pool_stop(rp);

  if (rp->pool) {
    nostr_simple_pool_free(rp->pool);
    rp->pool = NULL;
  }

  pool_release(rp);
}

static void pool_release(SignetRelayPool *rp) {
  if (!g_atomic_int_dec_and_test(&rp->refs)) return;
  for (size_t i = 0; i < rp->n_urls; i++) g_free(rp->urls[i]);
  free(rp->urls);
  free(rp->active_kinds);
  g_free(rp->filter_pubkey_hex);
  sodium_memzero(rp->auth_sk_hex, sizeof(rp->auth_sk_hex));
  g_hash_table_destroy(rp->relay_states);
  g_hash_table_destroy(rp->sources);
  g_cond_clear(&rp->reconfig_cond);
  g_mutex_clear(&rp->reconfig_mu);
  g_mutex_clear(&rp->mu);
  free(rp);
}

/* Mark/cancel/detach before joining any worker. Never join under rp->mu. */
static void retire_pool(SignetRelayPool *rp, NostrSimplePool *pool) {
  GPtrArray *states = g_ptr_array_new();
  GPtrArray *sources = g_ptr_array_new();
  g_mutex_lock(&rp->mu);
  GHashTableIter it;
  gpointer value;
  g_hash_table_iter_init(&it, rp->relay_states);
  while (g_hash_table_iter_next(&it, NULL, &value)) {
    RelayState *s = value;
    if (s->owner_pool != pool) continue;
    s->attached = FALSE;
    g_ptr_array_add(states, s);
    g_hash_table_iter_steal(&it);
  }
  g_hash_table_iter_init(&it, rp->sources);
  while (g_hash_table_iter_next(&it, NULL, &value)) {
    Deferred *d = value;
    if (d->state->owner_pool != pool) continue;
    g_ptr_array_add(sources, d->source);
    g_hash_table_iter_steal(&it);
  }
  g_mutex_unlock(&rp->mu);
  for (guint i = 0; i < sources->len; i++) {
    GSource *source = g_ptr_array_index(sources, i);
    g_source_destroy(source);
    g_source_unref(source);
  }
  nostr_simple_pool_set_subscription_callback(pool, NULL, NULL);
  for (guint i = 0; i < states->len; i++) {
    RelayState *s = g_ptr_array_index(states, i);
    nostr_relay_set_auth_callback(s->relay, NULL, NULL);
    nostr_relay_set_ok_callback(s->relay, NULL, NULL);
    nostr_relay_set_state_callback(s->relay, NULL, NULL);
    abort_watches(s);
    g_mutex_lock(&s->mu);
    while (g_atomic_int_get(&s->refs) > 1) g_cond_wait(&s->retired, &s->mu);
    g_mutex_unlock(&s->mu);
    relay_state_unref(s);
  }
  g_ptr_array_free(sources, TRUE);
  g_ptr_array_free(states, TRUE);
}

int signet_relay_pool_start(SignetRelayPool *rp) {
  if (!rp || !rp->pool) return -1;

  g_mutex_lock(&rp->mu);
  if (rp->started) {
    g_mutex_unlock(&rp->mu);
    return 0;
  }

  if (rp->disposing || rp->stop_source) { g_mutex_unlock(&rp->mu); return -1; }
  rp->stopping = FALSE;
  signet_relay_pool_register_auth(rp);
  nostr_simple_pool_set_subscription_callback(rp->pool, subscription_changed, rp);
  nostr_simple_pool_start(rp->pool);
  rp->started = TRUE;

  g_mutex_unlock(&rp->mu);
  return 0;
}

static void stop_source_release(gpointer data) { pool_release(data); }
static gboolean stop_from_main_context(gpointer data) {
  SignetRelayPool *rp = data;
  g_mutex_lock(&rp->mu);
  GSource *source = rp->stop_source;
  rp->stop_source = NULL;
  g_mutex_unlock(&rp->mu);
  if (source) g_source_unref(source);
  signet_relay_pool_stop(rp);
  return G_SOURCE_REMOVE;
}

void signet_relay_pool_stop(SignetRelayPool *rp) {
  if (!rp) return;
  g_mutex_lock(&rp->mu);
  if (g_private_get(&callback_pool) == rp) {
    /* Quiescence cannot wait on the callback initiating shutdown. Mark now,
     * then join after this invocation has relinquished its reference. */
    if (!rp->stopping && !rp->stop_source) {
      rp->stopping = TRUE;
      rp->started = FALSE;
      g_atomic_int_inc(&rp->refs);
      rp->stop_source = g_idle_source_new();
      g_source_set_callback(rp->stop_source, stop_from_main_context, rp, stop_source_release);
      g_source_attach(rp->stop_source, NULL);
    }
    g_mutex_unlock(&rp->mu);
    return;
  }
  GSource *stop_source = rp->stop_source;
  rp->stop_source = NULL;
  rp->stopping = TRUE;
  rp->started = FALSE;
  ++rp->reconfig_gen;
  while (rp->reconfig_pending) g_cond_wait(&rp->reconfig_cond, &rp->mu);
  NostrSimplePool *pool = rp->pool;
  g_mutex_unlock(&rp->mu);
  if (stop_source) {
    g_source_destroy(stop_source);
    g_source_unref(stop_source);
  }
  if (!pool) return;
  retire_pool(rp, pool);
  nostr_simple_pool_stop(pool);
}

void signet_relay_pool_set_subscription_callback(SignetRelayPool *rp,
                                                 SignetSubscriptionCallback cb,
                                                 void *data) {
  g_mutex_lock(&rp->mu);
  rp->on_subscription = cb;
  rp->subscription_data = data;
  g_mutex_unlock(&rp->mu);
}

/* NPA-04: Build a NostrFilters* from cached kinds + optional scoped params.
 * Caller must hold rp->mu. Returns NULL on failure. Caller frees result. */
static NostrFilters *signet_relay_pool_build_filters_locked(SignetRelayPool *rp) {
  if (!rp->active_kinds || rp->n_active_kinds == 0) return NULL;

  NostrFilter *filter = nostr_filter_new();
  if (!filter) return NULL;

  nostr_filter_set_kinds(filter, rp->active_kinds, rp->n_active_kinds);

  /* Scope: #p tag limits to events addressed to our pubkey */
  if (rp->filter_pubkey_hex && rp->filter_pubkey_hex[0]) {
    NostrTags *ftags = nostr_tags_new(0);
    if (ftags) {
      NostrTag *ptag = nostr_tag_new("p", rp->filter_pubkey_hex, NULL);
      if (ptag) nostr_tags_append(ftags, ptag);
      nostr_filter_set_tags(filter, ftags);
    }
  }

  /* Scope: since avoids replaying old events after reconnect */
  if (rp->filter_since > 0) {
    nostr_filter_set_since(filter, (NostrTimestamp)rp->filter_since);
  }

  NostrFilters *filters = nostr_filters_new();
  if (!filters) {
    nostr_filter_free(filter);
    return NULL;
  }

  nostr_filters_add(filters, filter);
  return filters;
}

/* fp-1r0k: why rp->mu is still held across the subscribe below.
 *
 * The stall this issue is about was NOT the mutex, it was the dial:
 * nostr_simple_pool_subscribe() called ensure_relay for every URL, so a
 * subscribe on an unreachable relay blocked for the libnostr connect timeout
 * (30s, not configurable) -- and because it blocked with rp->mu held, it also
 * blocked every caller that merely wanted to read the pool. signetd runs this
 * from its health tick every 30s, so an unreachable relay degraded the daemon to
 * roughly zero availability: no 25910, no NIP-46, no NIP-5L.
 *
 * Switching to subscribe_async removes the dial from this path entirely, which
 * leaves a critical section that does no network I/O at all: build a filter,
 * register URLs, fire on whatever is already connected.
 *
 * Keeping rp->mu is then deliberate, not an oversight. It is exactly what makes
 * reading rp->pool safe: signet_relay_pool_set_relays() swaps rp->pool under
 * rp->mu and hands the superseded pool to a background worker that frees it
 * without the lock. Releasing rp->mu here would need a separate in-use guard on
 * the inner pool to replace a guarantee we already have.
 */
int signet_relay_pool_subscribe_kinds(SignetRelayPool *rp, const int *kinds, size_t n_kinds) {
  if (!rp || !rp->pool) return -1;
  if (!kinds || n_kinds == 0) return 0;

  g_mutex_lock(&rp->mu);

  /* Update kinds but preserve existing scoped filter params (pubkey, since).
   * This allows reconnect paths to call subscribe_kinds() without losing
   * the scoped parameters set by the initial subscribe_scoped() call. */
  free(rp->active_kinds);
  rp->active_kinds = (int *)malloc(n_kinds * sizeof(int));
  if (rp->active_kinds) {
    memcpy(rp->active_kinds, kinds, n_kinds * sizeof(int));
    rp->n_active_kinds = n_kinds;
  }

  NostrFilters *filters = signet_relay_pool_build_filters_locked(rp);
  if (!filters) {
    g_mutex_unlock(&rp->mu);
    return -1;
  }

  /* fp-1r0k: _async -- never dials on the caller's thread. Relays that are not
   * up yet are registered and connected in the background; libnostr's reconcile
   * pass fires this same filter set on each one as it connects (signet sets
   * auto_unsub_on_eose false, which is what keeps that pass armed). */
  nostr_simple_pool_subscribe_async(rp->pool,
                                    (const char **)rp->urls, rp->n_urls,
                                    *filters, true);
  capture_required_locked(rp);

  g_mutex_unlock(&rp->mu);

  nostr_filters_free(filters);
  return 0;
}

int signet_relay_pool_subscribe_scoped(SignetRelayPool *rp,
                                       const int *kinds, size_t n_kinds,
                                       const char *pubkey_hex,
                                       int64_t since) {
  if (!rp || !rp->pool) return -1;
  if (!kinds || n_kinds == 0) return 0;

  g_mutex_lock(&rp->mu);

  /* Cache filter parameters for post-AUTH re-subscribe replay */
  free(rp->active_kinds);
  rp->active_kinds = (int *)malloc(n_kinds * sizeof(int));
  if (rp->active_kinds) {
    memcpy(rp->active_kinds, kinds, n_kinds * sizeof(int));
    rp->n_active_kinds = n_kinds;
  }

  g_free(rp->filter_pubkey_hex);
  rp->filter_pubkey_hex = pubkey_hex ? g_strdup(pubkey_hex) : NULL;
  rp->filter_since = since;

  /* Build scoped filter and subscribe */
  NostrFilters *filters = signet_relay_pool_build_filters_locked(rp);
  if (!filters) {
    g_mutex_unlock(&rp->mu);
    return -1;
  }

  /* fp-1r0k: _async -- never dials on the caller's thread. Relays that are not
   * up yet are registered and connected in the background; libnostr's reconcile
   * pass fires this same filter set on each one as it connects (signet sets
   * auto_unsub_on_eose false, which is what keeps that pass armed). */
  nostr_simple_pool_subscribe_async(rp->pool,
                                    (const char **)rp->urls, rp->n_urls,
                                    *filters, true);
  capture_required_locked(rp);

  g_mutex_unlock(&rp->mu);

  nostr_filters_free(filters);
  return 0;
}

int signet_relay_pool_publish_event_json(SignetRelayPool *rp, const char *event_json) {
  if (!rp || !rp->pool || !event_json) return -1;

  g_mutex_lock(&rp->mu);
  if (!rp->started) {
    g_mutex_unlock(&rp->mu);
    return -1;
  }

  /* Deserialize the event JSON into a NostrEvent */
  NostrEvent *evt = nostr_event_new();
  if (!evt) {
    g_mutex_unlock(&rp->mu);
    return -1;
  }

  if (!nostr_event_deserialize_compact(evt, event_json, NULL)) {
    nostr_event_free(evt);
    g_mutex_unlock(&rp->mu);
    return -1;
  }

  /* Publish to all connected relays in the pool, counting how many actually
   * received the event. Returning 0 unconditionally (even with zero connected
   * relays) would make callers believe a response was delivered when nothing
   * was sent. */
  NostrSimplePool *pool = rp->pool;
  int sent = 0;
  for (size_t i = 0; i < pool->relay_count; i++) {
    NostrRelay *relay = pool->relays[i];
    if (relay && nostr_relay_is_connected(relay)) {
      if (enqueue_event(relay, evt)) sent++;
    }
  }

  nostr_event_free(evt);
  g_mutex_unlock(&rp->mu);
  /* -2: started but no connected relay received the event (distinct from -1
   * used for invalid args / not-started). */
  return sent > 0 ? 0 : -2;
}

static int publish_ack(SignetRelayPool *rp, const char *event_json,
                       SignetPublishOkCallback cb, void *user_data,
                       const char *selected_url) {
  if (!rp || !rp->pool || !event_json) return -1;

  g_mutex_lock(&rp->mu);
  if (!rp->started) {
    g_mutex_unlock(&rp->mu);
    return -1;
  }

  NostrEvent *evt = nostr_event_new();
  if (!evt) {
    g_mutex_unlock(&rp->mu);
    return -1;
  }

  if (!nostr_event_deserialize_compact(evt, event_json, NULL)) {
    nostr_event_free(evt);
    g_mutex_unlock(&rp->mu);
    return -1;
  }

  /* Extract event ID for OK matching */
  char *eid = nostr_event_get_id(evt);

  NostrSimplePool *pool = rp->pool;
  int sent = 0;
  for (size_t i = 0; i < pool->relay_count; i++) {
    NostrRelay *relay = pool->relays[i];
    if (relay && nostr_relay_is_connected(relay)) {
      if (selected_url) {
        if (strcmp(selected_url, nostr_relay_get_url_const(relay))) continue;
        RelayState *state = g_hash_table_lookup(rp->relay_states, relay);
        if (!state) continue;
        g_mutex_lock(&state->mu);
        gboolean ready = state->ready && state->required &&
          !nostr_subscription_is_closed(state->required) &&
          nostr_subscription_is_eosed(state->required);
        g_mutex_unlock(&state->mu);
        if (!ready) continue;
      }
      if (cb && eid) {
        /* Register before send; AUTH and other publishes retain their watches. */
        watch_publish_locked(rp, relay, eid, cb, user_data, NULL);
      }
      if (enqueue_event(relay, evt)) sent++;
    }
  }

  free(eid);
  nostr_event_free(evt);
  g_mutex_unlock(&rp->mu);
  /* -2: started but no connected relay received the event. */
  return sent > 0 ? 0 : -2;
}

int signet_relay_pool_publish_event_json_ack(SignetRelayPool *rp, const char *json,
                                              SignetPublishOkCallback cb, void *data) {
  return publish_ack(rp, json, cb, data, NULL);
}
int signet_relay_pool_publish_selected_ack(SignetRelayPool *rp, const char *json,
                                           const char *url, SignetPublishOkCallback cb, void *data) {
  if (!url) return -1;
  return publish_ack(rp, json, cb, data, url);
}

typedef struct {
  gint refs;
  SignetRelayPool *rp;
  char *json;
  char *id;
  GHashTable *attempted;
} ReplyDelivery;
typedef struct { ReplyDelivery *delivery; size_t relay_index; } ReplyAttempt;

static void reply_delivery_unref(ReplyDelivery *d) {
  if (!g_atomic_int_dec_and_test(&d->refs)) return;
  sodium_memzero(d->json, strlen(d->json));
  g_free(d->json);
  free(d->id);
  g_hash_table_destroy(d->attempted);
  pool_release(d->rp);
  g_free(d);
}
static void reply_attempt_free(gpointer data) {
  ReplyAttempt *a = data;
  reply_delivery_unref(a->delivery);
  g_free(a);
}
static gboolean reply_next_locked(ReplyDelivery *d);
static void reply_ok(const char *id, bool accepted, const char *reason, void *data) {
  (void)id;
  ReplyAttempt *a = data;
  ReplyDelivery *d = a->delivery;
  SignetRelayPool *rp = d->rp;
  const char *result;
  g_mutex_lock(&rp->mu);
  if (accepted) { rp->reply_acks.accepted++; result = "accept"; }
  else if (!g_strcmp0(reason, "timeout:")) { rp->reply_acks.timed_out++; result = "timeout"; }
  else if (!g_strcmp0(reason, "transport-aborted:")) {
    rp->reply_acks.transport_aborted++; result = "transport-aborted";
  } else { rp->reply_acks.rejected++; result = "reject"; }
  if (!accepted && rp->started && !rp->stopping && !rp->disposing)
    reply_next_locked(d);
  g_mutex_unlock(&rp->mu);
  /* Only a stable relay index and fixed outcome labels: no ciphertext, request,
   * signer identity, arbitrary relay reason or credential-bearing URL. */
  g_message("[signetd] nip46-reply-ack relay=%zu result=%s", a->relay_index, result);
}
static gboolean reply_next_locked(ReplyDelivery *d) {
  SignetRelayPool *rp = d->rp;
  if (!rp->started || rp->stopping || rp->disposing) return FALSE;
  NostrEvent *evt = nostr_event_new();
  if (!evt || !nostr_event_deserialize_compact(evt, d->json, NULL)) {
    if (evt) nostr_event_free(evt);
    return FALSE;
  }
  gboolean sent = FALSE;
  for (size_t i = 0; i < rp->pool->relay_count; i++) {
    NostrRelay *relay = rp->pool->relays[i];
    const char *url = relay ? nostr_relay_get_url_const(relay) : NULL;
    if (!url || !nostr_relay_is_connected(relay) || g_hash_table_contains(d->attempted, url)) continue;
    g_hash_table_add(d->attempted, g_strdup(url));
    ReplyAttempt *a = g_new0(ReplyAttempt, 1);
    a->delivery = d;
    a->relay_index = i;
    g_atomic_int_inc(&d->refs);
    watch_publish_locked(rp, relay, d->id, reply_ok, a, reply_attempt_free);
    sent = enqueue_event(relay, evt); /* same signed event; never re-sign */
    break;
  }
  nostr_event_free(evt);
  return sent;
}
int signet_relay_pool_publish_reply(SignetRelayPool *rp, const char *json) {
  if (!rp || !json) return -1;
  NostrEvent *evt = nostr_event_new();
  if (!evt || !nostr_event_deserialize_compact(evt, json, NULL)) {
    if (evt) nostr_event_free(evt);
    return -1;
  }
  ReplyDelivery *d = g_new0(ReplyDelivery, 1);
  d->refs = 1;
  d->rp = rp;
  g_atomic_int_inc(&rp->refs);
  d->json = g_strdup(json);
  d->id = nostr_event_get_id(evt);
  d->attempted = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  nostr_event_free(evt);
  g_mutex_lock(&rp->mu);
  gboolean sent = reply_next_locked(d);
  g_mutex_unlock(&rp->mu);
  reply_delivery_unref(d);
  return sent ? 0 : -2;
}
void signet_relay_pool_get_reply_ack_stats(SignetRelayPool *rp, SignetReplyAckStats *out) {
  if (!rp || !out) return;
  g_mutex_lock(&rp->mu);
  *out = rp->reply_acks;
  g_mutex_unlock(&rp->mu);
}

int signet_relay_pool_handle_event_json(SignetRelayPool *rp, const char *event_json) {
  if (!rp || !event_json) return -1;
  if (!rp->on_event) return -1;

  JsonParser *p = json_parser_new();
  if (!p) return -1;

  GError *err = NULL;
  if (!json_parser_load_from_data(p, event_json, -1, &err)) {
    if (err) g_error_free(err);
    g_object_unref(p);
    return -1;
  }

  JsonNode *root = json_parser_get_root(p);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
    g_object_unref(p);
    return -1;
  }

  JsonObject *o = json_node_get_object(root);
  if (!o) {
    g_object_unref(p);
    return -1;
  }

  /* Verify the Schnorr signature before dispatching. This helper is a
   * direct parse+dispatch path that does NOT pass through the subscription
   * middleware (which verifies at relay_pool.c:85), so without this check a
   * forged/unsigned event could reach higher layers. Fail closed on any
   * event that does not deserialize or whose signature is invalid. */
  {
    NostrEvent *vevt = nostr_event_new();
    if (vevt) {
      bool sig_ok = nostr_event_deserialize_compact(vevt, event_json, NULL) &&
                    nostr_event_check_signature(vevt);
      nostr_event_free(vevt);
      if (!sig_ok) {
        g_object_unref(p);
        return -1;
      }
    }
  }

  /* Extract minimal fields. */
  int kind = 0;
  int64_t created_at = 0;
  const char *id = NULL;
  const char *pubkey = NULL;
  const char *content = NULL;

  if (json_object_has_member(o, "kind")) kind = (int)json_object_get_int_member(o, "kind");
  if (json_object_has_member(o, "created_at")) created_at = (int64_t)json_object_get_int_member(o, "created_at");
  if (json_object_has_member(o, "id")) id = json_object_get_string_member(o, "id");
  if (json_object_has_member(o, "pubkey")) pubkey = json_object_get_string_member(o, "pubkey");
  if (json_object_has_member(o, "content")) content = json_object_get_string_member(o, "content");

  SignetRelayEventView ev;
  ev.kind = kind;
  ev.created_at = created_at;
  ev.event_id_hex = id;
  ev.pubkey_hex = pubkey;
  ev.content = content;

  gpointer previous_pool = g_private_get(&callback_pool);
  g_private_set(&callback_pool, rp);
  rp->on_event(&ev, rp->user_data);
  g_private_set(&callback_pool, previous_pool);

  g_object_unref(p);
  return 0;
}

/* True when the pool is already serving exactly this URL set (order-
 * insensitive). Caller must hold rp->mu. */
static bool signet_relay_pool_urls_equal_locked(SignetRelayPool *rp,
                                                const char *const *urls,
                                                size_t n_urls) {
  if (rp->n_urls != n_urls) return false;
  for (size_t i = 0; i < n_urls; i++) {
    const char *want = urls[i] ? urls[i] : "";
    bool found = false;
    for (size_t j = 0; j < rp->n_urls && !found; j++) {
      if (rp->urls[j] && strcmp(rp->urls[j], want) == 0) found = true;
    }
    if (!found) return false;
  }
  return true;
}

/* fp-e08y: the blocking half of a relay-set change.
 *
 * Runs off the caller's thread (in practice the GLib main loop, under SIGHUP)
 * because every step here can block for tens of seconds: nostr_simple_pool_stop
 * joins libnostr worker threads, and ensure_relay dials each relay with a
 * 30s connect timeout.
 *
 * Deliberately does NOT hold rp->mu while connecting. Holding it would defeat
 * the point -- signing, publishing and the health tick all take rp->mu, so the
 * main loop would block on the mutex instead of on the connect. The pool it
 * works on (@fresh) and the URL list are private to the job, and
 * reconfig_mu keeps a later job from freeing @fresh underneath this one.
 */
static gpointer signet_relay_pool_reconfigure_worker(gpointer data) {
  SignetRelayReconfigure *job = (SignetRelayReconfigure *)data;
  SignetRelayPool *rp = job->rp;

  /* Serialize against any earlier reconfigure that may still be dialling the
   * pool this job is about to free. */
  g_mutex_lock(&rp->reconfig_mu);

  /* Tear the superseded pool down first, and without rp->mu held: stopping
   * joins libnostr workers, and a worker may be inside
   * signet_pool_event_middleware waiting on rp->mu. Freeing its relays also
   * releases their auth callback data. */
  if (job->old_pool) {
    retire_pool(rp, job->old_pool);
    if (job->old_started) nostr_simple_pool_stop(job->old_pool);
    nostr_simple_pool_free(job->old_pool);
  }

  /* A newer reconfigure (or a shutdown) may have landed while we waited. It
   * owns @fresh's teardown, so stop here rather than dialling a pool nobody
   * is going to use. */
  g_mutex_lock(&rp->mu);
  gboolean superseded = (rp->reconfig_gen != job->gen) || rp->disposing;
  g_mutex_unlock(&rp->mu);

  if (!superseded) {
    /* The slow part: up to ~30s per unreachable relay. Re-check disposing
     * between relays so a shutdown waits out at most one connect timeout
     * rather than one per configured relay. */
    for (size_t i = 0; i < job->n_urls && !superseded; i++) {
      g_mutex_lock(&rp->mu);
      if (rp->pool == job->fresh) {
        nostr_simple_pool_ensure_relay_async(job->fresh, job->urls[i]);
        signet_relay_pool_register_auth(rp);
      }
      g_mutex_unlock(&rp->mu);
      g_mutex_lock(&rp->mu);
      superseded = (rp->reconfig_gen != job->gen) || rp->disposing;
      g_mutex_unlock(&rp->mu);
    }
  }

  if (!superseded) {
    NostrFilters *filters = NULL;
    gboolean do_start = FALSE;

    g_mutex_lock(&rp->mu);
    if (rp->reconfig_gen == job->gen && !rp->disposing && rp->pool == job->fresh) {
      signet_relay_pool_register_auth(rp);
      /* Claim the not-started -> started transition under rp->mu, so that
       * exactly one of us replays the subscription below. fp-ieg8 made
       * nostr_simple_pool_start() itself idempotent -- it used to
       * pthread_create unconditionally and orphan the previous worker -- but
       * rp->started is signet's own state and the publish paths gate on it,
       * so the transition still needs claiming: the health tick can be
       * starting this same pool concurrently (fp-rym6 leaves that start in
       * place; it is what covers a pool this worker never reaches). */
      if (job->restart_after && !rp->started) {
        rp->started = TRUE;
        do_start = TRUE;
        /* Restore subscription intent on the new connections. active_kinds,
         * filter_pubkey_hex and filter_since were never cleared, so the
         * daemon's subscriptions survive the relay change without the caller
         * re-deriving them. A relay still unreachable at this point picks the
         * subscription up later regardless: signet sets auto_unsub_on_eose
         * false, so libnostr's pool_reconcile_subs re-fires filters_shared on
         * any relay that connects afterwards. */
        filters = signet_relay_pool_build_filters_locked(rp);
      }
    }
    g_mutex_unlock(&rp->mu);

    if (do_start) {
      nostr_simple_pool_start(job->fresh);
      if (filters) {
        /* Outside rp->mu: subscribe() calls ensure_relay internally, which
         * blocks again on any relay that has not come up. */
        g_mutex_lock(&rp->mu);
        if (rp->pool == job->fresh && !rp->stopping && !rp->disposing) {
          nostr_simple_pool_subscribe_async(job->fresh, (const char **)job->urls,
                                            job->n_urls, *filters, true);
          capture_required_locked(rp);
        }
        g_mutex_unlock(&rp->mu);
      }
    }
    if (filters) nostr_filters_free(filters);
  }

  g_mutex_unlock(&rp->reconfig_mu);

  for (size_t i = 0; i < job->n_urls; i++) g_free(job->urls[i]);
  free(job->urls);
  free(job);

  g_mutex_lock(&rp->mu);
  rp->reconfig_pending--;
  g_cond_broadcast(&rp->reconfig_cond);
  g_mutex_unlock(&rp->mu);

  return NULL;
}

int signet_relay_pool_set_relays(SignetRelayPool *rp,
                                 const char *const *urls,
                                 size_t n_urls) {
  if (!rp || !urls || n_urls == 0) return -1;

  g_mutex_lock(&rp->mu);

  if (rp->disposing) {
    g_mutex_unlock(&rp->mu);
    return -1;
  }

  if (signet_relay_pool_urls_equal_locked(rp, urls, n_urls)) {
    g_mutex_unlock(&rp->mu);
    return 1; /* no change */
  }

  /* Build the replacement URL array first: if allocation fails we must not
   * have torn down the working pool. */
  char **new_urls = (char **)calloc(n_urls, sizeof(char *));
  if (!new_urls) {
    g_mutex_unlock(&rp->mu);
    return -1;
  }
  for (size_t i = 0; i < n_urls; i++)
    new_urls[i] = g_strdup(urls[i] ? urls[i] : "");

  const gboolean was_started = rp->started;

  /* libnostr's SimplePool has no "replace the relay set" primitive, and
   * removing relays one by one leaves the subscription list pointing at
   * connections we are about to drop. Coordinated replacement instead: swap
   * the inner pool while keeping THIS SignetRelayPool handle alive, so every
   * borrowed pointer held by nip46/mgmt/nip5l stays valid. */
  NostrSimplePool *fresh = nostr_simple_pool_new();
  if (!fresh) {
    for (size_t i = 0; i < n_urls; i++) g_free(new_urls[i]);
    free(new_urls);
    g_mutex_unlock(&rp->mu);
    return -1;
  }

  /* The job needs its own URL copy: rp->urls can be replaced by the next
   * reconfigure while this job is still dialling. */
  SignetRelayReconfigure *job =
      (SignetRelayReconfigure *)calloc(1, sizeof(*job));
  char **job_urls = job ? (char **)calloc(n_urls, sizeof(char *)) : NULL;
  if (!job || !job_urls) {
    free(job);
    free(job_urls);
    nostr_simple_pool_free(fresh);
    for (size_t i = 0; i < n_urls; i++) g_free(new_urls[i]);
    free(new_urls);
    g_mutex_unlock(&rp->mu);
    return -1;
  }
  for (size_t i = 0; i < n_urls; i++) job_urls[i] = g_strdup(new_urls[i]);

  /* Publish the (empty, unstarted) replacement and take ownership of the old
   * pool before releasing the lock, so concurrent callers see a coherent
   * not-started pool rather than a dangling one. */
  job->rp = rp;
  job->fresh = fresh;
  job->old_pool = rp->pool;
  job->old_started = was_started;
  job->restart_after = was_started;
  job->urls = job_urls;
  job->n_urls = n_urls;

  GHashTableIter state_it;
  gpointer state_value;
  g_hash_table_iter_init(&state_it, rp->relay_states);
  while (g_hash_table_iter_next(&state_it, NULL, &state_value)) {
    RelayState *state = state_value;
    if (state->owner_pool == rp->pool) state->attached = FALSE;
  }
  rp->pool = fresh;
  /* The replacement pool is not running yet. Leaving started TRUE would make
   * publish paths iterate a pool with no relays; FALSE makes them return the
   * existing not-started error instead, and lets the worker (or the health
   * tick, whichever gets there first) claim the start transition. */
  rp->started = FALSE;

  if (rp->urls) {
    for (size_t i = 0; i < rp->n_urls; i++) g_free(rp->urls[i]);
    free(rp->urls);
  }
  rp->urls = new_urls;
  rp->n_urls = n_urls;

  /* Middleware and EOSE policy are pure setters -- do them here so events are
   * dispatched correctly the moment the first connection lands, and so
   * libnostr's reconcile pass (which is skipped for auto_unsub_on_eose pools)
   * is armed before any subscription is fired. */
  nostr_simple_pool_set_event_middleware_ex(fresh, signet_pool_event_middleware,
                                            rp);
  nostr_simple_pool_set_auto_unsub_on_eose(fresh, false);
  nostr_simple_pool_set_subscription_callback(fresh, subscription_changed, rp);

  job->gen = ++rp->reconfig_gen;
  rp->reconfig_pending++;

  g_mutex_unlock(&rp->mu);

  /* fp-e08y: everything that can block now happens here, not on the caller's
   * thread. The relay set is already switched from the caller's point of
   * view (get_urls reflects it immediately); only connectivity is pending.
   * That matches the old contract, which also returned 0 without ever
   * checking whether the connects succeeded -- ensure_relay returns void. */
  GThread *worker = g_thread_try_new("signet-relay-reconfigure",
                                     signet_relay_pool_reconfigure_worker,
                                     job, NULL);
  if (!worker) {
    /* Cannot spawn: fall back to doing the work inline rather than leaking
     * the old pool. Slow, but correct, and only on thread exhaustion. */
    g_warning("[signetd] relay reconfigure: thread spawn failed, "
              "connecting synchronously");
    signet_relay_pool_reconfigure_worker(job);
    return 0;
  }
  g_thread_unref(worker);
  return 0;
}

bool signet_relay_pool_wait_reconfigure(SignetRelayPool *rp,
                                        unsigned int timeout_ms) {
  if (!rp) return true;

  gint64 deadline = g_get_monotonic_time() + (gint64)timeout_ms * 1000;

  g_mutex_lock(&rp->mu);
  while (rp->reconfig_pending > 0) {
    if (!g_cond_wait_until(&rp->reconfig_cond, &rp->mu, deadline)) {
      gboolean pending = (rp->reconfig_pending > 0);
      g_mutex_unlock(&rp->mu);
      return !pending;
    }
  }
  g_mutex_unlock(&rp->mu);
  return true;
}

size_t signet_relay_pool_get_subscribed_kinds(SignetRelayPool *rp,
                                              int *out_kinds, size_t max_kinds) {
  if (!rp) return 0;
  g_mutex_lock(&rp->mu);
  size_t n = rp->n_active_kinds;
  if (out_kinds && max_kinds > 0) {
    size_t copy = n < max_kinds ? n : max_kinds;
    for (size_t i = 0; i < copy; i++) out_kinds[i] = rp->active_kinds[i];
  }
  g_mutex_unlock(&rp->mu);
  return n;
}

const char *const *signet_relay_pool_get_urls(SignetRelayPool *rp, size_t *out_count) {
  if (!rp || !out_count) {
    if (out_count) *out_count = 0;
    return NULL;
  }
  *out_count = rp->n_urls;
  return (const char *const *)rp->urls;
}

bool signet_relay_pool_is_connected(SignetRelayPool *rp) {
  if (!rp || !rp->pool) return false;

  g_mutex_lock(&rp->mu);
  NostrSimplePool *pool = rp->pool;
  bool any = false;

  for (size_t i = 0; i < pool->relay_count; i++) {
    if (pool->relays[i] && nostr_relay_is_connected(pool->relays[i])) {
      any = true;
      break;
    }
  }

  g_mutex_unlock(&rp->mu);
  return any;
}

unsigned signet_relay_pool_dial_attempts(SignetRelayPool *rp) {
  if (!rp || !rp->pool) return 0;

  g_mutex_lock(&rp->mu);
  NostrSimplePool *pool = rp->pool;
  unsigned total = 0;
  for (size_t i = 0; i < pool->relay_count; i++) {
    if (!pool->relays[i]) continue;
    int attempts = nostr_relay_get_reconnect_attempt(pool->relays[i]);
    if (attempts > 0) total += (unsigned)attempts;
  }
  g_mutex_unlock(&rp->mu);
  return total;
}

bool signet_relay_pool_dial_in_flight(SignetRelayPool *rp) {
  if (!rp || !rp->pool) return false;

  g_mutex_lock(&rp->mu);
  NostrSimplePool *pool = rp->pool;
  bool dialing = false;
  for (size_t i = 0; i < pool->relay_count; i++) {
    if (!pool->relays[i]) continue;
    if (nostr_relay_get_connection_state(pool->relays[i]) ==
        NOSTR_RELAY_STATE_CONNECTING) {
      dialing = true;
      break;
    }
  }
  g_mutex_unlock(&rp->mu);
  return dialing;
}

int64_t signet_relay_pool_update_since_from_latest(SignetRelayPool *rp) {
  if (!rp) return 0;

  g_mutex_lock(&rp->mu);
  int64_t latest = rp->last_event_ts;
  if (latest <= 0) {
    g_mutex_unlock(&rp->mu);
    return 0;
  }

  /* Subtract 60s skew to ensure we don't miss events near the boundary.
   * The replay cache handles any duplicates within this window. */
  int64_t since = latest - 60;
  if (since < 0) since = 0;

  rp->filter_since = since;
  g_mutex_unlock(&rp->mu);

  g_message("[signetd] NPA-03: updated since filter to %" G_GINT64_FORMAT
            " (latest_event=%" G_GINT64_FORMAT ", skew=60s)", since, latest);
  return since;
}

bool signet_relay_pool_is_subscribed(SignetRelayPool *rp) {
  if (!rp) return false;
  gboolean ready = FALSE;
  g_mutex_lock(&rp->mu);
  GHashTableIter it;
  gpointer value;
  g_hash_table_iter_init(&it, rp->relay_states);
  while (!rp->stopping && !rp->disposing && g_hash_table_iter_next(&it, NULL, &value)) {
    RelayState *s = value;
    if (s->owner_pool != rp->pool || !s->attached) continue;
    g_mutex_lock(&s->mu);
    ready = s->ready && s->required && nostr_relay_is_connected(s->relay) &&
      !nostr_subscription_is_closed(s->required) && nostr_subscription_is_eosed(s->required);
    g_mutex_unlock(&s->mu);
    if (ready) break;
  }
  g_mutex_unlock(&rp->mu);
  return ready;
}

bool signet_relay_pool_check_sub_closed(SignetRelayPool *rp) {
  if (!rp) return false;
  gboolean closed = FALSE;
  g_mutex_lock(&rp->mu);
  GHashTableIter it;
  gpointer value;
  g_hash_table_iter_init(&it, rp->relay_states);
  while (g_hash_table_iter_next(&it, NULL, &value)) {
    RelayState *s = value;
    if (s->owner_pool != rp->pool || !s->attached) continue;
    g_mutex_lock(&s->mu);
    if (s->closed_unreported) {
      closed = TRUE;
      s->closed_unreported = FALSE;
    }
    g_mutex_unlock(&s->mu);
  }
  g_mutex_unlock(&rp->mu);
  return closed;
}
