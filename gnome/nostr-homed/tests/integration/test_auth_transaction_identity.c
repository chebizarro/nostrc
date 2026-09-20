#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include "auth_transaction.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static nh_identity_ownership_result available(void *context, const char *name,
                                              uint32_t uid, uint32_t gid) {
  (void)context;
  (void)name;
  (void)uid;
  (void)gid;
  return NH_IDENTITY_OWNERSHIP_FREE;
}

int main(void) {
  char directory[] = "/tmp/nostr-auth-identity-XXXXXX";
  assert(mkdtemp(directory));
  nh_identity_config config;
  nh_identity_config_defaults(&config);
  snprintf(config.authority_path, sizeof config.authority_path,
           "%s/authority.db", directory);
  snprintf(config.projection_path, sizeof config.projection_path, "%s/nss.db",
           directory);
  snprintf(config.home_root, sizeof config.home_root, "%s/home", directory);
  nh_identity_store_options options = {0};
  options.config = &config;
  options.ownership_probe = available;
  options.flags = NH_IDENTITY_STORE_CREATE;
  nh_identity_store *store = NULL;
  assert(nh_identity_store_open(&options, &store) == NH_IDENTITY_OK);
  nh_auth_authority authority = nh_auth_authority_from_store(store);
  nh_auth_peer_snapshot peer = {0};
  peer.endpoint = NH_AUTH_ENDPOINT_AUTH;
  peer.pid = getpid();
  peer.process_start_id = 1;
  memset(peer.connection_id, 1, sizeof peer.connection_id);
  nh_auth_begin_request request = {NH_AUTH_PURPOSE_LINUX_LOGIN, "n_alice",
                                   "gdm-password", 1000};
  nh_auth_transaction *tx = NULL;
  assert(nh_auth_transaction_begin(&authority, &peer, &request, &tx) ==
         NH_AUTH_TX_UNKNOWN_ACCOUNT);
  const char *op = "00000000-0000-4000-8000-000000000001";
  const char *pubkey =
      "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798";
  nh_identity_enroll_request enroll = {0};
  enroll.username = request.username;
  enroll.pubkey_hex = pubkey;
  enroll.home_mode = NH_IDENTITY_HOME_CREATE;
  nh_identity_operation_state state;
  assert(nh_identity_operation_begin_enroll(store, op, &enroll, &state) ==
         NH_IDENTITY_OK);
  assert(nh_auth_transaction_begin(&authority, &peer, &request, &tx) ==
         NH_AUTH_TX_NOT_ACTIVE);
  nh_identity_home_evidence staged = {11, 101}, installed = {11, 202};
  assert(nh_identity_operation_advance_home(
             store, op, NH_IDENTITY_PHASE_RESERVED, NH_IDENTITY_PHASE_STAGED,
             &staged, &state) == NH_IDENTITY_OK);
  assert(nh_identity_operation_advance_home(
             store, op, NH_IDENTITY_PHASE_STAGED, NH_IDENTITY_PHASE_INSTALLED,
             &installed, &state) == NH_IDENTITY_OK);
  nh_identity_account account;
  assert(nh_identity_store_lookup_by_name(store, request.username, &account) ==
         NH_IDENTITY_OK);
  char provider[NH_IDENTITY_UUID_CAP];
  const uint8_t secret[] = {1, 2, 3,
                            4}; /* Store fixture, not a usable vault. */
  assert(nh_identity_provider_stage(
             store, "00000000-0000-4000-8000-000000000002", account.account_id,
             NH_IDENTITY_PROVIDER_LOCAL_ENCRYPTED_KEY, 1, "{}", secret,
             sizeof secret, provider) == NH_IDENTITY_OK);
  nh_identity_proof_attestation attestation = {0};
  strcpy(attestation.pubkey_hex, pubkey);
  attestation.key_generation = account.key_generation;
  assert(nh_identity_provider_activate(
             store, "00000000-0000-4000-8000-000000000003", provider,
             &attestation) == NH_IDENTITY_OK);
  assert(nh_identity_store_publish_projection(store, NULL) == NH_IDENTITY_OK);
  assert(nh_identity_operation_activate(store, op, &state) == NH_IDENTITY_OK);
  assert(nh_auth_transaction_begin(&authority, &peer, &request, &tx) ==
         NH_AUTH_TX_OK);
  assert(nh_auth_transaction_select_provider(
             tx, &peer, NH_IDENTITY_PROVIDER_LOCAL_ENCRYPTED_KEY, 1001) ==
         NH_AUTH_TX_OK);
  assert(nh_auth_transaction_begin_proof(tx, &peer, 1002, NULL) ==
         NH_AUTH_TX_OK);
  assert(nh_auth_transaction_start_verification(tx, &peer, 1003) ==
         NH_AUTH_TX_OK);
  nh_auth_receipt receipt;
  assert(nh_auth_transaction_finish_verification(
             tx, &peer, 1004, NH_AUTH_PROOF_OK, &receipt) == NH_AUTH_TX_OK);
  assert(nh_identity_account_set_status(
             store, "00000000-0000-4000-8000-000000000004", account.account_id,
             NH_IDENTITY_STATUS_DISABLED) == NH_IDENTITY_OK);
  assert(nh_auth_transaction_open_receipt(tx, &peer, 1005, &receipt) ==
         NH_AUTH_TX_NOT_ACTIVE);
  assert(nh_auth_transaction_get_state(tx) == NH_AUTH_TX_DENIED);
  nh_auth_transaction_free(tx);
  nh_identity_store_close(store);
  unlink(config.authority_path);
  unlink(config.projection_path);
  char sidecar[1024];
  const char *files[] = {"authority.db-wal", "authority.db-shm",
                         "authority.lock"};
  for (size_t i = 0; i < sizeof files / sizeof files[0]; ++i) {
    snprintf(sidecar, sizeof sidecar, "%s/%s", directory, files[i]);
    unlink(sidecar);
  }
  assert(rmdir(directory) == 0);
  return 0;
}
