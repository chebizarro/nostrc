/* SPDX-License-Identifier: MIT
 * Credential lifecycle acceptance tests.
 */

#include "signet/audit_logger.h"
#include "signet/capability.h"
#include "signet/key_store.h"
#include "signet/mgmt_protocol.h"
#include "signet/policy_store.h"
#include "signet/replay_cache.h"
#include "signet/revocation.h"
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
#include <json-glib/json-glib.h>
#include <sodium.h>
#include <sqlite3.h>

#ifdef SIGNET_ENABLE_TEST_HOOKS
#include "mgmt_protocol_test_hooks.h"
#endif

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

/* Every lease ever minted for a credential, burned or not. */
static int leases_for(SignetStore *store, const char *credential_id) {
  sqlite3 *db = signet_store_get_db(store);
  CHECK(db);
  sqlite3_stmt *stmt = NULL;
  CHECK(sqlite3_prepare_v2(db,
      "SELECT COUNT(*) FROM leases WHERE secret_id=?1", -1, &stmt,
      NULL) == SQLITE_OK);
  sqlite3_bind_text(stmt, 1, credential_id, -1, SQLITE_TRANSIENT);
  CHECK(sqlite3_step(stmt) == SQLITE_ROW);
  int count = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return count;
}

static void set_policy(Fixture *f, const char *identity, const char *json) {
  char *policy_error = NULL;
  CHECK(signet_policy_store_set_identity_json(
      f->identity_policy, identity, json, 2000000000, &policy_error) == 0);
  CHECK(policy_error == NULL);
}

static void grant_delivery(Fixture *f, const char *identity,
                           const char *requester_pk) {
  char *json = g_strdup_printf(
      "{\"default\":\"deny\",\"allow_clients\":[\"%s\"],"
      "\"allow_methods\":[\"credential.deliver\"]}", requester_pk);
  set_policy(f, identity, json);
  g_free(json);
}

static char *create_credential(Fixture *f, const char *owner,
                               const char *label, const char *value) {
  SignetStore *store = signet_key_store_get_store(f->keys);
  SignetSecretMetadata metadata;
  memset(&metadata, 0, sizeof(metadata));
  CHECK(signet_store_create_secret(
      store, owner, SIGNET_SECRET_API_TOKEN, label,
      (const uint8_t *)value, strlen(value), NULL, 0,
      "created", f->provisioner_pk, 2000000000,
      &metadata) == SIGNET_SECRET_OK);
  char *id = g_strdup(metadata.id);
  signet_secret_metadata_clear(&metadata);
  return id;
}

