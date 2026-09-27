/*
 * test_session_fed_policy — routing decisions per kind/tag, the local-only
 * contract, relay-URL admission, config keys, backoff and OK classes of
 * the session relay's upstream federation (bead nostrc-7d96).
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "json.h"
#include "session_fed_policy.h"
#include "session_routing.h"

#define PK_A "7e7e9c42a91bfef19fa929e5fda1b72e0ebc1a4c1141673e2794234d86addf4e"
#define NPUB_A "npub10elfcs4fr0l0r8af98jlmgdh9c8tcxjvz9qkw038js35mp4dma8qzvjptg"
#define PK_R "1111111111111111111111111111111111111111111111111111111111111111"
#define FAKE_ID "0000000000000000000000000000000000000000000000000000000000000001"
#define FAKE_SIG "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"

static char *ev_json(const char *pk, int kind, const char *tags) {
  return g_strdup_printf("{\"id\":\"%s\",\"pubkey\":\"%s\",\"created_at\":1700000000,"
                         "\"kind\":%d,\"tags\":%s,\"content\":\"\",\"sig\":\"%s\"}",
                         FAKE_ID, pk, kind, tags, FAKE_SIG);
}

static NostrEvent *mk(const char *pk, int kind, const char *tags) {
  char *j = ev_json(pk, kind, tags);
  NostrEvent *ev = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize(ev, j), ==, 0);
  g_free(j);
  return ev;
}

/* Lookup double: fixed relay lists per (pubkey, kind). */
typedef struct {
  const char *l10002, *l10050, *l10009;
  const char *acked; /* relay acked for any ref */
} Lists;

static char *lk_list(void *ud, const char *pk, int kind) {
  Lists *l = ud;
  const char *tags = kind == 10002 ? l->l10002 : kind == 10050 ? l->l10050 : l->l10009;
  return tags ? ev_json(pk, kind, tags) : NULL;
}

static GStrv lk_acked(void *ud, const char *ref, gboolean coord) {
  Lists *l = ud;
  (void)ref; (void)coord;
  if (!l->acked) return NULL;
  GStrv v = g_new0(char *, 2);
  v[0] = g_strdup(l->acked);
  return v;
}

static NsrFedConfig cfg_default(void) {
  NsrFedConfig c;
  nsr_fed_config_defaults(&c);
  return c;
}

static NsrFedRouteStatus route(const NsrFedConfig *c, NostrEvent *ev, Lists *l, GStrv *relays,
                               NsrFedLane *lane, char **reason) {
  NsrFedLookup lk = {lk_list, lk_acked, l};
  return nsr_fed_resolve(c, ev, &lk, relays, lane, reason);
}

static void test_routing_table(void) {
  g_assert_cmpint(nostr_session_route_class(1), ==, NSR_ROUTE_HOME_RELAYS);
  g_assert_cmpint(nostr_session_route_class(9), ==, NSR_ROUTE_GROUP_RELAY);
  g_assert_cmpint(nostr_session_route_class(12), ==, NSR_ROUTE_GROUP_RELAY);
  g_assert_cmpint(nostr_session_route_class(9000), ==, NSR_ROUTE_GROUP_RELAY);
  g_assert_cmpint(nostr_session_route_class(9021), ==, NSR_ROUTE_GROUP_RELAY);
  g_assert_cmpint(nostr_session_route_class(9031), ==, NSR_ROUTE_HOME_RELAYS);
  g_assert_cmpint(nostr_session_route_class(39000), ==, NSR_ROUTE_GROUP_RELAY);
  g_assert_cmpint(nostr_session_route_class(39005), ==, NSR_ROUTE_GROUP_RELAY); /* pins */
  g_assert_cmpint(nostr_session_route_class(39006), ==, NSR_ROUTE_HOME_RELAYS);
  g_assert_cmpint(nostr_session_route_class(9010), ==, NSR_ROUTE_GROUP_RELAY); /* pin list */
  g_assert_cmpint(nostr_session_route_class(1059), ==, NSR_ROUTE_NIP17_INBOX);
  g_assert_cmpint(nostr_session_route_class(14), ==, NSR_ROUTE_NIP17_INBOX);
  g_assert_cmpint(nostr_session_route_class(13), ==, NSR_ROUTE_NIP17_INBOX);
  g_assert_cmpint(nostr_session_route_class(5), ==, NSR_ROUTE_TOMBSTONE);
  g_assert_cmpint(nostr_session_route_class(10002), ==, NSR_ROUTE_HOME_RELAYS);
}

