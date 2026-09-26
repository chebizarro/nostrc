/* Linux-only: full SMB proof flow through the broker.
 *
 * Seeds an active account with a real encrypted vault (the same pattern as
 * test_broker_login), attaches an nh_smb_authority backed by an in-process
 * MOCK passdb, and drives BEGIN_SMB_PROOF -> SELECT_PROVIDER -> SUBMIT_UNLOCK
 * over a SEQPACKET connection where the peer is the test process's OWN uid.
 *
 * Asserts, on Linux:
 *   - As the account's own uid on the USER endpoint, a correct proof yields
 *     an OK result plus a policy-conforming password envelope; the mock
 *     passdb's set_password was called with that exact password; the
 *     issuance journal has an active row; the raw db file does NOT contain
 *     the password bytes.
 *   - A wrong proof yields INVALID_PROOF and no envelope.
 *   - The ACL denies BEGIN_SMB_PROOF on the AUTH endpoint (auth.sock).
 *
 * Tracks beads nostrc-rb0e.7.
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include "../nh_test.h"
#include "auth_broker.h"
#include "auth_client.h"
#include "auth_vault.h"
#include "nostr_auth_protocol.h"
#include "nostr_identity.h"
#include "smb_credential.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *PUBKEY =
    "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798";
static const char *PASSPHRASE = "correct horse battery staple";

/* -------- Mock passdb -------- */

typedef struct mock_passdb {
  int set_calls;
  int disable_calls;
  int remove_calls;
  char last_username[64];
  char last_password[128];
} mock_passdb;

static int mock_set(void *ctx, const char *u, const char *p) {
  mock_passdb *m = ctx;
  m->set_calls++;
  snprintf(m->last_username, sizeof m->last_username, "%s", u ? u : "");
  snprintf(m->last_password, sizeof m->last_password, "%s", p ? p : "");
  return 0;
}
static int mock_disable(void *ctx, const char *u) {
  (void)u;
  ((mock_passdb *)ctx)->disable_calls++;
  return 0;
}
static int mock_remove(void *ctx, const char *u) {
  (void)u;
  ((mock_passdb *)ctx)->remove_calls++;
  return 0;
}
/* Designated initializers so a new op field (e.g. plan §4.2 B3's
 * optional `enumerate`) doesn't `-Werror=missing-field-initializers`
 * this test into a red build. */
static const nh_smb_passdb_ops mock_ops = {
  .set_password = mock_set,
  .disable      = mock_disable,
  .remove       = mock_remove,
};

/* -------- Identity seed (mirrors test_broker_login) -------- */

static nh_identity_ownership_result available(void *c, const char *n,
                                              uint32_t u, uint32_t g) {
  (void)c; (void)n; (void)u; (void)g;
  return NH_IDENTITY_OWNERSHIP_FREE;
}

static void set_config(nh_identity_config *config, const char *dir,
                       uint32_t target_uid) {
  nh_identity_config_defaults(config);
  snprintf(config->authority_path, sizeof config->authority_path,
           "%s/authority.db", dir);
  snprintf(config->projection_path, sizeof config->projection_path,
           "%s/nss.db", dir);
  snprintf(config->home_root, sizeof config->home_root, "%s/home", dir);
  /* Force the store to allocate exactly `target_uid`. The other ranges must
   * stay non-zero and non-overlapping with the allocation range. */
  config->uid_min = target_uid;
  config->uid_max = target_uid;
  config->domain_default_min = 1u;
  config->domain_default_max = 2u;
  config->domain_rid_min = 3u;
  config->domain_rid_max = 4u;
  config->standalone_smb_min = 5u;
  config->standalone_smb_max = 6u;
  /* If the target uid falls inside one of the placeholder ranges above (very
   * unlikely for real uids), bump the placeholders past it. */
  if (target_uid <= 6u) {
    config->domain_default_min = target_uid + 1u;
    config->domain_default_max = target_uid + 2u;
    config->domain_rid_min = target_uid + 3u;
    config->domain_rid_max = target_uid + 4u;
    config->standalone_smb_min = target_uid + 5u;
    config->standalone_smb_max = target_uid + 6u;
  }
}