static void adopt(Fixture *f, const char *agent_id, char agent_pk[65]) {
  char agent_sk[65], out_pk[65] = {0};
  uint8_t raw[32];
  keypair(agent_sk, agent_pk);
  CHECK(hex32(agent_sk, raw) == 0);
  char *secret = g_strdup_printf("%s-pairing", agent_id);
  CHECK(signet_key_store_adopt_agent(
      f->keys, agent_id, raw, agent_pk, secret,
      f->bunker_pk, NULL, 0, out_pk, NULL) == SIGNET_ADOPT_OK);
  g_free(secret);
  sodium_memzero(raw, sizeof(raw));
  sodium_memzero(agent_sk, sizeof(agent_sk));
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

  /* S1b: ordinary NIP-46 policy matching is deliberately insufficient for
   * plaintext delivery. Missing dimensions, wildcards, and mixed policies
   * must all fail closed even if policy_store_get() would otherwise match. */
  const char *implicit_policies[] = {
    "{\"default\":\"deny\",\"allow_methods\":[\"*\"]}",
    NULL, /* exact client only, filled below */
    "{\"default\":\"deny\",\"allow_clients\":[\"*\"],"
      "\"allow_methods\":[\"credential.deliver\"]}",
    NULL, /* exact client plus wildcard/mixed NIP-46 methods */
    NULL, /* exact client plus mixed delivery/signing methods */
  };
  char *client_only = g_strdup_printf(
      "{\"default\":\"deny\",\"allow_clients\":[\"%s\"]}",
      f.provisioner_pk);
  char *wildcard_methods = g_strdup_printf(
      "{\"default\":\"deny\",\"allow_clients\":[\"%s\"],"
      "\"allow_methods\":[\"sign_event\",\"nip44_encrypt\",\"*\"]}",
      f.provisioner_pk);
  char *mixed_methods = g_strdup_printf(
      "{\"default\":\"deny\",\"allow_clients\":[\"%s\"],"
      "\"allow_methods\":[\"credential.deliver\",\"sign_event\"]}",
      f.provisioner_pk);
  implicit_policies[1] = client_only;
  implicit_policies[3] = wildcard_methods;
  implicit_policies[4] = mixed_methods;
  for (size_t i = 0; i < G_N_ELEMENTS(implicit_policies); i++) {
    CHECK(signet_policy_store_set_identity_json(
        f.identity_policy, "owner", implicit_policies[i],
        2000000000, &policy_error) == 0);
    CHECK(policy_error == NULL);
    char event_id[40];
    g_snprintf(event_id, sizeof(event_id), "deny-implicit-%zu", i);
    CHECK(handle(&f, SIGNET_MGMT_OP_DELIVER_CREDENTIAL,
                 request, event_id) == -1);
  }
  g_free(client_only);
  g_free(wildcard_methods);
  g_free(mixed_methods);

  /* Restore the exact provisioner/method grant. */
  char *allow = g_strdup_printf(
      "{\"default\":\"deny\",\"allow_clients\":[\"%s\"],"
      "\"allow_methods\":[\"credential.deliver\"]}", f.provisioner_pk);
  CHECK(signet_policy_store_set_identity_json(
      f.identity_policy, "owner", allow, 2000000000,
      &policy_error) == 0);
  CHECK(policy_error == NULL);

  /* A dedicated delivery grant cannot widen NIP-46 signing authority. */
  SignetPolicyKeyView sign_key = {
    .identity = "owner",
    .client_pubkey_hex = f.provisioner_pk,
    .method = "sign_event",
    .event_kind = 1,
  };
  SignetPolicyValue sign_decision;
  CHECK(signet_policy_store_get(
      f.identity_policy, &sign_key, 2000000000, &sign_decision) == 0);
  CHECK(sign_decision.decision == SIGNET_POLICY_RULE_DENY);

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

/* nostrc-v13e0: a requester that is NOT a provisioner, holding the owner's
 * exact credential.deliver grant, can deliver, inspect and list that owner's
 * credentials and nothing else; every other management op stays
 * provisioner-only; deny beats the grant. */
static void test_delivery_grant_requester_without_provisioner(void) {
  Fixture f;
  setup(&f);
  SignetStore *store = signet_key_store_get_store(f.keys);
  CHECK(store);
  char requester_sk[65], requester_pk[65];
  keypair(requester_sk, requester_pk);
  CHECK(!signet_store_is_provisioner(store, requester_pk));
  grant_delivery(&f, "owner", requester_pk);

  char other_pk[65];
  adopt(&f, "other", other_pk);
  grant_delivery(&f, "other", f.provisioner_pk);

  char *granted = create_credential(&f, "owner", "granted", "granted-value");
  char *foreign = create_credential(&f, "other", "foreign", "foreign-value");

#define AS_REQUESTER(op, json, evt) \
  handle_as(&f, requester_sk, requester_pk, (op), (json), (evt))

  char *deliver = g_strdup_printf(
      "{\"request_id\":\"r\",\"agent_id\":\"owner\","
      "\"credential_id\":\"%s\"}", granted);
  char *inspect = g_strdup_printf(
      "{\"request_id\":\"r\",\"credential_id\":\"%s\"}", granted);
  CHECK(AS_REQUESTER(SIGNET_MGMT_OP_DELIVER_CREDENTIAL, deliver, "g-1") == 0);
  CHECK(AS_REQUESTER(SIGNET_MGMT_OP_INSPECT_CREDENTIAL, inspect, "g-2") == 0);
  CHECK(AS_REQUESTER(SIGNET_MGMT_OP_LIST_CREDENTIALS,
                     "{\"request_id\":\"r\",\"agent_id\":\"owner\"}",
                     "g-3") == 0);
  CHECK(audit_rows_containing(store, "\"auth\":\"delivery_grant\"") > 0);
  CHECK(audit_rows_containing(store, "granted-value") == 0);

  /* Another owner's credential: no grant from "other", so nothing. */
  char *deliver_foreign = g_strdup_printf(
      "{\"request_id\":\"r\",\"agent_id\":\"other\","
      "\"credential_id\":\"%s\"}", foreign);
  char *inspect_foreign = g_strdup_printf(
      "{\"request_id\":\"r\",\"credential_id\":\"%s\"}", foreign);
  CHECK(AS_REQUESTER(SIGNET_MGMT_OP_DELIVER_CREDENTIAL,
                     deliver_foreign, "g-4") == -1);
  CHECK(AS_REQUESTER(SIGNET_MGMT_OP_INSPECT_CREDENTIAL,
                     inspect_foreign, "g-5") == -1);
  CHECK(AS_REQUESTER(SIGNET_MGMT_OP_LIST_CREDENTIALS,
                     "{\"request_id\":\"r\",\"agent_id\":\"other\"}",
                     "g-6") == -1);
  /* An unscoped list would enumerate every owner. */
  CHECK(AS_REQUESTER(SIGNET_MGMT_OP_LIST_CREDENTIALS,
                     "{\"request_id\":\"r\"}", "g-7") == -1);
  /* The owner's grant does not reach a credential the owner does not own. */
  char *deliver_cross = g_strdup_printf(
      "{\"request_id\":\"r\",\"agent_id\":\"owner\","
      "\"credential_id\":\"%s\"}", foreign);
  CHECK(AS_REQUESTER(SIGNET_MGMT_OP_DELIVER_CREDENTIAL,
                     deliver_cross, "g-8") == -1);
  /* The reply says no_capability; the audit keeps the real reason. */
  CHECK(audit_rows_containing(store, "not_owner") > 0);
  /* Grant refusals record the target credential and owner. */
  CHECK(audit_rows_containing(store, "\"owner\":\"other\"") > 0);
  sqlite3_stmt *st = NULL;
  CHECK(sqlite3_prepare_v2(signet_store_get_db(store),
      "SELECT COUNT(*) FROM audit_log WHERE operation='mgmt_unauthorized' "
      "AND secret_id=?1 AND instr(detail,'delivery_grant')>0", -1, &st,
      NULL) == SQLITE_OK);
  sqlite3_bind_text(st, 1, foreign, -1, SQLITE_TRANSIENT);
  CHECK(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) > 0);
  sqlite3_finalize(st);
  CHECK(AS_REQUESTER(SIGNET_MGMT_OP_INSPECT_CREDENTIAL,
      "{\"request_id\":\"r\",\"credential_id\":\"cred_missing\"}",
      "g-9") == -1);
  CHECK(leases_for(store, foreign) == 0);

  /* Every other management op stays provisioner-only. */
  char *rotate_b64 = g_base64_encode((const guchar *)"x", 1);
  char *rotate = g_strdup_printf(
      "{\"request_id\":\"r\",\"credential_id\":\"%s\","
      "\"payload_b64\":\"%s\"}", granted, rotate_b64);
  char *create = request_json("requester-made", "planted");
  char *grant_self = g_strdup_printf(
      "{\"request_id\":\"r\",\"provisioner_pubkey\":\"%s\"}", requester_pk);
  char *widen = g_strdup_printf(
      "{\"request_id\":\"r\",\"agent_id\":\"other\",\"policy\":"
      "{\"default\":\"deny\",\"allow_clients\":[\"%s\"],"
      "\"allow_methods\":[\"credential.deliver\"]}}", requester_pk);
  struct { SignetMgmtOp op; const char *json; } refused[] = {
    { SIGNET_MGMT_OP_CREATE_CREDENTIAL, create },
    { SIGNET_MGMT_OP_IMPORT_CREDENTIAL, create },
    { SIGNET_MGMT_OP_ROTATE_CREDENTIAL, rotate },
    { SIGNET_MGMT_OP_REVOKE_CREDENTIAL, inspect },
    { SIGNET_MGMT_OP_DELETE_CREDENTIAL, inspect },
    { SIGNET_MGMT_OP_GRANT_PROVISIONER, grant_self },
    { SIGNET_MGMT_OP_SET_POLICY, widen },
    { SIGNET_MGMT_OP_LIST_AGENTS, "{\"request_id\":\"r\"}" },
    { SIGNET_MGMT_OP_GET_STATUS, "{\"request_id\":\"r\"}" },
    { SIGNET_MGMT_OP_PROVISION_AGENT,
      "{\"request_id\":\"r\",\"agent_id\":\"planted\"}" },
    { SIGNET_MGMT_OP_REVOKE_AGENT,
      "{\"request_id\":\"r\",\"agent_id\":\"owner\"}" },
  };
  char owner_pk[65] = {0};
  CHECK(signet_key_store_get_agent_pubkey(f.keys, "owner", owner_pk,
                                          sizeof(owner_pk)));
  for (size_t i = 0; i < G_N_ELEMENTS(refused); i++) {
    char evt[32];
    g_snprintf(evt, sizeof(evt), "g-refused-%zu", i);
    /* Each is dropped before parsing, leaving one not_provisioner record. */
    int denials = audit_rows_containing(store, "not_provisioner");
    CHECK(AS_REQUESTER(refused[i].op, refused[i].json, evt) == -1);
    CHECK(audit_rows_containing(store, "not_provisioner") == denials + 1);
  }
  CHECK(!signet_store_is_provisioner(store, requester_pk));
  /* agent/revoke did not touch the owner: same identity, still usable. */
  char owner_pk_after[65] = {0};
  CHECK(signet_key_store_get_agent_pubkey(f.keys, "owner", owner_pk_after,
                                          sizeof(owner_pk_after)));
  CHECK(strcmp(owner_pk, owner_pk_after) == 0);
  CHECK(audit_rows_containing(store, "mgmt_revoke_agent") == 0);
  CHECK(audit_rows_containing(store, "mgmt_provision_agent") == 0);
  CHECK(audit_rows_containing(store, "mgmt_set_policy") == 0);
  SignetSecretMetadata metadata;
  memset(&metadata, 0, sizeof(metadata));
  CHECK(signet_store_get_secret_metadata(
      store, granted, 2000000000, &metadata) == SIGNET_SECRET_OK);
  CHECK(metadata.version == 1 &&
        metadata.status == SIGNET_SECRET_STATUS_ACTIVE);
  signet_secret_metadata_clear(&metadata);
  char planted_id[70];
  CHECK(signet_secret_id_generate("owner", SIGNET_SECRET_API_TOKEN,
                                   "requester-made", planted_id) == 0);
  CHECK(signet_store_get_secret_metadata(
      store, planted_id, 2000000000, &metadata) == SIGNET_SECRET_NOT_FOUND);
  CHECK(!signet_policy_store_has_exact_delivery_grant(
      f.identity_policy, "other", requester_pk, 2000000000));
  char agent_pk[65] = {0};
  CHECK(!signet_key_store_get_agent_pubkey(f.keys, "planted", agent_pk,
                                           sizeof(agent_pk)));

  /* Deny > grant: an explicit deny of the requester in the owner's policy,
   * then the live deny list, each refuse an otherwise exact grant. */
  char *deny_policy = g_strdup_printf(
      "{\"default\":\"deny\",\"allow_clients\":[\"%s\"],"
      "\"deny_clients\":[\"%s\"],"
      "\"allow_methods\":[\"credential.deliver\"]}",
      requester_pk, requester_pk);
  set_policy(&f, "owner", deny_policy);
  CHECK(AS_REQUESTER(SIGNET_MGMT_OP_DELIVER_CREDENTIAL, deliver, "g-d1") == -1);
  CHECK(AS_REQUESTER(SIGNET_MGMT_OP_INSPECT_CREDENTIAL, inspect, "g-d2") == -1);
  char *deny_method_policy = g_strdup_printf(
      "{\"default\":\"deny\",\"allow_clients\":[\"%s\"],"
      "\"allow_methods\":[\"credential.deliver\"],"
      "\"deny_methods\":[\"credential.deliver\"]}", requester_pk);
  set_policy(&f, "owner", deny_method_policy);
  g_free(deny_method_policy);
  CHECK(AS_REQUESTER(SIGNET_MGMT_OP_DELIVER_CREDENTIAL, deliver, "g-d3") == -1);
  CHECK(AS_REQUESTER(SIGNET_MGMT_OP_INSPECT_CREDENTIAL, inspect, "g-d3b") == -1);

  grant_delivery(&f, "owner", requester_pk);
  SignetDenyList *deny = signet_deny_list_new(store);
  CHECK(deny);
  signet_mgmt_handler_set_deny_list(f.mgmt, deny);
  CHECK(signet_deny_list_add(deny, requester_pk, NULL, "test",
                             2000000000) == 0);
  int leases_before = leases_for(store, granted);
  CHECK(AS_REQUESTER(SIGNET_MGMT_OP_DELIVER_CREDENTIAL, deliver, "g-d4") == -1);
  CHECK(AS_REQUESTER(SIGNET_MGMT_OP_INSPECT_CREDENTIAL, inspect, "g-d5") == -1);
  CHECK(AS_REQUESTER(SIGNET_MGMT_OP_LIST_CREDENTIALS,
                     "{\"request_id\":\"r\",\"agent_id\":\"owner\"}",
                     "g-d6") == -1);
  CHECK(leases_for(store, granted) == leases_before);
  CHECK(audit_rows_containing(store, "deny_listed") > 0);
  CHECK(signet_deny_list_remove(deny, requester_pk) == 0);
  /* A deny-listed owner is refused through the grant path as well. */
  CHECK(signet_deny_list_add(deny, owner_pk, "owner", "test",
                             2000000000) == 0);
  int owner_denials = audit_rows_containing(store, "deny_listed");
  CHECK(AS_REQUESTER(SIGNET_MGMT_OP_DELIVER_CREDENTIAL, deliver, "g-o1") == -1);
  CHECK(AS_REQUESTER(SIGNET_MGMT_OP_INSPECT_CREDENTIAL, inspect, "g-o2") == -1);
  CHECK(AS_REQUESTER(SIGNET_MGMT_OP_LIST_CREDENTIALS,
                     "{\"request_id\":\"r\",\"agent_id\":\"owner\"}",
                     "g-o3") == -1);
  CHECK(audit_rows_containing(store, "deny_listed") == owner_denials + 3);
  CHECK(leases_for(store, granted) == leases_before);
  CHECK(signet_deny_list_remove(deny, owner_pk) == 0);
  CHECK(AS_REQUESTER(SIGNET_MGMT_OP_DELIVER_CREDENTIAL, deliver, "g-d7") == 0);
  signet_mgmt_handler_set_deny_list(f.mgmt, NULL);
  signet_deny_list_free(deny);

  /* Withdrawing the grant withdraws the access. */
  set_policy(&f, "owner", "{\"default\":\"deny\"}");
  CHECK(AS_REQUESTER(SIGNET_MGMT_OP_DELIVER_CREDENTIAL, deliver, "g-w1") == -1);
  CHECK(AS_REQUESTER(SIGNET_MGMT_OP_INSPECT_CREDENTIAL, inspect, "g-w2") == -1);
#undef AS_REQUESTER

  /* The provisioner path is unchanged and audited as such. */
  grant_delivery(&f, "owner", f.provisioner_pk);
  CHECK(handle(&f, SIGNET_MGMT_OP_DELIVER_CREDENTIAL, deliver, "p-1") == 0);
  CHECK(audit_rows_containing(store, "\"auth\":\"provisioner\"") > 0);
  int64_t broken_id = 0;
  CHECK(signet_audit_verify_chain(store, 0, 0, &broken_id) == 0);

  sodium_memzero(requester_sk, sizeof(requester_sk));
  sodium_memzero(rotate_b64, strlen(rotate_b64));
  g_free(rotate_b64); g_free(rotate); g_free(create); g_free(grant_self);
  g_free(widen); g_free(deny_policy);
  g_free(deliver); g_free(inspect); g_free(deliver_foreign);
  g_free(inspect_foreign); g_free(deliver_cross);
  g_free(granted); g_free(foreign);
  teardown(&f);
  puts("test_delivery_grant_requester_without_provisioner: PASS");
}

