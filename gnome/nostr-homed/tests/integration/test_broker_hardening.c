/* Broker hardening integration test (beads nostrc-zcll.2).
 *
 * Drives a persistent broker across many SEQPACKET connections with a fake
 * monotonic clock (mmap'd MAP_SHARED so parent and child see the same value)
 * and a tight rate-limit policy, then asserts:
 *
 *   (a) SUBMIT_UNLOCK issued after the challenge deadline maps to EXPIRED
 *       (the broker-layer guard fires without even engaging the local
 *       provider; the same result also emerges from the SM's own timely()
 *       check on start_verification, so this exercises the mapping too).
 *   (b) After N failed proofs a fresh BEGIN_LOGIN for the same account
 *       returns RATE_LIMITED, and a correct proof issued *before* the
 *       budget trips both succeeds and resets the counter so the next
 *       failure lands in a fresh window.
 *
 * The test seeds n_alice with a real vault (same pattern as
 * test_broker_login.c) and runs as a normal user vs. root: as root the
 * hardening assertions must hold; as a non-root peer the AUTH-endpoint ACL
 * denies BEGIN_LOGIN outright, which we assert.
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include "../nh_test.h"
#include "auth_broker.h"
#include "auth_client.h"
#include "auth_ratelimit.h"
#include "auth_vault.h"
#include "nostr_auth_protocol.h"
#include "nostr_identity.h"

#include <errno.h>
#include <fcntl.h>
#include <jansson.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

/* Same secp256k1 private-key-1 fixture as test_broker_login.c. */
static const char *PUBKEY =
    "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798";
static const char *PASSPHRASE = "correct horse battery staple";

/* Enough of a starting clock to keep every deadline arithmetic well under
 * UINT64_MAX and above zero (so pre-clock reads inside the SM stay
 * non-decreasing). Arbitrary constant, just needs to be > 0. */
#define BASE_CLOCK_MS (1000ull * 1000ull) /* 1000 seconds. */

static nh_identity_ownership_result available(void *c, const char *n,
                                              uint32_t u, uint32_t g) {
  (void)c; (void)n; (void)u; (void)g;
  return NH_IDENTITY_OWNERSHIP_FREE;
}

static void set_config(nh_identity_config *config, const char *dir) {
  nh_identity_config_defaults(config);
  snprintf(config->authority_path, sizeof config->authority_path,
           "%s/authority.db", dir);
  snprintf(config->projection_path, sizeof config->projection_path,
           "%s/nss.db", dir);
  snprintf(config->home_root, sizeof config->home_root, "%s/home", dir);
}

static nh_identity_store *open_store(const char *dir, uint32_t flags) {
  static nh_identity_config config; /* must outlive the store */
  set_config(&config, dir);
  nh_identity_store_options options = {0};
  options.config = &config;
  options.ownership_probe = available;
  options.flags = flags;
  nh_identity_store *store = NULL;
  NH_CHECK(nh_identity_store_open(&options, &store) == NH_IDENTITY_OK);
  return store;
}

/* Copy of test_broker_login.c's seed helper. Local-provider vault sealed
 * with PASSPHRASE and bound to the account's assigned provider_id. */
