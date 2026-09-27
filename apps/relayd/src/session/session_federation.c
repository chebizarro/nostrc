/*
 * session_federation.c — see session_federation.h.
 */
#include "session_federation.h"

#include <gio/gio.h>
#include <stdlib.h>
#include <string.h>

#include <nostr-publish/nostr-publish-policy.h>
#include <nostr-publish/nostr-publish-transport.h>

#include "json.h"
#include "nostr-filter.h"

#define SIGNER_BUS "org.nostr.Signer"
#define SIGNER_PATH "/org/nostr/signer"
#define SIGNER_IFACE "org.nostr.Signer"
#define HOUSEKEEPING_INTERVAL_S 60
#define ACCOUNT_REPROBE_MIN_S 30
/* org.nostr.Signer may prompt; libnostr-publish's SignEvent timeout. */
#define SIGNER_TIMEOUT_S 30
/* How long an event waits for a GetPublicKey answer before re-routing
 * checks again (the answer makes it due at once). */
#define PROBE_WAIT_S (SIGNER_TIMEOUT_S + 15)

typedef struct {
  char *event_id;
  char *json;
  gint64 deadline; /* wall-clock s */
} Attempt;

typedef struct {
  NsrFederation *fed;
  char *key; /* "<lane>|<url>" */
  char *url;
  NsrFedLane lane;
  NostrPublishTransport *t; /* NULL while idle */
  GHashTable *inflight;     /* id -> Attempt*: EVENT sent, OK awaited */
  GQueue queued;            /* Attempt*: waiting for the connection */
  GHashTable *await_auth;   /* id -> Attempt*: auth-required, resend after AUTH */
  char *challenge;
  gboolean auth_signing;    /* AUTH event at the signer (worker thread) */
  char *auth_event_id;      /* our AUTH event, OK awaited */
  gint64 last_activity;
  /* Read by the D-Bus thread: written under fed->lock. */
  gboolean open;
  gboolean authed;
  char *last_error;
  gint64 last_ok_at;
} RelayConn;

struct NsrFederation {
  NsrFedConfig cfg;
  NsrOutbox *outbox;
  NostrStorage *storage;
  NostrPublishSigner *signer;
  gboolean dbus_signer;
  char *app_id;
  NsrFedObserver observer;
  void *observer_ud;

  GThread *thread;
  GMainContext *ctx;
  GMainLoop *loop;
  GSource *timer;
  gint wake_pending;
  gint stopping;
  GDBusConnection *bus;      /* engine thread */
  gboolean bus_tried;

  GHashTable *conns;         /* key -> RelayConn*; mutations under lock */
  gint64 next_housekeeping;

  /* org.nostr.Signer calls never block the engine thread (nostrc-8cc1):
   * NIP-42 AUTH is signed on a worker thread, GetPublicKey is an async
   * D-Bus call; both complete on the engine context. Engine thread only. */
  GCancellable *cancel;      /* cancelled when the engine stops */
  guint signer_calls;        /* in flight */
  gboolean probe_inflight;
  guint probe_gen;           /* GetPublicKey calls started */
  guint last_ok_gen;         /* generation of the last successful one */
  gint64 probe_started;      /* wall s the in-flight / last call started */
  /* Events by an unknown author waiting for an answer: id -> probe_gen
   * when first parked. An answer only decides the events parked before
   * that call started (a newer key may have signed a later event). */
  GHashTable *parked;
  gint64 last_probe_ok;      /* start (wall s) of the last successful call */
  gint64 next_probe;
  guint probe_failures;

  /* Accounts (under lock: the relay and D-Bus threads read them). */
  GMutex lock;
  GPtrArray *accounts;       /* hex, learned from the signer (persisted) */
  char *detail;
};

static gint64 wall_now(void) { return g_get_real_time() / G_USEC_PER_SEC; }

static void attempt_free(gpointer p) {
  Attempt *a = p;
  if (!a) return;
  g_free(a->event_id);
  g_free(a->json);
  g_free(a);
}