/* nostrc-pvj7b: expected_version releases only that version; a mismatch
 * decrypts nothing, mints no lease, and is audited. */
static void test_deliver_expected_version(void) {
  Fixture f;
  setup(&f);
  SignetStore *store = signet_key_store_get_store(f.keys);
  CHECK(store);
  char *cred = create_credential(&f, "owner", "pinned", "pinned-v1");
  char *expect1 = g_strdup_printf(
      "{\"request_id\":\"r\",\"agent_id\":\"owner\",\"credential_id\":\"%s\","
      "\"expected_version\":1}", cred);
  char *expect2 = g_strdup_printf(
      "{\"request_id\":\"r\",\"agent_id\":\"owner\",\"credential_id\":\"%s\","
      "\"expected_version\":2}", cred);
  char *unpinned = g_strdup_printf(
      "{\"request_id\":\"r\",\"agent_id\":\"owner\","
      "\"credential_id\":\"%s\"}", cred);

  CHECK(handle(&f, SIGNET_MGMT_OP_DELIVER_CREDENTIAL, expect1, "v-1") == 0);
  CHECK(leases_for(store, cred) == 1);
  CHECK(handle(&f, SIGNET_MGMT_OP_DELIVER_CREDENTIAL, expect2, "v-2") == -1);
  CHECK(leases_for(store, cred) == 1);
  CHECK(audit_rows_containing(store, "version_mismatch") > 0);

  CHECK(signet_store_rotate_secret_ex(
      store, cred, (const uint8_t *)"pinned-v2", 9, false, 0,
      2000000001, NULL) == SIGNET_SECRET_OK);
  int64_t audit_before = signet_audit_log_count(store);
  CHECK(handle(&f, SIGNET_MGMT_OP_DELIVER_CREDENTIAL, expect1, "v-3") == -1);
  CHECK(leases_for(store, cred) == 1);
  /* One access entry plus one management entry, as for any denial. */
  CHECK(signet_audit_log_count(store) == audit_before + 2);
  CHECK(handle(&f, SIGNET_MGMT_OP_DELIVER_CREDENTIAL, expect2, "v-4") == 0);
  CHECK(handle(&f, SIGNET_MGMT_OP_DELIVER_CREDENTIAL, unpinned, "v-5") == 0);
  CHECK(leases_for(store, cred) == 3);

  /* Revocation outranks a version mismatch. */
  SignetSecretMetadata metadata;
  memset(&metadata, 0, sizeof(metadata));
  CHECK(signet_store_revoke_secret(store, cred, 2000000002, &metadata) ==
        SIGNET_SECRET_OK);
  signet_secret_metadata_clear(&metadata);
  CHECK(handle(&f, SIGNET_MGMT_OP_DELIVER_CREDENTIAL, expect1, "v-6") == -1);
  CHECK(audit_rows_containing(store, "\"reason\":\"revoked\"") > 0);

  /* Only a positive JSON integer is an expected_version. */
  char *live = create_credential(&f, "owner", "malformed", "malformed-v1");
  const char *bad[] = { "0", "-1", "\"1\"", "1.5", "null", "true",
                        "4294967297" };
  for (size_t i = 0; i < G_N_ELEMENTS(bad); i++) {
    char *json = g_strdup_printf(
        "{\"request_id\":\"r\",\"agent_id\":\"owner\","
        "\"credential_id\":\"%s\",\"expected_version\":%s}", live, bad[i]);
    char evt[32];
    g_snprintf(evt, sizeof(evt), "v-bad-%zu", i);
    CHECK(handle(&f, SIGNET_MGMT_OP_DELIVER_CREDENTIAL, json, evt) == -1);
    g_free(json);
  }
  CHECK(leases_for(store, live) == 0);

  /* expected_version is a deliver parameter only. */
  char *inspect_pinned = g_strdup_printf(
      "{\"request_id\":\"r\",\"credential_id\":\"%s\","
      "\"expected_version\":1}", live);
  CHECK(handle(&f, SIGNET_MGMT_OP_INSPECT_CREDENTIAL, inspect_pinned,
               "v-ins") == -1);
  CHECK(handle(&f, SIGNET_MGMT_OP_REVOKE_CREDENTIAL, inspect_pinned,
               "v-rev") == -1);
  CHECK(signet_store_get_secret_metadata(store, live, 2000000000,
                                         &metadata) == SIGNET_SECRET_OK);
  CHECK(metadata.status == SIGNET_SECRET_STATUS_ACTIVE);
  signet_secret_metadata_clear(&metadata);
  g_free(inspect_pinned);

  int64_t broken_id = 0;
  CHECK(signet_audit_verify_chain(store, 0, 0, &broken_id) == 0);
  CHECK(audit_rows_containing(store, "pinned-v") == 0);
  g_free(expect1); g_free(expect2); g_free(unpinned);
  g_free(cred); g_free(live);
  teardown(&f);
  puts("test_deliver_expected_version: PASS");
}

