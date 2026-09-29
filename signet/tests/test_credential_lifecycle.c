/* SPDX-License-Identifier: MIT
 * Credential lifecycle acceptance tests.
 */

#include "signet/audit_logger.h"
#include "signet/capability.h"
#include "signet/key_store.h"
#include "signet/mgmt_protocol.h"
#include "signet/policy_store.h"
#include "signet/replay_cache.h"
#include "signet/store.h"
#include "signet/store_audit.h"
#include "signet/store_leases.h"
#include "signet/store_secrets.h"

#include <nostr-keys.h>
#include <nostr/nip44/nip44.h>

#include "test_check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <glib.h>
#include <sodium.h>
#include <sqlite3.h>

#define MASTER_KEY "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

typedef struct {
  char *db_path;
  SignetAuditLogger *audit;
  SignetKeyStore *keys;
  SignetMgmtHandler *mgmt;
  SignetPolicyRegistry *cap_policy;
  SignetPolicyStore *identity_policy;
  char *policy_path;
  char bunker_sk[65];
  char bunker_pk[65];
  char provisioner_sk[65];
  char provisioner_pk[65];
} Fixture;

static char *temp_db_path(void) {
  char tmpl[] = "/tmp/signet-credential-lifecycle-XXXXXX.db";
  int fd = mkstemps(tmpl, 3);
  CHECK(fd >= 0);
  close(fd);
  unlink(tmpl);
  return g_strdup(tmpl);
}

static void keypair(char sk[65], char pk[65]) {
  char *generated = nostr_key_generate_private();
  CHECK(generated && strlen(generated) == 64);
  char *public_key = nostr_key_get_public(generated);
  CHECK(public_key && strlen(public_key) == 64);
  memcpy(sk, generated, 65);
  memcpy(pk, public_key, 65);
  sodium_memzero(generated, strlen(generated));
  free(generated);
  free(public_key);
}

static int hex32(const char *hex, uint8_t out[32]) {
  size_t written = 0;
  return sodium_hex2bin(out, 32, hex, 64, NULL, &written, NULL) == 0 &&
         written == 32 ? 0 : -1;
}

static char *encrypt_request(const char *sender_sk,
                             const char *bunker_pk,
                             const char *json) {
  uint8_t sk[32], pk[32];
  CHECK(hex32(sender_sk, sk) == 0);
  CHECK(hex32(bunker_pk, pk) == 0);
  char *encrypted = NULL;
  CHECK(nostr_nip44_encrypt_v2(sk, pk, (const uint8_t *)json,
                                strlen(json), &encrypted) == 0);
  sodium_memzero(sk, sizeof(sk));
  return encrypted;
}

static int handle(Fixture *f, SignetMgmtOp op, const char *json,
                  const char *event_id) {
  char *encrypted = encrypt_request(f->provisioner_sk, f->bunker_pk, json);
  int rc = signet_mgmt_handler_handle_request(
      f->mgmt, f->provisioner_pk, encrypted, op, event_id, 2000000000);
  free(encrypted);
  return rc;
}

static int handle_as(Fixture *f, const char *sender_sk, const char *sender_pk,
                     SignetMgmtOp op, const char *json, const char *event_id) {
  char *encrypted = encrypt_request(sender_sk, f->bunker_pk, json);
  int rc = signet_mgmt_handler_handle_request(
      f->mgmt, sender_pk, encrypted, op, event_id, 2000000000);
  free(encrypted);
  return rc;
}

static int audit_rows_containing(SignetStore *store, const char *needle) {
  sqlite3 *db = signet_store_get_db(store);
  CHECK(db);
  sqlite3_stmt *stmt = NULL;
  CHECK(sqlite3_prepare_v2(db,
      "SELECT COUNT(*) FROM audit_log WHERE "
      "instr(COALESCE(agent_id,''),?1)>0 OR "
      "instr(COALESCE(operation,''),?1)>0 OR "
      "instr(COALESCE(secret_id,''),?1)>0 OR "
      "instr(COALESCE(detail,''),?1)>0", -1, &stmt, NULL) == SQLITE_OK);
  sqlite3_bind_text(stmt, 1, needle, -1, SQLITE_TRANSIENT);
  CHECK(sqlite3_step(stmt) == SQLITE_ROW);
  int count = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return count;
}

