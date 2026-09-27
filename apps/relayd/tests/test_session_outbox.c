/*
 * test_session_outbox — the federation outbox state machine (bead
 * nostrc-7d96): durable idempotent ingest, relay-list hints, routing
 * transitions, leases, per-relay results with backoff, expiry,
 * supersede / NIP-09 cancel, prune, counters, restart durability and the
 * retention hook.
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include <sqlite3.h>

#include "session_outbox.h"

#define PK "7e7e9c42a91bfef19fa929e5fda1b72e0ebc1a4c1141673e2794234d86addf4e"
#define PK2 "2222222222222222222222222222222222222222222222222222222222222222"
#define NOW 1700000000

typedef struct {
  gchar *dir;
  gchar *path;
  NsrOutbox *ob;
  NsrFedConfig cfg;
} Fx;

static void fx_open(Fx *f) {
  GError *err = NULL;
  f->ob = nsr_outbox_open(f->path, &err);
  g_assert_no_error(err);
  g_assert_nonnull(f->ob);
}

static void setup(Fx *f, gconstpointer d) {
  (void)d;
  f->dir = g_dir_make_tmp("nsr-outbox-XXXXXX", NULL);
  f->path = g_build_filename(f->dir, "outbox.sqlite3", NULL);
  nsr_fed_config_defaults(&f->cfg);
  f->cfg.backoff_initial_seconds = 10;
  f->cfg.backoff_max_seconds = 100;
  f->cfg.max_age_seconds = 1000;
  fx_open(f);
}

static void rm_rf(const char *dir) {
  GDir *d = g_dir_open(dir, 0, NULL);
  const char *n;
  while (d && (n = g_dir_read_name(d))) {
    gchar *p = g_build_filename(dir, n, NULL);
    g_unlink(p);
    g_free(p);
  }
  if (d) g_dir_close(d);
  g_rmdir(dir);
}

static void teardown(Fx *f, gconstpointer d) {
  (void)d;
  nsr_outbox_close(f->ob);
  rm_rf(f->dir);
  g_free(f->path);
  g_free(f->dir);
}

static char *id_n(int n) { return g_strdup_printf("%064x", n); }

static void ingest(Fx *f, const char *id, int kind, gint64 created, const char *rkey,
                   gboolean enqueue, gboolean hint, const char *json, gint64 now) {
  NsrOutboxIngest in = {.id = id, .pubkey = PK, .kind = kind, .created_at = created,
                        .replace_key = rkey, .json = json ? json : "{}",
                        .enqueue = enqueue, .hint = hint};
  GError *err = NULL;
  g_assert_cmpint(nsr_outbox_ingest(f->ob, &in, now, &err), ==, 0);
  g_assert_no_error(err);
}

static void hint(Fx *f, const char *pk, int kind, gint64 created, gint64 now) {
  char *id = g_strdup_printf("%s%d", pk, (int)created); /* any unique id */
  NsrOutboxIngest in = {.id = id, .pubkey = pk, .kind = kind, .created_at = created,
                        .json = "{}", .enqueue = FALSE, .hint = TRUE};
  GError *err = NULL;
  g_assert_cmpint(nsr_outbox_ingest(f->ob, &in, now, &err), ==, 0);
  g_assert_no_error(err);
  g_free(id);
}

static NsrFedBasis basis(const char *pk, int kind) {
  NsrFedBasis b;
  memset(&b, 0, sizeof b);
  g_strlcpy(b.pubkey, pk, sizeof b.pubkey);
  b.kind = kind;
  return b;
}

static char *target_of(Fx *f, const char *id, const char *relay, char **reason) {
  GPtrArray *t = NULL;
  char *st = NULL, *detail = NULL, *out = NULL;
  if (nsr_outbox_event_status(f->ob, id, &st, &detail, &t) == 0) {
    for (guint i = 0; i < t->len; i++) {
      NsrOutboxTargetInfo *x = g_ptr_array_index(t, i);
      if (strcmp(x->relay, relay) == 0) {
        out = g_strdup(x->state);
        if (reason) *reason = g_strdup(x->reason);
      }
    }
    g_ptr_array_unref(t);
  }
  g_free(st);
  g_free(detail);
  return out ? out : g_strdup("none");
}