/* NIP-29 "normal user-created events": a group accepts any kind carrying
 * an h tag, so the h tag, not the kind, makes an event group-scoped
 * (docs/nips/29.md @ db5fe3d; nostrc-zi3j). */
static void test_route_class_event(void) {
  static const struct {
    uint32_t kind;
    int h;
    NostrSessionRouteClass want;
  } cases[] = {
      {1, 0, NSR_ROUTE_HOME_RELAYS},      {1, 1, NSR_ROUTE_GROUP_RELAY},
      {30023, 0, NSR_ROUTE_HOME_RELAYS},  {30023, 1, NSR_ROUTE_GROUP_RELAY},
      {31922, 1, NSR_ROUTE_GROUP_RELAY},  {7, 1, NSR_ROUTE_GROUP_RELAY},
      {1111, 1, NSR_ROUTE_GROUP_RELAY},   {5, 0, NSR_ROUTE_TOMBSTONE},
      {5, 1, NSR_ROUTE_GROUP_RELAY},      {9, 0, NSR_ROUTE_GROUP_RELAY},
      {9010, 0, NSR_ROUTE_GROUP_RELAY},   {9022, 1, NSR_ROUTE_GROUP_RELAY},
      {39005, 0, NSR_ROUTE_GROUP_RELAY},  {39006, 0, NSR_ROUTE_HOME_RELAYS},
      /* user-level replaceable state stays home, h tag or not */
      {10009, 0, NSR_ROUTE_HOME_RELAYS},  {10009, 1, NSR_ROUTE_HOME_RELAYS},
      {10011, 0, NSR_ROUTE_HOME_RELAYS},  {10011, 1, NSR_ROUTE_HOME_RELAYS},
      {10002, 1, NSR_ROUTE_HOME_RELAYS},  {0, 1, NSR_ROUTE_HOME_RELAYS},
      {3, 1, NSR_ROUTE_HOME_RELAYS},
      /* NIP-17 transport and ephemeral kinds are never group traffic */
      {1059, 1, NSR_ROUTE_NIP17_INBOX},   {14, 1, NSR_ROUTE_NIP17_INBOX},
      {13, 1, NSR_ROUTE_NIP17_INBOX},     {20001, 1, NSR_ROUTE_HOME_RELAYS},
  };
  for (gsize i = 0; i < G_N_ELEMENTS(cases); i++) {
    NostrSessionRouteClass got = nostr_session_route_class_event(cases[i].kind, cases[i].h);
    if (got != cases[i].want)
      g_error("kind %u h=%d: class %s, want %s", cases[i].kind, cases[i].h,
              nostr_session_route_class_name(got), nostr_session_route_class_name(cases[i].want));
  }
}