static void seed(const char *dir) {
  nh_identity_store *store = open_store(dir, NH_IDENTITY_STORE_CREATE);
  nh_identity_enroll_request enroll = {0};
  enroll.username = "n_alice";
  enroll.pubkey_hex = PUBKEY;
  enroll.home_mode = NH_IDENTITY_HOME_CREATE;
  nh_identity_operation_state state;
  const char *op = "00000000-0000-4000-8000-000000000001";
  NH_CHECK(nh_identity_operation_begin_enroll(store, op, &enroll, &state) ==
           NH_IDENTITY_OK);
  nh_identity_home_evidence staged = {11, 101}, installed = {11, 202};
  NH_CHECK(nh_identity_operation_advance_home(store, op,
             NH_IDENTITY_PHASE_RESERVED, NH_IDENTITY_PHASE_STAGED, &staged,
             &state) == NH_IDENTITY_OK);
  NH_CHECK(nh_identity_operation_advance_home(store, op,
             NH_IDENTITY_PHASE_STAGED, NH_IDENTITY_PHASE_INSTALLED, &installed,
             &state) == NH_IDENTITY_OK);
  nh_identity_account account;
  NH_CHECK(nh_identity_store_lookup_by_name(store, "n_alice", &account) ==
           NH_IDENTITY_OK);
  char provider_id[NH_IDENTITY_UUID_CAP];
  const uint8_t placeholder[] = {1, 2, 3, 4};
  NH_CHECK(nh_identity_provider_stage(store,
             "00000000-0000-4000-8000-000000000002", account.account_id,
             NH_IDENTITY_PROVIDER_LOCAL_ENCRYPTED_KEY, 1, "{}", placeholder,
             sizeof placeholder, provider_id) == NH_IDENTITY_OK);
  nh_identity_proof_attestation attestation = {0};
  strcpy(attestation.pubkey_hex, PUBKEY);
  attestation.key_generation = account.key_generation;
  uint8_t secret[32] = {0};
  secret[31] = 1;
  nh_auth_vault_binding binding = {provider_id, account.account_id, PUBKEY,
                                   account.key_generation};
  uint8_t *blob = NULL;
  size_t blob_len = 0;
  NH_CHECK(nh_auth_vault_seal(secret, (const uint8_t *)PASSPHRASE,
             strlen(PASSPHRASE), &binding, &blob, &blob_len) ==
           NH_AUTH_VAULT_OK);
  NH_CHECK(nh_identity_provider_reseal(store,
             "00000000-0000-4000-8000-000000000004", provider_id, blob,
             blob_len) == NH_IDENTITY_OK);
  free(blob);
  NH_CHECK(nh_identity_provider_activate(store,
             "00000000-0000-4000-8000-000000000003", provider_id,
             &attestation) == NH_IDENTITY_OK);
  NH_CHECK(nh_identity_store_publish_projection(store, NULL) ==
           NH_IDENTITY_OK);
  NH_CHECK(nh_identity_operation_activate(store, op, &state) ==
           NH_IDENTITY_OK);
  nh_identity_store_close(store);
}

/* Shared-page clock. The child broker reads it via nh_auth_broker_set_clock;
 * the parent scribbles into it between operations to fast-forward time. */
static uint64_t clock_fn(void *ctx) { return *(volatile uint64_t *)ctx; }

/* ---- persistent broker server (child) ---------------------------------- */

typedef struct server_handle {
  pid_t pid;
  char sockpath[128];
} server_handle;

static void run_server(const char *dir, const char *sockpath,
                       uint64_t *shared_clock,
                       const nh_auth_ratelimit_config *rl_cfg) {
  nh_identity_store *store = open_store(dir, 0);
  nh_auth_broker *broker = nh_auth_broker_new(store);
  NH_CHECK(broker);
  nh_auth_broker_set_clock(broker, clock_fn, shared_clock);
  NH_CHECK(nh_auth_broker_set_ratelimit_config(broker, rl_cfg) == 0);

  int lfd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  NH_CHECK(lfd >= 0);
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof addr);
  addr.sun_family = AF_UNIX;
  NH_CHECK(strlen(sockpath) < sizeof addr.sun_path);
  strcpy(addr.sun_path, sockpath);
  (void)unlink(sockpath);
  NH_CHECK(bind(lfd, (struct sockaddr *)&addr, sizeof addr) == 0);
  NH_CHECK(listen(lfd, 4) == 0);

  /* Ignore SIGPIPE: a closed client connection must not kill the server. */
  signal(SIGPIPE, SIG_IGN);

  for (;;) {
    int c = accept(lfd, NULL, NULL);
    if (c < 0) {
      if (errno == EINTR) continue;
      break;
    }
    (void)nh_auth_broker_handle_connection(broker, c);
    close(c);
  }

  close(lfd);
  (void)unlink(sockpath);
  nh_auth_broker_free(broker);
  nh_identity_store_close(store);
  _exit(0);
}