#define ASSERT_TARGET(f, id, relay, want)          \
  do {                                             \
    char *_t = target_of(f, id, relay, NULL);      \
    g_assert_cmpstr(_t, ==, want);                 \
    g_free(_t);                                    \
  } while (0)

static char *state_of(Fx *f, const char *id) {
  char *st = NULL, *detail = NULL;
  if (nsr_outbox_event_status(f->ob, id, &st, &detail, NULL) != 0) return g_strdup("unknown");
  g_free(detail);
  return st;
}

#define ASSERT_STATE(f, id, want)          \
  do {                                     \
    char *_s = state_of(f, id);            \
    g_assert_cmpstr(_s, ==, want);         \
    g_free(_s);                            \
  } while (0)

static void route2(Fx *f, const char *id, gint64 now) {
  const char *relays[] = {"wss://a.example", "wss://b.example", NULL};
  g_assert_cmpint(nsr_outbox_set_routed(f->ob, id, relays, NSR_FED_LANE_IDENTIFIED, NULL, now), ==, 0);
}

static void test_ingest_idempotent(Fx *f, gconstpointer d) {
  (void)d;
  char *id = id_n(1);
  ingest(f, id, 1, NOW, NULL, TRUE, FALSE, "{\"a\":1}", NOW);
  ingest(f, id, 1, NOW, NULL, TRUE, FALSE, "{\"a\":1}", NOW + 5);
  GPtrArray *r = nsr_outbox_take_routable(f->ob, NOW, 10);
  g_assert_cmpuint(r->len, ==, 1);
  NsrOutboxEvent *e = g_ptr_array_index(r, 0);
  g_assert_cmpstr(e->id, ==, id);
  g_assert_cmpint(e->enqueued_at, ==, NOW);
  g_ptr_array_unref(r);
  NsrOutboxStats s;
  nsr_outbox_stats(f->ob, &s);
  g_assert_cmpuint(s.queued, ==, 1);
  nsr_outbox_stats_clear(&s);
  g_assert_false(nsr_outbox_eviction_eligible(f->ob, id));
  g_assert_true(nsr_outbox_eviction_eligible(f->ob, "ffff")); /* not queued */
  ASSERT_STATE(f, id, "new");
  g_free(id);
}

static void test_hints_and_reroute(Fx *f, gconstpointer d) {
  (void)d;
  ingest(f, "h1", 10002, NOW, NULL, FALSE, TRUE, "{\"v\":2}", NOW);
  ingest(f, "h0", 10002, NOW - 10, NULL, FALSE, TRUE, "{\"v\":1}", NOW); /* older: ignored */
  char *j = nsr_outbox_hint_json(f->ob, PK, 10002);
  g_assert_cmpstr(j, ==, "{\"v\":2}");
  g_free(j);
  g_assert_null(nsr_outbox_hint_json(f->ob, PK, 10050));

  char *id = id_n(2);
  ingest(f, id, 1, NOW, NULL, TRUE, FALSE, NULL, NOW);
  g_assert_cmpint(nsr_outbox_set_unroutable(f->ob, id, "no 10002", NOW + 500, FALSE, NULL, NOW), ==, 0);
  ASSERT_STATE(f, id, "unroutable");
  GPtrArray *r = nsr_outbox_take_routable(f->ob, NOW + 1, 10);
  g_assert_cmpuint(r->len, ==, 0);
  g_ptr_array_unref(r);
  g_assert_cmpint(nsr_outbox_next_due(f->ob), ==, NOW + 500);
  /* a newer relay list makes it due at once */
  ingest(f, "h2", 10002, NOW + 1, NULL, FALSE, TRUE, "{\"v\":3}", NOW + 1);
  r = nsr_outbox_take_routable(f->ob, NOW + 1, 10);
  g_assert_cmpuint(r->len, ==, 1);
  g_assert_cmpuint(((NsrOutboxEvent *)g_ptr_array_index(r, 0))->route_attempts, ==, 1);
  g_ptr_array_unref(r);
  /* reroute_all does the same */
  g_assert_cmpint(nsr_outbox_set_unroutable(f->ob, id, "x", NOW + 500, FALSE, NULL, NOW + 1), ==, 0);
  nsr_outbox_reroute_all(f->ob);
  g_assert_cmpint(nsr_outbox_next_due(f->ob), ==, 0);
  g_free(id);
}

