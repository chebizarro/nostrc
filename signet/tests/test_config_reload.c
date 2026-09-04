/* SPDX-License-Identifier: MIT
 *
 * test_config_reload.c - SIGHUP config reload tests (fp-56t).
 *
 * Covers the four behaviours the reload has to get right:
 *  1. invalid-candidate retention  — a bad config never disarms a running signer
 *  2. relay reconfigure            — subscriptions survive a relay-set change
 *  3. eager policy application     — policy applies on reload, not on next lookup
 *  4. provisioner precedence       — a provisioner revoked at runtime is NOT
 *                                    resurrected by a reload, even when the
 *                                    config file still lists it (decision D2)
 */

#include "test_check.h"

#include "signet/config_reload.h"
#include "signet/policy_store.h"
#include "signet/relay_pool.h"
#include "signet/signet_config.h"
#include "signet/store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <glib.h>
#include <glib/gstdio.h>

#define MASTER_KEY "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define BUNKER_SK  "1111111111111111111111111111111111111111111111111111111111111111"

#define PK_A "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define PK_B "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define PK_C "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"

/* CHECK() comes from test_check.h: always-evaluated, unlike assert() under
 * the project's Release/-DNDEBUG build. See fp-3126. */

static char *g_tmpdir = NULL;

/* ------------------------------- helpers ---------------------------------- */

static char *tmp_path(const char *name) {
  return g_build_filename(g_tmpdir, name, NULL);
}

static void write_file(const char *path, const char *contents) {
  GError *err = NULL;
  if (!g_file_set_contents(path, contents, -1, &err)) {
    fprintf(stderr, "write_file(%s) failed: %s\n", path,
            err ? err->message : "?");
    abort();
  }
}

static char *read_file(const char *path) {
  char *out = NULL;
  if (!g_file_get_contents(path, &out, NULL, NULL)) return NULL;
  return out;
}

static void set_mtime(const char *path, time_t when) {
  struct utimbuf ut = { .actime = when, .modtime = when };
  CHECK(utime(path, &ut) == 0);
}

/* Write a config file with the given relay list and provisioner list. */
static void write_config(const char *path, const char *relays,
                         const char *provisioners) {
  char *contents = g_strdup_printf(
      "[server]\n"
      "log_level = info\n"
      "health_port = 0\n"
      "\n"
      "[store]\n"
      "db_path = /tmp/signet-config-reload-test.db\n"
      "\n"
      "[nostr]\n"
      "relays = %s\n"
      "provisioner_pubkeys = %s\n"
      "identity = test-identity\n",
      relays, provisioners);
  write_file(path, contents);
  g_free(contents);
}

static SignetStore *open_store(char **out_path) {
  char *db_path = tmp_path("reload-store.db");
  g_unlink(db_path);
  SignetStoreConfig cfg = { .db_path = db_path, .master_key = MASTER_KEY };
  SignetStore *store = signet_store_open(&cfg);
  CHECK(store != NULL);
  if (out_path) *out_path = db_path; else g_free(db_path);
  return store;
}

static bool config_lists_provisioner(const char *config_path, const char *pk) {
  char *contents = read_file(config_path);
  CHECK(contents != NULL);
  bool found = strstr(contents, pk) != NULL;
  g_free(contents);
  return found;
}

/* ---------------- 1. invalid candidate retains last-valid ----------------- */

