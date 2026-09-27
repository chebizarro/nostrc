/*
 * test_session_federation — the session relay's upstream federation engine
 * in-process against fake remote relays on 127.0.0.1 (bead nostrc-7d96).
 *
 *   accept            OK true -> target acked
 *   reject            OK false "blocked:" -> target failed, others unaffected
 *   demand AUTH       auth-required -> NIP-42 AUTH signed by the (vtable)
 *                     signer -> resend -> acked
 *   drop connection   first connection closed mid-EVENT -> transient ->
 *                     retried on a fresh connection -> acked
 *   gift wrap         routed to the recipient's kind-10050 relays on the
 *                     anonymous lane: an AUTH-demanding inbox is NOT
 *                     answered (failed), no AUTH frame is ever sent
 *   local-only        NIP-70 ["-"] and kind 14 never queued; another
 *                     author's event queued but skipped
 *   unroutable        group message waits until a kind 10009 names the
 *                     group relay, then goes only there
 *   restart           relay down, engine + outbox closed mid-retry,
 *                     reopened with the relay up -> delivered
 *   retarget          (nostrc-jedb) a note pending on a relay the author's
 *                     newer kind 10002 drops is cancelled there and goes
 *                     to the relay the new list names
 *   signer prompts    (nostrc-8cc1) while org.nostr.Signer sits on a NIP-42
 *                     AUTH signature, or on the GetPublicKey that decides
 *                     the local account, other relays keep delivering
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>

#include "fake_remote_relay.h"
#include "nostr-keys.h"
#include "session_federation.h"

/* ── observer: last event state per id ────────────────────────────────── */

typedef struct {
  GMutex lock;
  GCond cond;
  GHashTable *state;   /* id -> event state */
  GHashTable *reasons; /* "id|relay" -> last reason */
} Obs;

static Obs obs;

static void on_progress(const char *id, const char *relay, const char *tstate, const char *reason,
                        const char *estate, void *ud) {
  (void)ud; (void)tstate;
  g_mutex_lock(&obs.lock);
  if (estate && *estate) g_hash_table_replace(obs.state, g_strdup(id), g_strdup(estate));
  if (relay && *relay)
    g_hash_table_replace(obs.reasons, g_strdup_printf("%s|%s", id, relay), g_strdup(reason));
  g_cond_broadcast(&obs.cond);
  g_mutex_unlock(&obs.lock);
}

static gboolean settled(const char *s) {
  return s && (!strcmp(s, "forwarded") || !strcmp(s, "partial") || !strcmp(s, "failed") ||
               !strcmp(s, "skipped") || !strcmp(s, "superseded") || !strcmp(s, "cancelled"));
}

/* Wait for a settled state (or @want exactly when non-NULL). */
static char *wait_state(const char *id, const char *want, int timeout_s) {
  gint64 end = g_get_monotonic_time() + (gint64)timeout_s * G_USEC_PER_SEC;
  g_mutex_lock(&obs.lock);
  for (;;) {
    const char *s = g_hash_table_lookup(obs.state, id);
    if (want ? g_strcmp0(s, want) == 0 : settled(s)) break;
    if (!g_cond_wait_until(&obs.cond, &obs.lock, end)) break;
  }
  char *s = g_strdup(g_hash_table_lookup(obs.state, id));
  g_mutex_unlock(&obs.lock);
  return s;
}

static gboolean wait_reason(const char *id, const char *relay, int timeout_s) {
  char *key = g_strdup_printf("%s|%s", id, relay);
  gint64 end = g_get_monotonic_time() + (gint64)timeout_s * G_USEC_PER_SEC;
  g_mutex_lock(&obs.lock);
  const char *r;
  while (!((r = g_hash_table_lookup(obs.reasons, key)) && *r))
    if (!g_cond_wait_until(&obs.cond, &obs.lock, end)) break;
  gboolean ok = (r = g_hash_table_lookup(obs.reasons, key)) && *r;
  g_mutex_unlock(&obs.lock);
  g_free(key);
  return ok;
}

/* ── keys, events, signer ─────────────────────────────────────────────── */

typedef struct {
  char *sk, *pk;
} Key;

static Key key_new(void) {
  Key k;
  k.sk = nostr_key_generate_private();
  k.pk = nostr_key_get_public(k.sk);
  g_assert_nonnull(k.pk);
  return k;
}

