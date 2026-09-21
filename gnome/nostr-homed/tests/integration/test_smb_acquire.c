/* Linux-only: headless drive of the nostr-smb-acquire testable core.
 *
 * Reuses the mock-passdb + seeded local-vault setup from test_smb_proof.c
 * and forks a broker on the USER endpoint. Rather than exec'ing the
 * `nostr-smb-acquire` binary (which needs a tty for getpass), we call the
 * core function nh_smb_acquire_run() directly on the connected fd with a
 * tempfile sink.
 *
 * Asserts:
 *   - A correct proof lands a mode-0600 credentials file at the temp path
 *     containing the right username and a non-empty password line, the
 *     secure envelope reports cleared, and no plaintext leaks to the
 *     journal.
 *   - A wrong proof writes NO credentials file and returns a non-OK
 *     acquire status + INVALID_PROOF broker result.
 *
 * Tracks beads nostrc-rb0e.8.
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
#include "nostr_smb_acquire.h"
#include "smb_credential.h"

#include <errno.h>
#include <fcntl.h>
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

/* -------- Mock passdb (mirrors test_smb_proof.c) -------- */

typedef struct mock_passdb {
  int set_calls;
  int disable_calls;
  int remove_calls;
} mock_passdb;

static int mock_set(void *ctx, const char *u, const char *p) {
  (void)u; (void)p;
  ((mock_passdb *)ctx)->set_calls++;
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
static const nh_smb_passdb_ops mock_ops = { mock_set, mock_disable,
                                            mock_remove };

/* -------- Identity seed (mirrors test_smb_proof.c) -------- */

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
  config->uid_min = target_uid;
  config->uid_max = target_uid;
  config->domain_default_min = 1u;
  config->domain_default_max = 2u;
  config->domain_rid_min = 3u;
  config->domain_rid_max = 4u;
  config->standalone_smb_min = 5u;
  config->standalone_smb_max = 6u;
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
  static nh_identity_config config;
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
  nh_identity_store *store =
      open_store(dir, target_uid, NH_IDENTITY_STORE_CREATE);
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
  secret[31] = 1;
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
  NH_CHECK(nh_identity_store_publish_projection(store, NULL) ==
           NH_IDENTITY_OK);
  NH_CHECK(nh_identity_operation_activate(store, op, &state) ==
           NH_IDENTITY_OK);
  nh_identity_store_close(store);
}

/* -------- Fork/serve helpers (mirrors test_smb_proof.c) -------- */