static void start_server(server_handle *h, const char *dir,
                         const char *tmpbase, uint64_t *shared_clock,
                         const nh_auth_ratelimit_config *rl_cfg) {
  snprintf(h->sockpath, sizeof h->sockpath, "%s/broker.sock", tmpbase);
  pid_t pid = fork();
  NH_CHECK(pid >= 0);
  if (pid == 0) {
    run_server(dir, h->sockpath, shared_clock, rl_cfg);
    _exit(0);
  }
  h->pid = pid;
  /* Wait for the child to bind. accept() blocks so a short spin on stat()
   * is fine and avoids a fixed sleep. */
  for (int i = 0; i < 200; i++) {
    struct stat st;
    if (stat(h->sockpath, &st) == 0) return;
    struct timespec ts = {0, 10 * 1000 * 1000}; /* 10 ms */
    nanosleep(&ts, NULL);
  }
  NH_CHECK(!"broker server never bound its socket");
}

static void stop_server(server_handle *h) {
  if (h->pid <= 0) return;
  kill(h->pid, SIGTERM);
  int status = 0;
  waitpid(h->pid, &status, 0);
  (void)unlink(h->sockpath);
  h->pid = 0;
}

/* ---- low-level request helpers ---------------------------------------- */

static int gen_request_id(char out[NH_AUTH_REQUEST_ID_HEX_LEN + 1]) {
  static uint8_t counter[NH_AUTH_REQUEST_ID_HEX_LEN / 2];
  static const char hex[] = "0123456789abcdef";
  /* Deterministic ids are fine — the broker only checks well-formedness. */
  for (size_t i = 0; i < sizeof counter; i++) counter[i]++;
  for (size_t i = 0; i < sizeof counter; i++) {
    out[i * 2] = hex[counter[i] >> 4];
    out[i * 2 + 1] = hex[counter[i] & 0xf];
  }
  out[NH_AUTH_REQUEST_ID_HEX_LEN] = '\0';
  return 0;
}

static int send_op(int fd, nh_auth_operation op, json_t *payload) {
  char *payload_json = json_dumps(payload, JSON_COMPACT);
  json_decref(payload);
  if (!payload_json) return -1;
  nh_auth_message m;
  memset(&m, 0, sizeof m);
  m.operation = op;
  m.payload_json = payload_json;
  m.transaction_id[0] = '\0';
  gen_request_id(m.request_id);
  int rc = nh_auth_send_message(fd, &m);
  free(payload_json);
  return rc;
}

static int recv_result(int fd, nh_auth_result *out) {
  unsigned char *pkt = NULL;
  size_t pkt_len = 0;
  if (nh_auth_recv_packet(fd, &pkt, &pkt_len) != 0) return -1;
  nh_auth_message resp;
  int rc = nh_auth_message_parse(pkt, pkt_len, &resp);
  free(pkt);
  if (rc != 0) return -1;
  int found = -1;
  if (resp.payload_json) {
    json_error_t e;
    json_t *root = json_loads(resp.payload_json, 0, &e);
    if (root) {
      json_t *r = json_object_get(root, "result");
      if (r && json_is_string(r)) {
        const char *name = json_string_value(r);
        for (int i = 0; i <= NH_AUTH_RESULT_INTERNAL_ERROR; i++) {
          const char *n = nh_auth_result_name((nh_auth_result)i);
          if (n && !strcmp(n, name)) {
            *out = (nh_auth_result)i;
            found = 0;
            break;
          }
        }
      }
      json_decref(root);
    }
  }
  nh_auth_message_clear(&resp);
  return found;
}