static void set_detail(NsrFederation *f, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static void set_detail(NsrFederation *f, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char *d = g_strdup_vprintf(fmt, ap);
  va_end(ap);
  g_mutex_lock(&f->lock);
  g_free(f->detail);
  f->detail = d;
  g_mutex_unlock(&f->lock);
}

static void notify(NsrFederation *f, const char *id, const char *relay, const char *tstate,
                   const char *reason, const char *estate) {
  if (f->observer)
    f->observer(id, relay ? relay : "", tstate ? tstate : "", reason ? reason : "",
                estate ? estate : "", f->observer_ud);
}

/* ── Session bus / signer (engine thread) ─────────────────────────────── */

static GDBusConnection *get_bus(NsrFederation *f) {
  if (f->bus || f->bus_tried) return f->bus;
  f->bus_tried = TRUE;
  GError *err = NULL;
  f->bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  if (!f->bus) {
    g_message("nostr-session-relayd: federation: no session bus: %s", err->message);
    g_error_free(err);
  }
  return f->bus;
}

/* ── Accounts ─────────────────────────────────────────────────────────── */

static gboolean accounts_known(NsrFederation *f) {
  if (f->cfg.n_accounts > 0) return TRUE;
  g_mutex_lock(&f->lock);
  gboolean k = f->accounts->len > 0;
  g_mutex_unlock(&f->lock);
  return k;
}

static gboolean is_account(NsrFederation *f, const char *pk) {
  if (f->cfg.n_accounts > 0) return nsr_fed_config_has_account(&f->cfg, pk);
  gboolean yes = FALSE;
  g_mutex_lock(&f->lock);
  for (guint i = 0; i < f->accounts->len && !yes; i++)
    yes = g_ascii_strcasecmp(g_ptr_array_index(f->accounts, i), pk) == 0;
  g_mutex_unlock(&f->lock);
  return yes;
}

static void pump(NsrFederation *f);

static void probe_failed(NsrFederation *f, gint64 now, const char *why) {
  guint shift = MIN(f->probe_failures, 6u);
  f->probe_failures++;
  f->next_probe = now + MIN(300, 5 << shift);
  set_detail(f, "waiting for the local account: org.nostr.Signer: %s", why);
}

static void on_probe_done(GObject *src, GAsyncResult *res, gpointer ud) {
  NsrFederation *f = ud;
  GError *err = NULL;
  GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
  f->probe_inflight = FALSE;
  f->signer_calls--;
  if (g_atomic_int_get(&f->stopping)) {
    if (r) g_variant_unref(r);
    g_clear_error(&err);
    return;
  }
  gint64 now = wall_now();
  const char *pk = NULL;
  char hex[65];
  if (r) g_variant_get(r, "(&s)", &pk);
  if (r && pk && nsr_fed_parse_pubkey(pk, hex) == 0) {
    g_mutex_lock(&f->lock);
    gboolean have = FALSE;
    for (guint i = 0; i < f->accounts->len && !have; i++)
      have = strcmp(g_ptr_array_index(f->accounts, i), hex) == 0;
    if (!have) g_ptr_array_add(f->accounts, g_strdup(hex));
    f->last_probe_ok = f->probe_started;
    g_mutex_unlock(&f->lock);
    f->last_ok_gen = f->probe_gen;
    f->probe_failures = 0;
    f->next_probe = now + ACCOUNT_REPROBE_MIN_S;
    nsr_outbox_add_account(f->outbox, hex);
    set_detail(f, "local account from org.nostr.Signer (%.12s…)", hex);
  } else {
    probe_failed(f, now, err ? err->message : "GetPublicKey returned an unusable key");
  }
  if (r) g_variant_unref(r);
  g_clear_error(&err);
  nsr_outbox_reroute_held(f->outbox);
  pump(f);
}

/* Ask org.nostr.Signer.GetPublicKey, asynchronously. Only runs when an
 * event by an unknown author is waiting, i.e. an app just published
 * something (and so just used the signer); may D-Bus-activate it. */
static void start_probe(NsrFederation *f, gint64 now) {
  GDBusConnection *bus = f->dbus_signer ? get_bus(f) : NULL;
  if (!bus) {
    probe_failed(f, now, !f->dbus_signer ? "federation_accounts is empty and the signer is disabled"
                                         : "no session bus");
    return;
  }
  /* The first call may D-Bus-activate the signer: give it time. */
  int timeout_ms = f->last_probe_ok == 0 && f->probe_failures == 0 ? SIGNER_TIMEOUT_S * 1000
                                                                   : 10000;
  f->probe_inflight = TRUE;
  f->probe_gen++;
  f->probe_started = now;
  f->signer_calls++;
  g_dbus_connection_call(bus, SIGNER_BUS, SIGNER_PATH, SIGNER_IFACE, "GetPublicKey", NULL,
                         G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, timeout_ms, f->cancel,
                         on_probe_done, f);
}

/* ── Relay-list lookups for routing (engine thread) ───────────────────── */

static char *lk_relay_list(void *ud, const char *pubkey, int kind) {
  NsrFederation *f = ud;
  char *json = nsr_outbox_hint_json(f->outbox, pubkey, kind);
  if (json || !f->storage || !f->storage->vt || !f->storage->vt->query ||
      !f->storage->vt->query_next)
    return json;
  NostrStorage *st = f->storage;
  NostrFilter *flt = nostr_filter_new();
  nostr_filter_add_author(flt, pubkey);
  nostr_filter_add_kind(flt, kind);
  int err = 0;
  void *it = st->vt->query(st, flt, 1, 1, 0, 0, &err);
  if (it) {
    NostrEvent *ev = nostr_event_new();
    size_t n = 1;
    if (st->vt->query_next(st, it, ev, &n) == 0 && n == 1) {
      char *s = nostr_event_serialize(ev);
      if (s) json = g_strdup(s);
      free(s);
    }
    nostr_event_free(ev);
    if (st->vt->query_free) st->vt->query_free(st, it);
  }
  nostr_filter_free(flt);
  return json;
}

static GStrv lk_acked(void *ud, const char *ref, gboolean coord) {
  NsrFederation *f = ud;
  return nsr_outbox_acked_relays(f->outbox, ref, coord);
}

/* ── Routing ──────────────────────────────────────────────────────────── */

static void route_one(NsrFederation *f, NsrOutboxEvent *e, gint64 now) {
  NostrEvent *ev = nostr_event_new();
  if (nostr_event_deserialize(ev, e->json) != 0) {
    nsr_outbox_set_final(f->outbox, e->id, "skipped", "invalid: unparsable event", now);
    notify(f, e->id, "", "", "invalid: unparsable event", "skipped");
    nostr_event_free(ev);
    return;
  }
  NostrTags *tags = nostr_event_get_tags(ev);
  /* Re-check: the config may have changed since the event was queued. */
  NsrFedVerdict v = nsr_fed_static_verdict(&f->cfg, e->kind, tags);
  if (v != NSR_FED_FORWARD) {
    nsr_outbox_set_final(f->outbox, e->id, "skipped", nsr_fed_verdict_reason(v), now);
    notify(f, e->id, "", "", nsr_fed_verdict_reason(v), "skipped");
    nostr_event_free(ev);
    return;
  }
  if (e->kind != 1059 && !is_account(f, e->pubkey)) {
    /* Decided when the signer answered after the event was queued:
     * strictly after in wall seconds (an answer in the same second may
     * predate it), or by a call started after it was parked here. */
    gpointer gen = NULL;
    gboolean parked = g_hash_table_lookup_extended(f->parked, e->id, NULL, &gen);
    gboolean decided = f->cfg.n_accounts > 0 || f->last_probe_ok > e->enqueued_at ||
                       (parked && f->last_ok_gen > GPOINTER_TO_UINT(gen));
    if (decided) {
      g_hash_table_remove(f->parked, e->id);
      const char *why = "not forwarded: the author is not a local account";
      nsr_outbox_set_final(f->outbox, e->id, "skipped", why, now);
      notify(f, e->id, "", "", why, "skipped");
    } else {
      if (!parked)
        g_hash_table_insert(f->parked, g_strdup(e->id), GUINT_TO_POINTER(f->probe_gen));
      if (!f->probe_inflight && now >= f->next_probe) start_probe(f, now);
      g_mutex_lock(&f->lock);
      char *why = g_strdup(f->detail ? f->detail : "waiting for the local account");
      g_mutex_unlock(&f->lock);
      nsr_outbox_set_unroutable(f->outbox, e->id, why,
                                f->probe_inflight ? now + PROBE_WAIT_S : f->next_probe, TRUE, NULL,
                                now);
      g_free(why);
    }
    nostr_event_free(ev);
    return;
  }
  g_hash_table_remove(f->parked, e->id);
  /* NIP-09: a deletion cancels what it deletes while still queued. */
  if (e->kind == 5) {
    size_t n = tags ? nostr_tags_size(tags) : 0;
    for (size_t i = 0; i < n; i++) {
      NostrTag *t = nostr_tags_get(tags, i);
      const char *k = t ? nostr_tag_get_key(t) : NULL;
      if (!k || nostr_tag_size(t) < 2) continue;
      if (strcmp(k, "e") == 0)
        nsr_outbox_cancel_ref(f->outbox, e->pubkey, nostr_tag_get(t, 1), FALSE, now);
      else if (strcmp(k, "a") == 0)
        nsr_outbox_cancel_ref(f->outbox, e->pubkey, nostr_tag_get(t, 1), TRUE, now);
    }
  }
  if (e->replace_key)
    nsr_outbox_supersede(f->outbox, e->replace_key, e->id, nostr_event_get_created_at(ev), now);

  NsrFedLookup lk = {lk_relay_list, lk_acked, f};
  GStrv relays = NULL;
  NsrFedLane lane = NSR_FED_LANE_IDENTIFIED;
  char *reason = NULL;
  NsrFedBasis basis;
  switch (nsr_fed_resolve(&f->cfg, ev, &lk, &relays, &lane, &reason, &basis)) {
    case NSR_FED_ROUTE_OK:
      if (nsr_outbox_set_routed(f->outbox, e->id, (const char *const *)relays, lane, &basis,
                                now) == 0)
        for (guint i = 0; relays[i]; i++) notify(f, e->id, relays[i], "pending", "", "pending");
      break;
    case NSR_FED_ROUTE_UNROUTABLE:
      nsr_outbox_set_unroutable(f->outbox, e->id, reason,
                                now + nsr_fed_backoff_delay(&f->cfg, e->route_attempts + 1,
                                                            g_random_double()),
                                FALSE, &basis, now);
      notify(f, e->id, "", "", reason, "unroutable");
      break;
    case NSR_FED_ROUTE_INVALID:
      nsr_outbox_set_final(f->outbox, e->id, "failed", reason, now);
      notify(f, e->id, "", "", reason, "failed");
      break;
  }
  g_strfreev(relays);
  g_free(reason);
  nostr_event_free(ev);
}

static void route_pending(NsrFederation *f, gint64 now) {
  GPtrArray *evs = nsr_outbox_take_routable(f->outbox, now, 128);
  for (guint i = 0; i < evs->len && !g_atomic_int_get(&f->stopping); i++)
    route_one(f, g_ptr_array_index(evs, i), now);
  g_ptr_array_unref(evs);
}

/* ── Connections ──────────────────────────────────────────────────────── */

static void conn_pump(RelayConn *rc);
static void conn_fail(RelayConn *rc, const char *why);

static gboolean conn_has_work(RelayConn *rc) {
  return g_hash_table_size(rc->inflight) > 0 || rc->queued.length > 0 ||
         g_hash_table_size(rc->await_auth) > 0;
}

static gboolean conn_tracks(RelayConn *rc, const char *id) {
  if (g_hash_table_contains(rc->inflight, id) || g_hash_table_contains(rc->await_auth, id))
    return TRUE;
  for (GList *l = rc->queued.head; l; l = l->next)
    if (strcmp(((Attempt *)l->data)->event_id, id) == 0) return TRUE;
  return FALSE;
}

static void conn_set_error(RelayConn *rc, const char *msg) {
  g_mutex_lock(&rc->fed->lock);
  g_free(rc->last_error);
  rc->last_error = g_strdup(msg ? msg : "");
  g_mutex_unlock(&rc->fed->lock);
}

/* Settle @a (owned; freed here) for this relay in the outbox. */
static void settle(RelayConn *rc, Attempt *a, NsrOutboxResult res, const char *reason) {
  NsrFederation *f = rc->fed;
  gint64 now = wall_now();
  const char *ts = NULL, *es = NULL;
  if (nsr_outbox_record(f->outbox, &f->cfg, a->event_id, rc->url, res, reason,
                        g_random_double(), now, &ts, &es) == 0)
    notify(f, a->event_id, rc->url, ts, reason, es);
  if (res == NSR_OUTBOX_ACKED) {
    g_mutex_lock(&f->lock);
    rc->last_ok_at = now;
    g_mutex_unlock(&f->lock);
  } else {
    conn_set_error(rc, reason);
  }
  rc->last_activity = now;
  attempt_free(a);
}

static gboolean send_event(RelayConn *rc, Attempt *a) {
  char *frame = g_strdup_printf("[\"EVENT\",%s]", a->json);
  GError *err = NULL;
  gboolean ok = nostr_publish_transport_send_frame(rc->t, frame, &err);
  g_free(frame);
  if (!ok) {
    char *why = g_strdup_printf("send failed: %s", err ? err->message : "?");
    g_clear_error(&err);
    settle(rc, a, NSR_OUTBOX_TRANSIENT, why);
    g_free(why);
    return FALSE;
  }
  g_hash_table_replace(rc->inflight, a->event_id, a);
  rc->last_activity = wall_now();
  return TRUE;
}

/* Release a transport. Safe from inside its own callbacks: the callbacks
 * are detached, disconnect() cancels the transport's own reconnect timer
 * (the engine owns retry policy), and the last unref is deferred. */
static gboolean unref_transport_idle(gpointer p) {
  nostr_publish_transport_unref(p);
  return G_SOURCE_REMOVE;
}

static void conn_drop_transport(RelayConn *rc) {
  if (!rc->t) return;
  NostrPublishTransport *t = rc->t;
  rc->t = NULL;
  nostr_publish_transport_set_state_callback(t, NULL, NULL);
  nostr_publish_transport_set_ok_callback(t, NULL, NULL);
  nostr_publish_transport_set_auth_callback(t, NULL, NULL);
  nostr_publish_transport_disconnect(t);
  GSource *s = g_idle_source_new();
  g_source_set_callback(s, unref_transport_idle, t, NULL);
  g_source_attach(s, rc->fed->ctx);
  g_source_unref(s);
  g_mutex_lock(&rc->fed->lock);
  rc->open = FALSE;
  rc->authed = FALSE;
  g_mutex_unlock(&rc->fed->lock);
  g_clear_pointer(&rc->challenge, g_free);
  g_clear_pointer(&rc->auth_event_id, g_free);
}

static void fail_table(RelayConn *rc, GHashTable *tbl, NsrOutboxResult res, const char *why) {
  GHashTableIter it;
  gpointer k, v;
  GPtrArray *all = g_ptr_array_new();
  g_hash_table_iter_init(&it, tbl);
  while (g_hash_table_iter_next(&it, &k, &v)) {
    g_ptr_array_add(all, v);
    g_hash_table_iter_steal(&it);
  }
  for (guint i = 0; i < all->len; i++) settle(rc, g_ptr_array_index(all, i), res, why);
  g_ptr_array_unref(all);
}

static void conn_fail(RelayConn *rc, const char *why) {
  conn_drop_transport(rc);
  fail_table(rc, rc->inflight, NSR_OUTBOX_TRANSIENT, why);
  fail_table(rc, rc->await_auth, NSR_OUTBOX_TRANSIENT, why);
  Attempt *a;
  while ((a = g_queue_pop_head(&rc->queued))) settle(rc, a, NSR_OUTBOX_TRANSIENT, why);
  conn_set_error(rc, why);
}

/* NIP-42 AUTH signing on a worker thread (nostrc-8cc1): org.nostr.Signer
 * may prompt for up to SIGNER_TIMEOUT_S, and creating its proxy may
 * D-Bus-activate it; neither may stall the other upstream connections.
 * One short-lived thread per AUTH (rare: once per connection), not the
 * shared GTask pool: a prompt would pin a pool thread, and per-thread state
 * (the D-Bus call's context, crypto RNG) is released when it exits. The job
 * owns everything it touches; the completion runs on the engine context
 * and finds the connection again by key. */
typedef struct {
  NsrFederation *f;            /* completion only (engine context) */
  GMainContext *ctx;           /* ref: the engine's */
  GCancellable *cancel;        /* ref */
  NostrPublishSigner *signer;  /* ref; NULL: create it on the worker */
  GDBusConnection *bus;        /* ref, for creating the signer */
  char *app_id, *conn_key, *challenge, *unsigned_json;
  NostrPublishSigner *created; /* out */
  char *signed_json;           /* out */
  GError *error;               /* out */
  gboolean proxy_failed;       /* out: the signer could not be reached */
} AuthJob;

static void auth_job_free(gpointer p) {
  AuthJob *j = p;
  if (j->ctx) g_main_context_unref(j->ctx);
  g_clear_object(&j->cancel);
  if (j->signer) nostr_publish_signer_unref(j->signer);
  if (j->created) nostr_publish_signer_unref(j->created);
  g_clear_object(&j->bus);
  g_free(j->app_id);
  g_free(j->conn_key);
  g_free(j->challenge);
  g_free(j->unsigned_json);
  g_free(j->signed_json);
  g_clear_error(&j->error);
  g_free(j);
}

static gboolean on_auth_signed(gpointer p);

static gpointer auth_thread(gpointer p) {
  AuthJob *j = p;
  NostrPublishSigner *s = j->signer;
  if (!s) {
    s = j->created = nostr_publish_signer_new_dbus(j->bus, j->app_id, &j->error);
    j->proxy_failed = s == NULL;
  }
  if (s)
    j->signed_json = nostr_publish_signer_sign_event_json(s, j->unsigned_json, j->cancel, &j->error);
  g_main_context_invoke_full(j->ctx, G_PRIORITY_DEFAULT, on_auth_signed, j, auth_job_free);
  return NULL;
}

static void json_str(GString *s, const char *v) {
  g_string_append_c(s, '"');
  for (const unsigned char *c = (const unsigned char *)v; *c; c++) {
    if (*c == '"' || *c == '\\') {
      g_string_append_c(s, '\\');
      g_string_append_c(s, (char)*c);
    } else if (*c < 0x20) {
      g_string_append_printf(s, "\\u%04x", *c);
    } else {
      g_string_append_c(s, (char)*c);
    }
  }
  g_string_append_c(s, '"');
}

/* Unsigned kind-22242 event (the shape nostr_publish_signer_sign_auth_event
 * builds; that call cannot be cancelled). */
static char *auth_event_json(const char *relay_url, const char *challenge, gint64 created_at) {
  GString *s = g_string_new(NULL);
  g_string_append_printf(s, "{\"kind\":22242,\"created_at\":%" G_GINT64_FORMAT
                            ",\"content\":\"\",\"tags\":[[\"relay\",", created_at);
  json_str(s, relay_url);
  g_string_append(s, "],[\"challenge\",");
  json_str(s, challenge);
  g_string_append(s, "]]}");
  return g_string_free(s, FALSE);
}

static void start_auth(RelayConn *rc);

/* Engine context; @p (AuthJob) is freed after it returns. */
static gboolean on_auth_signed(gpointer p) {
  AuthJob *j = p;
  NsrFederation *f = j->f;
  f->signer_calls--;
  if (g_atomic_int_get(&f->stopping)) return G_SOURCE_REMOVE;
  if (j->created && !f->signer) f->signer = g_steal_pointer(&j->created);
  RelayConn *rc = g_hash_table_lookup(f->conns, j->conn_key);
  if (!rc) return G_SOURCE_REMOVE;
  rc->auth_signing = FALSE;
  if (!rc->t) return G_SOURCE_REMOVE; /* the connection dropped meanwhile: attempts already settled */
  if (g_strcmp0(rc->challenge, j->challenge) != 0) {
    start_auth(rc); /* reconnected meanwhile: sign the new challenge */
    return G_SOURCE_REMOVE;
  }
  if (!j->signed_json) {
    NsrOutboxResult r = nostr_publish_signer_error_is_permanent(j->error) ? NSR_OUTBOX_PERMANENT
                                                                         : NSR_OUTBOX_TRANSIENT;
    char *msg = j->proxy_failed
                    ? g_strdup_printf("auth-required: cannot answer NIP-42 AUTH (org.nostr.Signer: %s)",
                                      j->error ? j->error->message : "?")
                    : g_strdup_printf("auth-required: signing NIP-42 AUTH failed: %s",
                                      j->error ? j->error->message : "?");
    fail_table(rc, rc->await_auth, r, msg);
    g_free(msg);
    conn_pump(rc);
    return G_SOURCE_REMOVE;
  }
  NostrEvent *ev = nostr_event_new();
  char *auth_id = NULL;
  if (nostr_event_deserialize(ev, j->signed_json) == 0) auth_id = nostr_event_get_id(ev);
  nostr_event_free(ev);
  char *frame = g_strdup_printf("[\"AUTH\",%s]", j->signed_json);
  if (!auth_id || !nostr_publish_transport_send_frame(rc->t, frame, NULL)) {
    g_free(frame);
    free(auth_id);
    fail_table(rc, rc->await_auth, NSR_OUTBOX_TRANSIENT, "auth-required: sending AUTH failed");
    conn_pump(rc);
    return G_SOURCE_REMOVE;
  }
  g_free(frame);
  rc->auth_event_id = g_strdup(auth_id);
  free(auth_id);
  /* Give the resend a full OK window after the prompt. */
  gint64 deadline = wall_now() + f->cfg.ok_timeout_seconds;
  GHashTableIter it;
  gpointer k, v;
  g_hash_table_iter_init(&it, rc->await_auth);
  while (g_hash_table_iter_next(&it, &k, &v)) ((Attempt *)v)->deadline = deadline;
  return G_SOURCE_REMOVE;
}

static void start_auth(RelayConn *rc) {
  NsrFederation *f = rc->fed;
  if (rc->auth_event_id || rc->auth_signing || !rc->challenge || !rc->t ||
      g_hash_table_size(rc->await_auth) == 0)
    return;
  AuthJob *j = g_new0(AuthJob, 1);
  if (f->signer) {
    j->signer = nostr_publish_signer_ref(f->signer);
  } else if (f->dbus_signer && get_bus(f)) {
    j->bus = g_object_ref(f->bus);
  } else {
    fail_table(rc, rc->await_auth, NSR_OUTBOX_TRANSIENT,
               f->dbus_signer ? "auth-required: cannot answer NIP-42 AUTH (no session bus for "
                                "org.nostr.Signer)"
                              : "auth-required: cannot answer NIP-42 AUTH (no signer configured)");
    auth_job_free(j);
    return;
  }
  j->app_id = g_strdup(f->app_id);
  j->conn_key = g_strdup(rc->key);
  j->challenge = g_strdup(rc->challenge);
  j->unsigned_json = auth_event_json(rc->url, rc->challenge, wall_now());
  rc->auth_signing = TRUE;
  f->signer_calls++;
  /* The signer may prompt: the waiting attempts outlive it. */
  gint64 deadline = wall_now() + SIGNER_TIMEOUT_S + f->cfg.ok_timeout_seconds;
  GHashTableIter it;
  gpointer k, v;
  g_hash_table_iter_init(&it, rc->await_auth);
  while (g_hash_table_iter_next(&it, &k, &v))
    ((Attempt *)v)->deadline = MAX(((Attempt *)v)->deadline, deadline);
  j->f = f;
  j->ctx = g_main_context_ref(f->ctx);
  j->cancel = g_object_ref(f->cancel);
  GError *err = NULL;
  GThread *th = g_thread_try_new("nsr-auth-sign", auth_thread, j, &err);
  if (!th) {
    j->error = err;
    g_main_context_invoke_full(f->ctx, G_PRIORITY_DEFAULT, on_auth_signed, j, auth_job_free);
    return;
  }
  g_thread_unref(th); /* runs detached; completes through the engine context */
}

static void on_auth_ok(RelayConn *rc, gboolean accepted, const char *reason) {
  g_clear_pointer(&rc->auth_event_id, g_free);
  if (!accepted) {
    char *msg = g_strdup_printf("auth-required: relay rejected our NIP-42 AUTH: %s",
                                reason ? reason : "");
    fail_table(rc, rc->await_auth, NSR_OUTBOX_TRANSIENT, msg);
    g_free(msg);
    return;
  }
  g_mutex_lock(&rc->fed->lock);
  rc->authed = TRUE;
  g_mutex_unlock(&rc->fed->lock);
  GHashTableIter it;
  gpointer k, v;
  GPtrArray *resend = g_ptr_array_new();
  g_hash_table_iter_init(&it, rc->await_auth);
  while (g_hash_table_iter_next(&it, &k, &v)) {
    g_ptr_array_add(resend, v);
    g_hash_table_iter_steal(&it);
  }
  for (guint i = 0; i < resend->len && rc->t; i++) send_event(rc, g_ptr_array_index(resend, i));
  for (guint i = 0; i < resend->len && !rc->t; i++) {
    /* connection vanished mid-loop: the rest go back through the outbox */
    Attempt *a = g_ptr_array_index(resend, i);
    if (!g_hash_table_contains(rc->inflight, a->event_id))
      settle(rc, a, NSR_OUTBOX_TRANSIENT, "connection lost");
  }
  g_ptr_array_unref(resend);
}

static void on_transport_ok(NostrPublishTransport *t, const gchar *event_id, gboolean accepted,
                            const gchar *reason, gpointer user_data) {
  (void)t;
  RelayConn *rc = user_data;
  if (rc->auth_event_id && g_strcmp0(event_id, rc->auth_event_id) == 0) {
    on_auth_ok(rc, accepted, reason);
    conn_pump(rc);
    return;
  }
  gpointer key = NULL, val = NULL;
  if (!g_hash_table_lookup_extended(rc->inflight, event_id, &key, &val)) return; /* late */
  g_hash_table_steal(rc->inflight, event_id);
  Attempt *a = val;
  switch (nsr_fed_classify_ok(accepted, reason)) {
    case NSR_FED_OK_ACCEPTED:
      settle(rc, a, NSR_OUTBOX_ACKED, reason);
      break;
    case NSR_FED_OK_AUTH_REQUIRED:
      if (rc->lane == NSR_FED_LANE_ANONYMOUS) {
        char *msg = g_strdup_printf(
            "auth-refused-for-gift-wrap: the inbox relay demands NIP-42 AUTH; gift wraps are "
            "never delivered authenticated (sender privacy). Relay said: %s", reason);
        settle(rc, a, NSR_OUTBOX_PERMANENT, msg);
        g_free(msg);
      } else if (rc->authed) {
        settle(rc, a, NSR_OUTBOX_PERMANENT, reason);
      } else {
        g_hash_table_replace(rc->await_auth, a->event_id, a);
        start_auth(rc);
      }
      break;
    case NSR_FED_OK_PERMANENT:
      settle(rc, a, NSR_OUTBOX_PERMANENT, reason);
      break;
    case NSR_FED_OK_TRANSIENT:
    default:
      settle(rc, a, NSR_OUTBOX_TRANSIENT, reason ? reason : "rejected without a reason");
      break;
  }
  conn_pump(rc);
}

static gchar *on_transport_auth(NostrPublishTransport *t, const gchar *challenge,
                                gpointer user_data) {
  (void)t;
  RelayConn *rc = user_data;
  /* Lazy NIP-42: remember the challenge; authenticate only once an EVENT
   * is refused with auth-required (and never on the anonymous lane). */
  g_free(rc->challenge);
  rc->challenge = g_strdup(challenge);
  if (rc->lane == NSR_FED_LANE_IDENTIFIED) start_auth(rc);
  return NULL;
}

static void on_transport_state(NostrPublishTransport *t, gboolean connected,
                               const GError *error, gpointer user_data) {
  (void)t;
  RelayConn *rc = user_data;
  if (connected) {
    g_mutex_lock(&rc->fed->lock);
    rc->open = TRUE;
    g_mutex_unlock(&rc->fed->lock);
    rc->last_activity = wall_now();
    conn_pump(rc);
    return;
  }
  gboolean was_open = rc->open;
  char *why = g_strdup_printf("%s: %s", was_open ? "connection lost" : "connect failed",
                              error ? error->message : "closed by peer");
  conn_fail(rc, why);
  g_free(why);
}

static void conn_connect(RelayConn *rc) {
  if (rc->t) return;
  rc->t = nostr_publish_transport_new_websocket(rc->url);
  nostr_publish_transport_set_state_callback(rc->t, on_transport_state, rc);
  nostr_publish_transport_set_ok_callback(rc->t, on_transport_ok, rc);
  nostr_publish_transport_set_auth_callback(rc->t, on_transport_auth, rc);
  rc->last_activity = wall_now();
  nostr_publish_transport_connect_async(rc->t);
}

/* Move queued attempts onto the wire while the relay has room. */
static void conn_pump(RelayConn *rc) {
  if (!rc->queued.length) return;
  if (!rc->t) {
    conn_connect(rc);
    return;
  }
  if (!rc->open) return;
  guint cap = (guint)rc->fed->cfg.max_inflight_per_relay;
  while (rc->t && rc->queued.length &&
         g_hash_table_size(rc->inflight) + g_hash_table_size(rc->await_auth) < cap)
    send_event(rc, g_queue_pop_head(&rc->queued));
}

static void conn_free(gpointer p) {
  RelayConn *rc = p;
  conn_drop_transport(rc);
  g_hash_table_unref(rc->inflight);
  g_hash_table_unref(rc->await_auth);
  g_queue_clear_full(&rc->queued, attempt_free);
  g_free(rc->key);
  g_free(rc->url);
  g_free(rc->challenge);
  g_free(rc->auth_event_id);
  g_free(rc->last_error);
  g_free(rc);
}

static RelayConn *conn_get(NsrFederation *f, NsrFedLane lane, const char *url) {
  char *key = g_strdup_printf("%d|%s", (int)lane, url);
  RelayConn *rc = g_hash_table_lookup(f->conns, key);
  if (rc) {
    g_free(key);
    return rc;
  }
  rc = g_new0(RelayConn, 1);
  rc->fed = f;
  rc->key = key;
  rc->url = g_strdup(url);
  rc->lane = lane;
  /* Values are owned by whichever table/queue holds them; keys borrow
   * Attempt::event_id. */
  rc->inflight = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, attempt_free);
  rc->await_auth = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, attempt_free);
  g_queue_init(&rc->queued);
  rc->last_error = g_strdup("");
  g_mutex_lock(&f->lock);
  g_hash_table_insert(f->conns, rc->key, rc);
  g_mutex_unlock(&f->lock);
  return rc;
}