static void test_invalid_candidate_retains_last_valid(void) {
  printf("  TEST invalid_candidate_retains_last_valid ... ");

  char *cfg_path = tmp_path("retain.conf");
  write_config(cfg_path, "wss://relay.one,wss://relay.two", PK_A);

  SignetConfig live;
  CHECK(signet_config_load(cfg_path, &live) == 0);
  CHECK(signet_config_validate(&live, NULL, 0) == 0);
  CHECK(live.n_relays == 2);

  char *db_path = NULL;
  SignetStore *store = open_store(&db_path);
  const char *seed[] = { PK_A };
  CHECK(signet_store_seed_provisioners(store, seed, 1, (int64_t)time(NULL)) == 0);

  /* Candidate with no relays fails validation. */
  write_config(cfg_path, "", PK_B);

  SignetReloadCtx ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.config_path = cfg_path;
  ctx.live = &live;
  ctx.store = store;

  SignetReloadReport report;
  int rc = signet_config_reload_apply(&ctx, (int64_t)time(NULL), &report);

  CHECK(rc == -1);
  CHECK(!report.applied);
  CHECK(report.error[0] != '\0');

  /* The running config is untouched... */
  CHECK(live.n_relays == 2);
  CHECK(strcmp(live.relays[0], "wss://relay.one") == 0);
  CHECK(live.n_provisioner_pubkeys == 1);
  CHECK(strcmp(live.provisioner_pubkeys[0], PK_A) == 0);

  /* ...and so is persisted authorization: a rejected candidate must not have
   * reconciled anything, or an invalid edit could silently change authority. */
  CHECK(signet_store_is_provisioner(store, PK_A));
  CHECK(!signet_store_is_provisioner(store, PK_B));

  /* A malformed candidate that loads but carries a bad pubkey is likewise
   * refused wholesale rather than partially applied. */
  write_config(cfg_path, "wss://relay.one", "not-a-valid-hex-pubkey");
  SignetProvisionerReconcile stats;
  const char *bad[] = { "not-a-valid-hex-pubkey" };
  CHECK(signet_store_reconcile_provisioners(store, bad, 1, 0,
                                             (int64_t)time(NULL), &stats) == -1);
  CHECK(signet_store_is_provisioner(store, PK_A));

  signet_store_close(store);
  g_unlink(db_path);
  g_free(db_path);
  signet_config_clear(&live);
  g_unlink(cfg_path);
  g_free(cfg_path);
  printf("PASS\n");
}

/* ------------- 2. subscriptions survive a relay-set change ---------------- */

static void test_subscriptions_survive_relay_change(void) {
  printf("  TEST subscriptions_survive_relay_change ... ");

  const char *initial[] = { "ws://127.0.0.1:1" };
  SignetRelayPoolConfig cfg = {
    .relays = initial,
    .n_relays = 1,
    .on_event = NULL,
    .user_data = NULL,
  };
  SignetRelayPool *rp = signet_relay_pool_new(&cfg);
  CHECK(rp != NULL);

  /* Establish subscription intent: exactly the kinds signetd needs. */
  const int kinds[] = { 24133, 25910, 1059 };
  (void)signet_relay_pool_subscribe_scoped(rp, kinds, 3, PK_A, 12345);
  CHECK(signet_relay_pool_get_subscribed_kinds(rp, NULL, 0) == 3);

  /* Same set (different order) is a no-op, not a needless reconnect. */
  const char *same[] = { "ws://127.0.0.1:1" };
  CHECK(signet_relay_pool_set_relays(rp, same, 1) == 1);

  /* Change the relay set. */
  const char *replacement[] = { "ws://127.0.0.1:2", "ws://127.0.0.1:3" };
  CHECK(signet_relay_pool_set_relays(rp, replacement, 2) == 0);

  size_t n_urls = 0;
  const char *const *urls = signet_relay_pool_get_urls(rp, &n_urls);
  CHECK(n_urls == 2);
  CHECK(strcmp(urls[0], "ws://127.0.0.1:2") == 0);
  CHECK(strcmp(urls[1], "ws://127.0.0.1:3") == 0);

  /* The subscription intent must survive: a relay change that silently
   * dropped kinds 25910/1059 would leave the daemon connected but deaf to
   * management commands. */
  int got[3] = { 0, 0, 0 };
  size_t n_kinds = signet_relay_pool_get_subscribed_kinds(rp, got, 3);
  CHECK(n_kinds == 3);
  CHECK(got[0] == 24133);
  CHECK(got[1] == 25910);
  CHECK(got[2] == 1059);

  signet_relay_pool_free(rp);
  printf("PASS\n");
}

/* ------- 2b. an unreachable relay must not stall the reload (fp-e08y) ----- */

/* A socket that accepts TCP but never speaks WebSocket. libnostr's connect
 * then sits in the handshake until its 30s timeout, which is exactly the
 * stall an operator triggers by typing a relay URL wrong. */