static void test_delivery_state_machine(Fx *f, gconstpointer d) {
  (void)d;
  char *id = id_n(3);
  ingest(f, id, 1, NOW, NULL, TRUE, FALSE, "{\"e\":3}", NOW);
  route2(f, id, NOW);
  ASSERT_STATE(f, id, "pending");
  GPtrArray *due = nsr_outbox_take_due(f->ob, NOW, 60, 10);
  g_assert_cmpuint(due->len, ==, 2);
  NsrOutboxDue *d0 = g_ptr_array_index(due, 0);
  g_assert_cmpstr(d0->json, ==, "{\"e\":3}");
  g_ptr_array_unref(due);
  /* leased: not handed out again until the lease lapses */
  due = nsr_outbox_take_due(f->ob, NOW + 1, 60, 10);
  g_assert_cmpuint(due->len, ==, 0);
  g_ptr_array_unref(due);
  due = nsr_outbox_take_due(f->ob, NOW + 60, 60, 10);
  g_assert_cmpuint(due->len, ==, 2);
  g_ptr_array_unref(due);

  const char *ts, *es;
  g_assert_cmpint(nsr_outbox_record(f->ob, &f->cfg, id, "wss://a.example", NSR_OUTBOX_ACKED, "",
                                    0.5, NOW + 61, &ts, &es), ==, 0);
  g_assert_cmpstr(ts, ==, "acked");
  g_assert_cmpstr(es, ==, "pending");
  g_assert_cmpint(nsr_outbox_record(f->ob, &f->cfg, id, "wss://b.example", NSR_OUTBOX_TRANSIENT,
                                    "rate-limited: slow", 0.5, NOW + 61, &ts, &es), ==, 0);
  g_assert_cmpstr(ts, ==, "pending");
  /* backoff: first retry after initial (10 s at jitter 0.5) */
  g_assert_cmpint(nsr_outbox_next_due(f->ob), ==, NOW + 71);
  /* second transient: 20 s */
  g_assert_cmpint(nsr_outbox_record(f->ob, &f->cfg, id, "wss://b.example", NSR_OUTBOX_TRANSIENT,
                                    "error: x", 0.5, NOW + 71, &ts, &es), ==, 0);
  g_assert_cmpint(nsr_outbox_next_due(f->ob), ==, NOW + 91);
  /* acked twice is rejected (late duplicate OK) */
  g_assert_cmpint(nsr_outbox_record(f->ob, &f->cfg, id, "wss://a.example", NSR_OUTBOX_ACKED, "",
                                    0.5, NOW + 72, NULL, NULL), ==, -1);
  g_assert_false(nsr_outbox_eviction_eligible(f->ob, id));
  g_assert_cmpint(nsr_outbox_record(f->ob, &f->cfg, id, "wss://b.example", NSR_OUTBOX_ACKED,
                                    "duplicate: have it", 0.5, NOW + 91, &ts, &es), ==, 0);
  g_assert_cmpstr(es, ==, "forwarded");
  ASSERT_STATE(f, id, "forwarded");
  g_assert_true(nsr_outbox_eviction_eligible(f->ob, id));

  GPtrArray *targets = NULL;
  char *st = NULL, *detail = NULL;
  g_assert_cmpint(nsr_outbox_event_status(f->ob, id, &st, &detail, &targets), ==, 0);
  g_assert_cmpuint(targets->len, ==, 2);
  NsrOutboxTargetInfo *tb = g_ptr_array_index(targets, 1);
  g_assert_cmpstr(tb->relay, ==, "wss://b.example");
  g_assert_cmpuint(tb->attempts, ==, 2);
  g_assert_cmpint(tb->acked_at, ==, NOW + 91);
  g_ptr_array_unref(targets);
  g_free(st);
  g_free(detail);

  GStrv acked = nsr_outbox_acked_relays(f->ob, id, FALSE);
  g_assert_cmpuint(g_strv_length(acked), ==, 2);
  g_strfreev(acked);

  NsrOutboxStats s;
  nsr_outbox_stats(f->ob, &s);
  g_assert_cmpuint(s.forwarded, ==, 1);
  g_assert_cmpuint(s.queued, ==, 0);
  g_assert_cmpstr(s.last_error, ==, "wss://b.example: error: x");
  nsr_outbox_stats_clear(&s);
  g_free(id);
}