static void dispatch_due(NsrFederation *f, gint64 now) {
  /* The outbox lease only matters after a crash (a live engine always
   * records a result first). It is deliberately NOT extended when an AUTH
   * prompt pushes an attempt's in-memory deadline: a lapsed lease just
   * makes the row due again, and conn_tracks() keeps a still-running
   * attempt from being dispatched twice. */
  int lease = 2 * f->cfg.ok_timeout_seconds + 30;
  GPtrArray *due = nsr_outbox_take_due(f->outbox, now, lease, 256);
  for (guint i = 0; i < due->len; i++) {
    NsrOutboxDue *d = g_ptr_array_index(due, i);
    RelayConn *rc = conn_get(f, d->lane, d->relay);
    if (conn_tracks(rc, d->event_id)) continue;
    Attempt *a = g_new0(Attempt, 1);
    a->event_id = g_strdup(d->event_id);
    a->json = g_strdup(d->json);
    a->deadline = now + f->cfg.ok_timeout_seconds;
    g_queue_push_tail(&rc->queued, a);
  }
  g_ptr_array_unref(due);
  GHashTableIter it;
  gpointer k, v;
  GPtrArray *all = g_ptr_array_new();
  g_hash_table_iter_init(&it, f->conns);
  while (g_hash_table_iter_next(&it, &k, &v)) g_ptr_array_add(all, v);
  for (guint i = 0; i < all->len; i++) conn_pump(g_ptr_array_index(all, i));
  g_ptr_array_unref(all);
}