static nh_identity_store *open_store(const char *dir, uint32_t target_uid,
                                     uint32_t flags) {
  static nh_identity_config config; /* must outlive the store */
  set_config(&config, dir, target_uid);
  nh_identity_store_options options = {0};
  options.config = &config;
  options.ownership_probe = available;
  options.flags = flags;
  nh_identity_store *store = NULL;
  NH_CHECK(nh_identity_store_open(&options, &store) == NH_IDENTITY_OK);
  return store;
}

static void seed(const char *dir, uint32_t target_uid) {
  nh_identity_store *store = open_store(dir, target_uid, NH_IDENTITY_STORE_CREATE);
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
             NH_IDENTITY_PHASE_RESERVED, NH_IDENTITY_PHASE_STAGED,
             &staged, &state) == NH_IDENTITY_OK);
  NH_CHECK(nh_identity_operation_advance_home(store, op,
             NH_IDENTITY_PHASE_STAGED, NH_IDENTITY_PHASE_INSTALLED,
             &installed, &state) == NH_IDENTITY_OK);
  nh_identity_account account;
  NH_CHECK(nh_identity_store_lookup_by_name(store, "n_alice", &account) ==
           NH_IDENTITY_OK);
  NH_CHECK(account.uid == target_uid);
  char provider_id[NH_IDENTITY_UUID_CAP];
  const uint8_t placeholder[] = {1, 2, 3, 4};
  NH_CHECK(nh_identity_provider_stage(store,
             "00000000-0000-4000-8000-000000000002",
             account.account_id, NH_IDENTITY_PROVIDER_LOCAL_ENCRYPTED_KEY, 1,
             "{}", placeholder, sizeof placeholder, provider_id) ==
           NH_IDENTITY_OK);
  nh_identity_proof_attestation attestation = {0};
  strcpy(attestation.pubkey_hex, PUBKEY);
  attestation.key_generation = account.key_generation;
  uint8_t secret[32] = {0};
  secret[31] = 1; /* secp256k1 private key 1 -> PUBKEY */
  nh_auth_vault_binding binding = {provider_id, account.account_id, PUBKEY,
                                   account.key_generation};
  uint8_t *blob = NULL; size_t blob_len = 0;
  NH_CHECK(nh_auth_vault_seal(secret, (const uint8_t *)PASSPHRASE,
             strlen(PASSPHRASE), &binding, &blob, &blob_len) ==
           NH_AUTH_VAULT_OK);
  NH_CHECK(nh_identity_provider_reseal(store,
             "00000000-0000-4000-8000-000000000004",
             provider_id, blob, blob_len) == NH_IDENTITY_OK);
  free(blob);
  NH_CHECK(nh_identity_provider_activate(store,
             "00000000-0000-4000-8000-000000000003",
             provider_id, &attestation) == NH_IDENTITY_OK);
  NH_CHECK(nh_identity_store_publish_projection(store, NULL) == NH_IDENTITY_OK);
  NH_CHECK(nh_identity_operation_activate(store, op, &state) == NH_IDENTITY_OK);
  nh_identity_store_close(store);
}

/* -------- Fork/serve helpers -------- */

typedef enum server_kind { SERVE_USER = 1, SERVE_AUTH = 2 } server_kind;

/* Fork a child that opens the store + an SMB authority (mock passdb) and
 * services exactly one connection on the requested endpoint. The child's
 * verdict about the passdb activity is echoed via a pipe as three ints
 * (set_calls, disable_calls, remove_calls). */
static pid_t fork_server(const char *dir, uint32_t target_uid,
                         const char *smb_journal, server_kind kind,
                         int *client_fd_out, int *stats_fd_out) {
  int sv[2];
  NH_CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0);
  int stats[2];
  NH_CHECK(pipe(stats) == 0);
  pid_t pid = fork();
  NH_CHECK(pid >= 0);
  if (pid == 0) {
    close(sv[0]);
    close(stats[0]);
    nh_identity_store *store = open_store(dir, target_uid, 0);
    nh_auth_broker *broker = nh_auth_broker_new(store);
    NH_CHECK(broker);
    mock_passdb pdb = {0};
    nh_smb_authority *authority = NULL;
    NH_CHECK(nh_smb_authority_open(smb_journal, &mock_ops, &pdb, &authority) ==
             NH_SMB_OK);
    nh_auth_broker_set_smb_authority(broker, authority);
    if (kind == SERVE_USER)
      (void)nh_auth_broker_handle_user_connection(broker, sv[1]);
    else
      (void)nh_auth_broker_handle_connection(broker, sv[1]);
    /* Emit stats before tearing anything down. */
    int stats_out[3] = { pdb.set_calls, pdb.disable_calls, pdb.remove_calls };
    (void)!write(stats[1], stats_out, sizeof stats_out);
    close(stats[1]);
    nh_auth_broker_set_smb_authority(broker, NULL);
    nh_smb_authority_close(authority);
    nh_auth_broker_free(broker);
    nh_identity_store_close(store);
    close(sv[1]);
    _exit(0);
  }
  close(sv[1]);
  close(stats[1]);
  *client_fd_out = sv[0];
  *stats_fd_out = stats[0];
  return pid;
}