static void test_partial_failed_expired(Fx *f, gconstpointer d) {
  (void)d;
  char *p = id_n(4), *x = id_n(5), *e = id_n(6);
  ingest(f, p, 1, NOW, NULL, TRUE, FALSE, NULL, NOW);
  ingest(f, x, 1, NOW, NULL, TRUE, FALSE, NULL, NOW);
  ingest(f, e, 1, NOW, NULL, TRUE, FALSE, NULL, NOW);
  route2(f, p, NOW);
  route2(f, x, NOW);
  route2(f, e, NOW);
  const char *es = NULL;
  nsr_outbox_record(f->ob, &f->cfg, p, "wss://a.example", NSR_OUTBOX_ACKED, "", 0.5, NOW, NULL, NULL);
  nsr_outbox_record(f->ob, &f->cfg, p, "wss://b.example", NSR_OUTBOX_PERMANENT, "blocked: no",
                    0.5, NOW, NULL, &es);
  g_assert_cmpstr(es, ==, "partial");
  nsr_outbox_record(f->ob, &f->cfg, x, "wss://a.example", NSR_OUTBOX_PERMANENT, "invalid: x", 0.5,
                    NOW, NULL, NULL);
  nsr_outbox_record(f->ob, &f->cfg, x, "wss://b.example", NSR_OUTBOX_PERMANENT, "invalid: x", 0.5,
                    NOW, NULL, &es);
  g_assert_cmpstr(es, ==, "failed");
  /* a transient failure past max_age settles the target as expired */
  const char *ts = NULL;
  nsr_outbox_record(f->ob, &f->cfg, e, "wss://a.example", NSR_OUTBOX_TRANSIENT, "error: down",
                    0.5, NOW + 1000, &ts, &es);
  g_assert_cmpstr(ts, ==, "failed");
  g_assert_cmpstr(es, ==, "pending");
  /* expire() settles the rest */
  g_assert_cmpint(nsr_outbox_expire(f->ob, NOW + 2000, 1000), ==, 1);
  ASSERT_STATE(f, e, "failed");
  GPtrArray *targets = NULL;
  char *st = NULL, *detail = NULL;
  nsr_outbox_event_status(f->ob, e, &st, &detail, &targets);
  NsrOutboxTargetInfo *t0 = g_ptr_array_index(targets, 0);
  g_assert_true(g_str_has_prefix(t0->reason, "expired: "));
  g_ptr_array_unref(targets);
  g_free(st);
  g_free(detail);
  /* an unroutable event expires too */
  char *u = id_n(7);
  ingest(f, u, 1, NOW, NULL, TRUE, FALSE, NULL, NOW);
  nsr_outbox_set_unroutable(f->ob, u, "no 10002", NOW + 10, FALSE, NULL, NOW);
  /* ...but one held for the local account never expires */
  char *h = id_n(8);
  ingest(f, h, 1, NOW, NULL, TRUE, FALSE, NULL, NOW);
  nsr_outbox_set_unroutable(f->ob, h, "waiting for the local account", NOW + 10, TRUE, NULL, NOW);
  g_assert_cmpint(nsr_outbox_expire(f->ob, NOW + 2000, 1000), ==, 1);
  ASSERT_STATE(f, h, "unroutable");
  nsr_outbox_reroute_all(f->ob);
  GPtrArray *rr = nsr_outbox_take_routable(f->ob, NOW + 2000, 10);
  g_assert_cmpuint(rr->len, ==, 1);
  g_ptr_array_unref(rr);
  const char *hr[] = {"wss://a.example", NULL};
  g_assert_cmpint(nsr_outbox_set_routed(f->ob, h, hr, NSR_FED_LANE_IDENTIFIED, NULL, NOW + 2000), ==, 0);
  g_free(h);
  nsr_outbox_event_status(f->ob, u, &st, &detail, NULL);
  g_assert_cmpstr(st, ==, "failed");
  g_assert_cmpstr(detail, ==, "expired: no 10002");
  g_free(st);
  g_free(detail);

  NsrOutboxStats s;
  nsr_outbox_stats(f->ob, &s);
  g_assert_cmpuint(s.partial, ==, 1);
  g_assert_cmpuint(s.failed, ==, 3);
  nsr_outbox_stats_clear(&s);

  /* prune settled rows; lifetime counters and delivery records survive */
  g_assert_cmpint(nsr_outbox_prune(f->ob, NOW + 5000, 100), ==, 4);
  ASSERT_STATE(f, p, "unknown");
  GStrv acked = nsr_outbox_acked_relays(f->ob, p, FALSE);
  g_assert_cmpuint(g_strv_length(acked), ==, 1);
  g_assert_cmpstr(acked[0], ==, "wss://a.example");
  g_strfreev(acked);
  nsr_outbox_stats(f->ob, &s);
  g_assert_cmpuint(s.partial, ==, 1);
  g_assert_cmpuint(s.failed, ==, 3);
  nsr_outbox_stats_clear(&s);
  GPtrArray *rc = nsr_outbox_relay_counts(f->ob);
  g_assert_cmpuint(rc->len, ==, 1); /* pruned targets cascaded; h still pending */
  g_ptr_array_unref(rc);
  g_free(p);
  g_free(x);
  g_free(e);
  g_free(u);
}