/* Expire attempts past their deadline; returns the earliest remaining
 * deadline or idle-disconnect time (G_MAXINT64 if none). */
static gint64 sweep_conns(NsrFederation *f, gint64 now) {
  gint64 next = G_MAXINT64;
  GHashTableIter it;
  gpointer k, v;
  GPtrArray *all = g_ptr_array_new();
  g_hash_table_iter_init(&it, f->conns);
  while (g_hash_table_iter_next(&it, &k, &v)) g_ptr_array_add(all, v);
  for (guint i = 0; i < all->len; i++) {
    RelayConn *rc = g_ptr_array_index(all, i);
    /* Queued past the deadline: the connection never came up. */
    Attempt *head = g_queue_peek_head(&rc->queued);
    if (head && head->deadline <= now && !rc->open) {
      conn_fail(rc, "connect timed out");
      continue;
    }
    GPtrArray *late = g_ptr_array_new();
    GHashTable *tables[2] = {rc->inflight, rc->await_auth};
    for (int t = 0; t < 2; t++) {
      GHashTableIter jt;
      gpointer jk, jv;
      g_hash_table_iter_init(&jt, tables[t]);
      while (g_hash_table_iter_next(&jt, &jk, &jv)) {
        Attempt *a = jv;
        if (a->deadline <= now) {
          g_ptr_array_add(late, a);
          g_ptr_array_add(late, GINT_TO_POINTER(t));
          g_hash_table_iter_steal(&jt);
        } else if (a->deadline < next) {
          next = a->deadline;
        }
      }
    }
    for (guint j = 0; j < late->len; j += 2) {
      int t = GPOINTER_TO_INT(g_ptr_array_index(late, j + 1));
      settle(rc, g_ptr_array_index(late, j), NSR_OUTBOX_TRANSIENT,
             t == 0 ? "timed out waiting for OK"
                    : "auth-required: timed out waiting for NIP-42 AUTH");
    }
    g_ptr_array_unref(late);
    for (GList *l = rc->queued.head; l; l = l->next)
      next = MIN(next, ((Attempt *)l->data)->deadline);
    if (rc->t && !conn_has_work(rc)) {
      gint64 idle_at = rc->last_activity + f->cfg.idle_disconnect_seconds;
      if (idle_at <= now) conn_drop_transport(rc);
      else next = MIN(next, idle_at);
    }
    conn_pump(rc);
  }
  g_ptr_array_unref(all);
  return next;
}