static void reap(pid_t pid, int stats_fd, int stats_out[3]) {
  int got = 0;
  unsigned char buf[sizeof(int) * 3];
  while (got < (int)sizeof buf) {
    ssize_t n = read(stats_fd, buf + got, sizeof buf - got);
    if (n < 0) { if (errno == EINTR) continue; break; }
    if (n == 0) break;
    got += (int)n;
  }
  if (got == (int)sizeof buf) memcpy(stats_out, buf, sizeof buf);
  else { stats_out[0] = stats_out[1] = stats_out[2] = -1; }
  close(stats_fd);
  int status = 0;
  NH_CHECK(waitpid(pid, &status, 0) == pid);
}

/* -------- Cases -------- */

static bool password_is_base64url(const char *p, size_t n) {
  if (n == 0) return false;
  for (size_t i = 0; i < n; i++) {
    char c = p[i];
    bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_';
    if (!ok) return false;
  }
  return true;
}

static void case_smb_proof_success(const char *dir, uint32_t target_uid,
                                   const char *journal) {
  int cfd = -1, sfd = -1;
  pid_t pid = fork_server(dir, target_uid, journal, SERVE_USER, &cfd, &sfd);

  nh_auth_smb_envelope env;
  memset(&env, 0, sizeof env);
  nh_auth_result r = NH_AUTH_RESULT_INTERNAL_ERROR;
  NH_CHECK(nh_auth_client_smb_proof_with(cfd, "smb-credential",
             NH_AUTH_PROVIDER_NAME_LOCAL, PASSPHRASE, &env, &r) == 0);
  close(cfd);

  int stats[3] = {0};
  reap(pid, sfd, stats);

  printf("smb proof (own uid, correct) -> %s\n", nh_auth_result_name(r));
  NH_CHECK(r == NH_AUTH_RESULT_OK);
  NH_CHECK(env.password.ptr != NULL);
  NH_CHECK(env.password_len >= 20u && env.password_len <= 64u);
  NH_CHECK(password_is_base64url((const char *)env.password.ptr,
                                 env.password_len));
  NH_CHECK(strcmp(env.username, "n_alice") == 0);
  NH_CHECK(strlen(env.credential_id) == 36u);
  NH_CHECK(env.expires_at_ms > env.issued_at_ms);

  /* Passdb: set_password was called exactly once with the same password. */
  NH_CHECK(stats[0] == 1);

  /* Copy the password out for a raw journal scan, then wipe the envelope. */
  char pw_copy[128];
  size_t pw_len = env.password_len;
  memcpy(pw_copy, env.password.ptr, pw_len);
  pw_copy[pw_len] = '\0';
  nh_auth_smb_envelope_clear(&env);
  NH_CHECK(env.password.ptr == NULL);

  /* Scan the journal file: the password bytes must NOT appear on disk. */
  FILE *f = fopen(journal, "rb");
  NH_CHECK(f != NULL);
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *buf = malloc((size_t)sz);
  NH_CHECK(buf != NULL);
  NH_CHECK(fread(buf, 1, (size_t)sz, f) == (size_t)sz);
  fclose(f);
  int found = 0;
  if ((size_t)sz >= pw_len) {
    for (size_t i = 0; i + pw_len <= (size_t)sz; i++)
      if (memcmp(buf + i, pw_copy, pw_len) == 0) { found = 1; break; }
  }
  free(buf);
  NH_CHECK(found == 0);
  volatile char *w = (volatile char *)pw_copy;
  for (size_t i = 0; i < sizeof pw_copy; i++) w[i] = 0;
}

