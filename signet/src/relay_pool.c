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
  GPtrArray       *old_auth_cb_data;  /* auth data belonging to @old_pool */
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
static void signet_relay_pool_clear_auth_cb_data(SignetRelayPool *rp);

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

  rp->on_event(&ev, rp->user_data);

  free((void *)ev.event_json);
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
  SignetRelayPool *rp  = rd->pool;
  const char      *rurl = rd->relay_url[0] ? rd->relay_url
                                            : nostr_relay_get_url_const(rd->relay);

  g_message("[signetd] auth-ok: Khatru confirmed auth, re-subscribing on %s", rurl);

  int *kinds = NULL;
  size_t n_kinds = 0;
  g_mutex_lock(&rp->mu);
  if (rp->active_kinds && rp->n_active_kinds > 0) {
    n_kinds = rp->n_active_kinds;
    kinds = (int *)malloc(n_kinds * sizeof(int));
    if (kinds) memcpy(kinds, rp->active_kinds, n_kinds * sizeof(int));
  }
  g_mutex_unlock(&rp->mu);

  g_free(rd);   /* ownership transferred — free before subscribe to avoid leak */

  if (kinds && n_kinds > 0) {
    if (signet_relay_pool_subscribe_kinds(rp, kinds, n_kinds) == 0) {
      g_message("[signetd] post-AUTH re-subscribed on %s", rurl);
    } else {
      g_warning("[signetd] post-AUTH re-subscribe failed on %s", rurl);
    }
    free(kinds);
  } else {
    g_warning("[signetd] post-AUTH re-subscribe skipped on %s: no active kinds", rurl);
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
   * rp->pool, rp->auth_cb_data and the filter cache. Refuse new reconfigures
   * and wait the in-flight ones out before tearing any of that down. */
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

  if (rp->urls) {
    for (size_t i = 0; i < rp->n_urls; i++) {
      g_free(rp->urls[i]);
    }
    free(rp->urls);
    rp->urls = NULL;
  }

  /* Free per-relay auth callback data. */
  signet_relay_pool_clear_auth_cb_data(rp);

  free(rp->active_kinds);
  rp->active_kinds = NULL;
  rp->n_active_kinds = 0;

  g_free(rp->filter_pubkey_hex);
  rp->filter_pubkey_hex = NULL;

  g_cond_clear(&rp->reconfig_cond);
  g_mutex_clear(&rp->reconfig_mu);
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
      nostr_relay_publish(relay, evt);
      sent++;
    }
  }

  nostr_event_free(evt);
  g_mutex_unlock(&rp->mu);
  /* -2: started but no connected relay received the event (distinct from -1
   * used for invalid args / not-started). */
  return sent > 0 ? 0 : -2;
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
  int sent = 0;
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
      sent++;
    }
  }

  free(eid);
  nostr_event_free(evt);
  g_mutex_unlock(&rp->mu);
  /* -2: started but no connected relay received the event. */
  return sent > 0 ? 0 : -2;
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

  rp->on_event(&ev, rp->user_data);

  g_object_unref(p);
  return 0;
}

/* Free the per-relay NIP-42 callback data owned by the pool. Caller must hold
 * rp->mu (or otherwise guarantee no relay is still using the callbacks). */
static void signet_relay_pool_clear_auth_cb_data(SignetRelayPool *rp) {
  if (!rp->auth_cb_data) return;
  for (guint i = 0; i < rp->auth_cb_data->len; i++) {
    SignetAuthCallbackData *d =
        (SignetAuthCallbackData *)g_ptr_array_index(rp->auth_cb_data, i);
    if (!d) continue;
    memset(d->sk_hex, 0, sizeof(d->sk_hex));
    g_mutex_clear(&d->challenge_mu);
    free(d);
  }
  g_ptr_array_free(rp->auth_cb_data, TRUE);
  rp->auth_cb_data = NULL;
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
   * signet_pool_event_middleware waiting on rp->mu. */
  if (job->old_pool) {
    if (job->old_started) nostr_simple_pool_stop(job->old_pool);
    nostr_simple_pool_free(job->old_pool);
  }
  /* Only safe once the old pool's relays (and therefore its callbacks) are
   * gone: this data was handed to those relays. */
  if (job->old_auth_cb_data) {
    for (guint i = 0; i < job->old_auth_cb_data->len; i++) {
      SignetAuthCallbackData *d =
          (SignetAuthCallbackData *)g_ptr_array_index(job->old_auth_cb_data, i);
      if (!d) continue;
      memset(d->sk_hex, 0, sizeof(d->sk_hex));
      g_mutex_clear(&d->challenge_mu);
      free(d);
    }
    g_ptr_array_free(job->old_auth_cb_data, TRUE);
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
      nostr_simple_pool_ensure_relay(job->fresh, job->urls[i]);
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
      /* Claim the not-started -> started transition under rp->mu.
       * nostr_simple_pool_start() is not idempotent (it pthread_creates
       * unconditionally and overwrites pool->thread), so if the health
       * tick's reconnect path already restarted this pool while we were
       * dialling, leave it alone. */
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
        nostr_simple_pool_subscribe(job->fresh, (const char **)job->urls,
                                    job->n_urls, *filters, true);
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
  job->old_auth_cb_data = rp->auth_cb_data;
  job->old_started = was_started;
  job->restart_after = was_started;
  job->urls = job_urls;
  job->n_urls = n_urls;

  rp->pool = fresh;
  rp->auth_cb_data = NULL;
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
