/* SPDX-License-Identifier: MIT
 *
 * relay_pool.c - Relay connectivity abstraction (Phase 5).
 *
 * Design notes:
 * - Uses a background thread to manage connect/reconnect and publishing.
 * - Uses a thread-safe async queue for outgoing event JSON strings.
 * - Uses json-glib to parse inbound event JSON for callback dispatch.
 * - Integrates with libnostr via runtime symbol lookup (dlsym) to avoid
 *   hard-coding libnostr headers/ABIs in this module's public interface.
 *
 * The daemon's libnostr event-receive callback glue should call
 * signet_relay_pool_handle_event_json() to deliver events into Signet.
 */

#include "signet/relay_pool.h"

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <dlfcn.h>

#include <glib.h>
#include <json-glib/json-glib.h>

/* --------------------------- libnostr relay API --------------------------- */

typedef void *(*signet_nostr_relay_new_fn)(const char *url);
typedef int (*signet_nostr_relay_connect_fn)(void *relay);
typedef void (*signet_nostr_relay_disconnect_fn)(void *relay);
typedef void (*signet_nostr_relay_free_fn)(void *relay);

/* NOTE: These are "best guess" signatures based on requested API names.
 * If libnostr differs, adjust here (and keep the rest of the pool stable). */
typedef int (*signet_nostr_relay_publish_fn)(void *relay, const char *event_json);
typedef int (*signet_nostr_relay_subscribe_fn)(void *relay, const char *sub_id, const char *filter_json);

typedef struct {
  signet_nostr_relay_new_fn relay_new;
  signet_nostr_relay_connect_fn relay_connect;
  signet_nostr_relay_disconnect_fn relay_disconnect;
  signet_nostr_relay_free_fn relay_free;
  signet_nostr_relay_publish_fn relay_publish;
  signet_nostr_relay_subscribe_fn relay_subscribe;

  gboolean available;
  char missing_symbol[64];
} SignetLibnostrApi;

static void signet_libnostr_api_init(SignetLibnostrApi *api) {
  memset(api, 0, sizeof(*api));

  /* Resolve from the current process (assumes libnostr is linked in). */
  api->relay_new = (signet_nostr_relay_new_fn)dlsym(RTLD_DEFAULT, "nostr_relay_new");
  if (!api->relay_new) {
    g_strlcpy(api->missing_symbol, "nostr_relay_new", sizeof(api->missing_symbol));
    return;
  }

  api->relay_connect = (signet_nostr_relay_connect_fn)dlsym(RTLD_DEFAULT, "nostr_relay_connect");
  if (!api->relay_connect) {
    g_strlcpy(api->missing_symbol, "nostr_relay_connect", sizeof(api->missing_symbol));
    return;
  }

  api->relay_disconnect = (signet_nostr_relay_disconnect_fn)dlsym(RTLD_DEFAULT, "nostr_relay_disconnect");
  if (!api->relay_disconnect) {
    g_strlcpy(api->missing_symbol, "nostr_relay_disconnect", sizeof(api->missing_symbol));
    return;
  }

  api->relay_publish = (signet_nostr_relay_publish_fn)dlsym(RTLD_DEFAULT, "nostr_relay_publish");
  if (!api->relay_publish) {
    g_strlcpy(api->missing_symbol, "nostr_relay_publish", sizeof(api->missing_symbol));
    return;
  }

  /* Optional: subscribe/free (pool works without them, but functionality reduced). */
  api->relay_subscribe = (signet_nostr_relay_subscribe_fn)dlsym(RTLD_DEFAULT, "nostr_relay_subscribe");
  api->relay_free = (signet_nostr_relay_free_fn)dlsym(RTLD_DEFAULT, "nostr_relay_free");

  api->available = TRUE;
}

typedef struct {
  char *url;
  void *relay;

  gboolean connected;

  /* backoff state */
  guint backoff_ms;        /* current */
  gint64 next_attempt_us;  /* monotonic time */
} SignetRelayConn;

/* ------------------------------ relay pool ------------------------------- */

struct SignetRelayPool {
  SignetRelayConn *conns;
  size_t n_conns;

  SignetRelayEventCallback on_event;
  void *user_data;

  /* desired subscription kinds */
  GArray *sub_kinds;               /* int */
  gboolean subscriptions_dirty;