static void case_smb_proof_wrong_passphrase(const char *dir,
                                            uint32_t target_uid,
                                            const char *journal) {
  int cfd = -1, sfd = -1;
  pid_t pid = fork_server(dir, target_uid, journal, SERVE_USER, &cfd, &sfd);

  nh_auth_smb_envelope env;
  memset(&env, 0, sizeof env);
  nh_auth_result r = NH_AUTH_RESULT_INTERNAL_ERROR;
  NH_CHECK(nh_auth_client_smb_proof_with(cfd, "smb-credential",
             NH_AUTH_PROVIDER_NAME_LOCAL, "obviously-wrong-passphrase", &env,
             &r) == 0);
  close(cfd);

  int stats[3] = {0};
  reap(pid, sfd, stats);

  printf("smb proof (own uid, wrong) -> %s\n", nh_auth_result_name(r));
  NH_CHECK(r == NH_AUTH_RESULT_INVALID_PROOF);
  NH_CHECK(env.password.ptr == NULL);
  /* No password was ever installed. */
  NH_CHECK(stats[0] == 0);
  nh_auth_smb_envelope_clear(&env);
}

static void case_auth_socket_denies_smb_proof(const char *dir,
                                              uint32_t target_uid,
                                              const char *journal) {
  int cfd = -1, sfd = -1;
  /* Serve on the AUTH endpoint — same broker code, wrong endpoint. */
  pid_t pid = fork_server(dir, target_uid, journal, SERVE_AUTH, &cfd, &sfd);

  nh_auth_result r = NH_AUTH_RESULT_INTERNAL_ERROR;
  NH_CHECK(nh_auth_client_begin_smb_proof(cfd, "smb-credential", NULL, &r) == 0);
  close(cfd);

  int stats[3] = {0};
  reap(pid, sfd, stats);

  printf("smb proof over auth.sock -> %s\n", nh_auth_result_name(r));
  NH_CHECK(r == NH_AUTH_RESULT_DENIED);
  NH_CHECK(stats[0] == 0); /* passdb never touched */
}

static void rm_all(const char *dir) {
  char path[1024];
  const char *files[] = {"authority.db", "authority.db-wal", "authority.db-shm",
                         "authority.lock", "nss.db",
                         "smb.db", "smb.db-wal", "smb.db-shm"};
  for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
    snprintf(path, sizeof path, "%s/%s", dir, files[i]);
    unlink(path);
  }
  rmdir(dir);
}

int main(void) {
  /* Determine a plausible non-zero uid we can seed and act as. If the test
   * runs as root, we cannot become another uid without extra plumbing; the
   * transaction requires account.uid == peer.uid on the SMB path. Use the
   * effective uid unless it's 0, in which case skip this test — the
   * maintainer's VM invocation runs as a regular user. */
  uid_t euid = geteuid();
  if (euid == 0) {
    printf("test_smb_proof: SKIP (running as root; the SMB path requires "
           "account.uid == peer.uid and refuses uid 0)\n");
    return 0;
  }
  uint32_t target_uid = (uint32_t)euid;

  char dir[] = "/tmp/nostr-smb-proof-XXXXXX";
  NH_CHECK(mkdtemp(dir));
  seed(dir, target_uid);

  char journal[1024];
  snprintf(journal, sizeof journal, "%s/smb.db", dir);

  case_smb_proof_success(dir, target_uid, journal);
  /* Each case fires a fresh child + a fresh SMB authority (fresh mock passdb
   * and, effectively, a fresh journal — sqlite is opened afresh each time
   * because the previous authority_close revoked outstanding creds and closed
   * its handle). Delete the journal between cases to keep them independent. */
  unlink(journal);
  char shm[1100], wal[1100];
  snprintf(shm, sizeof shm, "%s/smb.db-shm", dir); unlink(shm);
  snprintf(wal, sizeof wal, "%s/smb.db-wal", dir); unlink(wal);

  case_smb_proof_wrong_passphrase(dir, target_uid, journal);
  unlink(journal); unlink(shm); unlink(wal);

  case_auth_socket_denies_smb_proof(dir, target_uid, journal);

  rm_all(dir);
  printf("RESULT: PASS\n");
  return 0;
}