static void test_config(void) {
  NsrFedConfig c = cfg_default();
  g_assert_cmpint(c.enabled, ==, 1);
  g_assert_cmpint(c.n_accounts, ==, 0);
  g_assert_cmpint(nsr_fed_config_apply(&c, "federation", "0"), ==, 1);
  g_assert_cmpint(c.enabled, ==, 0);
  g_assert_cmpint(nsr_fed_config_apply(&c, "federation", "2"), ==, -1);
  g_assert_cmpint(nsr_fed_config_apply(&c, "federation_backoff_initial_seconds", "x"), ==, -1);
  g_assert_cmpint(nsr_fed_config_apply(&c, "federation_bogus", "1"), ==, -1);
  g_assert_cmpint(nsr_fed_config_apply(&c, "max_limit", "5"), ==, 0);
  g_assert_cmpint(nsr_fed_config_apply(&c, "federation_accounts", NPUB_A ", " PK_R), ==, 1);
  g_assert_cmpint(c.n_accounts, ==, 2);
  g_assert_cmpstr(c.accounts[0], ==, PK_A);
  g_assert_true(nsr_fed_config_has_account(&c, PK_R));
  g_assert_cmpint(nsr_fed_config_apply(&c, "federation_accounts", "npub1nope"), ==, -1);
  g_assert_cmpint(nsr_fed_config_apply(&c, "federation_local_only_kinds", "30078, 31000-31999"),
                  ==, 1);
  g_assert_cmpint(c.n_local_only, ==, 2);
  g_assert_cmpint(nsr_fed_config_apply(&c, "federation_local_only_kinds", "9-3"), ==, -1);

  char hex[65];
  g_assert_cmpint(nsr_fed_parse_pubkey(NPUB_A, hex), ==, 0);
  g_assert_cmpstr(hex, ==, PK_A);
  g_assert_cmpint(nsr_fed_parse_pubkey(PK_A, hex), ==, 0);
  char bad[64];
  memcpy(bad, NPUB_A, sizeof bad);
  bad[20] = bad[20] == 'q' ? 'p' : 'q'; /* checksum must catch it */
  g_assert_cmpint(nsr_fed_parse_pubkey(bad, hex), ==, -1);

  /* File: federation keys parsed, other keys ignored, quotes stripped. */
  gchar *dir = g_dir_make_tmp("nsr-fed-XXXXXX", NULL);
  gchar *path = g_build_filename(dir, "session-relay.conf", NULL);
  g_assert_true(g_file_set_contents(path,
      "# comment\nmax_limit = 500\nfederation_accounts = \"" NPUB_A "\"\n"
      "federation_backoff_initial_seconds = 2\nfederation_backoff_max_seconds = 1\n", -1, NULL));
  char err[256];
  g_assert_cmpint(nsr_fed_config_load(path, &c, err, sizeof err), ==, 0);
  g_assert_cmpint(c.n_accounts, ==, 1);
  g_assert_cmpint(c.backoff_initial_seconds, ==, 2);
  g_assert_cmpint(c.backoff_max_seconds, ==, 2); /* clamped up to initial */
  g_assert_true(g_file_set_contents(path, "federation_ok_timeout_seconds = 0\n", -1, NULL));
  g_assert_cmpint(nsr_fed_config_load(path, &c, err, sizeof err), ==, -1);
  g_assert_nonnull(strstr(err, "federation_ok_timeout_seconds"));
  g_assert_cmpint(nsr_fed_config_load("/nonexistent/x.conf", &c, err, sizeof err), ==, 0);
#ifdef NSR_CONF_EXAMPLE
  /* The shipped example documents the defaults, and parses. */
  NsrFedConfig d = cfg_default();
  g_assert_cmpint(nsr_fed_config_load(NSR_CONF_EXAMPLE, &c, err, sizeof err), ==, 0);
  g_assert_cmpmem(&c, sizeof c, &d, sizeof d);
#endif
  g_unlink(path);
  g_rmdir(dir);
  g_free(path);
  g_free(dir);
}

static void test_local_only_contract(void) {
  NsrFedConfig c = cfg_default();
  g_assert_cmpint(nsr_fed_config_apply(&c, "federation_local_only_kinds", "30078"), ==, 1);
  struct { int kind; const char *tags; NsrFedVerdict want; } cases[] = {
      {1, "[]", NSR_FED_FORWARD},
      {1, "[[\"-\"]]", NSR_FED_SKIP_PROTECTED},
      {30023, "[[\"d\",\"x\"],[\"-\"]]", NSR_FED_SKIP_PROTECTED},
      {13, "[]", NSR_FED_SKIP_NEVER_KIND},
      {14, "[]", NSR_FED_SKIP_NEVER_KIND},
      {22242, "[]", NSR_FED_SKIP_NEVER_KIND},
      {20001, "[]", NSR_FED_SKIP_NEVER_KIND},
      {29999, "[]", NSR_FED_SKIP_NEVER_KIND},
      {30078, "[[\"d\",\"app\"]]", NSR_FED_SKIP_LOCAL_KIND},
      {1059, "[[\"p\",\"" PK_R "\"]]", NSR_FED_FORWARD},
      {10002, "[]", NSR_FED_FORWARD},
      {5, "[]", NSR_FED_FORWARD},
  };
  for (gsize i = 0; i < G_N_ELEMENTS(cases); i++) {
    NostrEvent *ev = mk(PK_A, cases[i].kind, cases[i].tags);
    NsrFedVerdict v = nsr_fed_static_verdict(&c, cases[i].kind, nostr_event_get_tags(ev));
    if (v != cases[i].want)
      g_error("kind %d tags %s: verdict %d want %d", cases[i].kind, cases[i].tags, v,
              cases[i].want);
    if (v != NSR_FED_FORWARD) g_assert_true(g_str_has_prefix(nsr_fed_verdict_reason(v), "local-only"));
    nostr_event_free(ev);
  }
}

