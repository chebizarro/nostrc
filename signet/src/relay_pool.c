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
#include <nostr-keys.h>
#include <nostr-tag.h>
#include <nostr-kinds.h>
#include <nostr-subscription.h>
#include <nostr-connection.h>

#include <time.h>

/* ------------------------------ relay pool ------------------------------- */

struct SignetRelayPool {
  NostrSimplePool *pool;

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

  /* Per-relay auth callback data — tracked for cleanup on pool_free */
  GPtrArray *auth_cb_data;

  /* Last subscribed kinds — stored so post-auth re-subscribe can replay them */
  int   *active_kinds;
  size_t n_active_kinds;

  /* NPA-04: Scoped filter parameters for post-AUTH re-subscribe replay */
  char  *filter_pubkey_hex;  /* #p tag value (owned, may be NULL) */
  int64_t filter_since;      /* since timestamp (0 = unset) */

  /* NPA-03: Track latest event timestamp for since-based backfill on reconnect.
   * Uses gint for g_atomic_int_* compatibility (32-bit, wraps in 2038). */
  volatile gint last_event_ts;

  GMutex mu;
  gboolean started;
};

/* NPA-09: g_active_pool global eliminated.
 * libnostr now supports event_middleware_ex with user_data, so we pass
 * the SignetRelayPool pointer directly through the callback context. */

/* Forward declaration — defined after public API section */
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

  /* NPA-03: Track latest event timestamp for since-based backfill.
   * After reconnect, filter_since is updated to this value so we
   * don't reprocess events the replay cache has already evicted.
   * Uses relaxed atomic — exact ordering not critical for a HWM. */
  if (ev.created_at > 0) {
    gint old_val = g_atomic_int_get((volatile gint *)&rp->last_event_ts);
    if ((int64_t)ev.created_at > (int64_t)old_val) {
      g_atomic_int_set((volatile gint *)&rp->last_event_ts, (gint)ev.created_at);
    }
  }

  rp->on_event(&ev, rp->user_data);

  free(event_id);
}

/* ----------------------- NIP-42 auth callback ----------------------------- */

/* Data shared between the AUTH idle callback and the OK-triggered re-subscribe. */
typedef struct {
  NostrRelay      *relay;
  SignetRelayPool *pool;
  char             sk_hex[65];       /* signing key */
  char             challenge[64];    /* AUTH challenge */
  char             relay_url[256];   /* relay URL for relay tag */
  char             auth_event_id[65];/* event ID of sent AUTH — for OK matching */
} PostAuthResubData;

/* GLib idle callback: fires from the GLib main loop after Khatru confirms auth.
 * Called via g_idle_add from the OK response callback when OK true is received
 * for our AUTH event. This ensures REQ is never sent until Khatru has
 * authenticated the connection (no more race condition). */
static gboolean signet_post_auth_resubscribe(gpointer data) {
  PostAuthResubData *rd = (PostAuthResubData *)data;
  NostrRelay      *r   = rd->relay;
  SignetRelayPool *rp  = rd->pool;
  const char      *rurl = rd->relay_url[0] ? rd->relay_url
                                            : nostr_relay_get_url_const(r);

  g_message("[signetd] auth-ok: Khatru confirmed auth, re-subscribing on %s", rurl);

  /* NPA-04: Use shared scoped filter builder for consistent filtering */
  g_mutex_lock(&rp->mu);
  NostrFilters *filters = signet_relay_pool_build_filters_locked(rp);
  g_mutex_unlock(&rp->mu);

  g_free(rd);   /* ownership transferred — free before subscribe to avoid leak */

  if (filters) {
    GoContext *ctx = nostr_relay_get_context(r);
    Error *err = NULL;
    if (nostr_relay_subscribe(r, ctx, filters, &err)) {
      g_message("[signetd] post-AUTH re-subscribed on %s", rurl);
    } else {
      g_warning("[signetd] post-AUTH re-subscribe failed on %s: %s",
                rurl, err ? err->message : "unknown");
    }
    nostr_filters_free(filters);
  }

  return G_SOURCE_REMOVE;  /* one-shot */
}