static void test_supersede_and_cancel(Fx *f, gconstpointer d) {
  (void)d;
  const char *rk = "0:" PK ":";
  char *v1 = id_n(10), *v2 = id_n(11);
  ingest(f, v1, 0, NOW, rk, TRUE, FALSE, NULL, NOW);
  route2(f, v1, NOW);
  ingest(f, v2, 0, NOW + 5, rk, TRUE, FALSE, NULL, NOW + 5);
  g_assert_cmpint(nsr_outbox_supersede(f->ob, rk, v2, NOW + 5, NOW + 5), ==, 1);
  ASSERT_STATE(f, v1, "superseded");
  ASSERT_STATE(f, v2, "new");
  /* a late result for a cancelled target does not revive it (the OK is
   * still remembered as a delivery: see below) */
  g_assert_cmpint(nsr_outbox_record(f->ob, &f->cfg, v1, "wss://a.example", NSR_OUTBOX_ACKED, "",
                                    0.5, NOW + 6, NULL, NULL), ==, -1);
  /* an older version never supersedes a newer one */
  g_assert_cmpint(nsr_outbox_supersede(f->ob, rk, v1, NOW, NOW + 6), ==, 0);

  char *n = id_n(12);
  ingest(f, n, 1, NOW, NULL, TRUE, FALSE, NULL, NOW);
  g_assert_cmpint(nsr_outbox_cancel_ref(f->ob, "otherpk", n, FALSE, NOW), ==, 0); /* not author */
  g_assert_cmpint(nsr_outbox_cancel_ref(f->ob, PK, n, FALSE, NOW), ==, 1);
  ASSERT_STATE(f, n, "cancelled");
  g_assert_cmpint(nsr_outbox_cancel_ref(f->ob, PK, rk, TRUE, NOW), ==, 1); /* v2 by coordinate */
  /* delivery records by coordinate */
  char *v3 = id_n(13);
  ingest(f, v3, 0, NOW + 9, rk, TRUE, FALSE, NULL, NOW + 9);
  const char *one[] = {"wss://c.example", NULL};
  g_assert_cmpint(nsr_outbox_set_routed(f->ob, v3, one, NSR_FED_LANE_IDENTIFIED, NULL, NOW + 9), ==, 0);
  nsr_outbox_record(f->ob, &f->cfg, v3, "wss://c.example", NSR_OUTBOX_ACKED, "", 0.5, NOW + 9,
                    NULL, NULL);
  GStrv by_coord = nsr_outbox_acked_relays(f->ob, rk, TRUE);
  g_assert_cmpuint(g_strv_length(by_coord), ==, 2); /* v1's late OK at a, v3 at c */
  g_assert_cmpstr(by_coord[0], ==, "wss://a.example");
  g_assert_cmpstr(by_coord[1], ==, "wss://c.example");
  g_strfreev(by_coord);
  g_free(v3);
  ASSERT_STATE(f, v2, "cancelled");
  g_assert_true(nsr_outbox_eviction_eligible(f->ob, v1));
  g_free(v1);
  g_free(v2);
  g_free(n);
}