static void setup(Fixture *f) {
  memset(f, 0, sizeof(*f));
  keypair(f->bunker_sk, f->bunker_pk);
  keypair(f->provisioner_sk, f->provisioner_pk);
  f->db_path = temp_db_path();

  SignetAuditLoggerConfig acfg = {
    .path = NULL, .to_stdout = false, .flush_each_write = false
  };
  f->audit = signet_audit_logger_new(&acfg);
  SignetKeyStoreConfig kcfg = {
    .db_path = f->db_path, .master_key = MASTER_KEY
  };
  f->keys = signet_key_store_new(f->audit, &kcfg);
  CHECK(f->keys);

  f->policy_path = g_strdup_printf("%s.policy", f->db_path);
  f->identity_policy = signet_policy_store_file_new(f->policy_path);
  CHECK(f->identity_policy);

  const char *const provisioners[] = { f->provisioner_pk };
  SignetMgmtHandlerConfig mcfg = {
    .provisioner_pubkeys = provisioners,
    .n_provisioner_pubkeys = 1,
    .bunker_secret_key_hex = f->bunker_sk,
    .bunker_pubkey_hex = f->bunker_pk,
  };
  f->mgmt = signet_mgmt_handler_new(f->keys, NULL, NULL,
                                     f->identity_policy, &mcfg);
  CHECK(f->mgmt);

  f->cap_policy = signet_policy_registry_new();
  CHECK(f->cap_policy);
  char **caps = NULL;
  SignetAgentPolicy delivery_policy = {
    .name = (char *)"delivery",
    .capabilities = caps,
    .n_capabilities = 0,
    .rate_limit_per_hour = 1000,
  };
  CHECK(signet_policy_registry_add(f->cap_policy, &delivery_policy) == 0);
  CHECK(signet_policy_registry_assign(f->cap_policy, "owner", "delivery") == 0);
  signet_mgmt_handler_set_policy_registry(f->mgmt, f->cap_policy);

  char *delivery_json = g_strdup_printf(
      "{\"default\":\"deny\",\"allow_clients\":[\"%s\"],"
      "\"allow_methods\":[\"credential.deliver\"]}",
      f->provisioner_pk);
  char *policy_error = NULL;
  CHECK(signet_policy_store_set_identity_json(
      f->identity_policy, "owner", delivery_json, 2000000000,
      &policy_error) == 0);
  CHECK(policy_error == NULL);
  g_free(delivery_json);

  char agent_sk[65], agent_pk[65], out_pk[65] = {0};
  uint8_t raw[32];
  keypair(agent_sk, agent_pk);
  CHECK(hex32(agent_sk, raw) == 0);
  CHECK(signet_key_store_adopt_agent(
      f->keys, "owner", raw, agent_pk, "pairing-secret",
      f->bunker_pk, NULL, 0, out_pk, NULL) == SIGNET_ADOPT_OK);
  sodium_memzero(raw, sizeof(raw));
  sodium_memzero(agent_sk, sizeof(agent_sk));
}

static void teardown(Fixture *f) {
  signet_mgmt_handler_free(f->mgmt);
  signet_policy_registry_free(f->cap_policy);
  signet_policy_store_free(f->identity_policy);
  signet_key_store_free(f->keys);
  signet_audit_logger_free(f->audit);
  unlink(f->db_path);
  unlink(f->policy_path);
  g_free(f->db_path);
  g_free(f->policy_path);
  sodium_memzero(f->bunker_sk, sizeof(f->bunker_sk));
  sodium_memzero(f->provisioner_sk, sizeof(f->provisioner_sk));
}

static char *request_json(const char *label, const char *payload) {
  char *b64 = g_base64_encode((const guchar *)payload, strlen(payload));
  char *json = g_strdup_printf(
      "{\"request_id\":\"r\",\"agent_id\":\"owner\","
      "\"secret_type\":\"api_token\",\"label\":\"%s\","
      "\"policy_id\":\"least-privilege\",\"expires_at\":2000000100,"
      "\"payload_b64\":\"%s\"}", label, b64);
  sodium_memzero(b64, strlen(b64));
  g_free(b64);
  return json;
}