/* ── Retargeting (nostrc-jedb) ────────────────────────────────────────── */

/* Forget a not-yet-sent attempt of @id on a relay the list dropped (one
 * already on the wire just has its late OK ignored). */
static void drop_queued(NsrFederation *f, NsrFedLane lane, const char *url, const char *id) {
  char *key = g_strdup_printf("%d|%s", (int)lane, url);
  RelayConn *rc = g_hash_table_lookup(f->conns, key);
  g_free(key);
  if (!rc) return;
  for (GList *l = rc->queued.head; l;) {
    GList *next = l->next;
    Attempt *a = l->data;
    if (strcmp(a->event_id, id) == 0) {
      g_queue_delete_link(&rc->queued, l);
      attempt_free(a);
    }
    l = next;
  }
}

/* A newer 10002 / 10050 re-resolves the deliveries routed from the old
 * one: relays it dropped lose their pending targets, relays it added get
 * one. Group writes are never retargeted (their relay is the group's
 * identity; nsr_fed_resolve() gives them no basis). */
static void retarget_pending(NsrFederation *f, gint64 now) {
  GPtrArray *evs = nsr_outbox_take_retarget(f->outbox, 128);
  for (guint i = 0; i < evs->len && !g_atomic_int_get(&f->stopping); i++) {
    NsrOutboxEvent *e = g_ptr_array_index(evs, i);
    NostrEvent *ev = nostr_event_new();
    if (nostr_event_deserialize(ev, e->json) != 0) {
      nostr_event_free(ev);
      continue;
    }
    NsrFedLookup lk = {lk_relay_list, lk_acked, f};
    GStrv relays = NULL;
    NsrFedLane lane = NSR_FED_LANE_IDENTIFIED;
    char *reason = NULL;
    if (nsr_fed_resolve(&f->cfg, ev, &lk, &relays, &lane, &reason, NULL) != NSR_FED_ROUTE_OK) {
      /* The new list names no usable relay (e.g. only read relays): keep
       * delivering where the author's previous list pointed. */
      g_message("nostr-session-relayd: federation: %.12s… keeps its targets: %s", e->id,
                reason ? reason : "no route");
    } else {
      GPtrArray *added = NULL, *dropped = NULL;
      const char *es = NULL;
      if (nsr_outbox_retarget(f->outbox, e->id, (const char *const *)relays, now, &added, &dropped,
                              &es) == 0) {
        for (guint j = 0; j < dropped->len; j++) {
          drop_queued(f, lane, dropped->pdata[j], e->id);
          notify(f, e->id, dropped->pdata[j], "cancelled", "dropped from the relay list", es);
        }
        for (guint j = 0; j < added->len; j++) notify(f, e->id, added->pdata[j], "pending", "", es);
      }
      g_ptr_array_unref(added);
      g_ptr_array_unref(dropped);
    }
    g_strfreev(relays);
    g_free(reason);
    nostr_event_free(ev);
  }
  g_ptr_array_unref(evs);
}