static void test_restart_durability(Fx *f, gconstpointer d) {
  (void)d;
  char *id = id_n(20);
  ingest(f, id, 1, NOW, NULL, TRUE, FALSE, "{\"x\":1}", NOW);
  route2(f, id, NOW);
  GPtrArray *due = nsr_outbox_take_due(f->ob, NOW, 90, 10);
  g_assert_cmpuint(due->len, ==, 2);
  g_ptr_array_unref(due);
  nsr_outbox_add_account(f->ob, PK);
  nsr_outbox_add_account(f->ob, PK);
  /* "crash" with both targets leased */
  nsr_outbox_close(f->ob);
  fx_open(f);
  ASSERT_STATE(f, id, "pending");
  g_assert_cmpint(nsr_outbox_next_due(f->ob), ==, NOW + 90); /* lease survives */
  nsr_outbox_release_lease(f->ob, id, "wss://a.example", NOW + 1);
  due = nsr_outbox_take_due(f->ob, NOW + 1, 90, 10);
  g_assert_cmpuint(due->len, ==, 1);
  g_assert_cmpstr(((NsrOutboxDue *)g_ptr_array_index(due, 0))->relay, ==, "wss://a.example");
  g_ptr_array_unref(due);
  GStrv acc = nsr_outbox_get_accounts(f->ob);
  g_assert_cmpuint(g_strv_length(acc), ==, 1);
  g_assert_cmpstr(acc[0], ==, PK);
  g_strfreev(acc);
  GPtrArray *rc = nsr_outbox_relay_counts(f->ob);
  g_assert_cmpuint(rc->len, ==, 2);
  g_assert_cmpuint(((NsrOutboxRelayCounts *)g_ptr_array_index(rc, 0))->pending, ==, 1);
  g_ptr_array_unref(rc);
  g_free(id);
}

static void test_final_states(Fx *f, gconstpointer d) {
  (void)d;
  char *id = id_n(30);
  ingest(f, id, 1, NOW, NULL, TRUE, FALSE, NULL, NOW);
  g_assert_cmpint(nsr_outbox_set_final(f->ob, id, "skipped", "not a local account", NOW), ==, 0);
  ASSERT_STATE(f, id, "skipped");
  /* routing a settled event is refused */
  const char *relays[] = {"wss://a.example", NULL};
  g_assert_cmpint(nsr_outbox_set_routed(f->ob, id, relays, NSR_FED_LANE_IDENTIFIED, NULL, NOW), ==, -1);
  NsrOutboxStats s;
  nsr_outbox_stats(f->ob, &s);
  g_assert_cmpuint(s.skipped, ==, 1);
  nsr_outbox_stats_clear(&s);
  g_free(id);
}

/* nostrc-jedb: a newer relay list only releases the events waiting for it,
 * and re-resolves the pending deliveries routed from it. */
static void test_scoped_reroute(Fx *f, gconstpointer d) {
  (void)d;
  char *home = id_n(40), *wrap = id_n(41);
  ingest(f, home, 1, NOW, NULL, TRUE, FALSE, NULL, NOW);
  ingest(f, wrap, 1059, NOW, NULL, TRUE, FALSE, NULL, NOW);
  NsrFedBasis b1 = basis(PK, 10002), b2 = basis(PK2, 10050);
  g_assert_cmpint(nsr_outbox_set_unroutable(f->ob, home, "no 10002", NOW + 500, FALSE, &b1, NOW),
                  ==, 0);
  g_assert_cmpint(nsr_outbox_set_unroutable(f->ob, wrap, "no 10050", NOW + 500, FALSE, &b2, NOW),
                  ==, 0);
  hint(f, PK2, 10050, NOW, NOW + 1); /* the recipient's inbox list */
  GPtrArray *r = nsr_outbox_take_routable(f->ob, NOW + 1, 10);
  g_assert_cmpuint(r->len, ==, 1);
  g_assert_cmpstr(((NsrOutboxEvent *)g_ptr_array_index(r, 0))->id, ==, wrap);
  g_ptr_array_unref(r);
  hint(f, PK, 10050, NOW, NOW + 1); /* same author, another kind: nothing */
  r = nsr_outbox_take_routable(f->ob, NOW + 1, 10);
  g_assert_cmpuint(r->len, ==, 1);
  g_ptr_array_unref(r);
  hint(f, PK, 10002, NOW, NOW + 1);
  r = nsr_outbox_take_routable(f->ob, NOW + 1, 10);
  g_assert_cmpuint(r->len, ==, 2);
  g_ptr_array_unref(r);
  g_free(home);
  g_free(wrap);
}

