/* Linux-only: full local-provider login proof through the broker.
 * Seeds an active account with a real encrypted vault, then drives
 * BEGIN_LOGIN -> SELECT_PROVIDER -> SUBMIT_UNLOCK over a SEQPACKET connection.
 * As root the correct passphrase yields OK (+receipt) and a wrong one
 * INVALID_PROOF; as a non-root peer the ACL denies. Tracks nostrc-zcll.2.
 *
 * The vault is sealed with the provider_id the store assigned and swapped into
 * providers.secret_blob directly (test-only) because provider_stage assigns the
 * AAD-bound provider_id after taking the secret (see the filed A enrollment bug).
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *PUBKEY =
    "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798";
static const char *PASSPHRASE = "correct horse battery staple";

static nh_identity_ownership_result available(void *c, const char *n,
                                              uint32_t u, uint32_t g) {
  (void)c; (void)n; (void)u; (void)g;
  return NH_IDENTITY_OWNERSHIP_FREE;
}

static void set_config(nh_identity_config *config, const char *dir) {
  nh_identity_config_defaults(config);
  snprintf(config->authority_path, sizeof config->authority_path, "%s/authority.db", dir);
  snprintf(config->projection_path, sizeof config->projection_path, "%s/nss.db", dir);
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

/* Seed an active account + enabled local provider, seal a real vault bound to
 * the assigned provider_id, and swap it into the stored secret_blob. */
static void seed(const char *dir) {
  nh_identity_store *store = open_store(dir, NH_IDENTITY_STORE_CREATE);
  nh_identity_enroll_request enroll = {0};
  enroll.username = "n_alice";
  enroll.pubkey_hex = PUBKEY;
  enroll.home_mode = NH_IDENTITY_HOME_CREATE;
  nh_identity_operation_state state;
  const char *op = "00000000-0000-4000-8000-000000000001";
  NH_CHECK(nh_identity_operation_begin_enroll(store, op, &enroll, &state) == NH_IDENTITY_OK);
  nh_identity_home_evidence staged = {11, 101}, installed = {11, 202};
  NH_CHECK(nh_identity_operation_advance_home(store, op, NH_IDENTITY_PHASE_RESERVED,
             NH_IDENTITY_PHASE_STAGED, &staged, &state) == NH_IDENTITY_OK);
  NH_CHECK(nh_identity_operation_advance_home(store, op, NH_IDENTITY_PHASE_STAGED,
             NH_IDENTITY_PHASE_INSTALLED, &installed, &state) == NH_IDENTITY_OK);
  nh_identity_account account;
  NH_CHECK(nh_identity_store_lookup_by_name(store, "n_alice", &account) == NH_IDENTITY_OK);
  char provider_id[NH_IDENTITY_UUID_CAP];
  const uint8_t placeholder[] = {1, 2, 3, 4};
  NH_CHECK(nh_identity_provider_stage(store, "00000000-0000-4000-8000-000000000002",
             account.account_id, NH_IDENTITY_PROVIDER_LOCAL_ENCRYPTED_KEY, 1, "{}",
             placeholder, sizeof placeholder, provider_id) == NH_IDENTITY_OK);
  nh_identity_proof_attestation attestation = {0};
  strcpy(attestation.pubkey_hex, PUBKEY);
  attestation.key_generation = account.key_generation;
  uint8_t secret[32] = {0};
  secret[31] = 1; /* secp256k1 private key 1 -> PUBKEY */
  nh_auth_vault_binding binding = {provider_id, account.account_id, PUBKEY,
                                   account.key_generation};
  uint8_t *blob = NULL; size_t blob_len = 0;
  NH_CHECK(nh_auth_vault_seal(secret, (const uint8_t *)PASSPHRASE,
             strlen(PASSPHRASE), &binding, &blob, &blob_len) == NH_AUTH_VAULT_OK);
  NH_CHECK(nh_identity_provider_reseal(store, "00000000-0000-4000-8000-000000000004",
             provider_id, blob, blob_len) == NH_IDENTITY_OK);
  free(blob);
  NH_CHECK(nh_identity_provider_activate(store, "00000000-0000-4000-8000-000000000003",
             provider_id, &attestation) == NH_IDENTITY_OK);
  NH_CHECK(nh_identity_store_publish_projection(store, NULL) == NH_IDENTITY_OK);
  NH_CHECK(nh_identity_operation_activate(store, op, &state) == NH_IDENTITY_OK);
  nh_identity_store_close(store);
}

static nh_auth_result login(int server_conns, const char *dir,
                            const char *username, const char *passphrase) {
  int sv[2];
  NH_CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0);
  pid_t pid = fork();
  NH_CHECK(pid >= 0);
  if (pid == 0) {
    close(sv[0]);
    nh_identity_store *store = open_store(dir, 0);
    nh_auth_broker *broker = nh_auth_broker_new(store);
    NH_CHECK(broker);
    for (int i = 0; i < server_conns; i++)
      (void)nh_auth_broker_handle_connection(broker, sv[1]);
    nh_auth_broker_free(broker);
    nh_identity_store_close(store);
    close(sv[1]);
    _exit(0);
  }
  close(sv[1]);
  nh_auth_result result = NH_AUTH_RESULT_INTERNAL_ERROR;
  NH_CHECK(nh_auth_client_login(sv[0], username, "gdm-password", passphrase,
                                &result) == 0);
  close(sv[0]);
  int status = 0;
  NH_CHECK(waitpid(pid, &status, 0) == pid);
  return result;
}

int main(void) {
  char dir[] = "/tmp/nostr-login-XXXXXX";
  NH_CHECK(mkdtemp(dir));
  seed(dir);
  int is_root = (geteuid() == 0);

  nh_auth_result r = login(1, dir, "n_alice", PASSPHRASE);
  printf("correct passphrase -> %s\n", nh_auth_result_name(r));
  NH_CHECK(r == (is_root ? NH_AUTH_RESULT_OK : NH_AUTH_RESULT_DENIED));

  if (is_root) {
    r = login(1, dir, "n_alice", "wrong wrong wrong pass");
    printf("wrong passphrase -> %s\n", nh_auth_result_name(r));
    NH_CHECK(r == NH_AUTH_RESULT_INVALID_PROOF);
  }

  char path[1024];
  const char *files[] = {"authority.db", "authority.db-wal", "authority.db-shm",
                         "authority.lock", "nss.db"};
  for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
    snprintf(path, sizeof path, "%s/%s", dir, files[i]);
    unlink(path);
  }
  rmdir(dir);
  printf("RESULT: PASS (root=%d)\n", is_root);
  return 0;
}
