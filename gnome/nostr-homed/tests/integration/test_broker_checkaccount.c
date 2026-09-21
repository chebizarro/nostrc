/* Linux-only: exercises the real SOCK_SEQPACKET + SO_PEERCRED + ACL + authority
 * path for CHECK_ACCOUNT via nh_auth_broker_handle_connection and the client.
 * As root the ACL admits CHECK_ACCOUNT and real account status is returned; as a
 * non-root peer every AUTH-endpoint request is denied. Tracks nostrc-zcll.2. */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include "../nh_test.h"
#include "auth_broker.h"
#include "auth_client.h"
#include "nostr_auth_protocol.h"
#include "nostr_identity.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static nh_identity_ownership_result available(void *c, const char *n,
                                              uint32_t u, uint32_t g) {
  (void)c; (void)n; (void)u; (void)g;
  return NH_IDENTITY_OWNERSHIP_FREE;
}

static void seed_active_account(const char *dir, const char *username) {
  nh_identity_config config;
  nh_identity_config_defaults(&config);
  snprintf(config.authority_path, sizeof config.authority_path, "%s/authority.db", dir);
  snprintf(config.projection_path, sizeof config.projection_path, "%s/nss.db", dir);
  snprintf(config.home_root, sizeof config.home_root, "%s/home", dir);
  nh_identity_store_options options = {0};
  options.config = &config;
  options.ownership_probe = available;
  options.flags = NH_IDENTITY_STORE_CREATE;
  nh_identity_store *store = NULL;
  NH_CHECK(nh_identity_store_open(&options, &store) == NH_IDENTITY_OK);

  const char *pubkey = "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798";
  nh_identity_enroll_request enroll = {0};
  enroll.username = username;
  enroll.pubkey_hex = pubkey;
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
  NH_CHECK(nh_identity_store_lookup_by_name(store, username, &account) == NH_IDENTITY_OK);
  char provider[NH_IDENTITY_UUID_CAP];
  const uint8_t secret[] = {1, 2, 3, 4};
  NH_CHECK(nh_identity_provider_stage(store, "00000000-0000-4000-8000-000000000002",
             account.account_id, NH_IDENTITY_PROVIDER_LOCAL_ENCRYPTED_KEY, 1, "{}",
             secret, sizeof secret, provider) == NH_IDENTITY_OK);
  nh_identity_proof_attestation attestation = {0};
  strcpy(attestation.pubkey_hex, pubkey);
  attestation.key_generation = account.key_generation;
  NH_CHECK(nh_identity_provider_activate(store, "00000000-0000-4000-8000-000000000003",
             provider, &attestation) == NH_IDENTITY_OK);
  NH_CHECK(nh_identity_store_publish_projection(store, NULL) == NH_IDENTITY_OK);
  NH_CHECK(nh_identity_operation_activate(store, op, &state) == NH_IDENTITY_OK);
  nh_identity_store_close(store);
}

static nh_identity_store *open_readonly(const char *dir) {
  nh_identity_config config;
  nh_identity_config_defaults(&config);
  snprintf(config.authority_path, sizeof config.authority_path, "%s/authority.db", dir);
  snprintf(config.projection_path, sizeof config.projection_path, "%s/nss.db", dir);
  snprintf(config.home_root, sizeof config.home_root, "%s/home", dir);
  nh_identity_store_options options = {0};
  options.config = &config;
  options.ownership_probe = available;
  options.flags = 0;
  nh_identity_store *store = NULL;
  NH_CHECK(nh_identity_store_open(&options, &store) == NH_IDENTITY_OK);
  return store;
}

int main(void) {
  char dir[] = "/tmp/nostr-broker-XXXXXX";
  NH_CHECK(mkdtemp(dir));
  seed_active_account(dir, "n_alice");

  int sv[2];
  NH_CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0);
  const int checks = 2;
  pid_t pid = fork();
  NH_CHECK(pid >= 0);
  if (pid == 0) {
    close(sv[0]);
    nh_identity_store *store = open_readonly(dir);
    nh_auth_broker *broker = nh_auth_broker_new(store);
    NH_CHECK(broker);
    (void)checks;
    (void)nh_auth_broker_handle_connection(broker, sv[1]);
    nh_auth_broker_free(broker);
    nh_identity_store_close(store);
    close(sv[1]);
    _exit(0);
  }
  close(sv[1]);
  int is_root = (geteuid() == 0);

  nh_auth_result result;
  NH_CHECK(nh_auth_client_check_account(sv[0], "n_alice", &result) == 0);
  printf("n_alice -> %s\n", nh_auth_result_name(result));
  NH_CHECK(result == (is_root ? NH_AUTH_RESULT_OK : NH_AUTH_RESULT_DENIED));

  NH_CHECK(nh_auth_client_check_account(sv[0], "nobody_here", &result) == 0);
  printf("nobody_here -> %s\n", nh_auth_result_name(result));
  NH_CHECK(result == (is_root ? NH_AUTH_RESULT_UNKNOWN_ACCOUNT : NH_AUTH_RESULT_DENIED));

  close(sv[0]);
  int status = 0;
  NH_CHECK(waitpid(pid, &status, 0) == pid);

  /* best-effort cleanup */
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