static void test_retarget(Fx *f, gconstpointer d) {
  (void)d;
  char *id = id_n(50), *other = id_n(51);
  ingest(f, id, 1, NOW, NULL, TRUE, FALSE, NULL, NOW);
  ingest(f, other, 1, NOW, NULL, TRUE, FALSE, NULL, NOW);
  NsrFedBasis b = basis(PK, 10002);
  const char *ab[] = {"wss://a.example", "wss://b.example", NULL};
  g_assert_cmpint(nsr_outbox_set_routed(f->ob, id, ab, NSR_FED_LANE_IDENTIFIED, &b, NOW), ==, 0);
  g_assert_cmpint(nsr_outbox_set_routed(f->ob, other, ab, NSR_FED_LANE_IDENTIFIED, NULL, NOW), ==, 0);
  nsr_outbox_record(f->ob, &f->cfg, id, "wss://a.example", NSR_OUTBOX_ACKED, "", 0.5, NOW, NULL,
                    NULL);
  GPtrArray *t = nsr_outbox_take_retarget(f->ob, 10);
  g_assert_cmpuint(t->len, ==, 0);
  g_ptr_array_unref(t);
  hint(f, PK, 10002, NOW, NOW + 1);
  hint(f, PK, 10002, NOW, NOW + 2); /* not newer: no second flag needed */
  t = nsr_outbox_take_retarget(f->ob, 10);
  g_assert_cmpuint(t->len, ==, 1); /* `other` has no basis: not retargeted */
  g_assert_cmpstr(((NsrOutboxEvent *)g_ptr_array_index(t, 0))->id, ==, id);
  g_ptr_array_unref(t);
  /* Taking clears the flag; a newer list arriving while the engine
   * re-resolves flags the event again (no lost update). */
  t = nsr_outbox_take_retarget(f->ob, 10);
  g_assert_cmpuint(t->len, ==, 0);
  g_ptr_array_unref(t);
  hint(f, PK, 10002, NOW + 1, NOW + 2);
  t = nsr_outbox_take_retarget(f->ob, 10);
  g_assert_cmpuint(t->len, ==, 1);
  g_ptr_array_unref(t);

  /* new list: a, c -- b's pending target is cancelled, c added, a stays acked */
  GPtrArray *added = NULL, *dropped = NULL;
  const char *es = NULL;
  const char *ac[] = {"wss://a.example", "wss://c.example", NULL};
  g_assert_cmpint(nsr_outbox_retarget(f->ob, id, ac, NOW + 3, &added, &dropped, &es), ==, 0);
  g_assert_cmpuint(added->len, ==, 1);
  g_assert_cmpstr(added->pdata[0], ==, "wss://c.example");
  g_assert_cmpuint(dropped->len, ==, 1);
  g_assert_cmpstr(dropped->pdata[0], ==, "wss://b.example");
  g_assert_cmpstr(es, ==, "pending");
  g_ptr_array_unref(added);
  g_ptr_array_unref(dropped);
  ASSERT_TARGET(f, id, "wss://a.example", "acked");
  ASSERT_TARGET(f, id, "wss://c.example", "pending");
  char *why = NULL;
  g_free(target_of(f, id, "wss://b.example", &why));
  g_assert_cmpstr(why, ==, "dropped from the relay list");
  g_free(why);
  ASSERT_TARGET(f, id, "wss://b.example", "cancelled");
  /* b had the event on the wire: its late OK is not a pending result, but
   * the delivery is remembered (a later NIP-09 deletion goes there too). */
  g_assert_cmpint(nsr_outbox_record(f->ob, &f->cfg, id, "wss://b.example", NSR_OUTBOX_ACKED, "",
                                    0.5, NOW + 3, NULL, NULL), ==, -1);
  ASSERT_TARGET(f, id, "wss://b.example", "cancelled");
  GStrv acked = nsr_outbox_acked_relays(f->ob, id, FALSE);
  g_assert_true(g_strv_contains((const gchar *const *)acked, "wss://b.example"));
  g_strfreev(acked);
  /* b listed again: revived */
  g_assert_cmpint(nsr_outbox_retarget(f->ob, id, ab, NOW + 4, &added, &dropped, &es), ==, 0);
  g_assert_cmpuint(added->len, ==, 1);
  g_assert_cmpstr(added->pdata[0], ==, "wss://b.example");
  g_assert_cmpuint(dropped->len, ==, 1); /* c */
  g_ptr_array_unref(added);
  g_ptr_array_unref(dropped);
  ASSERT_TARGET(f, id, "wss://b.example", "pending");
  /* only the acked relay left: nothing pending, the event is forwarded */
  const char *a[] = {"wss://a.example", NULL};
  g_assert_cmpint(nsr_outbox_retarget(f->ob, id, a, NOW + 5, &added, &dropped, &es), ==, 0);
  g_assert_cmpstr(es, ==, "forwarded");
  g_ptr_array_unref(added);
  g_ptr_array_unref(dropped);
  ASSERT_STATE(f, id, "forwarded");
  /* a settled event is left alone */
  g_assert_cmpint(nsr_outbox_retarget(f->ob, id, ab, NOW + 6, &added, &dropped, &es), ==, 0);
  g_assert_cmpuint(added->len + dropped->len, ==, 0);
  g_ptr_array_unref(added);
  g_ptr_array_unref(dropped);
  ASSERT_STATE(f, id, "forwarded");
  g_free(id);
  g_free(other);
}