/* Review (nostrc-v13e0): a grant holder flooding cheap requests cannot evict
 * another sender's event ids and then replay them; a request outside the
 * clock window never executes. */
static void test_replay_flood_cannot_reexecute(void) {
  Fixture f;
  setup(&f);
  SignetStore *store = signet_key_store_get_store(f.keys);
  SignetReplayCacheConfig rcfg = {
    .max_entries = 64, .ttl_seconds = 600, .skew_seconds = 120,
    .max_entries_per_scope = 8, .refuse_when_full = true,
  };
  SignetReplayCache *replay = signet_replay_cache_new(&rcfg);
  SignetReplayCache *replay_self = signet_replay_cache_new(&rcfg);
  CHECK(replay && replay_self);
  signet_mgmt_handler_set_replay_cache(f.mgmt, replay);
  signet_mgmt_handler_set_self_replay_cache(f.mgmt, replay_self);
  signet_mgmt_handler_set_requester_rate_limit(f.mgmt, 1000, 1000);

  char flooder_sk[65], flooder_pk[65], victim_sk[65], victim_pk[65];
  keypair(flooder_sk, flooder_pk);
  keypair(victim_sk, victim_pk);
  char other_pk[65];
  adopt(&f, "other", other_pk);
  grant_delivery(&f, "owner", victim_pk);
  grant_delivery(&f, "other", flooder_pk);
  char *victim_cred = create_credential(&f, "owner", "victim", "victim-v1");
  char *flood_cred = create_credential(&f, "other", "flood", "flood-v1");

  char *victim_deliver = g_strdup_printf(
      "{\"request_id\":\"r\",\"agent_id\":\"owner\","
      "\"credential_id\":\"%s\"}", victim_cred);
  CHECK(handle_as(&f, victim_sk, victim_pk, SIGNET_MGMT_OP_DELIVER_CREDENTIAL,
                  victim_deliver, "victim-evt") == 0);
  CHECK(leases_for(store, victim_cred) == 1);
  char *rotate_b64 = g_base64_encode((const guchar *)"rotated", 7);
  char *rotate = g_strdup_printf(
      "{\"request_id\":\"r\",\"credential_id\":\"%s\","
      "\"payload_b64\":\"%s\"}", victim_cred, rotate_b64);
  CHECK(handle(&f, SIGNET_MGMT_OP_ROTATE_CREDENTIAL, rotate,
               "provisioner-evt") == 0);

  /* Flood far past the flooder's quota and the whole cache. */
  char *flood_inspect = g_strdup_printf(
      "{\"request_id\":\"r\",\"credential_id\":\"%s\"}", flood_cred);
  int executed = 0;
  for (int i = 0; i < 200; i++) {
    char evt[32];
    g_snprintf(evt, sizeof(evt), "flood-%d", i);
    if (handle_as(&f, flooder_sk, flooder_pk,
                  SIGNET_MGMT_OP_INSPECT_CREDENTIAL, flood_inspect, evt) == 0)
      executed++;
  }
  CHECK(executed == 8);

  /* Neither earlier request executes again. */
  CHECK(handle_as(&f, victim_sk, victim_pk, SIGNET_MGMT_OP_DELIVER_CREDENTIAL,
                  victim_deliver, "victim-evt") == -1);
  CHECK(leases_for(store, victim_cred) == 1);
  CHECK(handle(&f, SIGNET_MGMT_OP_ROTATE_CREDENTIAL, rotate,
               "provisioner-evt") == -1);
  CHECK(signet_store_secret_history_count(store, victim_cred) == 1);
  /* The victim still has its own quota. */
  CHECK(handle_as(&f, victim_sk, victim_pk, SIGNET_MGMT_OP_DELIVER_CREDENTIAL,
                  victim_deliver, "victim-evt-2") == 0);
  CHECK(leases_for(store, victim_cred) == 2);

  /* The request's own (authenticated) created_at bounds replay: outside the
   * window it is refused, inside it executes once. */
  char *rpc = g_strdup_printf(
      "{\"jsonrpc\":\"2.0\",\"id\":\"t\",\"method\":\"credential/deliver\","
      "\"params\":{\"agent_id\":\"owner\",\"credential_id\":\"%s\"}}",
      victim_cred);
  CHECK(signet_mgmt_handler_handle_intent(f.mgmt, victim_pk, rpc, "stale",
                                          2000000000 - 121, 2000000000) == -1);
  CHECK(signet_mgmt_handler_handle_intent(f.mgmt, victim_pk, rpc, "ahead",
                                          2000000000 + 121, 2000000000) == -1);
  CHECK(leases_for(store, victim_cred) == 2);
  CHECK(signet_mgmt_handler_handle_intent(f.mgmt, victim_pk, rpc, "fresh",
                                          2000000000 - 100, 2000000000) == 0);
  CHECK(signet_mgmt_handler_handle_intent(f.mgmt, victim_pk, rpc, "fresh",
                                          2000000000 - 100, 2000000060) == -1);
  CHECK(leases_for(store, victim_cred) == 3);

  sodium_memzero(flooder_sk, sizeof(flooder_sk));
  sodium_memzero(victim_sk, sizeof(victim_sk));
  g_free(rpc); g_free(flood_inspect); g_free(rotate); g_free(rotate_b64);
  g_free(victim_deliver); g_free(victim_cred); g_free(flood_cred);
  signet_mgmt_handler_set_replay_cache(f.mgmt, NULL);
  signet_mgmt_handler_set_self_replay_cache(f.mgmt, NULL);
  teardown(&f);
  signet_replay_cache_free(replay);
  signet_replay_cache_free(replay_self);
  puts("test_replay_flood_cannot_reexecute: PASS");
}