/* Full flow: BEGIN_LOGIN → SELECT_PROVIDER(local) → SUBMIT_UNLOCK(secret).
 * On any step other than the final SUBMIT_UNLOCK returning a non-OK/non-
 * INTERACTION_REQUIRED result, the last observed result is returned so the
 * caller can assert against it.
 *
 * `advance_after_select` — if non-zero, the shared clock is bumped by that
 * many milliseconds after SELECT_PROVIDER and before SUBMIT_UNLOCK, so the
 * challenge deadline can be pushed into the past. */
static nh_auth_result run_login(const char *sockpath, uint64_t *shared_clock,
                                const char *secret,
                                uint64_t advance_after_select) {
  int fd = -1;
  NH_CHECK(nh_auth_client_connect(sockpath, &fd) == 0);

  nh_auth_result r = NH_AUTH_RESULT_INTERNAL_ERROR;

  json_t *begin = json_object();
  json_object_set_new(begin, "username", json_string("n_alice"));
  json_object_set_new(begin, "service", json_string("gdm-password"));
  NH_CHECK(send_op(fd, NH_AUTH_OP_BEGIN_LOGIN, begin) == 0);
  NH_CHECK(recv_result(fd, &r) == 0);
  if (r != NH_AUTH_RESULT_OK) {
    close(fd);
    return r;
  }

  json_t *sel = json_object();
  json_object_set_new(sel, "provider", json_string("local"));
  NH_CHECK(send_op(fd, NH_AUTH_OP_SELECT_PROVIDER, sel) == 0);
  NH_CHECK(recv_result(fd, &r) == 0);
  if (r != NH_AUTH_RESULT_INTERACTION_REQUIRED) {
    close(fd);
    return r;
  }

  if (advance_after_select)
    *(volatile uint64_t *)shared_clock += advance_after_select;

  json_t *unlock = json_object();
  json_object_set_new(unlock, "secret", json_string(secret));
  NH_CHECK(send_op(fd, NH_AUTH_OP_SUBMIT_UNLOCK, unlock) == 0);
  NH_CHECK(recv_result(fd, &r) == 0);
  close(fd);
  return r;
}

/* ---- root-only scenarios ---------------------------------------------- */

static void run_root_scenarios(const char *sockpath, uint64_t *shared_clock) {
  /* (a) Challenge deadline: after SELECT_PROVIDER, jump the clock past
   *     NH_AUTH_CHALLENGE_LIFETIME_SEC. SUBMIT_UNLOCK must map to EXPIRED
   *     regardless of the passphrase being correct. */
  *shared_clock = BASE_CLOCK_MS;
  uint64_t bump =
      (uint64_t)NH_AUTH_CHALLENGE_LIFETIME_SEC * 1000ull + 5000ull;
  nh_auth_result r = run_login(sockpath, shared_clock, PASSPHRASE, bump);
  printf("expired-flow -> %s\n", nh_auth_result_name(r));
  NH_CHECK(r == NH_AUTH_RESULT_EXPIRED);

  /* Reset the clock and clear any rate-limit dust from prior attempts.
   * Successful login before the budget trips should PASS and reset the
   * counter. */
  *shared_clock = BASE_CLOCK_MS + 10 * 60ull * 1000ull; /* +10 min */

  r = run_login(sockpath, shared_clock, PASSPHRASE, 0);
  printf("good-proof-baseline -> %s\n", nh_auth_result_name(r));
  NH_CHECK(r == NH_AUTH_RESULT_OK);

  /* (b) Rate limit. Configured with max_failures=2 (see main), so:
   *   fail 1: INVALID_PROOF
   *   fail 2: INVALID_PROOF, triggers cooldown
   *   attempt 3: RATE_LIMITED (BEGIN_LOGIN gated before SM is touched)
   *
   * Then push the clock past the cooldown, confirm we get INVALID_PROOF
   * again (not rate-limited), then run a good proof to reset. */
  *shared_clock += 1000ull; /* small monotonic step */
  r = run_login(sockpath, shared_clock, "wrong-1", 0);
  printf("bad-proof-1 -> %s\n", nh_auth_result_name(r));
  NH_CHECK(r == NH_AUTH_RESULT_INVALID_PROOF);

  *shared_clock += 1000ull;
  r = run_login(sockpath, shared_clock, "wrong-2", 0);
  printf("bad-proof-2 -> %s\n", nh_auth_result_name(r));
  NH_CHECK(r == NH_AUTH_RESULT_INVALID_PROOF);

  *shared_clock += 1000ull;
  r = run_login(sockpath, shared_clock, "wrong-3", 0);
  printf("bad-proof-3 (in cooldown) -> %s\n", nh_auth_result_name(r));
  NH_CHECK(r == NH_AUTH_RESULT_RATE_LIMITED);

  /* Advance beyond the cooldown (>= configured cooldown_ms; we use 60s).
   * A correct proof should still work and MUST reset the counter — otherwise
   * the next failure would immediately re-trip the (already-at-budget)
   * counter and lock the account out again. */
  *shared_clock += 61ull * 1000ull;
  r = run_login(sockpath, shared_clock, PASSPHRASE, 0);
  printf("good-proof-after-cooldown -> %s\n", nh_auth_result_name(r));
  NH_CHECK(r == NH_AUTH_RESULT_OK);

  /* Post-reset, a fresh failure should NOT be immediately rate-limited
   * (the counter starts a new window). */
  *shared_clock += 1000ull;
  r = run_login(sockpath, shared_clock, "wrong-4", 0);
  printf("bad-proof-4 (fresh window after reset) -> %s\n",
         nh_auth_result_name(r));
  NH_CHECK(r == NH_AUTH_RESULT_INVALID_PROOF);
}