static void test_replace_key(void) {
  NostrEvent *ev = mk(PK_A, 30023, "[[\"d\",\"slug\"]]");
  char *k = nsr_fed_replace_key(30023, PK_A, nostr_event_get_tags(ev));
  g_assert_cmpstr(k, ==, "30023:" PK_A ":slug");
  g_free(k);
  k = nsr_fed_replace_key(10002, PK_A, NULL);
  g_assert_cmpstr(k, ==, "10002:" PK_A ":");
  g_free(k);
  k = nsr_fed_replace_key(0, PK_A, NULL);
  g_assert_cmpstr(k, ==, "0:" PK_A ":");
  g_free(k);
  g_assert_null(nsr_fed_replace_key(1, PK_A, NULL));
  g_assert_null(nsr_fed_replace_key(1059, PK_A, NULL));
  nostr_event_free(ev);
}

static void test_urls(void) {
  const char *ok[] = {"wss://relay.example", "wss://relay.example/", "wss://r.example:443/path",
                      "ws://127.0.0.1:7777", "ws://localhost", "ws://[::1]:9000",
                      "WSS://Relay.Example"};
  const char *no[] = {"ws://relay.example", "http://relay.example", "wss://", "wss://user@h",
                      "wss://a b", "wss:/x", "", "ws://127.0.0.1.evil.example",
                      "ws://localhost.example"};
  for (gsize i = 0; i < G_N_ELEMENTS(ok); i++)
    if (!nsr_fed_url_acceptable(ok[i], FALSE)) g_error("should accept %s", ok[i]);
  for (gsize i = 0; i < G_N_ELEMENTS(no); i++)
    if (nsr_fed_url_acceptable(no[i], FALSE)) g_error("should reject %s", no[i]);
  g_assert_true(nsr_fed_url_acceptable("ws://relay.example", TRUE));
  g_assert_false(nsr_fed_url_acceptable(NULL, TRUE));
  char *longu = g_strnfill(600, 'a');
  char *u = g_strdup_printf("wss://%s", longu);
  g_assert_false(nsr_fed_url_acceptable(u, FALSE));
  g_free(u);
  g_free(longu);
}

static void assert_relays(GStrv got, const char *const *want) {
  guint n = got ? g_strv_length(got) : 0;
  guint w = g_strv_length((GStrv)want);
  if (n != w) g_error("got %u relays, want %u (first %s)", n, w, n ? got[0] : "-");
  for (guint i = 0; i < w; i++) g_assert_cmpstr(got[i], ==, want[i]);
}