/* Review (nostrc-v13e0): a non-provisioner sender is rate-limited before
 * its request is decrypted or read; the refusal costs no store work. */
static void test_requester_rate_limit(void) {
  Fixture f;
  setup(&f);
  SignetStore *store = signet_key_store_get_store(f.keys);
  char requester_sk[65], requester_pk[65], stranger_sk[65], stranger_pk[65];
  keypair(requester_sk, requester_pk);
  keypair(stranger_sk, stranger_pk);
  grant_delivery(&f, "owner", requester_pk);
  char *cred = create_credential(&f, "owner", "limited", "limited-v1");
  char *inspect = g_strdup_printf(
      "{\"request_id\":\"r\",\"credential_id\":\"%s\"}", cred);
  signet_mgmt_handler_set_requester_rate_limit(f.mgmt, 5, 60);
  for (int i = 0; i < 5; i++) {
    char evt[32];
    g_snprintf(evt, sizeof(evt), "rl-%d", i);
    CHECK(handle_as(&f, requester_sk, requester_pk,
                    SIGNET_MGMT_OP_INSPECT_CREDENTIAL, inspect, evt) == 0);
  }
  /* Strangers (no grant) draw from their own buckets, and their refusals
   * are audited only while they are within rate. */
  for (int i = 0; i < 5; i++) {
    char evt[32];
    g_snprintf(evt, sizeof(evt), "st-%d", i);
    CHECK(handle_as(&f, stranger_sk, stranger_pk,
                    SIGNET_MGMT_OP_INSPECT_CREDENTIAL, inspect, evt) == -1);
  }
  int64_t audit_before = signet_audit_log_count(store);
  CHECK(handle_as(&f, requester_sk, requester_pk,
                  SIGNET_MGMT_OP_INSPECT_CREDENTIAL, inspect, "rl-over") == -1);
  CHECK(handle_as(&f, stranger_sk, stranger_pk,
                  SIGNET_MGMT_OP_DELIVER_CREDENTIAL, inspect, "st-over") == -1);
  CHECK(signet_audit_log_count(store) == audit_before);
  /* Provisioners are not limited. */
  for (int i = 0; i < 10; i++) {
    char evt[32];
    g_snprintf(evt, sizeof(evt), "pv-%d", i);
    CHECK(handle(&f, SIGNET_MGMT_OP_INSPECT_CREDENTIAL, inspect, evt) == 0);
  }
  sodium_memzero(requester_sk, sizeof(requester_sk));
  sodium_memzero(stranger_sk, sizeof(stranger_sk));
  g_free(inspect); g_free(cred);
  teardown(&f);
  puts("test_requester_rate_limit: PASS");
}