/* ---- non-root scenario ------------------------------------------------- */

static void run_nonroot_scenarios(const char *sockpath,
                                  uint64_t *shared_clock) {
  /* The AUTH endpoint ACL denies BEGIN_LOGIN for uid != 0 outright, before
   * any rate-limit or deadline check runs. This mirrors test_broker_login.c
   * on the non-root path. */
  *shared_clock = BASE_CLOCK_MS;
  nh_auth_result r = run_login(sockpath, shared_clock, PASSPHRASE, 0);
  printf("nonroot begin_login -> %s\n", nh_auth_result_name(r));
  NH_CHECK(r == NH_AUTH_RESULT_DENIED);
}

/* ---- entry ------------------------------------------------------------- */

int main(void) {
  char dir[] = "/tmp/nostr-hardening-XXXXXX";
  NH_CHECK(mkdtemp(dir));
  seed(dir);

  uint64_t *shared_clock = mmap(NULL, sizeof *shared_clock,
                                PROT_READ | PROT_WRITE,
                                MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  NH_CHECK(shared_clock != MAP_FAILED);
  *shared_clock = BASE_CLOCK_MS;

  nh_auth_ratelimit_config rl = {0};
  nh_auth_ratelimit_config_defaults(&rl);
  /* Trip after 2 failed proofs; keep a 1 minute cooldown so the "in
   * cooldown -> RATE_LIMITED" step is unambiguous. */
  rl.max_failures = 2u;
  rl.window_ms = 5u * 60u * 1000u;
  rl.cooldown_ms = 60u * 1000u;

  server_handle srv;
  memset(&srv, 0, sizeof srv);
  start_server(&srv, dir, dir, shared_clock, &rl);

  int is_root = (geteuid() == 0);
  if (is_root)
    run_root_scenarios(srv.sockpath, shared_clock);
  else
    run_nonroot_scenarios(srv.sockpath, shared_clock);

  stop_server(&srv);
  munmap(shared_clock, sizeof *shared_clock);

  char path[1024];
  const char *files[] = {"authority.db", "authority.db-wal",
                         "authority.db-shm", "authority.lock", "nss.db",
                         "broker.sock"};
  for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
    snprintf(path, sizeof path, "%s/%s", dir, files[i]);
    unlink(path);
  }
  rmdir(dir);
  printf("RESULT: PASS (root=%d)\n", is_root);
  return 0;
}