/* OK response callback fired from the libnostr relay worker thread.
 * Checks if this OK is for our pending AUTH event; if so, schedules
 * re-subscribe via g_idle_add (GLib main loop thread-safe). */
static void signet_ok_response_callback(const char *event_id, bool ok,
                                         const char *reason, void *user_data) {
  PostAuthResubData *rd = (PostAuthResubData *)user_data;
  if (!rd || !event_id) return;

  /* Only act on the OK for our specific AUTH event */
  if (strcmp(event_id, rd->auth_event_id) != 0) return;

  const char *rurl = rd->relay_url[0] ? rd->relay_url : "(unknown)";

  if (ok) {
    g_message("[signetd] auth-ok: relay=%s event=%s OK=true — scheduling re-subscribe", rurl, event_id);
    /* Deregister the OK callback to avoid firing again */
    nostr_relay_set_ok_callback(rd->relay, NULL, NULL);
    /* Schedule resubscribe from GLib main loop (not from relay worker thread) */
    g_idle_add(signet_post_auth_resubscribe, rd);
  } else {
    g_warning("[signetd] auth-ok: relay=%s event=%s OK=false reason=\"%s\" — auth REJECTED",
              rurl, event_id, reason ? reason : "");
    /* Auth rejected. Clear pending state; auth_sent will be reset on next challenge. */
    nostr_relay_set_ok_callback(rd->relay, NULL, NULL);
    g_free(rd);
  }
}

/* GLib idle callback: runs from the GLib main loop (NOT the LWS callback chain).
 * Builds and sends the NIP-42 AUTH response, then schedules the re-subscribe. */
static gboolean signet_send_auth_idle(gpointer data) {
  PostAuthResubData *rd   = (PostAuthResubData *)data;
  NostrRelay        *r    = rd->relay;
  const char        *rurl = rd->relay_url[0] ? rd->relay_url
                                              : nostr_relay_get_url_const(r);

  g_message("[signetd] auth-idle: building AUTH for %s challenge=%.16s", rurl, rd->challenge);

  if (rd->sk_hex[0] && rd->challenge[0]) {
    NostrEvent *evt = nostr_event_new();
    if (evt) {
      nostr_event_set_kind(evt, NOSTR_KIND_CLIENT_AUTHENTICATION);
      nostr_event_set_created_at(evt, (int64_t)time(NULL));
      nostr_event_set_content(evt, "");

      NostrTags *tags = nostr_tags_new(0);
      if (tags) {
        NostrTag *t_relay     = nostr_tag_new("relay",     rurl,         NULL);
        NostrTag *t_challenge = nostr_tag_new("challenge", rd->challenge, NULL);
        if (t_relay)     nostr_tags_append(tags, t_relay);
        if (t_challenge) nostr_tags_append(tags, t_challenge);
        nostr_event_set_tags(evt, tags);
      }

      if (nostr_event_sign(evt, rd->sk_hex) == 0) {
        char *event_json = nostr_event_serialize_compact(evt);
        if (event_json) {
          /* Store the AUTH event ID so the OK callback can match the response */
          char *id_start = strstr(event_json, "\"id\":\"");
          if (id_start) {
            id_start += 6; /* skip `"id":"` */
            size_t copy_len = 0;
            while (id_start[copy_len] && id_start[copy_len] != '"' && copy_len < 64)
              copy_len++;
            memcpy(rd->auth_event_id, id_start, copy_len);
            rd->auth_event_id[copy_len] = '\0';
          }

          /* Register OK callback BEFORE sending AUTH so we don't miss the response */
          nostr_relay_set_ok_callback(r, signet_ok_response_callback, rd);

          char *auth_envelope = g_strdup_printf("[\"AUTH\",%s]", event_json);
          free(event_json);
          if (auth_envelope) {
            /* nostr_relay_write owns the string — pass a strdup'd copy */
            GoChannel *wch = nostr_relay_write(r, strdup(auth_envelope));
            g_free(auth_envelope);
            if (wch) {
              g_message("[signetd] NIP-42 AUTH sent to %s (via idle+write_queue), waiting for OK", rurl);
              /* NOTE: do NOT unref wch prematurely. write_operations holds a
               * pointer to the answer channel (req->answer) and sends the write
               * result back to it.  Calling go_channel_unref here before
               * write_ops processes the request causes a use-after-free that
               * crashes the write worker thread.  The channel is cleaned up
               * by write_operations after it sends the result. */
            } else {
              g_warning("[signetd] auth-idle: nostr_relay_write returned NULL for %s", rurl);
              nostr_relay_set_ok_callback(r, NULL, NULL);
              g_free(rd);
              nostr_event_free(evt);
              return G_SOURCE_REMOVE;
            }
          }
        }
      } else {
        g_warning("[signetd] auth-idle: sign failed for %s", rurl);
        g_free(rd);
      }
      nostr_event_free(evt);
    }
  }

  /* rd is now owned by the OK callback (signet_ok_response_callback).
   * Do NOT free rd or schedule a timer here — subscribe happens only after OK true. */
  return G_SOURCE_REMOVE;  /* one-shot */
}