#ifdef SIGNET_ENABLE_TEST_HOOKS
typedef struct {
  char *recipient;
  char *reply;
  int count;
} ReplyCapture;

static void capture_reply(void *data, const char *recipient,
                          const char *reply_json) {
  ReplyCapture *c = data;
  g_free(c->recipient);
  if (c->reply) sodium_memzero(c->reply, strlen(c->reply));
  g_free(c->reply);
  c->recipient = g_strdup(recipient);
  c->reply = g_strdup(reply_json);
  c->count++;
}

/* Send a ContextVM JSON-RPC request through the intent path and return the
 * parsed reply (caller unrefs). */
static JsonNode *intent(Fixture *f, ReplyCapture *c, const char *sender_pk,
                        const char *rpc, const char *evt) {
  int before = c->count;
  (void)signet_mgmt_handler_handle_intent(f->mgmt, sender_pk, rpc, evt,
                                          2000000000, 2000000000);
  CHECK(c->count == before + 1);
  CHECK(g_strcmp0(c->recipient, sender_pk) == 0);
  JsonNode *n = json_from_string(c->reply, NULL);
  CHECK(n && JSON_NODE_HOLDS_OBJECT(n));
  return n;
}

static const char *error_data(JsonNode *reply) {
  JsonObject *o = json_node_get_object(reply);
  CHECK(json_object_has_member(o, "error"));
  return json_object_get_string_member(
      json_object_get_object_member(o, "error"), "data");
}

/* nostrc-edjyc + nostrc-pvj7b + nostrc-v13e0 on the wire: refusals echo the
 * request id, delivery reports its version, and a requester without a grant
 * cannot tell a missing credential from someone else's. */