/* An outbox written by the v1 schema (7d96) opens, gains the v2 columns, and
 * its unroutable rows (no basis) are still released by any newer list. */
static void test_migrate_v1(Fx *f, gconstpointer d) {
  (void)d;
  nsr_outbox_close(f->ob);
  f->ob = NULL;
  g_unlink(f->path);
  sqlite3 *db = NULL;
  g_assert_cmpint(sqlite3_open(f->path, &db), ==, SQLITE_OK);
  char *id = id_n(60);
  char *sql = g_strdup_printf(
      "CREATE TABLE events(id TEXT PRIMARY KEY, pubkey TEXT NOT NULL, kind INTEGER NOT NULL,"
      " created_at INTEGER NOT NULL, replace_key TEXT, json TEXT NOT NULL, state TEXT NOT NULL,"
      " detail TEXT NOT NULL DEFAULT '', lane INTEGER NOT NULL DEFAULT 0,"
      " enqueued_at INTEGER NOT NULL, next_route_at INTEGER NOT NULL DEFAULT 0,"
      " route_attempts INTEGER NOT NULL DEFAULT 0, hold INTEGER NOT NULL DEFAULT 0,"
      " settled_at INTEGER);"
      "INSERT INTO events(id, pubkey, kind, created_at, json, state, enqueued_at, next_route_at)"
      " VALUES('%s', '%s', 1, %d, '{}', 'unroutable', %d, %d);"
      "PRAGMA user_version = 1;",
      id, PK, NOW, NOW, NOW + 500);
  g_assert_cmpint(sqlite3_exec(db, sql, NULL, NULL, NULL), ==, SQLITE_OK);
  g_free(sql);
  sqlite3_close(db);
  fx_open(f);
  ASSERT_STATE(f, id, "unroutable");
  hint(f, PK2, 10050, NOW, NOW + 1); /* any newer list releases a legacy row */
  GPtrArray *r = nsr_outbox_take_routable(f->ob, NOW + 1, 10);
  g_assert_cmpuint(r->len, ==, 1);
  g_ptr_array_unref(r);
  NsrFedBasis b = basis(PK, 10002);
  const char *one[] = {"wss://a.example", NULL};
  g_assert_cmpint(nsr_outbox_set_routed(f->ob, id, one, NSR_FED_LANE_IDENTIFIED, &b, NOW + 1), ==, 0);
  nsr_outbox_close(f->ob);
  fx_open(f); /* reopening a v2 database is a no-op */
  hint(f, PK, 10002, NOW + 1, NOW + 2);
  GPtrArray *t = nsr_outbox_take_retarget(f->ob, 10);
  g_assert_cmpuint(t->len, ==, 1);
  g_ptr_array_unref(t);
  g_free(id);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
#define T(name, fn) g_test_add("/outbox/" name, Fx, NULL, setup, fn, teardown)
  T("ingest-idempotent", test_ingest_idempotent);
  T("hints-and-reroute", test_hints_and_reroute);
  T("delivery-state-machine", test_delivery_state_machine);
  T("partial-failed-expired", test_partial_failed_expired);
  T("supersede-and-cancel", test_supersede_and_cancel);
  T("restart-durability", test_restart_durability);
  T("final-states", test_final_states);
  T("scoped-reroute", test_scoped_reroute);
  T("retarget", test_retarget);
  T("migrate-v1", test_migrate_v1);
  return g_test_run();
}