/* ── Pump & loop ──────────────────────────────────────────────────────── */

static gboolean on_timer(gpointer p);

static void rearm(NsrFederation *f, gint64 wake_at, gint64 now) {
  if (f->timer) {
    g_source_destroy(f->timer);
    g_source_unref(f->timer);
    f->timer = NULL;
  }
  if (g_atomic_int_get(&f->stopping)) return;
  gint64 delay_ms;
  if (wake_at <= now) delay_ms = 100;
  else delay_ms = MIN((wake_at - now) * 1000, 30 * 1000);
  f->timer = g_timeout_source_new((guint)delay_ms);
  g_source_set_callback(f->timer, on_timer, f, NULL);
  g_source_attach(f->timer, f->ctx);
}

static void pump(NsrFederation *f) {
  if (g_atomic_int_get(&f->stopping)) return;
  gint64 now = wall_now();
  route_pending(f, now);
  retarget_pending(f, now);
  if (now >= f->next_housekeeping) {
    int n = nsr_outbox_expire(f->outbox, now, f->cfg.max_age_seconds);
    if (n > 0) g_message("nostr-session-relayd: federation: %d queued event(s) expired", n);
    nsr_outbox_prune(f->outbox, now, f->cfg.keep_settled_seconds);
    f->next_housekeeping = now + HOUSEKEEPING_INTERVAL_S;
  }
  dispatch_due(f, now);
  now = wall_now();
  gint64 next = sweep_conns(f, now);
  next = MIN(next, nsr_outbox_next_due(f->outbox));
  next = MIN(next, f->next_housekeeping);
  rearm(f, next, now);
}