static void test_contextvm_reply_shapes(void) {
  Fixture f;
  setup(&f);
  ReplyCapture cap = {0};
  signet_mgmt_handler_set_reply_hook(f.mgmt, capture_reply, &cap);
  char requester_sk[65], requester_pk[65];
  keypair(requester_sk, requester_pk);
  sodium_memzero(requester_sk, sizeof(requester_sk));
  grant_delivery(&f, "owner", requester_pk);
  char other_pk[65];
  adopt(&f, "other", other_pk);
  char *cred = create_credential(&f, "owner", "wire", "wire-v1");
  char *foreign = create_credential(&f, "other", "foreign", "foreign-v1");

  /* Refusal before the request path: string and integer ids are echoed. */
  JsonNode *r = intent(&f, &cap, requester_pk,
      "{\"jsonrpc\":\"2.0\",\"id\":\"req-7\",\"method\":\"agent/list\","
      "\"params\":{}}", "w-1");
  JsonObject *o = json_node_get_object(r);
  CHECK(g_strcmp0(json_object_get_string_member(o, "id"), "req-7") == 0);
  CHECK(json_object_get_int_member(
      json_object_get_object_member(o, "error"), "code") == -32002);
  CHECK(g_strcmp0(error_data(r), "not_provisioner") == 0);
  json_node_unref(r);
  r = intent(&f, &cap, requester_pk,
      "{\"jsonrpc\":\"2.0\",\"id\":42,\"method\":\"config/grant-provisioner\","
      "\"params\":{}}", "w-2");
  o = json_node_get_object(r);
  CHECK(json_object_get_int_member(o, "id") == 42);
  CHECK(g_strcmp0(error_data(r), "not_provisioner") == 0);
  json_node_unref(r);
  r = intent(&f, &cap, f.provisioner_pk,
      "{\"jsonrpc\":\"2.0\",\"id\":\"req-8\",\"method\":\"agent/nope\"}",
      "w-3");
  o = json_node_get_object(r);
  CHECK(g_strcmp0(json_object_get_string_member(o, "id"), "req-8") == 0);
  CHECK(g_strcmp0(error_data(r), "unknown_method") == 0);
  json_node_unref(r);
  /* An id that is neither a short string nor an integer is not reflected. */
  GString *long_id = g_string_new(NULL);
  for (int i = 0; i < 300; i++) g_string_append_c(long_id, 'x');
  char *long_rpc = g_strdup_printf(
      "{\"jsonrpc\":\"2.0\",\"id\":\"%s\",\"method\":\"agent/list\"}",
      long_id->str);
  r = intent(&f, &cap, requester_pk, long_rpc, "w-4");
  CHECK(json_node_get_node_type(json_object_get_member(
      json_node_get_object(r), "id")) == JSON_NODE_NULL);
  json_node_unref(r);
  g_free(long_rpc);
  g_string_free(long_id, TRUE);

  /* Delivery result carries the released version; the id is echoed. */
  char *rpc = g_strdup_printf(
      "{\"jsonrpc\":\"2.0\",\"id\":\"d-1\",\"method\":\"credential/deliver\","
      "\"params\":{\"agent_id\":\"owner\",\"credential_id\":\"%s\","
      "\"expected_version\":1}}", cred);
  r = intent(&f, &cap, requester_pk, rpc, "w-5");
  o = json_node_get_object(r);
  CHECK(g_strcmp0(json_object_get_string_member(o, "id"), "d-1") == 0);
  JsonObject *res = json_object_get_object_member(o, "result");
  CHECK(res);
  CHECK(json_object_get_int_member(res, "version") == 1);
  CHECK(g_strcmp0(json_object_get_string_member(res, "credential_id"), cred) == 0);
  CHECK(json_object_get_int_member(res, "lease_expires_at") == 2000000060);
  gsize len = 0;
  guchar *payload = g_base64_decode(
      json_object_get_string_member(res, "payload_b64"), &len);
  CHECK(len == 7 && memcmp(payload, "wire-v1", 7) == 0);
  sodium_memzero(payload, len);
  g_free(payload);
  json_node_unref(r);
  g_free(rpc);

  SignetStore *store = signet_key_store_get_store(f.keys);
  CHECK(signet_store_rotate_secret_ex(
      store, cred, (const uint8_t *)"wire-v2", 7, false, 0, 2000000001,
      NULL) == SIGNET_SECRET_OK);
  rpc = g_strdup_printf(
      "{\"jsonrpc\":\"2.0\",\"id\":\"d-2\",\"method\":\"credential/deliver\","
      "\"params\":{\"agent_id\":\"owner\",\"credential_id\":\"%s\","
      "\"expected_version\":1}}", cred);
  r = intent(&f, &cap, requester_pk, rpc, "w-6");
  o = json_node_get_object(r);
  CHECK(g_strcmp0(json_object_get_string_member(o, "id"), "d-2") == 0);
  CHECK(!json_object_has_member(o, "result"));
  CHECK(g_strcmp0(error_data(r), "version_mismatch") == 0);
  CHECK(strstr(cap.reply, "wire-v") == NULL);
  json_node_unref(r);
  g_free(rpc);
  rpc = g_strdup_printf(
      "{\"jsonrpc\":\"2.0\",\"id\":\"d-3\",\"method\":\"credential/deliver\","
      "\"params\":{\"agent_id\":\"owner\",\"credential_id\":\"%s\"}}", cred);
  r = intent(&f, &cap, requester_pk, rpc, "w-7");
  res = json_object_get_object_member(json_node_get_object(r), "result");
  CHECK(res && json_object_get_int_member(res, "version") == 2);
  json_node_unref(r);
  g_free(rpc);

  /* Inspect through the grant returns the same metadata a provisioner sees. */
  rpc = g_strdup_printf(
      "{\"jsonrpc\":\"2.0\",\"id\":\"i-1\",\"method\":\"credential/inspect\","
      "\"params\":{\"credential_id\":\"%s\"}}", cred);
  r = intent(&f, &cap, requester_pk, rpc, "w-8");
  char *granted_view = g_strdup(cap.reply);
  res = json_object_get_object_member(json_node_get_object(r), "result");
  CHECK(res && json_object_get_int_member(res, "version") == 2);
  CHECK(g_strcmp0(json_object_get_string_member(res, "agent_id"), "owner") == 0);
  json_node_unref(r);
  r = intent(&f, &cap, f.provisioner_pk, rpc, "w-9");
  CHECK(g_strcmp0(cap.reply, granted_view) == 0);
  json_node_unref(r);
  g_free(granted_view);
  g_free(rpc);

  /* No existence oracle: another owner's credential and a missing one are
   * refused identically. */
  char *rpc_foreign = g_strdup_printf(
      "{\"jsonrpc\":\"2.0\",\"id\":\"i-2\",\"method\":\"credential/inspect\","
      "\"params\":{\"credential_id\":\"%s\"}}", foreign);
  r = intent(&f, &cap, requester_pk, rpc_foreign, "w-10");
  CHECK(g_strcmp0(error_data(r), "no_capability") == 0);
  char *foreign_reply = g_strdup(cap.reply);
  json_node_unref(r);
  r = intent(&f, &cap, requester_pk,
      "{\"jsonrpc\":\"2.0\",\"id\":\"i-2\",\"method\":\"credential/inspect\","
      "\"params\":{\"credential_id\":\"cred_missing\"}}", "w-11");
  CHECK(g_strcmp0(cap.reply, foreign_reply) == 0);
  json_node_unref(r);
  g_free(foreign_reply);
  g_free(rpc_foreign);

  /* Same for deliver under the owner's own grant: another owner's
   * credential (not_owner in the audit) and a made-up id (not_found)
   * produce byte-identical replies. */
  char *rpc_cross = g_strdup_printf(
      "{\"jsonrpc\":\"2.0\",\"id\":\"x-1\",\"method\":\"credential/deliver\","
      "\"params\":{\"agent_id\":\"owner\",\"credential_id\":\"%s\"}}",
      foreign);
  r = intent(&f, &cap, requester_pk, rpc_cross, "w-x1");
  CHECK(g_strcmp0(error_data(r), "no_capability") == 0);
  char *cross_reply = g_strdup(cap.reply);
  json_node_unref(r);
  r = intent(&f, &cap, requester_pk,
      "{\"jsonrpc\":\"2.0\",\"id\":\"x-1\",\"method\":\"credential/deliver\","
      "\"params\":{\"agent_id\":\"owner\",\"credential_id\":"
      "\"cred_0000000000000000000000000000000000000000000000000000000000000000\"}}",
      "w-x2");
  CHECK(g_strcmp0(cap.reply, cross_reply) == 0);
  json_node_unref(r);
  g_free(cross_reply);
  g_free(rpc_cross);

  /* Every other management method is refused on the ContextVM path before
   * it is read, with the requester's id echoed. */
  const char *forbidden[] = {
    "credential/create", "credential/import", "credential/rotate",
    "credential/revoke", "credential/delete", "config/grant-provisioner",
    "config/revoke-provisioner", "agent/set-policy", "agent/list",
    "agent/get-status", "agent/provision", "agent/revoke",
    "agent/rotate-key", "agent/adopt-existing", "agent/restore-existing",
    "agent/list-clients", "agent/revoke-client", "agent/writer-acquire",
    "agent/writer-revoke",
  };
  for (size_t i = 0; i < G_N_ELEMENTS(forbidden); i++) {
    char *frpc = g_strdup_printf(
        "{\"jsonrpc\":\"2.0\",\"id\":\"f-%zu\",\"method\":\"%s\","
        "\"params\":{\"agent_id\":\"owner\",\"credential_id\":\"%s\"}}",
        i, forbidden[i], cred);
    char evt[32], want_id[32];
    g_snprintf(evt, sizeof(evt), "w-f%zu", i);
    g_snprintf(want_id, sizeof(want_id), "f-%zu", i);
    r = intent(&f, &cap, requester_pk, frpc, evt);
    o = json_node_get_object(r);
    CHECK(g_strcmp0(json_object_get_string_member(o, "id"), want_id) == 0);
    CHECK(json_object_get_int_member(
        json_object_get_object_member(o, "error"), "code") == -32002);
    CHECK(g_strcmp0(error_data(r), "not_provisioner") == 0);
    json_node_unref(r);
    g_free(frpc);
  }

  /* List through the grant is scoped to the granting owner. */
  r = intent(&f, &cap, requester_pk,
      "{\"jsonrpc\":\"2.0\",\"id\":\"l-1\",\"method\":\"credential/list\","
      "\"params\":{\"agent_id\":\"owner\"}}", "w-12");
  res = json_object_get_object_member(json_node_get_object(r), "result");
  CHECK(res && json_object_get_int_member(res, "count") == 1);
  CHECK(strstr(cap.reply, foreign) == NULL);
  json_node_unref(r);
  r = intent(&f, &cap, requester_pk,
      "{\"jsonrpc\":\"2.0\",\"id\":\"l-2\",\"method\":\"credential/list\","
      "\"params\":{\"agent_id\":\"other\"}}", "w-13");
  CHECK(g_strcmp0(json_object_get_string_member(json_node_get_object(r), "id"),
               "l-2") == 0);
  CHECK(g_strcmp0(error_data(r), "no_capability") == 0);
  json_node_unref(r);

  /* Integer ids are echoed on the request path too. */
  r = intent(&f, &cap, f.provisioner_pk,
      "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"credential/list\","
      "\"params\":{\"agent_id\":\"owner\"}}", "w-14");
  CHECK(json_object_get_int_member(json_node_get_object(r), "id") == 7);
  json_node_unref(r);

  /* Replay-window and rate-limit refusals are typed and echo the id. */
  SignetReplayCacheConfig rcfg = {
    .max_entries = 64, .ttl_seconds = 600, .skew_seconds = 120,
    .max_entries_per_scope = 8, .refuse_when_full = true,
  };
  SignetReplayCache *replay = signet_replay_cache_new(&rcfg);
  signet_mgmt_handler_set_replay_cache(f.mgmt, replay);
  signet_mgmt_handler_set_self_replay_cache(f.mgmt, replay);
  const char *skew_rpc =
      "{\"jsonrpc\":\"2.0\",\"id\":\"s-1\",\"method\":\"credential/list\","
      "\"params\":{\"agent_id\":\"owner\"}}";
  (void)signet_mgmt_handler_handle_intent(f.mgmt, requester_pk, skew_rpc,
                                          "w-s1", 2000000000 - 600,
                                          2000000000);
  r = json_from_string(cap.reply, NULL);
  CHECK(g_strcmp0(json_object_get_string_member(json_node_get_object(r), "id"),
                  "s-1") == 0);
  CHECK(g_strcmp0(error_data(r), "request_time_skew") == 0);
  json_node_unref(r);
  signet_mgmt_handler_set_requester_rate_limit(f.mgmt, 0, 0);
  r = intent(&f, &cap, requester_pk, skew_rpc, "w-s2");
  CHECK(g_strcmp0(json_object_get_string_member(json_node_get_object(r), "id"),
                  "s-1") == 0);
  CHECK(g_strcmp0(error_data(r), "rate_limited") == 0);
  json_node_unref(r);
  signet_mgmt_handler_set_replay_cache(f.mgmt, NULL);
  signet_mgmt_handler_set_self_replay_cache(f.mgmt, NULL);
  signet_replay_cache_free(replay);

  signet_mgmt_handler_set_reply_hook(f.mgmt, NULL, NULL);
  g_free(cap.recipient);
  if (cap.reply) sodium_memzero(cap.reply, strlen(cap.reply));
  g_free(cap.reply);
  g_free(cred); g_free(foreign);
  teardown(&f);
  puts("test_contextvm_reply_shapes: PASS");
}
#endif

int main(void) {
  CHECK(sodium_init() >= 0);
  test_encrypted_contextvm_lifecycle();
  test_expiry_and_unauthorized();
  test_delivery_denials_and_requester_audit();
  test_plaintext_mutation_rejected();
  test_credential_mutation_replay_rejected();
  test_delivery_grant_requester_without_provisioner();
  test_deliver_expected_version();
  test_replay_flood_cannot_reexecute();
  test_requester_rate_limit();
#ifdef SIGNET_ENABLE_TEST_HOOKS
  test_contextvm_reply_shapes();
#else
  puts("test_contextvm_reply_shapes: SKIP (needs SIGNET_ENABLE_TEST_HOOKS)");
#endif
  puts("credential lifecycle tests: ALL PASS");
  return 0;
}