static void test_encrypted_contextvm_lifecycle(void) {
  Fixture f;
  setup(&f);
  SignetStore *store = signet_key_store_get_store(f.keys);
  CHECK(store);

  char *create = request_json("primary", "first-token-value");
  CHECK(handle(&f, SIGNET_MGMT_OP_CREATE_CREDENTIAL,
                create, "create-1") == 0);
  char primary_id[70];
  CHECK(signet_secret_id_generate("owner", SIGNET_SECRET_API_TOKEN,
                                   "primary", primary_id) == 0);

  SignetSecretMetadata metadata;
  memset(&metadata, 0, sizeof(metadata));
  CHECK(signet_store_get_secret_metadata(
      store, primary_id, 2000000000, &metadata) == SIGNET_SECRET_OK);
  CHECK(strcmp(metadata.provenance, "created") == 0);
  CHECK(strcmp(metadata.created_by, f.provisioner_pk) == 0);
  CHECK(strcmp(metadata.policy_id, "least-privilege") == 0);
  CHECK(metadata.status == SIGNET_SECRET_STATUS_ACTIVE);
  signet_secret_metadata_clear(&metadata);

  /* Same deterministic slot must refuse overwrite and preserve the payload. */
  char *duplicate = request_json("primary", "must-not-overwrite");
  CHECK(handle(&f, SIGNET_MGMT_OP_CREATE_CREDENTIAL,
                duplicate, "create-2") == -1);
  SignetSecretRecord record;
  memset(&record, 0, sizeof(record));
  CHECK(signet_store_get_secret_at(
      store, primary_id, 2000000001, &record) == SIGNET_SECRET_OK);
  CHECK(record.payload_len == strlen("first-token-value"));
  CHECK(memcmp(record.payload, "first-token-value", record.payload_len) == 0);
  signet_secret_record_clear(&record);

  /* Import is a distinct provenance path, but uses the same protected payload. */
  char *imported = request_json("imported", "imported-token");
  CHECK(handle(&f, SIGNET_MGMT_OP_IMPORT_CREDENTIAL,
                imported, "import-1") == 0);
  char imported_id[70];
  CHECK(signet_secret_id_generate("owner", SIGNET_SECRET_API_TOKEN,
                                   "imported", imported_id) == 0);
  CHECK(signet_store_get_secret_metadata(
      store, imported_id, 2000000000, &metadata) == SIGNET_SECRET_OK);
  CHECK(strcmp(metadata.provenance, "imported") == 0);
  signet_secret_metadata_clear(&metadata);

  char *rotate_b64 =
      g_base64_encode((const guchar *)"rotated-token", strlen("rotated-token"));
  char *rotate = g_strdup_printf(
      "{\"request_id\":\"r\",\"credential_id\":\"%s\","
      "\"expires_at\":2000000200,\"payload_b64\":\"%s\"}",
      primary_id, rotate_b64);
  sodium_memzero(rotate_b64, strlen(rotate_b64));
  g_free(rotate_b64);
  CHECK(handle(&f, SIGNET_MGMT_OP_ROTATE_CREDENTIAL,
                rotate, "rotate-1") == 0);
  CHECK(signet_store_secret_history_count(store, primary_id) == 1);
  CHECK(signet_store_get_secret_metadata(
      store, primary_id, 2000000000, &metadata) == SIGNET_SECRET_OK);
  CHECK(metadata.version == 2 && metadata.active_version == 2);
  signet_secret_metadata_clear(&metadata);

  char *inspect = g_strdup_printf(
      "{\"request_id\":\"r\",\"credential_id\":\"%s\"}", primary_id);
  CHECK(handle(&f, SIGNET_MGMT_OP_INSPECT_CREDENTIAL,
                inspect, "inspect-1") == 0);
  CHECK(handle(&f, SIGNET_MGMT_OP_LIST_CREDENTIALS,
                "{\"request_id\":\"r\",\"agent_id\":\"owner\"}",
                "list-1") == 0);

  /* Delivery uses the unified policy/lease/audit path and never reads the
   * encrypted store directly from the CLI. */
  char *deliver = g_strdup_printf(
      "{\"request_id\":\"r\",\"agent_id\":\"owner\","
      "\"credential_id\":\"%s\"}", primary_id);
  int64_t audit_before_delivery = signet_audit_log_count(store);
  CHECK(handle(&f, SIGNET_MGMT_OP_DELIVER_CREDENTIAL,
               deliver, "deliver-1") == 0);
  /* One owner access entry plus one requester-attributed management entry. */
  CHECK(signet_audit_log_count(store) == audit_before_delivery + 2);
  SignetLeaseRecord *leases = NULL;
  size_t lease_count = 0;
  CHECK(signet_store_list_active_leases(
      store, "owner", 2000000000, &leases, &lease_count) == 0);
  /* The delivery lease is burned before decryption; no reusable active lease
   * survives the encrypted response. */
  CHECK(lease_count == 0);
  signet_lease_list_free(leases, lease_count);

  CHECK(handle(&f, SIGNET_MGMT_OP_REVOKE_CREDENTIAL,
                inspect, "revoke-1") == 0);
  CHECK(signet_store_get_secret_at(
      store, primary_id, 2000000001, &record) == SIGNET_SECRET_REVOKED);
  CHECK(handle(&f, SIGNET_MGMT_OP_INSPECT_CREDENTIAL,
                inspect, "inspect-revoked") == 0);
  CHECK(handle(&f, SIGNET_MGMT_OP_DELETE_CREDENTIAL,
                inspect, "delete-1") == 0);
  CHECK(signet_store_get_secret_metadata(
      store, primary_id, 2000000001, &metadata) == SIGNET_SECRET_NOT_FOUND);

  g_free(create);
  g_free(duplicate);
  g_free(imported);
  g_free(rotate);
  g_free(inspect);
  g_free(deliver);
  teardown(&f);
  puts("test_encrypted_contextvm_lifecycle: PASS");
}