static gboolean on_timer(gpointer p) {
  NsrFederation *f = p;
  if (f->timer) {
    g_source_unref(f->timer);
    f->timer = NULL;
  }
  pump(f);
  return G_SOURCE_REMOVE;
}

static gboolean on_wake(gpointer p) {
  NsrFederation *f = p;
  g_atomic_int_set(&f->wake_pending, 0);
  pump(f);
  return G_SOURCE_REMOVE;
}

void nsr_federation_wake(NsrFederation *f) {
  if (!f || !f->ctx || g_atomic_int_get(&f->stopping)) return;
  if (g_atomic_int_compare_and_exchange(&f->wake_pending, 0, 1)) {
    GSource *s = g_idle_source_new();
    g_source_set_callback(s, on_wake, f, NULL);
    g_source_attach(s, f->ctx);
    g_source_unref(s);
  }
}

static void release_table(NsrFederation *f, RelayConn *rc, GHashTable *tbl, gint64 now) {
  GHashTableIter it;
  gpointer k, v;
  g_hash_table_iter_init(&it, tbl);
  while (g_hash_table_iter_next(&it, &k, &v))
    nsr_outbox_release_lease(f->outbox, ((Attempt *)v)->event_id, rc->url, now);
}

static gpointer engine_thread(gpointer p) {
  NsrFederation *f = p;
  g_main_context_push_thread_default(f->ctx);
  nsr_federation_wake(f);
  g_main_loop_run(f->loop);

  /* Shutdown: unfinished attempts become due again right away on the next
   * start (they stay pending in the outbox). */
  gint64 now = wall_now();
  if (f->timer) {
    g_source_destroy(f->timer);
    g_source_unref(f->timer);
    f->timer = NULL;
  }
  GHashTableIter it;
  gpointer k, v;
  g_hash_table_iter_init(&it, f->conns);
  while (g_hash_table_iter_next(&it, &k, &v)) {
    RelayConn *rc = v;
    release_table(f, rc, rc->inflight, now);
    release_table(f, rc, rc->await_auth, now);
    for (GList *l = rc->queued.head; l; l = l->next)
      nsr_outbox_release_lease(f->outbox, ((Attempt *)l->data)->event_id, rc->url, now);
  }
  g_mutex_lock(&f->lock);
  GHashTable *conns = f->conns;
  f->conns = g_hash_table_new(g_str_hash, g_str_equal);
  g_mutex_unlock(&f->lock);
  g_hash_table_unref(conns); /* conn_free drops transports (deferred unref) */
  /* Signer calls in flight: cancel them and collect their completions
   * (they see `stopping` and touch nothing). Creating the signer's proxy
   * cannot be cancelled, so this may wait for a D-Bus activation. */
  g_cancellable_cancel(f->cancel);
  while (f->signer_calls > 0) g_main_context_iteration(f->ctx, TRUE);
  if (f->signer) {
    nostr_publish_signer_unref(f->signer);
    f->signer = NULL;
  }
  while (g_main_context_iteration(f->ctx, FALSE)) {
  }
  g_clear_object(&f->bus);
  while (g_main_context_iteration(f->ctx, FALSE)) {
  }
  g_main_context_pop_thread_default(f->ctx);
  return NULL;
}

/* ── Public API ───────────────────────────────────────────────────────── */

NsrFederation *nsr_federation_new(const NsrFederationInit *init) {
  g_return_val_if_fail(init && init->cfg && init->outbox, NULL);
  NsrFederation *f = g_new0(NsrFederation, 1);
  f->cfg = *init->cfg;
  f->outbox = init->outbox;
  f->storage = init->storage;
  f->signer = init->signer ? nostr_publish_signer_ref(init->signer) : NULL;
  f->dbus_signer = init->dbus_signer;
  f->app_id = g_strdup(init->app_id ? init->app_id : NSR_FED_DEFAULT_APP_ID);
  g_mutex_init(&f->lock);
  f->accounts = g_ptr_array_new_with_free_func(g_free);
  if (f->cfg.n_accounts == 0) {
    GStrv known = nsr_outbox_get_accounts(f->outbox);
    for (guint i = 0; known && known[i]; i++) g_ptr_array_add(f->accounts, g_strdup(known[i]));
    g_strfreev(known);
  }
  if (f->cfg.n_accounts > 0)
    f->detail = g_strdup_printf("%zu local account(s) from federation_accounts",
                                f->cfg.n_accounts);
  else if (f->accounts->len > 0)
    f->detail = g_strdup_printf("%u local account(s) remembered from org.nostr.Signer",
                                f->accounts->len);
  else
    f->detail = g_strdup("local account not known yet (asks org.nostr.Signer on first event)");
  f->conns = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, conn_free);
  f->parked = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  f->cancel = g_cancellable_new();
  f->ctx = g_main_context_new();
  f->loop = g_main_loop_new(f->ctx, FALSE);
  return f;
}

void nsr_federation_set_observer(NsrFederation *f, NsrFedObserver cb, void *ud) {
  f->observer = cb;
  f->observer_ud = ud;
}

gboolean nsr_federation_start(NsrFederation *f, GError **error) {
  f->thread = g_thread_try_new("nsr-federation", engine_thread, f, error);
  return f->thread != NULL;
}