  /* publish queue: char* event_json */
  GAsyncQueue *outq;

  /* background thread */
  GThread *thr;
  gint stop_flag;                 /* atomic int via g_atomic_int_* */
  gboolean started;

  /* global state */
  GMutex mu;                      /* protects sub_kinds/subscriptions_dirty and connection flags */
  gboolean any_connected;

  SignetLibnostrApi api;
};

static void signet_relay_conn_clear(SignetLibnostrApi *api, SignetRelayConn *c) {
  if (!c) return;

  if (c->relay && api && api->relay_disconnect) {
    api->relay_disconnect(c->relay);
  }
  if (c->relay && api && api->relay_free) {
    api->relay_free(c->relay);
  }

  c->relay = NULL;
  c->connected = FALSE;
  c->backoff_ms = 0;
  c->next_attempt_us = 0;

  g_clear_pointer(&c->url, g_free);
}

static guint signet_backoff_next_ms(guint current_ms) {
  const guint min_ms = 250;
  const guint max_ms = 30000;

  if (current_ms == 0) return min_ms;
  guint next = current_ms * 2;
  if (next < min_ms) next = min_ms;
  if (next > max_ms) next = max_ms;
  return next;
}

static char *signet_build_kinds_filter_json(const int *kinds, size_t n_kinds) {
  /* Builds a minimal Nostr filter object: {"kinds":[...]} */
  JsonBuilder *b = json_builder_new();
  if (!b) return NULL;

  json_builder_begin_object(b);

  json_builder_set_member_name(b, "kinds");
  json_builder_begin_array(b);
  for (size_t i = 0; i < n_kinds; i++) {
    json_builder_add_int_value(b, kinds[i]);
  }
  json_builder_end_array(b);

  json_builder_end_object(b);

  JsonGenerator *g = json_generator_new();
  if (!g) {
    g_object_unref(b);
    return NULL;
  }

  JsonNode *root = json_builder_get_root(b);
  json_generator_set_root(g, root);
  json_generator_set_pretty(g, FALSE);

  char *out = json_generator_to_data(g, NULL);

  json_node_free(root);
  g_object_unref(g);
  g_object_unref(b);
  return out; /* must be g_free() */
}

static void signet_pool_recompute_any_connected_locked(SignetRelayPool *rp) {
  gboolean any = FALSE;
  for (size_t i = 0; i < rp->n_conns; i++) {
    if (rp->conns[i].connected) {
      any = TRUE;
      break;
    }
  }
  rp->any_connected = any;
}

static void signet_pool_try_subscribe_locked(SignetRelayPool *rp) {
  if (!rp->api.available) return;
  if (!rp->api.relay_subscribe) return;
  if (!rp->subscriptions_dirty) return;
  if (!rp->sub_kinds || rp->sub_kinds->len == 0) {
    rp->subscriptions_dirty = FALSE;
    return;
  }

  /* Snapshot kinds under lock. */
  const size_t n = (size_t)rp->sub_kinds->len;
  int *kinds = g_new0(int, n);
  if (!kinds) return;

  for (size_t i = 0; i < n; i++) {
    kinds[i] = g_array_index(rp->sub_kinds, int, (guint)i);
  }

  g_autofree char *filter = signet_build_kinds_filter_json(kinds, n);
  g_free(kinds);

  if (!filter) return;

  /* Subscribe on each connected relay. Use a stable sub id. */
  for (size_t i = 0; i < rp->n_conns; i++) {
    SignetRelayConn *c = &rp->conns[i];
    if (!c->connected || !c->relay) continue;

    int rc = rp->api.relay_subscribe(c->relay, "signet", filter);
    if (rc != 0) {
      /* Don't flip connected just for subscribe failures; reconnect loop may fix it. */
    }
  }

  rp->subscriptions_dirty = FALSE;
}

static void signet_pool_try_connect_one(SignetRelayPool *rp, SignetRelayConn *c, gint64 now_us) {
  if (!rp || !c) return;
  if (!rp->api.available) return;
  if (c->connected) return;

  if (c->next_attempt_us != 0 && now_us < c->next_attempt_us) return;

  if (!c->relay) {
    c->relay = rp->api.relay_new(c->url);
    if (!c->relay) {
      c->backoff_ms = signet_backoff_next_ms(c->backoff_ms);
      c->next_attempt_us = now_us + ((gint64)c->backoff_ms * 1000);
      return;
    }
  }

  int rc = rp->api.relay_connect(c->relay);
  if (rc == 0) {
    c->connected = TRUE;
    c->backoff_ms = 0;
    c->next_attempt_us = 0;
  } else {
    c->connected = FALSE;
    c->backoff_ms = signet_backoff_next_ms(c->backoff_ms);
    c->next_attempt_us = now_us + ((gint64)c->backoff_ms * 1000);
  }
}