static void test_expiry_and_unauthorized(void) {
  Fixture f;
  setup(&f);
  SignetStore *store = signet_key_store_get_store(f.keys);
  CHECK(store);

  SignetSecretMetadata metadata;
  memset(&metadata, 0, sizeof(metadata));
  CHECK(signet_store_create_secret(
      store, "owner", SIGNET_SECRET_CREDENTIAL, "expires",
      (const uint8_t *)"value", 5, NULL, 2000000010,
      "created", f.provisioner_pk, 2000000000,
      &metadata) == SIGNET_SECRET_OK);
  char *expired_id = g_strdup(metadata.id);
  signet_secret_metadata_clear(&metadata);

  SignetSecretRecord record;
  memset(&record, 0, sizeof(record));
  CHECK(signet_store_get_secret_at(
      store, expired_id, 2000000009, &record) == SIGNET_SECRET_OK);
  signet_secret_record_clear(&record);
  CHECK(signet_store_get_secret_at(
      store, expired_id, 2000000010, &record) == SIGNET_SECRET_EXPIRED);
  CHECK(signet_store_rotate_secret_ex(
      store, expired_id, (const uint8_t *)"new", 3,
      false, 0, 2000000011, NULL) == SIGNET_SECRET_EXPIRED);
  CHECK(signet_store_rotate_secret_ex(
      store, expired_id, (const uint8_t *)"new", 3,
      true, 2000000100, 2000000011, NULL) == SIGNET_SECRET_OK);
  CHECK(signet_store_secret_history_count(store, expired_id) == 1);

  char attacker_sk[65], attacker_pk[65];
  keypair(attacker_sk, attacker_pk);
  char *request = request_json("attacker", "stolen");
  char *encrypted = encrypt_request(attacker_sk, f.bunker_pk, request);
  CHECK(signet_mgmt_handler_handle_request(
      f.mgmt, attacker_pk, encrypted, SIGNET_MGMT_OP_CREATE_CREDENTIAL,
      "unauthorized-1", 2000000000) == -1);
  char attacker_id[70];
  CHECK(signet_secret_id_generate("owner", SIGNET_SECRET_API_TOKEN,
                                   "attacker", attacker_id) == 0);
  CHECK(signet_store_get_secret_metadata(
      store, attacker_id, 2000000000, &metadata) == SIGNET_SECRET_NOT_FOUND);

  free(encrypted);
  g_free(request);
  g_free(expired_id);
  sodium_memzero(attacker_sk, sizeof(attacker_sk));
  teardown(&f);
  puts("test_expiry_and_unauthorized: PASS");
}