static gboolean quit_loop(gpointer p) {
  g_main_loop_quit(p);
  return G_SOURCE_REMOVE;
}

void nsr_federation_stop(NsrFederation *f) {
  if (!f) return;
  g_atomic_int_set(&f->stopping, 1);
  if (f->thread) {
    GSource *s = g_idle_source_new();
    g_source_set_priority(s, G_PRIORITY_HIGH);
    g_source_set_callback(s, quit_loop, f->loop, NULL);
    g_source_attach(s, f->ctx);
    g_source_unref(s);
    g_thread_join(f->thread);
    f->thread = NULL;
  }
}

void nsr_federation_free(NsrFederation *f) {
  if (!f) return;
  nsr_federation_stop(f);
  g_hash_table_unref(f->conns);
  if (f->signer) nostr_publish_signer_unref(f->signer);
  g_clear_object(&f->bus);
  g_main_loop_unref(f->loop);
  g_main_context_unref(f->ctx);
  g_ptr_array_unref(f->accounts);
  g_hash_table_unref(f->parked);
  g_object_unref(f->cancel);
  g_mutex_clear(&f->lock);
  g_free(f->detail);
  g_free(f->app_id);
  g_free(f);
}

/* Relay thread (nostrc-elgy): TRUE when @ev certainly is not a local
 * account's, so caching it costs no outbox row (and no fsync). Only an
 * authoritative federation_accounts decides that up front. With accounts
 * learned from org.nostr.Signer an unknown key may be an account the
 * signer will report later (the user switches back to it and republishes
 * old events): those stay queued and the engine decides, as the user's own
 * events must never be dropped silently. Gift wraps are signed by
 * throw-away keys: never decided here. */
static gboolean provably_foreign(NsrFederation *f, int kind, const char *pk) {
  if (kind == 1059 || !pk || f->cfg.n_accounts == 0) return FALSE;
  return !nsr_fed_config_has_account(&f->cfg, pk);
}

int nsr_federation_offer(NsrFederation *f, NostrEvent *ev) {
  if (!f || !ev) return 0;
  int kind = nostr_event_get_kind(ev);
  NostrTags *tags = nostr_event_get_tags(ev);
  gboolean hint = kind == 10002 || kind == 10050 || kind == 10009;
  gboolean enqueue = nsr_fed_static_verdict(&f->cfg, kind, tags) == NSR_FED_FORWARD;
  const char *pk = nostr_event_get_pubkey(ev);
  if (enqueue && provably_foreign(f, kind, pk)) enqueue = FALSE;
  if (!hint && !enqueue) return 0;
  char *id = nostr_event_get_id(ev);
  char *json = nostr_event_serialize(ev);
  char *rkey = nsr_fed_replace_key(kind, pk, tags);
  int rc = -1;
  if (id && json && pk) {
    NsrOutboxIngest in = {
        .id = id, .pubkey = pk, .kind = kind,
        .created_at = nostr_event_get_created_at(ev),
        .replace_key = rkey, .json = json, .enqueue = enqueue, .hint = hint,
    };
    GError *err = NULL;
    rc = nsr_outbox_ingest(f->outbox, &in, wall_now(), &err);
    if (rc != 0) {
      g_warning("nostr-session-relayd: federation: %s", err ? err->message : "outbox write failed");
      g_clear_error(&err);
    }
  }
  free(id);
  free(json);
  g_free(rkey);
  if (rc == 0) nsr_federation_wake(f);
  return rc;
}

void nsr_federation_status(NsrFederation *f, NsrFedStatus *out) {
  memset(out, 0, sizeof *out);
  out->state = accounts_known(f) ? "active" : "waiting-for-account";
  g_mutex_lock(&f->lock);
  out->detail = g_strdup(f->detail ? f->detail : "");
  GHashTableIter it;
  gpointer k, v;
  g_hash_table_iter_init(&it, f->conns);
  while (g_hash_table_iter_next(&it, &k, &v))
    if (((RelayConn *)v)->open) out->relays_connected++;
  g_mutex_unlock(&f->lock);
  nsr_outbox_stats(f->outbox, &out->outbox);
}

void nsr_federation_status_clear(NsrFedStatus *s) {
  g_clear_pointer(&s->detail, g_free);
  nsr_outbox_stats_clear(&s->outbox);
}

void nsr_fed_relay_info_free(gpointer p) {
  NsrFedRelayInfo *r = p;
  if (!r) return;
  g_free(r->url);
  g_free(r->last_error);
  g_free(r);
}

static gint cmp_info(gconstpointer a, gconstpointer b) {
  const NsrFedRelayInfo *x = *(NsrFedRelayInfo *const *)a, *y = *(NsrFedRelayInfo *const *)b;
  return strcmp(x->url, y->url);
}

GPtrArray *nsr_federation_relays(NsrFederation *f) {
  GHashTable *by_url = g_hash_table_new(g_str_hash, g_str_equal);
  GPtrArray *out = g_ptr_array_new_with_free_func(nsr_fed_relay_info_free);
  GPtrArray *counts = nsr_outbox_relay_counts(f->outbox);
  for (guint i = 0; i < counts->len; i++) {
    NsrOutboxRelayCounts *c = g_ptr_array_index(counts, i);
    NsrFedRelayInfo *r = g_new0(NsrFedRelayInfo, 1);
    r->url = g_strdup(c->relay);
    r->pending = c->pending;
    r->acked = c->acked;
    r->failed = c->failed;
    r->last_error = g_strdup("");
    g_ptr_array_add(out, r);
    g_hash_table_insert(by_url, r->url, r);
  }
  g_ptr_array_unref(counts);
  g_mutex_lock(&f->lock);
  GHashTableIter it;
  gpointer k, v;
  g_hash_table_iter_init(&it, f->conns);
  while (g_hash_table_iter_next(&it, &k, &v)) {
    RelayConn *rc = v;
    NsrFedRelayInfo *r = g_hash_table_lookup(by_url, rc->url);
    if (!r) {
      r = g_new0(NsrFedRelayInfo, 1);
      r->url = g_strdup(rc->url);
      r->last_error = g_strdup("");
      g_ptr_array_add(out, r);
      g_hash_table_insert(by_url, r->url, r);
    }
    r->connected = r->connected || rc->open;
    if (rc->lane == NSR_FED_LANE_IDENTIFIED) r->authed = rc->authed;
    r->last_ok_at = MAX(r->last_ok_at, rc->last_ok_at);
    if (rc->last_error && *rc->last_error &&
        (!*r->last_error || rc->lane == NSR_FED_LANE_IDENTIFIED)) {
      g_free(r->last_error);
      r->last_error = g_strdup(rc->last_error);
    }
  }
  g_mutex_unlock(&f->lock);
  g_hash_table_unref(by_url);
  g_ptr_array_sort(out, cmp_info);
  return out;
}

NsrOutbox *nsr_federation_get_outbox(NsrFederation *f) { return f->outbox; }