static int open_stalling_listener(guint16 *out_port) {
  int s = socket(AF_INET, SOCK_STREAM, 0);
  CHECK(s >= 0);
  int one = 1;
  (void)setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0; /* ephemeral */
  CHECK(bind(s, (struct sockaddr *)&addr, sizeof(addr)) == 0);
  CHECK(listen(s, 8) == 0);

  socklen_t len = sizeof(addr);
  CHECK(getsockname(s, (struct sockaddr *)&addr, &len) == 0);
  *out_port = ntohs(addr.sin_port);
  return s;
}

static void test_unreachable_relay_does_not_block_reload(void) {
  printf("  TEST unreachable_relay_does_not_block_reload ... ");

  guint16 port = 0;
  int listener = open_stalling_listener(&port);
  char *stall_url = g_strdup_printf("ws://127.0.0.1:%u", (unsigned)port);

  /* Port 1 refuses immediately, so constructing the pool stays fast. */
  const char *initial[] = { "ws://127.0.0.1:1" };
  SignetRelayPoolConfig cfg = {
    .relays = initial,
    .n_relays = 1,
    .on_event = NULL,
    .user_data = NULL,
  };
  SignetRelayPool *rp = signet_relay_pool_new(&cfg);
  CHECK(rp != NULL);

  const int kinds[] = { 24133, 25910, 1059 };
  (void)signet_relay_pool_subscribe_scoped(rp, kinds, 3, PK_A, 12345);

  /* The reload itself must return promptly. Synchronously this took the full
   * libnostr connect timeout (30s), during which signetd served nothing. */
  const char *unreachable[] = { NULL };
  unreachable[0] = stall_url;
  gint64 reload_start = g_get_monotonic_time();
  CHECK(signet_relay_pool_set_relays(rp, unreachable, 1) == 0);
  gint64 reload_us = g_get_monotonic_time() - reload_start;
  CHECK(reload_us < 5 * G_USEC_PER_SEC);

  gint64 t0;

  /* And the daemon must stay responsive while the connect runs in the
   * background. Both of these take rp->mu, so they would block behind the
   * dialling thread if it held the lock across the connect. */
  t0 = g_get_monotonic_time();
  size_t n_urls = 0;
  const char *const *urls = signet_relay_pool_get_urls(rp, &n_urls);
  CHECK(n_urls == 1);
  CHECK(strcmp(urls[0], stall_url) == 0);

  int got[3] = { 0, 0, 0 };
  CHECK(signet_relay_pool_get_subscribed_kinds(rp, got, 3) == 3);
  CHECK(got[0] == 24133);
  CHECK(got[1] == 25910);
  CHECK(got[2] == 1059);
  CHECK(g_get_monotonic_time() - t0 < 5 * G_USEC_PER_SEC);

  /* Closing the listener resets the pending connection, so the background
   * dial fails fast instead of sitting out its timeout. */
  close(listener);
  CHECK(signet_relay_pool_wait_reconfigure(rp, 60000));
  gint64 total_us = g_get_monotonic_time() - reload_start;

  /* Say which mode this ran in rather than passing silently. Some
   * environments (no working libwebsockets TLS context, no /dev/urandom in a
   * sandbox) fail every connect instantly, and then no arrangement of
   * sockets can produce the stall this test is about. When the dial WAS
   * slow, assert the real property: the reload returned in a small fraction
   * of the time the connect actually took. */
  if (total_us > 500 * 1000) {
    fprintf(stderr, "[reload=%lldus of %lldus dial] ",
            (long long)reload_us, (long long)total_us);
    CHECK(reload_us < total_us / 10);
  } else {
    fprintf(stderr, "[connects fail fast here; timing bound only] ");
  }

  /* Subscription intent survives the failed connect: nothing re-derives it,
   * so a relay that comes up later still gets kinds 24133/25910/1059. */
  memset(got, 0, sizeof(got));
  CHECK(signet_relay_pool_get_subscribed_kinds(rp, got, 3) == 3);
  CHECK(got[1] == 25910);

  signet_relay_pool_free(rp);
  g_free(stall_url);
  printf("PASS\n");
}

/* -------------------- 3. eager policy reload ------------------------------ */