static void test_delivery_denials_and_requester_audit(void) {
  Fixture f;
  setup(&f);
  SignetStore *store = signet_key_store_get_store(f.keys);
  CHECK(store);
  SignetSecretMetadata metadata;
  memset(&metadata, 0, sizeof(metadata));
  CHECK(signet_store_create_secret(
      store, "owner", SIGNET_SECRET_API_TOKEN, "denials",
      (const uint8_t *)"delivery-canary-secret", 22, NULL, 0,
      "created", f.provisioner_pk, 2000000000,
      &metadata) == SIGNET_SECRET_OK);
  char *cred_id = g_strdup(metadata.id);
  signet_secret_metadata_clear(&metadata);
  char *request = g_strdup_printf(
      "{\"request_id\":\"r\",\"agent_id\":\"owner\","
      "\"credential_id\":\"%s\"}", cred_id);

  /* Non-provisioners are rejected before parse/decrypt and attributed by
   * authenticated sender pubkey in the delivery audit. */
  char attacker_sk[65], attacker_pk[65];
  keypair(attacker_sk, attacker_pk);
  CHECK(handle_as(&f, attacker_sk, attacker_pk,
                  SIGNET_MGMT_OP_DELIVER_CREDENTIAL,
                  request, "deny-nonprovisioner") == -1);
  CHECK(audit_rows_containing(store, attacker_pk) > 0);
  CHECK(audit_rows_containing(store, "credential_deliver") > 0);

  /* Remove the explicit delivery grant. A provisioner still cannot deliver. */
  char *policy_error = NULL;
  CHECK(signet_policy_store_set_identity_json(
      f.identity_policy, "owner", "{\"default\":\"deny\"}",
      2000000000, &policy_error) == 0);
  CHECK(policy_error == NULL);
  CHECK(handle(&f, SIGNET_MGMT_OP_DELIVER_CREDENTIAL,
               request, "deny-capability") == -1);
  CHECK(audit_rows_containing(store, "no_capability") > 0);

  /* Restore the exact provisioner/method grant. */
  char *allow = g_strdup_printf(
      "{\"default\":\"deny\",\"allow_clients\":[\"%s\"],"
      "\"allow_methods\":[\"credential.deliver\"]}", f.provisioner_pk);
  CHECK(signet_policy_store_set_identity_json(
      f.identity_policy, "owner", allow, 2000000000,
      &policy_error) == 0);
  CHECK(policy_error == NULL);

  /* Wrong owner is denied before payload decrypt. */
  char other_sk[65], other_pk[65], out_pk[65] = {0};
  uint8_t raw[32];
  keypair(other_sk, other_pk);
  CHECK(hex32(other_sk, raw) == 0);
  CHECK(signet_key_store_adopt_agent(
      f.keys, "other", raw, other_pk, "other-secret", f.bunker_pk,
      NULL, 0, out_pk, NULL) == SIGNET_ADOPT_OK);
  sodium_memzero(raw, sizeof(raw));
  char *other_allow = g_strdup_printf(
      "{\"default\":\"deny\",\"allow_clients\":[\"%s\"],"
      "\"allow_methods\":[\"credential.deliver\"]}", f.provisioner_pk);
  CHECK(signet_policy_store_set_identity_json(
      f.identity_policy, "other", other_allow, 2000000000,
      &policy_error) == 0);
  char *wrong_owner = g_strdup_printf(
      "{\"request_id\":\"r\",\"agent_id\":\"other\","
      "\"credential_id\":\"%s\"}", cred_id);
  CHECK(handle(&f, SIGNET_MGMT_OP_DELIVER_CREDENTIAL,
               wrong_owner, "deny-owner") == -1);
  CHECK(audit_rows_containing(store, "not_owner") > 0);

  CHECK(signet_store_create_secret(
      store, "owner", SIGNET_SECRET_API_TOKEN, "revoked",
      (const uint8_t *)"revoked-value", 13, NULL, 0,
      "created", f.provisioner_pk, 2000000000,
      &metadata) == SIGNET_SECRET_OK);
  char *revoked_id = g_strdup(metadata.id);
  signet_secret_metadata_clear(&metadata);
  CHECK(signet_store_revoke_secret(
      store, revoked_id, 2000000000, &metadata) == SIGNET_SECRET_OK);
  signet_secret_metadata_clear(&metadata);
  char *revoked_req = g_strdup_printf(
      "{\"request_id\":\"r\",\"agent_id\":\"owner\","
      "\"credential_id\":\"%s\"}", revoked_id);
  CHECK(handle(&f, SIGNET_MGMT_OP_DELIVER_CREDENTIAL,
               revoked_req, "deny-revoked") == -1);
  CHECK(audit_rows_containing(store, "\"reason\":\"revoked\"") > 0);

  CHECK(signet_store_create_secret(
      store, "owner", SIGNET_SECRET_API_TOKEN, "expired",
      (const uint8_t *)"expired-value", 13, NULL, 1999999999,
      "created", f.provisioner_pk, 1999999900,
      &metadata) == SIGNET_SECRET_OK);
  char *expired_id = g_strdup(metadata.id);
  signet_secret_metadata_clear(&metadata);
  char *expired_req = g_strdup_printf(
      "{\"request_id\":\"r\",\"agent_id\":\"owner\","
      "\"credential_id\":\"%s\"}", expired_id);
  CHECK(handle(&f, SIGNET_MGMT_OP_DELIVER_CREDENTIAL,
               expired_req, "deny-expired") == -1);
  CHECK(audit_rows_containing(store, "\"reason\":\"expired\"") > 0);

  /* Corrupt ciphertext after metadata authorization: delivery fails after the
   * one-use lease has already burned and leaves no reusable active lease. */
  sqlite3 *db = signet_store_get_db(store);
  sqlite3_stmt *stmt = NULL;
  CHECK(sqlite3_prepare_v2(db,
      "UPDATE secrets SET payload=x'00' WHERE id=?1", -1,
      &stmt, NULL) == SQLITE_OK);
  sqlite3_bind_text(stmt, 1, cred_id, -1, SQLITE_TRANSIENT);
  CHECK(sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);
  CHECK(handle(&f, SIGNET_MGMT_OP_DELIVER_CREDENTIAL,
               request, "deny-decrypt") == -1);
  SignetLeaseRecord *leases = NULL;
  size_t lease_count = 0;
  CHECK(signet_store_list_active_leases(
      store, "owner", 2000000000, &leases, &lease_count) == 0);
  CHECK(lease_count == 0);
  signet_lease_list_free(leases, lease_count);

  CHECK(audit_rows_containing(store, "delivery-canary-secret") == 0);
  CHECK(audit_rows_containing(store, "\"requester\"") > 0);
  CHECK(audit_rows_containing(store, f.provisioner_pk) > 0);
  int64_t broken_id = 0;
  CHECK(signet_audit_verify_chain(store, 0, 0, &broken_id) == 0);

  sodium_memzero(attacker_sk, sizeof(attacker_sk));
  sodium_memzero(other_sk, sizeof(other_sk));
  g_free(allow); g_free(other_allow); g_free(wrong_owner);
  g_free(revoked_id); g_free(revoked_req);
  g_free(expired_id); g_free(expired_req);
  g_free(request); g_free(cred_id);
  teardown(&f);
  puts("test_delivery_denials_and_requester_audit: PASS");
}