static void signet_pool_disconnect_all(SignetRelayPool *rp) {
  if (!rp) return;
  for (size_t i = 0; i < rp->n_conns; i++) {
    if (rp->conns[i].relay && rp->api.relay_disconnect) {
      rp->api.relay_disconnect(rp->conns[i].relay);
    }
    rp->conns[i].connected = FALSE;
  }

  g_mutex_lock(&rp->mu);
  signet_pool_recompute_any_connected_locked(rp);
  g_mutex_unlock(&rp->mu);
}

static gpointer signet_relay_pool_thread_main(gpointer data) {
  SignetRelayPool *rp = (SignetRelayPool *)data;

  while (g_atomic_int_get(&rp->stop_flag) == 0) {
    const gint64 now_us = g_get_monotonic_time();

    /* Connect/reconnect pass. */
    g_mutex_lock(&rp->mu);
    for (size_t i = 0; i < rp->n_conns; i++) {
      signet_pool_try_connect_one(rp, &rp->conns[i], now_us);
    }

    /* If we connected anything and subscriptions are dirty, attempt subscribe. */
    signet_pool_recompute_any_connected_locked(rp);
    signet_pool_try_subscribe_locked(rp);
    g_mutex_unlock(&rp->mu);

    /* Publish pass: pop one message with timeout. */
    gpointer item = g_async_queue_timeout_pop(rp->outq, 250 * 1000); /* 250ms */
    if (item) {
      /* Sentinel (never a valid malloc/g_strdup string pointer in this module). */
      if (item == GINT_TO_POINTER(1)) {
        continue;
      }

      char *event_json = (char *)item;

      /* Publish to all connected relays (best-effort). */
      g_mutex_lock(&rp->mu);
      for (size_t i = 0; i < rp->n_conns; i++) {
        SignetRelayConn *c = &rp->conns[i];
        if (!c->connected || !c->relay) continue;

        if (!rp->api.available || !rp->api.relay_publish) continue;

        int rc = rp->api.relay_publish(c->relay, event_json);
        if (rc != 0) {
          /* Treat publish failure as a connectivity hint; reconnect loop will try again. */
          c->connected = FALSE;
          c->backoff_ms = signet_backoff_next_ms(c->backoff_ms);
          c->next_attempt_us = g_get_monotonic_time() + ((gint64)c->backoff_ms * 1000);
        }
      }
      signet_pool_recompute_any_connected_locked(rp);
      g_mutex_unlock(&rp->mu);

      g_free(event_json);
    }
  }

  /* Shutdown: disconnect all relays (don't free; free occurs in stop/free). */
  signet_pool_disconnect_all(rp);
  return NULL;
}

static void signet_free_conns(SignetRelayPool *rp) {
  if (!rp || !rp->conns) return;
  for (size_t i = 0; i < rp->n_conns; i++) {
    signet_relay_conn_clear(&rp->api, &rp->conns[i]);
  }
  g_free(rp->conns);
  rp->conns = NULL;
  rp->n_conns = 0;
}

/* ------------------------------ public API -------------------------------- */

