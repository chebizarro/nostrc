/* SPDX-License-Identifier: MIT
 *
 * relay_pool.c - Relay connectivity using libnostr NostrSimplePool.
 *
 * Wraps NostrSimplePool for multi-relay management, subscriptions,
 * publishing, and event dispatch. No dlsym, no hand-rolled reconnect
 * logic — all handled by libnostr.
 */

#include "signet/relay_pool.h"

#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <json-glib/json-glib.h>

/* libnostr */
#include <nostr-simple-pool.h>
#include <nostr-relay.h>
#include <nostr-event.h>
#include <nostr-filter.h>

/* ------------------------------ relay pool ------------------------------- */

struct SignetRelayPool {
  NostrSimplePool *pool;

  /* Relay URLs (kept for subscribe/publish iteration) */
  char **urls;
  size_t n_urls;

  /* Event callback */
  SignetRelayEventCallback on_event;
  void *user_data;

  GMutex mu;
  gboolean started;
};

/* Single-pool-per-process assumption: signetd and signetctl each create one
 * relay pool. libnostr's middleware API does not carry user_data, so keep the
 * active pool in a process-global atomic pointer. */
static SignetRelayPool *g_active_pool = NULL;

/* ----------------------- event middleware bridge -------------------------- */

static void signet_pool_event_middleware(NostrIncomingEvent *incoming) {
  SignetRelayPool *rp = g_atomic_pointer_get((void **)&g_active_pool);
  if (!rp || !incoming || !incoming->event || !rp->on_event) return;

  char *event_id = nostr_event_get_id(incoming->event);

  SignetRelayEventView ev;
  ev.kind = nostr_event_get_kind(incoming->event);
  ev.created_at = nostr_event_get_created_at(incoming->event);
  ev.event_id_hex = event_id;
  ev.pubkey_hex = nostr_event_get_pubkey(incoming->event);
  ev.content = nostr_event_get_content(incoming->event);

  rp->on_event(&ev, rp->user_data);

  free(event_id);
}

/* ------------------------------ public API -------------------------------- */

SignetRelayPool *signet_relay_pool_new(const SignetRelayPoolConfig *cfg) {
  if (!cfg) return NULL;

  SignetRelayPool *rp = (SignetRelayPool *)calloc(1, sizeof(*rp));
  if (!rp) return NULL;

  g_mutex_init(&rp->mu);

  rp->on_event = cfg->on_event;
  rp->user_data = cfg->user_data;

  /* Create the underlying NostrSimplePool */
  rp->pool = nostr_simple_pool_new();
  if (!rp->pool) {
    g_mutex_clear(&rp->mu);
    free(rp);
    return NULL;
  }

  /* Wire live incoming-event dispatch through libnostr SimplePool. */
  nostr_simple_pool_set_event_middleware(rp->pool, signet_pool_event_middleware);
  nostr_simple_pool_set_auto_unsub_on_eose(rp->pool, false);

  /* Store URLs and add relays to the pool */
  if (cfg->n_relays > 0 && cfg->relays) {
    rp->urls = (char **)calloc(cfg->n_relays, sizeof(char *));
    if (!rp->urls) {
      nostr_simple_pool_free(rp->pool);
      g_mutex_clear(&rp->mu);
      free(rp);
      return NULL;
    }
    rp->n_urls = cfg->n_relays;

    for (size_t i = 0; i < cfg->n_relays; i++) {
      rp->urls[i] = g_strdup(cfg->relays[i] ? cfg->relays[i] : "");
      nostr_simple_pool_ensure_relay(rp->pool, rp->urls[i]);
    }
  }

  rp->started = FALSE;
  return rp;
}

void signet_relay_pool_free(SignetRelayPool *rp) {
  if (!rp) return;

  signet_relay_pool_stop(rp);

  if (rp->pool) {
    nostr_simple_pool_free(rp->pool);
    rp->pool = NULL;
  }

  if (rp->urls) {
    for (size_t i = 0; i < rp->n_urls; i++) {
      g_free(rp->urls[i]);
    }
    free(rp->urls);
    rp->urls = NULL;
  }

  g_mutex_clear(&rp->mu);
  free(rp);
}

int signet_relay_pool_start(SignetRelayPool *rp) {
  if (!rp || !rp->pool) return -1;

  g_mutex_lock(&rp->mu);
  if (rp->started) {
    g_mutex_unlock(&rp->mu);
    return 0;
  }

  g_atomic_pointer_set((void **)&g_active_pool, rp);
  nostr_simple_pool_start(rp->pool);
  rp->started = TRUE;

  g_mutex_unlock(&rp->mu);
  return 0;
}

void signet_relay_pool_stop(SignetRelayPool *rp) {
  if (!rp || !rp->pool) return;

  g_mutex_lock(&rp->mu);
  if (!rp->started) {
    g_mutex_unlock(&rp->mu);
    return;
  }

  nostr_simple_pool_stop(rp->pool);
  if (g_atomic_pointer_get((void **)&g_active_pool) == rp) {
    g_atomic_pointer_set((void **)&g_active_pool, NULL);
  }
  rp->started = FALSE;

  g_mutex_unlock(&rp->mu);
}

int signet_relay_pool_subscribe_kinds(SignetRelayPool *rp, const int *kinds, size_t n_kinds) {
  if (!rp || !rp->pool) return -1;
  if (!kinds || n_kinds == 0) return 0;

  /* Build a NostrFilter with the requested kinds */
  NostrFilter *filter = nostr_filter_new();
  if (!filter) return -1;

  nostr_filter_set_kinds(filter, kinds, n_kinds);

  NostrFilters *filters = nostr_filters_new();
  if (!filters) {
    nostr_filter_free(filter);
    return -1;
  }

  nostr_filters_add(filters, filter);
  /* filter contents moved into filters; don't free filter separately */

  g_mutex_lock(&rp->mu);

  /* Subscribe across all URLs with de-duplication enabled */
  nostr_simple_pool_subscribe(rp->pool,
                               (const char **)rp->urls, rp->n_urls,
                               *filters, true);

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

  /* Publish to all relays in the pool */
  NostrSimplePool *pool = rp->pool;
  for (size_t i = 0; i < pool->relay_count; i++) {
    NostrRelay *relay = pool->relays[i];
    if (relay && nostr_relay_is_connected(relay)) {
      nostr_relay_publish(relay, evt);
    }
  }

  nostr_event_free(evt);
  g_mutex_unlock(&rp->mu);
  return 0;
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

  rp->on_event(&ev, rp->user_data);

  g_object_unref(p);
  return 0;
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