/* Item 4: a PLAINTEXT (non-NIP-44) mutation request must be rejected before
 * parsing — even from an authorized provisioner — and must not mutate state. */
static void test_plaintext_mutation_rejected(void) {
  Fixture f;
  setup(&f);
  SignetStore *store = signet_key_store_get_store(f.keys);
  CHECK(store);

  /* Valid request JSON, but sent as raw plaintext instead of NIP-44. */
  char *create = request_json("plainette", "sneaky-token");
  CHECK(signet_mgmt_handler_handle_request(
      f.mgmt, f.provisioner_pk, create, SIGNET_MGMT_OP_CREATE_CREDENTIAL,
      "plaintext-1", 2000000000) == -1);

  char plain_id[70];
  CHECK(signet_secret_id_generate("owner", SIGNET_SECRET_API_TOKEN,
                                   "plainette", plain_id) == 0);
  SignetSecretMetadata metadata;
  memset(&metadata, 0, sizeof(metadata));
  CHECK(signet_store_get_secret_metadata(
      store, plain_id, 2000000000, &metadata) == SIGNET_SECRET_NOT_FOUND);

  g_free(create);
  teardown(&f);
  puts("test_plaintext_mutation_rejected: PASS");
}

/* Item 4: with a replay cache attached, a replayed credential mutation event
 * id executes at most once — the duplicate is rejected and state unchanged. */
