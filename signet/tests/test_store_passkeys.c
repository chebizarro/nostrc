/* SPDX-License-Identifier: MIT */
/*
 * Phase 1 passkey vault test: N passkeys for one agent, PSK-wrapped payload
 * round-trip, credential-id lookup, agent/RP lookup, and excludeCredentials.
 *
 * Standalone like tests/phase0: this file supplies the tiny SignetStore shim
 * that store_passkeys.c needs, so no full signetd/libnostr build is required.
 */

#include "signet/store_passkeys.h"
#include "store_passkeys_schema.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>
#include <sodium.h>

#define OK(cond, msg) do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); return 1; } } while (0)

struct SignetStore {
  sqlite3 *db;
};

sqlite3 *signet_store_get_db(SignetStore *store) {
  return store ? store->db : NULL;
}

static int exec_sql(sqlite3 *db, const char *sql) {
  char *err = NULL;
  int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
  if (rc != SQLITE_OK) {
    fprintf(stderr, "sql error: %s\n", err ? err : "?");
    sqlite3_free(err);
    return -1;
  }
  return 0;
}

static bool blob_contains(const uint8_t *haystack,
                          size_t haystack_len,
                          const uint8_t *needle,
                          size_t needle_len) {
  if (!haystack || !needle || needle_len == 0 || haystack_len < needle_len) return false;
  for (size_t i = 0; i + needle_len <= haystack_len; i++) {
    if (memcmp(haystack + i, needle, needle_len) == 0) return true;
  }
  return false;
}

static int count_agent(sqlite3 *db, const char *agent_id, int *out_count) {
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(db,
                         "SELECT COUNT(*) FROM passkey_credentials WHERE agent_id = ?;",
                         -1, &stmt, NULL) != SQLITE_OK) {
    return -1;
  }
  sqlite3_bind_text(stmt, 1, agent_id, -1, SQLITE_STATIC);
  int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) *out_count = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return rc == SQLITE_ROW ? 0 : -1;
}

static int assert_payload_does_not_contain(sqlite3 *db,
                                           const uint8_t *credential_id,
                                           size_t credential_id_len,
                                           const uint8_t *private_key,
                                           size_t private_key_len) {
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(db,
                         "SELECT payload, nonce, sign_count FROM passkey_credentials WHERE credential_id = ?;",
                         -1, &stmt, NULL) != SQLITE_OK) {
    return -1;
  }
  sqlite3_bind_blob(stmt, 1, credential_id, (int)credential_id_len, SQLITE_STATIC);
  int rc = sqlite3_step(stmt);
  if (rc != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    return -1;
  }
  const uint8_t *payload = (const uint8_t *)sqlite3_column_blob(stmt, 0);
  int payload_len = sqlite3_column_bytes(stmt, 0);
  const uint8_t *nonce = (const uint8_t *)sqlite3_column_blob(stmt, 1);
  int nonce_len = sqlite3_column_bytes(stmt, 1);
  int sign_count = sqlite3_column_int(stmt, 2);
  bool ok = payload && payload_len > 0 && nonce &&
            nonce_len == crypto_secretbox_NONCEBYTES && sign_count == 0 &&
            !blob_contains(payload, (size_t)payload_len, private_key, private_key_len);
  sqlite3_finalize(stmt);
  return ok ? 0 : -1;
}

static void fill_bytes(uint8_t *dst, size_t len, uint8_t seed) {
  for (size_t i = 0; i < len; i++) dst[i] = (uint8_t)(seed + i);
}