static void test_policy_reload_is_eager(void) {
  printf("  TEST policy_reload_is_eager ... ");

  char *pol_path = tmp_path("policies.conf");
  write_file(pol_path,
             "[identity.test-identity]\n"
             "default = \"deny\"\n"
             "allow_methods = \"sign_event\"\n");

  SignetPolicyStore *ps = signet_policy_store_file_new(pol_path);
  CHECK(ps != NULL);

  SignetPolicyKeyView key = {
    .identity = "test-identity",
    .method = "sign_event",
    .client_pubkey_hex = PK_A,
    .event_kind = 1,
  };
  SignetPolicyValue val;
  int64_t now = (int64_t)time(NULL);

  memset(&val, 0, sizeof(val));
  CHECK(signet_policy_store_get(ps, &key, now, &val) == 0);
  CHECK(val.decision == SIGNET_POLICY_RULE_ALLOW);

  /* Rewrite the policy so sign_event is denied. */
  write_file(pol_path,
             "[identity.test-identity]\n"
             "default = \"deny\"\n"
             "deny_methods = \"sign_event\"\n");

  /* Without an explicit reload the store keeps serving the old policy: this
   * is precisely the lazy behaviour that made SIGHUP a no-op on an idle
   * daemon. Asserting it here is what gives the next assertion its meaning. */
  memset(&val, 0, sizeof(val));
  CHECK(signet_policy_store_get(ps, &key, now, &val) == 0);
  CHECK(val.decision == SIGNET_POLICY_RULE_ALLOW);

  /* Eager reload applies the new policy immediately. */
  CHECK(signet_policy_store_reload(ps, now) == 0);

  memset(&val, 0, sizeof(val));
  CHECK(signet_policy_store_get(ps, &key, now, &val) == 0);
  CHECK(val.decision == SIGNET_POLICY_RULE_DENY);

  signet_policy_store_free(ps);
  g_unlink(pol_path);
  g_free(pol_path);
  printf("PASS\n");
}

/* ------- 4. provisioner precedence: no resurrection across reload --------- */