static pid_t fork_server(const char *dir, uint32_t target_uid,
                         const char *smb_journal,
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
    (void)nh_auth_broker_handle_user_connection(broker, sv[1]);
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

/* Read the credentials file and return the password portion in *pw_out
 * (heap; caller frees).  Also asserts the username line matches expected
 * and returns the password length via *pw_len_out. */
static void slurp_and_parse(const char *path, const char *expect_user,
                            char **pw_out, size_t *pw_len_out) {
  struct stat sb;
  NH_CHECK(stat(path, &sb) == 0);
  NH_CHECK((sb.st_mode & 0777) == 0600);
  NH_CHECK(S_ISREG(sb.st_mode));
  FILE *f = fopen(path, "rb");
  NH_CHECK(f != NULL);
  char buf[1024];
  size_t n = fread(buf, 1, sizeof buf - 1, f);
  fclose(f);
  buf[n] = '\0';

  char user_line[128];
  snprintf(user_line, sizeof user_line, "username=%s\n", expect_user);
  NH_CHECK(strstr(buf, user_line) != NULL);

  const char *pw_key = "password=";
  const char *p = strstr(buf, pw_key);
  NH_CHECK(p != NULL);
  p += strlen(pw_key);
  const char *nl = strchr(p, '\n');
  NH_CHECK(nl != NULL);
  size_t plen = (size_t)(nl - p);
  NH_CHECK(plen > 0);
  char *pw = malloc(plen + 1);
  NH_CHECK(pw != NULL);
  memcpy(pw, p, plen);
  pw[plen] = '\0';
  *pw_out = pw;
  *pw_len_out = plen;
}

static void case_success(const char *dir, uint32_t target_uid,
                         const char *journal, const char *creds_path) {
  int cfd = -1, sfd = -1;
  pid_t pid = fork_server(dir, target_uid, journal, &cfd, &sfd);

  /* Ensure no pre-existing file. */
  (void)unlink(creds_path);

  nh_smb_acquire_sink sink = {
    .creds_path = creds_path,
    .stdout_stream = NULL,
  };
  nh_auth_result result = NH_AUTH_RESULT_INTERNAL_ERROR;
  nh_smb_acquire_diag diag;
  memset(&diag, 0, sizeof diag);

  nh_smb_acquire_status st = nh_smb_acquire_run(cfd,
      "smb-credential", NH_AUTH_PROVIDER_NAME_LOCAL, PASSPHRASE,
      &sink, &result, &diag, stderr);
  close(cfd);

  int stats[3] = {0};
  reap(pid, sfd, stats);

  printf("acquire (correct passphrase) -> status=%d result=%s\n",
         (int)st, nh_auth_result_name(result));
  NH_CHECK(st == NH_SMB_ACQUIRE_OK);
  NH_CHECK(result == NH_AUTH_RESULT_OK);
  NH_CHECK(diag.envelope_cleared);
  NH_CHECK(diag.creds_file_written);
  NH_CHECK(strcmp(diag.account_username, "n_alice") == 0);
  NH_CHECK(stats[0] == 1); /* passdb set_password fired */

  char *pw = NULL; size_t pw_len = 0;
  slurp_and_parse(creds_path, "n_alice", &pw, &pw_len);
  NH_CHECK(pw_len >= 20u && pw_len <= 64u);

  /* Journal must not contain the plaintext password. */
  FILE *jf = fopen(journal, "rb");
  NH_CHECK(jf != NULL);
  fseek(jf, 0, SEEK_END);
  long jsz = ftell(jf);
  fseek(jf, 0, SEEK_SET);
  char *jbuf = malloc((size_t)jsz);
  NH_CHECK(jbuf != NULL);
  NH_CHECK(fread(jbuf, 1, (size_t)jsz, jf) == (size_t)jsz);
  fclose(jf);
  int found = 0;
  if ((size_t)jsz >= pw_len) {
    for (size_t i = 0; i + pw_len <= (size_t)jsz; i++)
      if (memcmp(jbuf + i, pw, pw_len) == 0) { found = 1; break; }
  }
  free(jbuf);
  NH_CHECK(found == 0);

  /* Wipe our local copy. */
  volatile char *w = (volatile char *)pw;
  for (size_t i = 0; i < pw_len; i++) w[i] = 0;
  free(pw);
  unlink(creds_path);
}

static void case_wrong_passphrase(const char *dir, uint32_t target_uid,
                                  const char *journal,
                                  const char *creds_path) {
  int cfd = -1, sfd = -1;
  pid_t pid = fork_server(dir, target_uid, journal, &cfd, &sfd);

  (void)unlink(creds_path);

  nh_smb_acquire_sink sink = {
    .creds_path = creds_path,
    .stdout_stream = NULL,
  };
  nh_auth_result result = NH_AUTH_RESULT_OK;
  nh_smb_acquire_diag diag;
  memset(&diag, 0, sizeof diag);

  nh_smb_acquire_status st = nh_smb_acquire_run(cfd,
      "smb-credential", NH_AUTH_PROVIDER_NAME_LOCAL,
      "obviously-wrong-passphrase", &sink, &result, &diag, stderr);
  close(cfd);

  int stats[3] = {0};
  reap(pid, sfd, stats);

  printf("acquire (wrong passphrase) -> status=%d result=%s\n",
         (int)st, nh_auth_result_name(result));
  NH_CHECK(st != NH_SMB_ACQUIRE_OK);
  NH_CHECK(st == NH_SMB_ACQUIRE_ERR_PROOF);
  NH_CHECK(result == NH_AUTH_RESULT_INVALID_PROOF);
  NH_CHECK(!diag.creds_file_written);
  NH_CHECK(diag.envelope_cleared);
  NH_CHECK(stats[0] == 0);

  /* File must NOT exist. */
  struct stat sb;
  int r = stat(creds_path, &sb);
  NH_CHECK(r != 0);
  NH_CHECK(errno == ENOENT);
}

static void rm_all(const char *dir) {
  char path[1024];
  const char *files[] = {"authority.db", "authority.db-wal",
                         "authority.db-shm", "authority.lock", "nss.db",
                         "smb.db", "smb.db-wal", "smb.db-shm",
                         "credentials.ok", "credentials.bad"};
  for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
    snprintf(path, sizeof path, "%s/%s", dir, files[i]);
    unlink(path);
  }
  rmdir(dir);
}

int main(void) {
  uid_t euid = geteuid();
  if (euid == 0) {
    printf("test_smb_acquire: SKIP (running as root)\n");
    return 0;
  }
  uint32_t target_uid = (uint32_t)euid;

  char dir[] = "/tmp/nostr-smb-acquire-XXXXXX";
  NH_CHECK(mkdtemp(dir));
  seed(dir, target_uid);

  char journal[1024];
  snprintf(journal, sizeof journal, "%s/smb.db", dir);
  char creds_ok[1200];
  snprintf(creds_ok, sizeof creds_ok, "%s/credentials.ok", dir);
  char creds_bad[1200];
  snprintf(creds_bad, sizeof creds_bad, "%s/credentials.bad", dir);

  case_success(dir, target_uid, journal, creds_ok);

  /* Clean the journal so the next case starts fresh (mirrors test_smb_proof). */
  unlink(journal);
  char shm[1100], wal[1100];
  snprintf(shm, sizeof shm, "%s/smb.db-shm", dir); unlink(shm);
  snprintf(wal, sizeof wal, "%s/smb.db-wal", dir); unlink(wal);

  case_wrong_passphrase(dir, target_uid, journal, creds_bad);

  rm_all(dir);
  printf("RESULT: PASS\n");
  return 0;
}