SignetRelayPool *signet_relay_pool_new(const SignetRelayPoolConfig *cfg) {
  if (!cfg) return NULL;

  SignetRelayPool *rp = g_new0(SignetRelayPool, 1);
  if (!rp) return NULL;

  g_mutex_init(&rp->mu);

  rp->on_event = cfg->on_event;
  rp->user_data = cfg->user_data;

  rp->sub_kinds = g_array_new(FALSE, FALSE, sizeof(int));
  if (!rp->sub_kinds) {
    signet_relay_pool_free(rp);
    return NULL;
  }

  rp->subscriptions_dirty = TRUE;

  rp->outq = g_async_queue_new();
  if (!rp->outq) {
    signet_relay_pool_free(rp);
    return NULL;
  }

  /* Init libnostr symbol table (may be unavailable in some build configurations). */
  signet_libnostr_api_init(&rp->api);

  if (cfg->n_relays > 0 && cfg->relays) {
    rp->conns = g_new0(SignetRelayConn, cfg->n_relays);
    if (!rp->conns) {
      signet_relay_pool_free(rp);
      return NULL;
    }
    rp->n_conns = cfg->n_relays;

    for (size_t i = 0; i < cfg->n_relays; i++) {
      const char *u = cfg->relays[i] ? cfg->relays[i] : "";
      rp->conns[i].url = g_strdup(u);
      rp->conns[i].relay = NULL;
      rp->conns[i].connected = FALSE;
      rp->conns[i].backoff_ms = 0;
      rp->conns[i].next_attempt_us = 0;

      if (!rp->conns[i].url) {
        signet_relay_pool_free(rp);
        return NULL;
      }
    }
  }

  rp->started = FALSE;
  rp->any_connected = FALSE;
  g_atomic_int_set(&rp->stop_flag, 0);

  return rp;
}

void signet_relay_pool_free(SignetRelayPool *rp) {
  if (!rp) return;

  signet_relay_pool_stop(rp);

  if (rp->outq) {
    /* Drain queue. */
    gpointer item = NULL;
    while ((item = g_async_queue_try_pop(rp->outq)) != NULL) {
      if (item == GINT_TO_POINTER(1)) continue;
      g_free(item);
    }
    g_async_queue_unref(rp->outq);
    rp->outq = NULL;
  }

  if (rp->sub_kinds) {
    g_array_free(rp->sub_kinds, TRUE);
    rp->sub_kinds = NULL;
  }

  signet_free_conns(rp);

  g_mutex_clear(&rp->mu);
  g_free(rp);
}

int signet_relay_pool_start(SignetRelayPool *rp) {
  if (!rp) return -1;
  if (rp->started) return 0;

  g_atomic_int_set(&rp->stop_flag, 0);

  rp->thr = g_thread_new("signet-relay-pool", signet_relay_pool_thread_main, rp);
  if (!rp->thr) {
    return -1;
  }

  rp->started = TRUE;
  return 0;
}

void signet_relay_pool_stop(SignetRelayPool *rp) {
  if (!rp) return;
  if (!rp->started) return;

  g_atomic_int_set(&rp->stop_flag, 1);

  /* Wake the thread quickly. */
  if (rp->outq) g_async_queue_push(rp->outq, GINT_TO_POINTER(1));

  if (rp->thr) {
    g_thread_join(rp->thr);
    rp->thr = NULL;
  }

  /* Disconnect/free relay handles. */
  g_mutex_lock(&rp->mu);
  if (rp->conns) {
    for (size_t i = 0; i < rp->n_conns; i++) {
      SignetRelayConn *c = &rp->conns[i];
      if (c->relay && rp->api.relay_disconnect) rp->api.relay_disconnect(c->relay);
      if (c->relay && rp->api.relay_free) rp->api.relay_free(c->relay);
      c->relay = NULL;
      c->connected = FALSE;
      c->backoff_ms = 0;
      c->next_attempt_us = 0;
    }
  }
  signet_pool_recompute_any_connected_locked(rp);
  g_mutex_unlock(&rp->mu);

  rp->started = FALSE;
}

int signet_relay_pool_subscribe_kinds(SignetRelayPool *rp, const int *kinds, size_t n_kinds) {
  if (!rp) return -1;

  g_mutex_lock(&rp->mu);

  g_array_set_size(rp->sub_kinds, 0);
  for (size_t i = 0; i < n_kinds; i++) {
    int k = kinds[i];
    g_array_append_val(rp->sub_kinds, k);
  }
  rp->subscriptions_dirty = TRUE;

  g_mutex_unlock(&rp->mu);
  return 0;
}

int signet_relay_pool_publish_event_json(SignetRelayPool *rp, const char *event_json) {
  if (!rp || !event_json) return -1;
  if (!rp->started || !rp->outq) return -1;

  char *copy = g_strdup(event_json);
  if (!copy) return -1;

  g_async_queue_push(rp->outq, copy);
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
  if (!rp) return false;
  g_mutex_lock(&rp->mu);
  gboolean v = rp->any_connected;
  g_mutex_unlock(&rp->mu);
  return v ? true : false;
}