static void test_credential_mutation_replay_rejected(void) {
  Fixture f;
  setup(&f);
  SignetStore *store = signet_key_store_get_store(f.keys);
  CHECK(store);

  SignetReplayCacheConfig rcfg = {
    .max_entries = 128, .ttl_seconds = 300, .skew_seconds = 300
  };
  SignetReplayCache *replay = signet_replay_cache_new(&rcfg);
  CHECK(replay);
  signet_mgmt_handler_set_replay_cache(f.mgmt, replay);

  char *create = request_json("replayed", "original-value");
  CHECK(handle(&f, SIGNET_MGMT_OP_CREATE_CREDENTIAL,
                create, "replay-evt-1") == 0);
  char cred_id[70];
  CHECK(signet_secret_id_generate("owner", SIGNET_SECRET_API_TOKEN,
                                   "replayed", cred_id) == 0);

  /* Replay of the SAME event id carrying a rotate must be dropped. */
  char *b64 = g_base64_encode((const guchar *)"replayed-value",
                              strlen("replayed-value"));
  char *rotate = g_strdup_printf(
      "{\"request_id\":\"r\",\"credential_id\":\"%s\","
      "\"payload_b64\":\"%s\"}", cred_id, b64);
  g_free(b64);
  CHECK(handle(&f, SIGNET_MGMT_OP_ROTATE_CREDENTIAL,
                rotate, "replay-evt-1") == -1);

  SignetSecretRecord record;
  memset(&record, 0, sizeof(record));
  CHECK(signet_store_get_secret_at(
      store, cred_id, 2000000001, &record) == SIGNET_SECRET_OK);
  CHECK(record.payload_len == strlen("original-value"));
  CHECK(memcmp(record.payload, "original-value", record.payload_len) == 0);
  CHECK(record.version == 1 && record.active_version == 1);
  signet_secret_record_clear(&record);
  CHECK(signet_store_secret_history_count(store, cred_id) == 0);

  /* Delivery also participates in the same replay domain. A fresh encrypted
   * intent delivers once; replaying its event id cannot release again. */
  char *deliver = g_strdup_printf(
      "{\"request_id\":\"r\",\"agent_id\":\"owner\","
      "\"credential_id\":\"%s\"}", cred_id);
  int64_t audit_before_delivery = signet_audit_log_count(store);
  CHECK(handle(&f, SIGNET_MGMT_OP_DELIVER_CREDENTIAL,
               deliver, "deliver-replay-1") == 0);
  CHECK(signet_audit_log_count(store) == audit_before_delivery + 2);
  CHECK(handle(&f, SIGNET_MGMT_OP_DELIVER_CREDENTIAL,
               deliver, "deliver-replay-1") == -1);
  CHECK(signet_audit_log_count(store) == audit_before_delivery + 2);

  /* A fresh event id still executes. */
  char *b64b = g_base64_encode((const guchar *)"rotated-value",
                               strlen("rotated-value"));
  char *rotate2 = g_strdup_printf(
      "{\"request_id\":\"r\",\"credential_id\":\"%s\","
      "\"payload_b64\":\"%s\"}", cred_id, b64b);
  g_free(b64b);
  CHECK(handle(&f, SIGNET_MGMT_OP_ROTATE_CREDENTIAL,
                rotate2, "replay-evt-2") == 0);
  CHECK(signet_store_secret_history_count(store, cred_id) == 1);

  g_free(create);
  g_free(deliver);
  g_free(rotate);
  g_free(rotate2);
  signet_mgmt_handler_set_replay_cache(f.mgmt, NULL);
  teardown(&f);
  signet_replay_cache_free(replay);
  puts("test_credential_mutation_replay_rejected: PASS");
}

int main(void) {
  CHECK(sodium_init() >= 0);
  test_encrypted_contextvm_lifecycle();
  test_expiry_and_unauthorized();
  test_delivery_denials_and_requester_audit();
  test_plaintext_mutation_rejected();
  test_credential_mutation_replay_rejected();
  puts("credential lifecycle tests: ALL PASS");
  return 0;
}