int main(void) {
  OK(sodium_init() >= 0, "sodium_init");

  sqlite3 *db = NULL;
  OK(sqlite3_open(":memory:", &db) == SQLITE_OK, "open sqlite");
  OK(exec_sql(db, SIGNET_PASSKEY_CREDENTIALS_SCHEMA_SQL) == 0, "passkey schema");

  SignetStore store = { db };
  uint8_t psk[SIGNET_PASSKEY_PSK_LEN];
  uint8_t wrong_psk[SIGNET_PASSKEY_PSK_LEN];
  randombytes_buf(psk, sizeof(psk));
  randombytes_buf(wrong_psk, sizeof(wrong_psk));

  const char *agent_id = "agent-alpha";
  const char *rp_ids[3] = { "github.com", "github.com", "example.com" };
  uint8_t aaguid[SIGNET_PASSKEY_AAGUID_LEN];
  fill_bytes(aaguid, sizeof(aaguid), 0xa0);

  uint8_t credential_ids[3][20];
  uint8_t user_handles[3][8];
  uint8_t private_keys[3][48];
  uint8_t cose_keys[3][77];

  for (int i = 0; i < 3; i++) {
    fill_bytes(credential_ids[i], sizeof(credential_ids[i]), (uint8_t)(0x10 + i * 0x10));
    fill_bytes(user_handles[i], sizeof(user_handles[i]), (uint8_t)(0x40 + i * 0x10));
    fill_bytes(private_keys[i], sizeof(private_keys[i]), (uint8_t)(0x70 + i * 0x10));
    fill_bytes(cose_keys[i], sizeof(cose_keys[i]), (uint8_t)(0xb0 + i * 0x10));

    SignetPasskeyCreate create = {
      .credential_id = credential_ids[i],
      .credential_id_len = sizeof(credential_ids[i]),
      .agent_id = agent_id,
      .rp_id = rp_ids[i],
      .user_handle = user_handles[i],
      .user_handle_len = sizeof(user_handles[i]),
      .aaguid = aaguid,
      .discoverable = true,
      .created_at = 1000 + i,
      .backend_id = "software-openssl",
      .cose_alg = SIGNET_PASSKEY_COSE_ALG_ES256,
      .key_blob = private_keys[i],
      .key_blob_len = sizeof(private_keys[i]),
      .cose_public_key = cose_keys[i],
      .cose_public_key_len = sizeof(cose_keys[i]),
      .user_name = i == 0 ? "octo" : "agent-user",
      .user_display_name = i == 0 ? "Octo Agent" : "Agent User",
    };

    OK(signet_store_passkey_create(&store, &create, psk, sizeof(psk)) == 0,
       "create passkey");
    OK(assert_payload_does_not_contain(db, credential_ids[i], sizeof(credential_ids[i]),
                                       private_keys[i], sizeof(private_keys[i])) == 0,
       "private key not stored in plaintext columns");

    if (i == 0) {
      OK(signet_store_passkey_create(&store, &create, psk, sizeof(psk)) == 1,
         "duplicate credential_id rejected");
    }
  }

  int count = 0;
  OK(count_agent(db, agent_id, &count) == 0, "count agent rows");
  OK(count == 3, "N passkeys for one agent");

  SignetPasskeyCredential one;
  OK(signet_store_passkey_find_by_credential_id(&store,
                                                credential_ids[0], sizeof(credential_ids[0]),
                                                psk, sizeof(psk), &one) == 0,
     "find by credential_id");
  OK(one.sign_count == 0, "sign_count fixed at zero");
  OK(one.payload_version == SIGNET_PASSKEY_PAYLOAD_VERSION, "payload version");
  OK(strcmp(one.backend_id, "software-openssl") == 0, "backend id round-trip");
  OK(one.cose_alg == SIGNET_PASSKEY_COSE_ALG_ES256, "alg round-trip");
  OK(strcmp(one.rp_id, "github.com") == 0, "rp_id round-trip");
  OK(one.key_blob_len == sizeof(private_keys[0]) &&
     memcmp(one.key_blob, private_keys[0], sizeof(private_keys[0])) == 0,
     "private key blob unwraps");
  OK(one.cose_public_key_len == sizeof(cose_keys[0]) &&
     memcmp(one.cose_public_key, cose_keys[0], sizeof(cose_keys[0])) == 0,
     "COSE public key round-trip");
  OK(strcmp(one.user_name, "octo") == 0 &&
     strcmp(one.user_display_name, "Octo Agent") == 0,
     "user metadata round-trip");
  signet_passkey_credential_clear(&one);

  OK(signet_store_passkey_find_by_credential_id(&store,
                                                credential_ids[0], sizeof(credential_ids[0]),
                                                wrong_psk, sizeof(wrong_psk), &one) == -1,
     "wrong PSK rejected");

  SignetPasskeyCredential *github_records = NULL;
  size_t github_count = 0;
  OK(signet_store_passkey_find_by_agent_rp(&store, agent_id, "github.com",
                                           psk, sizeof(psk),
                                           &github_records, &github_count) == 0,
     "find by agent+rp");
  OK(github_count == 2, "two github passkeys for one agent");
  signet_passkey_credential_list_free(github_records, github_count);

  const uint8_t *exclude_ids[1] = { credential_ids[1] };
  size_t exclude_lens[1] = { sizeof(credential_ids[1]) };
  bool has_excluded = false;
  OK(signet_store_passkey_has_excluded(&store, agent_id, "github.com",
                                       exclude_ids, exclude_lens, 1,
                                       &has_excluded) == 0,
     "excludeCredentials query");
  OK(has_excluded, "excludeCredentials detects duplicate for RP");

  has_excluded = true;
  OK(signet_store_passkey_has_excluded(&store, agent_id, "not-github.example",
                                       exclude_ids, exclude_lens, 1,
                                       &has_excluded) == 0,
     "excludeCredentials query for other RP");
  OK(!has_excluded, "excludeCredentials scoped to RP");

  sqlite3_close(db);
  printf("PASS test_store_passkeys (N-per-agent; PSK-wrapped payload round-trip; lookup paths; excludeCredentials; wrong-PSK rejected)\n");
  return 0;
}