/* NPA-05: Legacy signet_post_auth_resub_thread removed.
 * The 800ms g_usleep timeout-based AUTH wait was fragile — too short for
 * high-latency relays, too long for LANs.  The OK-callback path
 * (signet_send_auth_idle → signet_ok_response_callback →
 * signet_post_auth_resubscribe) is the correct protocol-driven flow. */

/* User-data struct threaded through per-relay auth callbacks.
 * Pointer to owning pool so the callback can replay subscriptions after auth. */
typedef struct {
  char sk_hex[65];          /* bunker private key hex */
  SignetRelayPool *pool;    /* back-pointer for post-auth re-subscribe */
  gint auth_sent;           /* atomic: 0 = first challenge, 1 = thread already running */
  char last_challenge[64];  /* challenge string that triggered the running thread;
                             * if a NEW challenge arrives (relay reconnected), reset
                             * auth_sent so a fresh thread is spawned */
  GMutex challenge_mu;      /* protects last_challenge */
} SignetAuthCallbackData;

static void signet_relay_auth_callback(NostrRelay *relay,
                                       const char *challenge,
                                       void *user_data) {
  if (!relay || !challenge || !user_data) return;
  const SignetAuthCallbackData *d = (const SignetAuthCallbackData *)user_data;
  if (!d->sk_hex[0]) return;

  const char *url = nostr_relay_get_url_const(relay);

  /* All NIP-42 work (sign, send, wait, re-subscribe) happens in a background
   * thread NOT in the LWS callback chain.  lws_cancel_service() only wakes
   * the LWS loop when called from a *different* thread — calling it from
   * within LWS_CALLBACK_CLIENT_RECEIVE (where this callback fires) is a
   * no-op, so the queued AUTH write would never drain to the wire.
   *
   * Spawn a thread for the FIRST challenge on each relay connection.
   * Khatru re-challenges on every unauthenticated REQ (same challenge, same
   * WebSocket) — those are dropped.  But when the relay reconnects after a
   * WebSocket close, Khatru issues a NEW challenge string.  Detect this by
   * comparing with last_challenge: if different, reset auth_sent and spawn
   * a fresh thread. */
  SignetAuthCallbackData *mdata = (SignetAuthCallbackData *)user_data;
  g_mutex_lock(&mdata->challenge_mu);
  gboolean new_connection = (strcmp(mdata->last_challenge, challenge) != 0);
  if (new_connection) {
    /* Different challenge → relay reconnected.  Reset so we can re-auth. */
    g_strlcpy(mdata->last_challenge, challenge, sizeof(mdata->last_challenge));
    g_atomic_int_set(&mdata->auth_sent, 0);
    g_message("[signetd] NIP-42 new connection detected (challenge changed) for %s", url ? url : "?");
  }
  g_mutex_unlock(&mdata->challenge_mu);

  if (g_atomic_int_compare_and_exchange(&mdata->auth_sent, 0, 1) == FALSE) {
    g_message("[signetd] NIP-42 challenge ignored (thread already running) for %s", url ? url : "?");
    return;
  }

  SignetRelayPool *rp = d->pool;
  if (!rp) return;

  PostAuthResubData *rd = g_new0(PostAuthResubData, 1);
  rd->relay = relay;
  rd->pool  = rp;
  g_strlcpy(rd->sk_hex,    d->sk_hex,       sizeof(rd->sk_hex));
  g_strlcpy(rd->challenge, challenge,        sizeof(rd->challenge));
  /* Use auth_relay_tag_url override if set (allows connecting via internal address
   * while signing AUTH events with the relay's public URL). */
  const char *auth_url = (rp->auth_relay_tag_url[0])
                         ? rp->auth_relay_tag_url
                         : (url ? url : "");
  g_strlcpy(rd->relay_url, auth_url, sizeof(rd->relay_url));

  /* Schedule AUTH send from GLib main loop (idle priority).
   * The idle callback runs outside the LWS callback chain — lws_cancel_service
   * will properly wake the LWS send loop. */
  g_idle_add(signet_send_auth_idle, rd);
}