static void test_route_home(void) {
  NsrFedConfig c = cfg_default();
  Lists l = {
      .l10002 = "[[\"r\",\"wss://w.example\",\"write\"],[\"r\",\"wss://r.example\",\"read\"],"
                "[\"r\",\"wss://both.example\"],[\"r\",\"http://x.example\"],"
                "[\"r\",\"ws://plain.example\"],[\"r\",\"ws://127.0.0.1:1\",\"write\"],"
                "[\"r\",\"wss://w.example\",\"write\"]]",
  };
  NostrEvent *ev = mk(PK_A, 1, "[]");
  GStrv relays = NULL;
  NsrFedLane lane;
  char *reason = NULL;
  g_assert_cmpint(route(&c, ev, &l, &relays, &lane, &reason), ==, NSR_FED_ROUTE_OK);
  const char *want[] = {"wss://w.example", "wss://both.example", "ws://127.0.0.1:1", NULL};
  assert_relays(relays, want);
  g_assert_cmpint(lane, ==, NSR_FED_LANE_IDENTIFIED);
  g_strfreev(relays);

  /* No kind 10002 known: unroutable, never a fallback relay. */
  Lists none = {0};
  g_assert_cmpint(route(&c, ev, &none, &relays, &lane, &reason), ==, NSR_FED_ROUTE_UNROUTABLE);
  g_assert_null(relays);
  g_assert_nonnull(strstr(reason, "10002"));
  g_free(reason);
  nostr_event_free(ev);

  /* The relay list itself goes to the write relays it names. */
  ev = mk(PK_A, 10002, "[[\"r\",\"wss://new.example\"],[\"r\",\"wss://ro.example\",\"read\"]]");
  g_assert_cmpint(route(&c, ev, &none, &relays, &lane, &reason), ==, NSR_FED_ROUTE_OK);
  const char *want2[] = {"wss://new.example", NULL};
  assert_relays(relays, want2);
  g_strfreev(relays);
  nostr_event_free(ev);

  /* Cap. */
  c.max_relays_per_event = 1;
  ev = mk(PK_A, 1, "[]");
  g_assert_cmpint(route(&c, ev, &l, &relays, &lane, &reason), ==, NSR_FED_ROUTE_OK);
  const char *want3[] = {"wss://w.example", NULL};
  assert_relays(relays, want3);
  g_strfreev(relays);
  nostr_event_free(ev);

  /* Tombstone: home relays plus where the deleted event was acked. */
  c = cfg_default();
  Lists del = {.l10002 = "[[\"r\",\"wss://w.example\"]]", .acked = "wss://group.example"};
  ev = mk(PK_A, 5, "[[\"e\",\"" FAKE_ID "\"]]");
  g_assert_cmpint(route(&c, ev, &del, &relays, &lane, &reason), ==, NSR_FED_ROUTE_OK);
  const char *want4[] = {"wss://w.example", "wss://group.example", NULL};
  assert_relays(relays, want4);
  g_strfreev(relays);
  nostr_event_free(ev);
}

static void test_route_group(void) {
  NsrFedConfig c = cfg_default();
  Lists l = {.l10002 = "[[\"r\",\"wss://home.example\"]]",
             .l10009 = "[[\"group\",\"abc\",\"wss://groups.example\",\"Name\"],"
                       "[\"group\",\"def\",\"http://bad.example\"]]"};
  GStrv relays = NULL;
  NsrFedLane lane;
  char *reason = NULL;
  /* kind-10009 entry; never the home relays */
  NostrEvent *ev = mk(PK_A, 9, "[[\"h\",\"abc\"]]");
  g_assert_cmpint(route(&c, ev, &l, &relays, &lane, &reason), ==, NSR_FED_ROUTE_OK);
  const char *want[] = {"wss://groups.example", NULL};
  assert_relays(relays, want);
  g_strfreev(relays);
  nostr_event_free(ev);
  /* relay hint in the h tag wins */
  ev = mk(PK_A, 11, "[[\"h\",\"abc\",\"wss://hint.example\"]]");
  g_assert_cmpint(route(&c, ev, &l, &relays, &lane, &reason), ==, NSR_FED_ROUTE_OK);
  const char *want2[] = {"wss://hint.example", NULL};
  assert_relays(relays, want2);
  g_strfreev(relays);
  nostr_event_free(ev);
  /* moderation kinds route the same way */
  ev = mk(PK_A, 9021, "[[\"h\",\"abc\"]]");
  g_assert_cmpint(route(&c, ev, &l, &relays, &lane, &reason), ==, NSR_FED_ROUTE_OK);
  assert_relays(relays, want);
  g_strfreev(relays);
  nostr_event_free(ev);
  /* nostrc-ytua: the h tag carries the bare id; no relay is derived from a
   * "host'id" value (a client-side reference form a group relay rejects) */
  ev = mk(PK_A, 9021, "[[\"h\",\"relay.groups.example'xyz\"]]");
  g_assert_cmpint(route(&c, ev, &l, &relays, &lane, &reason), ==, NSR_FED_ROUTE_UNROUTABLE);
  g_assert_null(relays);
  g_assert_nonnull(strstr(reason, "bare group id"));
  g_clear_pointer(&reason, g_free);
  nostr_event_free(ev);
  /* unknown group, bad 10009 URL: unroutable */
  ev = mk(PK_A, 9, "[[\"h\",\"def\"]]");
  g_assert_cmpint(route(&c, ev, &l, &relays, &lane, &reason), ==, NSR_FED_ROUTE_UNROUTABLE);
  g_assert_nonnull(strstr(reason, "def"));
  g_clear_pointer(&reason, g_free);
  nostr_event_free(ev);
  /* no h tag: invalid */
  ev = mk(PK_A, 9, "[]");
  g_assert_cmpint(route(&c, ev, &l, &relays, &lane, &reason), ==, NSR_FED_ROUTE_INVALID);
  g_clear_pointer(&reason, g_free);
  nostr_event_free(ev);
}