static void test_revoked_provisioner_not_resurrected(void) {
  printf("  TEST revoked_provisioner_not_resurrected ... ");

  char *cfg_path = tmp_path("provisioners.conf");
  char *db_path = NULL;
  SignetStore *store = open_store(&db_path);

  const int64_t t0 = (int64_t)time(NULL) - 1000;

  /* Config authorizes A and B; the daemon seeds them at first start. */
  write_config(cfg_path, "wss://relay.one", PK_A "," PK_B);
  set_mtime(cfg_path, (time_t)t0);

  const char *seed[] = { PK_A, PK_B };
  CHECK(signet_store_seed_provisioners(store, seed, 2, t0) == 0);
  CHECK(signet_store_is_provisioner(store, PK_A));
  CHECK(signet_store_is_provisioner(store, PK_B));

  SignetConfig live;
  CHECK(signet_config_load(cfg_path, &live) == 0);

  SignetReloadCtx ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.config_path = cfg_path;
  ctx.live = &live;
  ctx.store = store;

  /* The invariant is established once at startup. */
  bool adopted = false;
  CHECK(signet_config_reload_adopt_persisted(&ctx, t0, &adopted) == 0);
  CHECK(adopted);

  /* --- Operator revokes B at runtime. The config file still lists it. --- */
  const int64_t t_revoke = t0 + 100;
  CHECK(signet_store_revoke_provisioner_ex(store, PK_B, PK_A, t_revoke) == 0);
  CHECK(!signet_store_is_provisioner(store, PK_B));
  CHECK(signet_store_provisioner_revoked_at(store, PK_B) == t_revoke);

  /* Simulate the dangerous state directly: a config file that predates the
   * revocation and still names the revoked provisioner. This is exactly what
   * "config wins" would resurrect. */
  write_config(cfg_path, "wss://relay.one", PK_A "," PK_B);
  set_mtime(cfg_path, (time_t)t0);
  CHECK(config_lists_provisioner(cfg_path, PK_B));

  SignetReloadReport report;
  CHECK(signet_config_reload_apply(&ctx, t_revoke + 10, &report) == 0);
  CHECK(report.applied);

  /* THE assertion: the reload did not restore revoked authority. */
  CHECK(!signet_store_is_provisioner(store, PK_B));
  CHECK(report.provisioners.refused_resurrect == 1);
  CHECK(signet_store_is_provisioner(store, PK_A));

  /* And the divergence was repaired by write-back rather than left to recur:
   * the config file no longer claims B is a provisioner. */
  CHECK(!report.provisioner_writeback_failed);
  CHECK(!config_lists_provisioner(cfg_path, PK_B));
  CHECK(config_lists_provisioner(cfg_path, PK_A));

  /* Write-back rewrites the file, so unrelated keys must survive it. */
  {
    char *rewritten = read_file(cfg_path);
    CHECK(rewritten != NULL);
    CHECK(strstr(rewritten, "test-identity") != NULL);
    CHECK(strstr(rewritten, "wss://relay.one") != NULL);
    g_free(rewritten);
  }

  /* A second reload is quiet: config and store now agree. */
  signet_config_clear(&live);
  CHECK(signet_config_load(cfg_path, &live) == 0);
  memset(&report, 0, sizeof(report));
  CHECK(signet_config_reload_apply(&ctx, t_revoke + 20, &report) == 0);
  CHECK(report.provisioners.refused_resurrect == 0);
  CHECK(report.provisioners.granted == 0);
  CHECK(report.provisioners.revoked == 0);
  CHECK(!signet_store_is_provisioner(store, PK_B));

  /* --- Config is still desired state for everything else. --- */

  /* Adding C in config grants it on reload. */
  write_config(cfg_path, "wss://relay.one", PK_A "," PK_C);
  set_mtime(cfg_path, (time_t)(t_revoke + 200));
  signet_config_clear(&live);
  CHECK(signet_config_load(cfg_path, &live) == 0);
  memset(&report, 0, sizeof(report));
  CHECK(signet_config_reload_apply(&ctx, t_revoke + 210, &report) == 0);
  CHECK(signet_store_is_provisioner(store, PK_C));
  CHECK(report.provisioners.granted == 1);

  /* Dropping A from config revokes it on reload (config wins). */
  write_config(cfg_path, "wss://relay.one", PK_C);
  set_mtime(cfg_path, (time_t)(t_revoke + 300));
  signet_config_clear(&live);
  CHECK(signet_config_load(cfg_path, &live) == 0);
  memset(&report, 0, sizeof(report));
  CHECK(signet_config_reload_apply(&ctx, t_revoke + 310, &report) == 0);
  CHECK(!signet_store_is_provisioner(store, PK_A));
  CHECK(report.provisioners.revoked == 1);
  /* That revocation is itself tombstoned, so re-adding a stale config cannot
   * bring A back either. */
  CHECK(signet_store_provisioner_revoked_at(store, PK_A) == t_revoke + 310);

  /* --- A deliberate operator re-grant DOES work: the config source is newer
   * than the tombstone, which is how an operator says "yes, really". --- */
  const int64_t t_regrant = t_revoke + 1000;
  write_config(cfg_path, "wss://relay.one", PK_B "," PK_C);
  set_mtime(cfg_path, (time_t)t_regrant);
  signet_config_clear(&live);
  CHECK(signet_config_load(cfg_path, &live) == 0);
  memset(&report, 0, sizeof(report));
  CHECK(signet_config_reload_apply(&ctx, t_regrant + 10, &report) == 0);
  CHECK(report.provisioners.refused_resurrect == 0);
  CHECK(signet_store_is_provisioner(store, PK_B));
  CHECK(signet_store_provisioner_revoked_at(store, PK_B) == 0);

  signet_config_clear(&live);
  signet_store_close(store);
  g_unlink(db_path);
  g_free(db_path);
  g_unlink(cfg_path);
  g_free(cfg_path);
  printf("PASS\n");
}

/* An env-sourced provisioner list can never outrank a runtime revocation:
 * it cannot be written back, and it cannot have been authored after the
 * process started. */