/* Register the auth callback on every relay in the pool.
 * Must be called after relays have been added via ensure_relay. */
static void signet_relay_pool_register_auth(SignetRelayPool *rp) {
  if (!rp->auth_sk_hex[0]) return;

  /* Allocate one callback-data struct per relay, tracked in auth_cb_data
   * for cleanup in signet_relay_pool_free(). */
  NostrSimplePool *pool = rp->pool;
  if (!rp->auth_cb_data)
    rp->auth_cb_data = g_ptr_array_new();
  for (size_t i = 0; i < pool->relay_count; i++) {
    NostrRelay *relay = pool->relays[i];
    if (!relay) continue;
    SignetAuthCallbackData *d = (SignetAuthCallbackData *)calloc(1, sizeof(*d));
    if (!d) continue;
    memcpy(d->sk_hex, rp->auth_sk_hex, sizeof(d->sk_hex));
    d->pool = rp;
    d->last_challenge[0] = '\0';
    g_mutex_init(&d->challenge_mu);
    nostr_relay_set_auth_callback(relay, signet_relay_auth_callback, d);
    g_ptr_array_add(rp->auth_cb_data, d);
  }
}

/* ------------------------------ public API -------------------------------- */

SignetRelayPool *signet_relay_pool_new(const SignetRelayPoolConfig *cfg) {
  if (!cfg) return NULL;

  SignetRelayPool *rp = (SignetRelayPool *)calloc(1, sizeof(*rp));
  if (!rp) return NULL;

  g_mutex_init(&rp->mu);

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
    g_mutex_clear(&rp->mu);
    free(rp);
    return NULL;
  }

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

  /* Register NIP-42 auth callback on each relay */
  signet_relay_pool_register_auth(rp);

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

  /* Free per-relay auth callback data. */
  if (rp->auth_cb_data) {
    for (guint i = 0; i < rp->auth_cb_data->len; i++) {
      SignetAuthCallbackData *d = (SignetAuthCallbackData *)g_ptr_array_index(rp->auth_cb_data, i);
      if (d) {
        memset(d->sk_hex, 0, sizeof(d->sk_hex));
        g_mutex_clear(&d->challenge_mu);
        free(d);
      }
    }
    g_ptr_array_free(rp->auth_cb_data, TRUE);
    rp->auth_cb_data = NULL;
  }

  free(rp->active_kinds);
  rp->active_kinds = NULL;
  rp->n_active_kinds = 0;

  g_free(rp->filter_pubkey_hex);
  rp->filter_pubkey_hex = NULL;

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
  rp->started = FALSE;

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

  nostr_simple_pool_subscribe(rp->pool,
                               (const char **)rp->urls, rp->n_urls,
                               *filters, true);

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

/* NPA-02: Publish OK tracking.
 * We install a temporary OK callback per-relay that matches a specific
 * event ID and forwards to the user callback, then restores the AUTH callback. */
typedef struct {
  char event_id[65];
  SignetPublishOkCallback user_cb;
  void *user_data;
  /* Saved AUTH callback to restore after our publish OK fires. */
  void (*saved_ok_cb)(const char *, bool, const char *, void *);
  void *saved_ok_data;
  NostrRelay *relay;
} PublishOkCtx;