/* nostrc-ytua: a NIP-29 group is (relay, id) — forks share the id on other
 * relays (docs/nips/29.md @ db5fe3d "Forking a group"). */
static void test_route_group_forks(void) {
  NsrFedConfig c = cfg_default();
  Lists forks = {.l10002 = "[[\"r\",\"wss://home.example\"]]",
                 .l10009 = "[[\"group\",\"pizza\",\"wss://a.example\",\"Pizza\"],"
                           "[\"group\",\"pizza\",\"wss://b.example\",\"Pizza (fork)\"],"
                           "[\"group\",\"pasta\",\"wss://A.example/\"],"
                           "[\"group\",\"pasta\",\"wss://a.example\"]]"};
  GStrv relays = NULL;
  NsrFedLane lane;
  char *reason = NULL;
  /* Two entries for the id, no relay named: unroutable, never the first. */
  NostrEvent *ev = mk(PK_A, 9, "[[\"h\",\"pizza\"]]");
  g_assert_cmpint(route(&c, ev, &forks, &relays, &lane, &reason), ==, NSR_FED_ROUTE_UNROUTABLE);
  g_assert_null(relays);
  g_assert_nonnull(strstr(reason, "2 relays"));
  g_assert_nonnull(strstr(reason, "wss://a.example"));
  g_assert_nonnull(strstr(reason, "wss://b.example"));
  g_clear_pointer(&reason, g_free);
  nostr_event_free(ev);
  /* The event names its fork: routed there only. */
  ev = mk(PK_A, 9, "[[\"h\",\"pizza\",\"wss://b.example\"]]");
  g_assert_cmpint(route(&c, ev, &forks, &relays, &lane, &reason), ==, NSR_FED_ROUTE_OK);
  const char *b[] = {"wss://b.example", NULL};
  assert_relays(relays, b);
  g_strfreev(relays);
  nostr_event_free(ev);
  /* Same relay listed twice (case / trailing slash): one group. */
  ev = mk(PK_A, 1, "[[\"h\",\"pasta\"]]");
  g_assert_cmpint(route(&c, ev, &forks, &relays, &lane, &reason), ==, NSR_FED_ROUTE_OK);
  const char *a[] = {"wss://A.example/", NULL};
  assert_relays(relays, a);
  g_strfreev(relays);
  nostr_event_free(ev);
  /* A named relay that is not admissible is not replaced by a listed one. */
  ev = mk(PK_A, 9, "[[\"h\",\"pasta\",\"ws://plain.example\"]]");
  g_assert_cmpint(route(&c, ev, &forks, &relays, &lane, &reason), ==, NSR_FED_ROUTE_UNROUTABLE);
  g_assert_null(relays);
  g_clear_pointer(&reason, g_free);
  nostr_event_free(ev);

  GStrv v = nsr_fed_group_relays_from_10009(
      "{\"id\":\"" FAKE_ID "\",\"pubkey\":\"" PK_A "\",\"created_at\":1,\"kind\":10009,"
      "\"tags\":[[\"group\",\"pizza\",\"wss://a.example\"],[\"group\",\"pizza\"],"
      "[\"group\",\"pizza\",\"wss://b.example\"],[\"group\",\"other\",\"wss://c.example\"]],"
      "\"content\":\"\",\"sig\":\"" FAKE_SIG "\"}", "pizza");
  const char *two[] = {"wss://a.example", "wss://b.example", NULL};
  assert_relays(v, two);
  g_strfreev(v);
}