static void test_env_sourced_set_cannot_resurrect(void) {
  printf("  TEST env_sourced_set_cannot_resurrect ... ");

  char *db_path = NULL;
  SignetStore *store = open_store(&db_path);

  const int64_t t0 = (int64_t)time(NULL) - 500;
  const char *seed[] = { PK_A };
  CHECK(signet_store_seed_provisioners(store, seed, 1, t0) == 0);
  CHECK(signet_store_revoke_provisioner_ex(store, PK_A, NULL, t0 + 10) == 0);

  /* source mtime 0 models the environment override. */
  SignetProvisionerReconcile stats;
  const char *desired[] = { PK_A };
  CHECK(signet_store_reconcile_provisioners(store, desired, 1, 0, t0 + 20,
                                             &stats) == 0);
  CHECK(stats.refused_resurrect == 1);
  CHECK(stats.granted == 0);
  CHECK(!signet_store_is_provisioner(store, PK_A));

  signet_store_close(store);
  g_unlink(db_path);
  g_free(db_path);
  printf("PASS\n");
}

/* ------------------ restart-only keys are warned, not applied ------------- */

static void test_restart_only_keys_warned_not_applied(void) {
  printf("  TEST restart_only_keys_warned_not_applied ... ");

  char *cfg_path = tmp_path("restart-only.conf");
  write_config(cfg_path, "wss://relay.one", PK_A);

  SignetConfig live;
  CHECK(signet_config_load(cfg_path, &live) == 0);
  const int live_health_port = live.health_port;

  /* Change a listener and a storage binding: both restart-only under D1. */
  char *contents = g_strdup_printf(
      "[server]\n"
      "log_level = debug\n"
      "health_port = 9999\n"
      "\n"
      "[store]\n"
      "db_path = /tmp/some-other-database.db\n"
      "\n"
      "[nostr]\n"
      "relays = wss://relay.one\n"
      "provisioner_pubkeys = %s\n"
      "identity = a-different-identity\n",
      PK_A);
  write_file(cfg_path, contents);
  g_free(contents);

  SignetReloadCtx ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.config_path = cfg_path;
  ctx.live = &live;

  SignetReloadReport report;
  CHECK(signet_config_reload_apply(&ctx, (int64_t)time(NULL), &report) == 0);
  CHECK(report.applied);

  /* db_path, health_port and identity each changed and each was warned. */
  CHECK(report.restart_only_changed >= 3);

  /* Restart-only values keep running as they were... */
  CHECK(live.health_port == live_health_port);
  CHECK(strcmp(live.db_path, "/tmp/signet-config-reload-test.db") == 0);
  CHECK(strcmp(live.identity, "test-identity") == 0);

  /* ...while a live-reloadable scalar did move. */
  CHECK(live.log_level == SIGNET_LOG_DEBUG);

  signet_config_clear(&live);
  g_unlink(cfg_path);
  g_free(cfg_path);
  printf("PASS\n");
}

/* --------------------------------- main ---------------------------------- */

int main(void) {
  GError *err = NULL;
  g_tmpdir = g_dir_make_tmp("signet-config-reload-XXXXXX", &err);
  CHECK(g_tmpdir != NULL);

  /* The reload path reads the environment; keep the test hermetic. */
  g_unsetenv("SIGNET_RELAYS");
  g_unsetenv("SIGNET_PROVISIONER_PUBKEYS");
  g_unsetenv("SIGNET_DB_PATH");
  g_unsetenv("SIGNET_POLICY_PATH");
  g_unsetenv("SIGNET_HEALTH_PORT");
  g_unsetenv("SIGNET_LOG_LEVEL");
  g_unsetenv("SIGNET_BUNKER_NSEC");

  char *nsec_path = tmp_path("bunker.hex");
  write_file(nsec_path, BUNKER_SK);
  g_setenv("SIGNET_BUNKER_NSEC_FILE", nsec_path, TRUE);

  printf("test_config_reload:\n");
  test_invalid_candidate_retains_last_valid();
  test_subscriptions_survive_relay_change();
  test_unreachable_relay_does_not_block_reload();
  test_policy_reload_is_eager();
  test_revoked_provisioner_not_resurrected();
  test_env_sourced_set_cannot_resurrect();
  test_restart_only_keys_warned_not_applied();

  g_unlink(nsec_path);
  g_free(nsec_path);
  g_rmdir(g_tmpdir);
  g_free(g_tmpdir);

  printf("test_config_reload: ALL PASS\n");
  return 0;
}