static void signet_publish_ok_handler(const char *event_id, bool ok,
                                       const char *reason, void *user_data) {
  PublishOkCtx *ctx = (PublishOkCtx *)user_data;
  if (!ctx) return;

  if (event_id && strcmp(event_id, ctx->event_id) == 0) {
    /* This OK is for our published event — fire user callback */
    if (ctx->user_cb) {
      ctx->user_cb(event_id, ok, reason, ctx->user_data);
    }
    if (!ok) {
      g_warning("[signetd] publish-ok: relay rejected event %s: %s",
                event_id, reason ? reason : "(no reason)");
    }
    /* Restore saved AUTH OK callback */
    nostr_relay_set_ok_callback(ctx->relay, ctx->saved_ok_cb, ctx->saved_ok_data);
    g_free(ctx);
  } else {
    /* Not our event — forward to saved callback (AUTH handler) */
    if (ctx->saved_ok_cb) {
      ctx->saved_ok_cb(event_id, ok, reason, ctx->saved_ok_data);
    }
  }
}

int signet_relay_pool_publish_event_json_ack(SignetRelayPool *rp,
                                              const char *event_json,
                                              SignetPublishOkCallback cb,
                                              void *user_data) {
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
  for (size_t i = 0; i < pool->relay_count; i++) {
    NostrRelay *relay = pool->relays[i];
    if (relay && nostr_relay_is_connected(relay)) {
      if (cb && eid) {
        /* Install per-event OK tracker that chains to existing callback */
        PublishOkCtx *ctx = g_new0(PublishOkCtx, 1);
        g_strlcpy(ctx->event_id, eid, sizeof(ctx->event_id));
        ctx->user_cb = cb;
        ctx->user_data = user_data;
        ctx->relay = relay;
        /* NOTE: We don't have a getter for the existing ok_callback/user_data.
         * The saved pointers will be NULL, which is fine — the AUTH path
         * re-registers its own callback before sending AUTH. */
        ctx->saved_ok_cb = NULL;
        ctx->saved_ok_data = NULL;
        nostr_relay_set_ok_callback(relay, signet_publish_ok_handler, ctx);
      }
      nostr_relay_publish(relay, evt);
    }
  }

  free(eid);
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

int64_t signet_relay_pool_update_since_from_latest(SignetRelayPool *rp) {
  if (!rp) return 0;

  int64_t latest = (int64_t)g_atomic_int_get((volatile gint *)&rp->last_event_ts);
  if (latest <= 0) return 0;

  /* Subtract 60s skew to ensure we don't miss events near the boundary.
   * The replay cache handles any duplicates within this window. */
  int64_t since = latest - 60;
  if (since < 0) since = 0;

  g_mutex_lock(&rp->mu);
  rp->filter_since = since;
  g_mutex_unlock(&rp->mu);

  g_message("[signetd] NPA-03: updated since filter to %" G_GINT64_FORMAT
            " (latest_event=%" G_GINT64_FORMAT ", skew=60s)", since, latest);
  return since;
}

bool signet_relay_pool_is_subscribed(SignetRelayPool *rp) {
  if (!rp || !rp->pool) return false;

  g_mutex_lock(&rp->mu);
  NostrSimplePool *pool = rp->pool;
  bool any_eosed = false;

  /* NPA-10: Check all active subscriptions for EOSE (End of Stored Events).
   * EOSE means the relay accepted our REQ and sent back all stored events.
   * Before EOSE, the subscription may still be pending (waiting for AUTH). */
  for (size_t i = 0; i < pool->subs_count; i++) {
    if (pool->subs[i] && nostr_subscription_is_eosed(pool->subs[i])) {
      any_eosed = true;
      break;
    }
  }

  g_mutex_unlock(&rp->mu);
  return any_eosed;
}

bool signet_relay_pool_check_sub_closed(SignetRelayPool *rp) {
  if (!rp || !rp->pool) return false;

  g_mutex_lock(&rp->mu);
  NostrSimplePool *pool = rp->pool;
  bool any_closed = false;

  /* NPA-06: Check all active subscriptions for CLOSED state.
   * A CLOSED frame from the relay means our subscription was terminated
   * (auth-required, policy, rate limit, etc.). */
  for (size_t i = 0; i < pool->subs_count; i++) {
    if (pool->subs[i] && nostr_subscription_is_closed(pool->subs[i])) {
      any_closed = true;
      break;
    }
  }

  g_mutex_unlock(&rp->mu);
  return any_closed;
}