/* nostrc-zi3j: an h tag makes any kind group-scoped — the event goes to the
 * group relay only, never to the author's home relays — while user-level
 * lists stay home. */
static void test_route_group_any_kind(void) {
  NsrFedConfig c = cfg_default();
  Lists l = {.l10002 = "[[\"r\",\"wss://home.example\"]]",
             .l10009 = "[[\"group\",\"abc\",\"wss://groups.example\"]]"};
  const char *group[] = {"wss://groups.example", NULL};
  const char *home[] = {"wss://home.example", NULL};
  struct {
    int kind;
    const char *tags;
    const char *const *want;
  } cases[] = {
      {1, "[[\"h\",\"abc\"]]", group},
      {30023, "[[\"d\",\"slug\"],[\"h\",\"abc\"]]", group},
      {5, "[[\"e\",\"" FAKE_ID "\"],[\"h\",\"abc\"]]", group},
      {39005, "[[\"d\",\"abc\"],[\"e\",\"" FAKE_ID "\"]]", group}, /* relay pins: id in d */
      {1, "[[\"h\",\"\"]]", home},                                 /* empty h: not a group */
      {10009, "[[\"group\",\"abc\",\"wss://groups.example\"],[\"h\",\"abc\"]]", home},
      {10011, "[[\"h\",\"abc\"]]", home},
  };
  for (gsize i = 0; i < G_N_ELEMENTS(cases); i++) {
    NostrEvent *ev = mk(PK_A, cases[i].kind, cases[i].tags);
    GStrv relays = NULL;
    NsrFedLane lane;
    char *reason = NULL;
    NsrFedRouteStatus st = route(&c, ev, &l, &relays, &lane, &reason);
    if (st != NSR_FED_ROUTE_OK)
      g_error("kind %d %s: status %d (%s)", cases[i].kind, cases[i].tags, st, reason);
    assert_relays(relays, cases[i].want);
    g_strfreev(relays);
    nostr_event_free(ev);
  }
}

static void test_route_inbox(void) {
  NsrFedConfig c = cfg_default();
  Lists l = {.l10002 = "[[\"r\",\"wss://home.example\"]]",
             .l10050 = "[[\"relay\",\"wss://inbox1.example\"],[\"relay\",\"wss://inbox2.example\"],"
                       "[\"r\",\"wss://not-a-relay-tag.example\"]]"};
  GStrv relays = NULL;
  NsrFedLane lane;
  char *reason = NULL;
  NostrEvent *ev = mk("2222222222222222222222222222222222222222222222222222222222222222", 1059,
                      "[[\"p\",\"" PK_R "\"]]");
  g_assert_cmpint(route(&c, ev, &l, &relays, &lane, &reason), ==, NSR_FED_ROUTE_OK);
  const char *want[] = {"wss://inbox1.example", "wss://inbox2.example", NULL};
  assert_relays(relays, want);
  g_assert_cmpint(lane, ==, NSR_FED_LANE_ANONYMOUS);
  g_strfreev(relays);
  /* No 10050: unroutable, no fallback to 10002. */
  Lists only10002 = {.l10002 = l.l10002};
  g_assert_cmpint(route(&c, ev, &only10002, &relays, &lane, &reason), ==,
                  NSR_FED_ROUTE_UNROUTABLE);
  g_assert_nonnull(strstr(reason, "10050"));
  g_clear_pointer(&reason, g_free);
  nostr_event_free(ev);
  /* No recipient: invalid. Seal / rumor: invalid (never forwarded). */
  ev = mk(PK_A, 1059, "[]");
  g_assert_cmpint(route(&c, ev, &l, &relays, &lane, &reason), ==, NSR_FED_ROUTE_INVALID);
  g_clear_pointer(&reason, g_free);
  nostr_event_free(ev);
  ev = mk(PK_A, 14, "[[\"p\",\"" PK_R "\"]]");
  g_assert_cmpint(route(&c, ev, &l, &relays, &lane, &reason), ==, NSR_FED_ROUTE_INVALID);
  g_clear_pointer(&reason, g_free);
  nostr_event_free(ev);
}

