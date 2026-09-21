/* nh-seed-authority: dev/test helper — create a persistent authority DB with one
 * active account backed by a real local encrypted-key vault, for live broker/PAM
 * testing. Seals the vault to the provider_id the store assigns and swaps it into
 * providers.secret_blob directly (test-only; see the filed A enrollment bug).
 *
 * Usage: nh-seed-authority <dir> <username> <passphrase>
 * The account's key is secp256k1 private key 1 (well-known test key).
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include "auth_vault.h"
#include "nostr_identity.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *PUBKEY =
    "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798";

static nh_identity_ownership_result available(void *c, const char *n,
                                              uint32_t u, uint32_t g) {
  (void)c; (void)n; (void)u; (void)g;
  return NH_IDENTITY_OWNERSHIP_FREE;
}

#define CHECK(cond, msg) do { if (!(cond)) { fprintf(stderr, "seed: %s\n", msg); return 1; } } while (0)

int main(int argc, char **argv) {
  if (argc != 4) { fprintf(stderr, "usage: %s <dir> <username> <passphrase>\n", argv[0]); return 2; }
  const char *dir = argv[1], *username = argv[2], *passphrase = argv[3];
  if (strlen(passphrase) < NH_AUTH_VAULT_PASSPHRASE_MIN) {
    fprintf(stderr, "seed: passphrase must be >= %u chars\n", NH_AUTH_VAULT_PASSPHRASE_MIN);
    return 2;
  }
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
  nh_identity_rc oprc = nh_identity_store_open(&options, &store);
  if (oprc != NH_IDENTITY_OK) { fprintf(stderr, "seed: store open: %s (%s)\n", nh_identity_rc_name(oprc), store ? nh_identity_store_error_detail(store) : "no detail"); return 1; }

  nh_identity_enroll_request enroll = {0};
  enroll.username = username;
  enroll.pubkey_hex = PUBKEY;
  enroll.home_mode = NH_IDENTITY_HOME_CREATE;
  nh_identity_operation_state state;
  const char *op = "00000000-0000-4000-8000-000000000001";
  CHECK(nh_identity_operation_begin_enroll(store, op, &enroll, &state) == NH_IDENTITY_OK, "enroll");
  nh_identity_home_evidence staged = {11, 101}, installed = {11, 202};
  CHECK(nh_identity_operation_advance_home(store, op, NH_IDENTITY_PHASE_RESERVED,
          NH_IDENTITY_PHASE_STAGED, &staged, &state) == NH_IDENTITY_OK, "stage home");
  CHECK(nh_identity_operation_advance_home(store, op, NH_IDENTITY_PHASE_STAGED,
          NH_IDENTITY_PHASE_INSTALLED, &installed, &state) == NH_IDENTITY_OK, "install home");
  nh_identity_account account;
  CHECK(nh_identity_store_lookup_by_name(store, username, &account) == NH_IDENTITY_OK, "lookup");
  char provider_id[NH_IDENTITY_UUID_CAP];
  const uint8_t placeholder[] = {1, 2, 3, 4};
  CHECK(nh_identity_provider_stage(store, "00000000-0000-4000-8000-000000000002",
          account.account_id, NH_IDENTITY_PROVIDER_LOCAL_ENCRYPTED_KEY, 1, "{}",
          placeholder, sizeof placeholder, provider_id) == NH_IDENTITY_OK, "provider stage");
  nh_identity_proof_attestation attestation = {0};
  strcpy(attestation.pubkey_hex, PUBKEY);
  attestation.key_generation = account.key_generation;
  CHECK(nh_identity_provider_activate(store, "00000000-0000-4000-8000-000000000003",
          provider_id, &attestation) == NH_IDENTITY_OK, "provider activate");
  CHECK(nh_identity_store_publish_projection(store, NULL) == NH_IDENTITY_OK, "publish");
  CHECK(nh_identity_operation_activate(store, op, &state) == NH_IDENTITY_OK, "activate account");
  CHECK(nh_identity_store_lookup_by_name(store, username, &account) == NH_IDENTITY_OK, "relookup");

  uint8_t secret[32] = {0};
  secret[31] = 1;
  nh_auth_vault_binding binding = {provider_id, account.account_id, PUBKEY, account.key_generation};
  uint8_t *blob = NULL; size_t blob_len = 0;
  CHECK(nh_auth_vault_seal(secret, (const uint8_t *)passphrase, strlen(passphrase),
          &binding, &blob, &blob_len) == NH_AUTH_VAULT_OK, "vault seal");
  nh_identity_store_close(store);

  char db[1024];
  snprintf(db, sizeof db, "%s/authority.db", dir);
  sqlite3 *raw = NULL;
  CHECK(sqlite3_open(db, &raw) == SQLITE_OK, "sqlite open");
  sqlite3_stmt *st = NULL;
  CHECK(sqlite3_prepare_v2(raw, "UPDATE providers SET secret_blob=? WHERE provider_id=?",
          -1, &st, NULL) == SQLITE_OK, "prepare");
  sqlite3_bind_blob(st, 1, blob, (int)blob_len, SQLITE_STATIC);
  sqlite3_bind_text(st, 2, provider_id, -1, SQLITE_STATIC);
  CHECK(sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(raw) == 1, "swap secret_blob");
  sqlite3_finalize(st);
  sqlite3_close(raw);
  free(blob);
  printf("seeded active account %s (uid=%u) in %s\n", username, account.uid, dir);
  return 0;
}