static gint64 s_clock = 1700000000;

/* Signed event; @tags is a JSON array. Distinct created_at per call. */
static NostrEvent *mk(const Key *k, int kind, const char *tags, const char *content) {
  char *j = g_strdup_printf("{\"kind\":%d,\"created_at\":%" G_GINT64_FORMAT
                            ",\"tags\":%s,\"content\":\"%s\",\"pubkey\":\"%s\"}",
                            kind, ++s_clock, tags, content, k->pk);
  NostrEvent *ev = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize(ev, j), ==, 0);
  g_free(j);
  g_assert_cmpint(nostr_event_sign(ev, k->sk), ==, 0);
  return ev;
}

static gchar *vt_sign(gpointer ud, const gchar *unsigned_json, GCancellable *c, GError **error) {
  (void)c;
  Key *k = ud;
  NostrEvent *ev = nostr_event_new();
  if (nostr_event_deserialize(ev, unsigned_json) != 0) {
    nostr_event_free(ev);
    g_set_error_literal(error, NOSTR_PUBLISH_SIGNER_ERROR, NOSTR_PUBLISH_SIGNER_ERROR_MALFORMED,
                        "unparsable");
    return NULL;
  }
  nostr_event_set_pubkey(ev, k->pk);
  g_assert_cmpint(nostr_event_sign(ev, k->sk), ==, 0);
  char *s = nostr_event_serialize(ev);
  gchar *out = g_strdup(s);
  free(s);
  nostr_event_free(ev);
  return out;
}

/* ── fixture ──────────────────────────────────────────────────────────── */

static gchar *s_dir, *s_db;
static NsrFedConfig s_cfg;
static NostrPublishSigner *s_signer;

static NsrFederation *engine_up(NsrOutbox **ob_out) {
  GError *err = NULL;
  NsrOutbox *ob = nsr_outbox_open(s_db, &err);
  g_assert_no_error(err);
  NsrFederationInit fi = {.cfg = &s_cfg, .outbox = ob, .signer = s_signer};
  NsrFederation *fed = nsr_federation_new(&fi);
  nsr_federation_set_observer(fed, on_progress, NULL);
  g_assert_true(nsr_federation_start(fed, &err));
  *ob_out = ob;
  return fed;
}

static char *offer(NsrFederation *fed, NostrEvent *ev) {
  g_assert_cmpint(nsr_federation_offer(fed, ev), ==, 0);
  char *id = nostr_event_get_id(ev);
  nostr_event_free(ev);
  return id;
}