static void test_backoff(void) {
  NsrFedConfig c = cfg_default();
  c.backoff_initial_seconds = 10;
  c.backoff_max_seconds = 100;
  g_assert_cmpint(nsr_fed_backoff_delay(&c, 1, 0.5), ==, 10);
  g_assert_cmpint(nsr_fed_backoff_delay(&c, 2, 0.5), ==, 20);
  g_assert_cmpint(nsr_fed_backoff_delay(&c, 3, 0.5), ==, 40);
  g_assert_cmpint(nsr_fed_backoff_delay(&c, 4, 0.5), ==, 80);
  g_assert_cmpint(nsr_fed_backoff_delay(&c, 5, 0.5), ==, 100);
  g_assert_cmpint(nsr_fed_backoff_delay(&c, 60, 0.5), ==, 100);
  g_assert_cmpint(nsr_fed_backoff_delay(&c, 1, 0.0), ==, 8);
  g_assert_cmpint(nsr_fed_backoff_delay(&c, 1, 0.999), ==, 11);
  g_assert_cmpint(nsr_fed_backoff_delay(&c, 9, 0.999), ==, 100); /* jitter never exceeds max */
  c.backoff_initial_seconds = 1;
  g_assert_cmpint(nsr_fed_backoff_delay(&c, 1, 0.0), ==, 1); /* never below 1 s */
}

static void test_ok_classes(void) {
  g_assert_cmpint(nsr_fed_classify_ok(TRUE, ""), ==, NSR_FED_OK_ACCEPTED);
  g_assert_cmpint(nsr_fed_classify_ok(FALSE, "duplicate: have it"), ==, NSR_FED_OK_ACCEPTED);
  g_assert_cmpint(nsr_fed_classify_ok(FALSE, "auth-required: x"), ==, NSR_FED_OK_AUTH_REQUIRED);
  g_assert_cmpint(nsr_fed_classify_ok(FALSE, "blocked: x"), ==, NSR_FED_OK_PERMANENT);
  g_assert_cmpint(nsr_fed_classify_ok(FALSE, "invalid: x"), ==, NSR_FED_OK_PERMANENT);
  g_assert_cmpint(nsr_fed_classify_ok(FALSE, "restricted: x"), ==, NSR_FED_OK_PERMANENT);
  g_assert_cmpint(nsr_fed_classify_ok(FALSE, "rate-limited: slow"), ==, NSR_FED_OK_TRANSIENT);
  g_assert_cmpint(nsr_fed_classify_ok(FALSE, "error: disk"), ==, NSR_FED_OK_TRANSIENT);
  g_assert_cmpint(nsr_fed_classify_ok(FALSE, NULL), ==, NSR_FED_OK_TRANSIENT);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  nostr_json_init();
  g_test_add_func("/fed-policy/routing-table", test_routing_table);
  g_test_add_func("/fed-policy/route-class-event", test_route_class_event);
  g_test_add_func("/fed-policy/config", test_config);
  g_test_add_func("/fed-policy/local-only", test_local_only_contract);
  g_test_add_func("/fed-policy/replace-key", test_replace_key);
  g_test_add_func("/fed-policy/urls", test_urls);
  g_test_add_func("/fed-policy/route-home", test_route_home);
  g_test_add_func("/fed-policy/route-group", test_route_group);
  g_test_add_func("/fed-policy/route-group-any-kind", test_route_group_any_kind);
  g_test_add_func("/fed-policy/route-group-forks", test_route_group_forks);
  g_test_add_func("/fed-policy/route-inbox", test_route_inbox);
  g_test_add_func("/fed-policy/backoff", test_backoff);
  g_test_add_func("/fed-policy/ok-classes", test_ok_classes);
  return g_test_run();
}