static char *target_state(NsrOutbox *ob, const char *id, const char *relay, char **reason) {
  GPtrArray *t = NULL;
  char *st = NULL, *detail = NULL, *out = NULL;
  if (nsr_outbox_event_status(ob, id, &st, &detail, &t) == 0) {
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

#define ASSERT_TARGET(ob, id, relay, want)                            \
  do {                                                                \
    char *_t = target_state(ob, id, relay, NULL);                     \
    if (strcmp(_t, want) != 0)                                        \
      g_error("%s @ %s: target %s, want %s", id, relay, _t, want);    \
    g_free(_t);                                                       \
  } while (0)

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

static void test_federation(void) {
  FakeRelay *A = fake_relay_start(FAKE_ACCEPT, 0);
  FakeRelay *B = fake_relay_start(FAKE_REJECT, 0);
  FakeRelay *C = fake_relay_start(FAKE_AUTH, 0);
  FakeRelay *D = fake_relay_start(FAKE_DROP_FIRST, 0);
  FakeRelay *E = fake_relay_start(FAKE_AUTH, 0);
  g_assert_true(A && B && C && D && E);

  Key acct = key_new(), rcpt = key_new(), eph = key_new();
  nsr_fed_config_defaults(&s_cfg);
  g_assert_cmpint(nsr_fed_config_apply(&s_cfg, "federation_accounts", acct.pk), ==, 1);
  s_cfg.backoff_initial_seconds = 1;
  s_cfg.backoff_max_seconds = 2;
  s_cfg.ok_timeout_seconds = 5;
  s_cfg.idle_disconnect_seconds = 3;
  NostrPublishSignerVTable vt = {vt_sign, NULL};
  s_signer = nostr_publish_signer_new_from_vtable(&vt, &acct);
  s_dir = g_dir_make_tmp("nsr-fed-it-XXXXXX", NULL);
  s_db = g_build_filename(s_dir, "outbox.sqlite3", NULL);
  NsrOutbox *ob = NULL;
  NsrFederation *fed = engine_up(&ob);

  /* ── accept / reject / AUTH / drop ── */
  char *tags = g_strdup_printf("[[\"r\",\"%s\"],[\"r\",\"%s\",\"write\"],[\"r\",\"%s\"],"
                               "[\"r\",\"%s\"],[\"r\",\"wss://ignored.example\",\"read\"]]",
                               A->url, B->url, C->url, D->url);
  char *rl = offer(fed, mk(&acct, 10002, tags, ""));
  g_free(tags);
  char *n1 = offer(fed, mk(&acct, 1, "[]", "hello upstream"));
  char *st = wait_state(n1, NULL, 30);
  g_assert_cmpstr(st, ==, "partial"); /* B rejects */
  g_free(st);
  ASSERT_TARGET(ob, n1, A->url, "acked");
  ASSERT_TARGET(ob, n1, C->url, "acked");
  ASSERT_TARGET(ob, n1, D->url, "acked");
  ASSERT_TARGET(ob, n1, B->url, "failed");
  char *why = NULL;
  g_free(target_state(ob, n1, B->url, &why));
  g_assert_true(g_str_has_prefix(why, "blocked:"));
  g_free(why);
  g_assert_cmpuint(fake_auth_count(C, acct.pk), >=, 1);
  /* The relay list went first: refused with auth-required, AUTH, resent;
   * the note then rode the authenticated connection. */
  g_assert_cmpuint(fake_count_id(C, rl), >=, 2);
  g_assert_cmpuint(fake_count_id(C, n1), >=, 1);
  g_assert_cmpuint(fake_count_id(A, rl), ==, 1); /* the relay list went out too */
  st = wait_state(rl, NULL, 30);
  g_assert_cmpstr(st, ==, "partial");
  g_free(st);
  /* D dropped its first connection on whichever EVENT came first; both
   * events were then acked on a fresh connection. */
  g_assert_cmpuint(D->drops, ==, 1);
  g_assert_cmpuint(fake_count_id(D, rl) + fake_count_id(D, n1), >=, 3);
  ASSERT_TARGET(ob, rl, D->url, "acked");

  /* ── gift wrap: recipient inbox relays, anonymous lane ── */
  tags = g_strdup_printf("[[\"relay\",\"%s\"],[\"relay\",\"%s\"]]", A->url, E->url);
  char *inbox = offer(fed, mk(&rcpt, 10050, tags, "")); /* not ours: hint only */
  g_free(tags);
  /* nostrc-elgy: federation_accounts is authoritative, so another author's
   * event is never queued (no outbox row, no fsync); its relay list still
   * routes the wrap below. */
  g_assert_cmpint(nsr_outbox_event_status(ob, inbox, NULL, NULL, NULL), ==, -1);
  tags = g_strdup_printf("[[\"p\",\"%s\"]]", rcpt.pk);
  char *wrap = offer(fed, mk(&eph, 1059, tags, "ciphertext"));
  g_free(tags);
  st = wait_state(wrap, NULL, 30);
  g_assert_cmpstr(st, ==, "partial");
  g_free(st);
  ASSERT_TARGET(ob, wrap, A->url, "acked");
  ASSERT_TARGET(ob, wrap, E->url, "failed");
  g_free(target_state(ob, wrap, E->url, &why));
  g_assert_true(g_str_has_prefix(why, "auth-refused-for-gift-wrap:"));
  g_free(why);
  g_assert_cmpuint(E->auth_frames, ==, 0);
  g_assert_cmpuint(fake_count_id(B, wrap), ==, 0); /* never the sender's relays */

  /* ── local-only and other authors ── */
  NostrEvent *prot = mk(&acct, 1, "[[\"-\"]]", "protected");
  char *prot_id = offer(fed, prot);
  char *rumor = offer(fed, mk(&acct, 14, "[]", "dm plaintext"));
  char *theirs = offer(fed, mk(&rcpt, 1, "[]", "cached note of someone else"));
  g_assert_cmpint(nsr_outbox_event_status(ob, theirs, NULL, NULL, NULL), ==, -1);
  g_assert_cmpint(nsr_outbox_event_status(ob, prot_id, NULL, NULL, NULL), ==, -1);
  g_assert_cmpint(nsr_outbox_event_status(ob, rumor, NULL, NULL, NULL), ==, -1);

  /* ── group message waits for its relay ── */
  char *grp = offer(fed, mk(&acct, 9, "[[\"h\",\"testgroup\"]]", "hi group"));
  st = wait_state(grp, "unroutable", 30);
  g_assert_cmpstr(st, ==, "unroutable");
  g_free(st);
  tags = g_strdup_printf("[[\"group\",\"testgroup\",\"%s\"]]", A->url);
  char *groups = offer(fed, mk(&acct, 10009, tags, ""));
  g_free(tags);
  st = wait_state(grp, NULL, 30);
  g_assert_cmpstr(st, ==, "forwarded");
  g_free(st);
  g_assert_cmpuint(fake_count_id(A, grp), ==, 1);
  g_assert_cmpuint(fake_count_id(B, grp) + fake_count_id(C, grp) + fake_count_id(D, grp), ==, 0);
  st = wait_state(groups, NULL, 30);
  g_free(st);

  /* nothing local-only / foreign ever reached a remote */
  FakeRelay *all[] = {A, B, C, D, E};
  for (gsize i = 0; i < G_N_ELEMENTS(all); i++) {
    g_assert_cmpuint(fake_count_id(all[i], prot_id), ==, 0);
    g_assert_cmpuint(fake_count_id(all[i], rumor), ==, 0);
    g_assert_cmpuint(fake_count_id(all[i], theirs), ==, 0);
    g_assert_cmpuint(fake_count_id(all[i], inbox), ==, 0);
  }

  /* ── observability ── */
  NsrFedStatus fs;
  nsr_federation_status(fed, &fs);
  g_assert_cmpstr(fs.state, ==, "active");
  g_assert_cmpuint(fs.outbox.forwarded, >=, 1);
  g_assert_cmpuint(fs.outbox.partial, >=, 3);
  g_assert_cmpuint(fs.outbox.skipped, ==, 0); /* foreign events never queued */
  g_assert_cmpstr(fs.outbox.last_error, !=, "");
  nsr_federation_status_clear(&fs);
  GPtrArray *relays = nsr_federation_relays(fed);
  gboolean saw_c_authed = FALSE, saw_a = FALSE;
  for (guint i = 0; i < relays->len; i++) {
    NsrFedRelayInfo *r = g_ptr_array_index(relays, i);
    if (!strcmp(r->url, C->url)) saw_c_authed = r->authed || r->acked >= 2;
    if (!strcmp(r->url, A->url)) {
      saw_a = TRUE;
      g_assert_cmpuint(r->acked, >=, 4);
      g_assert_cmpint(r->last_ok_at, >, 0);
    }
  }
  g_assert_true(saw_a && saw_c_authed);
  g_ptr_array_unref(relays);

  /* ── restart durability: relay down, engine restarted, relay up ── */
  FakeRelay *probe = fake_relay_start(FAKE_ACCEPT, 0);
  guint gport = probe->port;
  char gurl[64];
  g_strlcpy(gurl, probe->url, sizeof gurl);
  fake_relay_stop(probe); /* port now closed */
  tags = g_strdup_printf("[[\"r\",\"%s\"]]", gurl);
  char *rl2 = offer(fed, mk(&acct, 10002, tags, ""));
  g_free(tags);
  char *n3 = offer(fed, mk(&acct, 1, "[]", "survives a restart"));
  g_assert_true(wait_reason(n3, gurl, 30)); /* at least one failed attempt */
  st = wait_state(rl, NULL, 1);
  g_free(st);
  nsr_federation_free(fed);
  nsr_outbox_close(ob);
  FakeRelay *G = fake_relay_start(FAKE_ACCEPT, gport);
  g_assert_nonnull(G);
  fed = engine_up(&ob);
  st = wait_state(n3, "forwarded", 30);
  g_assert_cmpstr(st, ==, "forwarded");
  g_free(st);
  g_assert_cmpuint(fake_count_id(G, n3), >=, 1);
  st = wait_state(rl2, "forwarded", 30);
  g_assert_cmpstr(st, ==, "forwarded");
  g_free(st);
  g_assert_true(nsr_outbox_eviction_eligible(ob, n3));

  nsr_federation_free(fed);
  nsr_outbox_close(ob);
  FakeRelay *stop[] = {A, B, C, D, E, G};
  for (gsize i = 0; i < G_N_ELEMENTS(stop); i++) fake_relay_stop(stop[i]);
  nostr_publish_signer_unref(s_signer);
  Key *ks[] = {&acct, &rcpt, &eph};
  for (gsize i = 0; i < G_N_ELEMENTS(ks); i++) {
    free(ks[i]->sk);
    free(ks[i]->pk);
  }
  char *ids[] = {rl, n1, inbox, wrap, prot_id, rumor, theirs, grp, groups, rl2, n3};
  for (gsize i = 0; i < G_N_ELEMENTS(ids); i++) free(ids[i]);
  rm_rf(s_dir);
  g_free(s_db);
  g_free(s_dir);
}

/* ── nostrc-8cc1: signer calls do not stall the engine ─────────────────── */

typedef struct {
  GMutex lock;
  GCond cond;
  gboolean entered, open;
} Gate;

static void gate_init(Gate *g) {
  memset(g, 0, sizeof *g);
  g_mutex_init(&g->lock);
  g_cond_init(&g->cond);
}

static void gate_clear(Gate *g) {
  g_mutex_clear(&g->lock);
  g_cond_clear(&g->cond);
}

/* The "prompt": block until the test opens the gate (bounded). */
static void gate_pass(Gate *g) {
  gint64 end = g_get_monotonic_time() + 20 * G_USEC_PER_SEC;
  g_mutex_lock(&g->lock);
  g->entered = TRUE;
  g_cond_broadcast(&g->cond);
  while (!g->open)
    if (!g_cond_wait_until(&g->cond, &g->lock, end)) break;
  g_mutex_unlock(&g->lock);
}

static gboolean gate_wait_entered(Gate *g, int timeout_s) {
  gint64 end = g_get_monotonic_time() + (gint64)timeout_s * G_USEC_PER_SEC;
  g_mutex_lock(&g->lock);
  while (!g->entered)
    if (!g_cond_wait_until(&g->cond, &g->lock, end)) break;
  gboolean e = g->entered;
  g_mutex_unlock(&g->lock);
  return e;
}

static gboolean gate_is_open(Gate *g) {
  g_mutex_lock(&g->lock);
  gboolean o = g->open;
  g_mutex_unlock(&g->lock);
  return o;
}

static void gate_open(Gate *g) {
  g_mutex_lock(&g->lock);
  g->open = TRUE;
  g_cond_broadcast(&g->cond);
  g_mutex_unlock(&g->lock);
}

typedef struct {
  Key *key;
  Gate gate;
} SlowSigner;

static gchar *vt_sign_slow(gpointer ud, const gchar *unsigned_json, GCancellable *c, GError **error) {
  SlowSigner *s = ud;
  gate_pass(&s->gate);
  return vt_sign(s->key, unsigned_json, c, error);
}

static void fresh_outbox(const char *tag) {
  s_dir = g_dir_make_tmp(tag, NULL);
  s_db = g_build_filename(s_dir, "outbox.sqlite3", NULL);
}

static void drop_outbox(void) {
  rm_rf(s_dir);
  g_clear_pointer(&s_db, g_free);
  g_clear_pointer(&s_dir, g_free);
}

/* An AUTH signature waiting on the user must not hold up another relay. */
static void test_auth_prompt_does_not_stall(void) {
  FakeRelay *A = fake_relay_start(FAKE_ACCEPT, 0);
  FakeRelay *C = fake_relay_start(FAKE_AUTH, 0);
  g_assert_true(A && C);
  Key acct = key_new();
  nsr_fed_config_defaults(&s_cfg);
  g_assert_cmpint(nsr_fed_config_apply(&s_cfg, "federation_accounts", acct.pk), ==, 1);
  s_cfg.ok_timeout_seconds = 5;
  SlowSigner slow = {.key = &acct};
  gate_init(&slow.gate);
  NostrPublishSignerVTable vt = {vt_sign_slow, NULL};
  s_signer = nostr_publish_signer_new_from_vtable(&vt, &slow);
  fresh_outbox("nsr-fed-auth-XXXXXX");
  NsrOutbox *ob = NULL;
  NsrFederation *fed = engine_up(&ob);

  char *tags = g_strdup_printf("[[\"r\",\"%s\"]]", C->url);
  char *rl = offer(fed, mk(&acct, 10002, tags, "")); /* -> C: auth-required -> signer */
  g_free(tags);
  g_assert_true(gate_wait_entered(&slow.gate, 20));
  /* While the signer "prompts": a group message for relay A goes out. */
  tags = g_strdup_printf("[[\"h\",\"g\",\"%s\"]]", A->url);
  char *msg = offer(fed, mk(&acct, 9, tags, "while the signer prompts"));
  g_free(tags);
  char *st = wait_state(msg, "forwarded", 10);
  g_assert_cmpstr(st, ==, "forwarded");
  g_free(st);
  g_assert_false(gate_is_open(&slow.gate)); /* still prompting */
  gate_open(&slow.gate);
  st = wait_state(rl, "forwarded", 20);
  g_assert_cmpstr(st, ==, "forwarded");
  g_free(st);
  g_assert_cmpuint(fake_auth_count(C, acct.pk), >=, 1);

  nsr_federation_free(fed);
  nsr_outbox_close(ob);
  nostr_publish_signer_unref(s_signer);
  s_signer = NULL;
  gate_clear(&slow.gate);
  fake_relay_stop(A);
  fake_relay_stop(C);
  free(acct.sk);
  free(acct.pk);
  free(rl);
  free(msg);
  drop_outbox();
}

/* nostrc-jedb: pending deliveries follow a relay-list change. */
static void test_retarget_on_relay_list_change(void) {
  FakeRelay *S = fake_relay_start(FAKE_SILENT, 0);
  FakeRelay *A = fake_relay_start(FAKE_ACCEPT, 0);
  g_assert_true(S && A);
  Key acct = key_new();
  nsr_fed_config_defaults(&s_cfg);
  g_assert_cmpint(nsr_fed_config_apply(&s_cfg, "federation_accounts", acct.pk), ==, 1);
  s_cfg.ok_timeout_seconds = 3;
  s_cfg.backoff_initial_seconds = 1;
  s_cfg.backoff_max_seconds = 2;
  NostrPublishSignerVTable vt = {vt_sign, NULL};
  s_signer = nostr_publish_signer_new_from_vtable(&vt, &acct);
  fresh_outbox("nsr-fed-retarget-XXXXXX");
  NsrOutbox *ob = NULL;
  NsrFederation *fed = engine_up(&ob);

  char *tags = g_strdup_printf("[[\"r\",\"%s\"]]", S->url);
  char *rl1 = offer(fed, mk(&acct, 10002, tags, ""));
  g_free(tags);
  char *note = offer(fed, mk(&acct, 1, "[]", "retarget me"));
  g_assert_true(fake_wait_id(S, note, 1, 20)); /* on the wire, never answered */
  ASSERT_TARGET(ob, note, S->url, "pending");

  tags = g_strdup_printf("[[\"r\",\"%s\"]]", A->url);
  char *rl2 = offer(fed, mk(&acct, 10002, tags, "")); /* the author moved */
  g_free(tags);
  char *st = wait_state(note, "forwarded", 20);
  g_assert_cmpstr(st, ==, "forwarded");
  g_free(st);
  ASSERT_TARGET(ob, note, A->url, "acked");
  char *why = NULL;
  g_free(target_state(ob, note, S->url, &why));
  g_assert_cmpstr(why, ==, "dropped from the relay list");
  g_free(why);
  ASSERT_TARGET(ob, note, S->url, "cancelled");
  char *rl1_state = NULL; /* superseded by rl2 (not signalled: see nostrc-z1my) */
  g_assert_cmpint(nsr_outbox_event_status(ob, rl1, &rl1_state, NULL, NULL), ==, 0);
  g_assert_cmpstr(rl1_state, ==, "superseded");
  g_free(rl1_state);

  nsr_federation_free(fed);
  nsr_outbox_close(ob);
  nostr_publish_signer_unref(s_signer);
  s_signer = NULL;
  fake_relay_stop(S);
  fake_relay_stop(A);
  free(acct.sk);
  free(acct.pk);
  free(rl1);
  free(rl2);
  free(note);
  drop_outbox();
}

/* A fake org.nostr.Signer whose GetPublicKey "prompts" behind a gate. */
typedef struct {
  const char *pk;
  const char *addr; /* private connection: the singleton is the engine's */
  Gate gate;
  GThread *th;
  GMainContext *ctx;
  GMainLoop *loop;
  gboolean ready;
  GMutex lock;
  GCond cond;
} PkSigner;

static void pk_signer_call(GDBusConnection *c, const gchar *sender, const gchar *path,
                           const gchar *iface, const gchar *method, GVariant *params,
                           GDBusMethodInvocation *inv, gpointer ud) {
  (void)c; (void)sender; (void)path; (void)iface; (void)params;
  PkSigner *s = ud;
  if (!g_str_equal(method, "GetPublicKey")) {
    g_dbus_method_invocation_return_dbus_error(inv, "org.nostr.Signer.Error.Denied", "no");
    return;
  }
  gate_pass(&s->gate);
  g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", s->pk));
}

static void pk_signer_acquired(GDBusConnection *c, const gchar *n, gpointer ud) {
  (void)c; (void)n;
  PkSigner *s = ud;
  g_mutex_lock(&s->lock);
  s->ready = TRUE;
  g_cond_broadcast(&s->cond);
  g_mutex_unlock(&s->lock);
}

static gpointer pk_signer_thread(gpointer p) {
  PkSigner *s = p;
  g_main_context_push_thread_default(s->ctx);
  GDBusConnection *c = g_dbus_connection_new_for_address_sync(
      s->addr, G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                   G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
      NULL, NULL, NULL);
  g_assert_nonnull(c);
  GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(
      "<node><interface name='org.nostr.Signer'><method name='GetPublicKey'>"
      "<arg type='s' direction='out'/></method></interface></node>", NULL);
  static const GDBusInterfaceVTable vt = {pk_signer_call, NULL, NULL, {0}};
  guint reg = g_dbus_connection_register_object(c, "/org/nostr/signer", node->interfaces[0], &vt,
                                                s, NULL, NULL);
  guint own = g_bus_own_name_on_connection(c, "org.nostr.Signer", G_BUS_NAME_OWNER_FLAGS_NONE,
                                           pk_signer_acquired, NULL, s, NULL);
  g_main_loop_run(s->loop);
  g_bus_unown_name(own);
  g_dbus_connection_unregister_object(c, reg);
  g_dbus_connection_close_sync(c, NULL, NULL);
  g_object_unref(c);
  g_dbus_node_info_unref(node);
  while (g_main_context_iteration(s->ctx, FALSE)) {
  }
  g_main_context_pop_thread_default(s->ctx);
  return NULL;
}

/* A GetPublicKey waiting on the user must not hold up a gift wrap (which
 * needs no local account). */
static void test_account_probe_does_not_stall(void) {
  gchar *daemon = g_find_program_in_path("dbus-daemon");
  if (!daemon) {
    g_test_skip("no dbus-daemon");
    return;
  }
  g_free(daemon);
  GTestDBus *bus = g_test_dbus_new(G_TEST_DBUS_NONE);
  g_test_dbus_up(bus); /* sets DBUS_SESSION_BUS_ADDRESS for this process */
  FakeRelay *A = fake_relay_start(FAKE_ACCEPT, 0);
  g_assert_nonnull(A);
  Key acct = key_new(), rcpt = key_new(), eph = key_new();
  PkSigner sg = {.pk = acct.pk, .addr = g_test_dbus_get_bus_address(bus)};
  gate_init(&sg.gate);
  g_mutex_init(&sg.lock);
  g_cond_init(&sg.cond);
  sg.ctx = g_main_context_new();
  sg.loop = g_main_loop_new(sg.ctx, FALSE);
  sg.th = g_thread_new("fake-signer", pk_signer_thread, &sg);
  g_mutex_lock(&sg.lock);
  while (!sg.ready) g_cond_wait(&sg.cond, &sg.lock);
  g_mutex_unlock(&sg.lock);

  nsr_fed_config_defaults(&s_cfg); /* no federation_accounts: ask the signer */
  s_cfg.ok_timeout_seconds = 5;
  fresh_outbox("nsr-fed-probe-XXXXXX");
  GError *err = NULL;
  NsrOutbox *ob = nsr_outbox_open(s_db, &err);
  g_assert_no_error(err);
  NsrFederationInit fi = {.cfg = &s_cfg, .outbox = ob, .dbus_signer = TRUE};
  NsrFederation *fed = nsr_federation_new(&fi);
  nsr_federation_set_observer(fed, on_progress, NULL);
  g_assert_true(nsr_federation_start(fed, &err));

  char *tags = g_strdup_printf("[[\"r\",\"%s\"]]", A->url);
  char *rl = offer(fed, mk(&acct, 10002, tags, "")); /* unknown author: GetPublicKey */
  g_free(tags);
  g_assert_true(gate_wait_entered(&sg.gate, 20));
  tags = g_strdup_printf("[[\"relay\",\"%s\"]]", A->url);
  char *inbox = offer(fed, mk(&rcpt, 10050, tags, ""));
  g_free(tags);
  tags = g_strdup_printf("[[\"p\",\"%s\"]]", rcpt.pk);
  char *wrap = offer(fed, mk(&eph, 1059, tags, "ciphertext"));
  g_free(tags);
  char *st = wait_state(wrap, "forwarded", 10);
  g_assert_cmpstr(st, ==, "forwarded");
  g_free(st);
  g_assert_false(gate_is_open(&sg.gate)); /* the signer is still "prompting" */
  gate_open(&sg.gate);
  st = wait_state(rl, "forwarded", 20);
  g_assert_cmpstr(st, ==, "forwarded");
  g_free(st);
  /* Someone else's relay list, queued while that call was in flight: the
   * answer may predate it, so the next call decides it -- meanwhile it
   * waits and never leaves. */
  char *inbox_state = NULL;
  g_assert_cmpint(nsr_outbox_event_status(ob, inbox, &inbox_state, NULL, NULL), ==, 0);
  g_assert_cmpstr(inbox_state, ==, "unroutable");
  g_free(inbox_state);
  g_assert_cmpuint(fake_count_id(A, inbox), ==, 0);
  NsrFedStatus fs;
  nsr_federation_status(fed, &fs);
  g_assert_cmpstr(fs.state, ==, "active");
  nsr_federation_status_clear(&fs);

  /* nostrc-elgy: with signer-learned accounts an unknown key's event is
   * still queued (it may be an account the signer reports later); only an
   * authoritative federation_accounts skips the row (see /federation/engine). */
  char *foreign = offer(fed, mk(&rcpt, 1, "[]", "cached, signed long ago"));
  g_assert_cmpint(nsr_outbox_event_status(ob, foreign, NULL, NULL, NULL), ==, 0);
  free(foreign);

  nsr_federation_free(fed);
  nsr_outbox_close(ob);
  g_main_loop_quit(sg.loop);
  g_thread_join(sg.th);
  g_main_loop_unref(sg.loop);
  g_main_context_unref(sg.ctx);
  g_mutex_clear(&sg.lock);
  g_cond_clear(&sg.cond);
  gate_clear(&sg.gate);
  fake_relay_stop(A);
  g_test_dbus_down(bus);
  g_object_unref(bus);
  Key *ks[] = {&acct, &rcpt, &eph};
  for (gsize i = 0; i < G_N_ELEMENTS(ks); i++) {
    free(ks[i]->sk);
    free(ks[i]->pk);
  }
  free(rl);
  free(inbox);
  free(wrap);
  drop_outbox();
}

int main(int argc, char **argv) {
  /* Keep host GIO modules out: libproxy and the network-monitor portal
   * would open the user's session bus (and keep the process-wide
   * connection alive past g_test_dbus_down()). The probe test starts its
   * own bus; nothing here may talk to the real one. */
  g_setenv("GIO_USE_VFS", "local", TRUE);
  g_setenv("GIO_USE_PROXY_RESOLVER", "dummy", TRUE);
  g_setenv("GIO_USE_NETWORK_MONITOR", "base", TRUE);
  g_setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/nsr-test-bus", TRUE);
  g_test_init(&argc, &argv, NULL);
  nostr_json_init();
  g_mutex_init(&obs.lock);
  g_cond_init(&obs.cond);
  obs.state = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  obs.reasons = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  g_test_add_func("/federation/engine", test_federation);
  g_test_add_func("/federation/retarget-on-relay-list-change", test_retarget_on_relay_list_change);
  g_test_add_func("/federation/auth-prompt-does-not-stall", test_auth_prompt_does_not_stall);
  g_test_add_func("/federation/account-probe-does-not-stall", test_account_probe_does_not_stall);
  int rc = g_test_run();
  g_hash_table_unref(obs.state);
  g_hash_table_unref(obs.reasons);
  return rc;
}
