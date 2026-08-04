/* SPDX-License-Identifier: MIT
 * Credential lifecycle acceptance tests.
 */

#include "signet/audit_logger.h"
#include "signet/key_store.h"
#include "signet/mgmt_protocol.h"
#include "signet/store.h"
#include "signet/store_secrets.h"

#include <nostr-keys.h>
#include <nostr/nip44/nip44.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <glib.h>
#include <sodium.h>

#define MASTER_KEY "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

typedef struct {
  char *db_path;
  SignetAuditLogger *audit;
  SignetKeyStore *keys;
  SignetMgmtHandler *mgmt;
  char bunker_sk[65];
  char bunker_pk[65];
  char provisioner_sk[65];
  char provisioner_pk[65];
} Fixture;

static char *temp_db_path(void) {
  char tmpl[] = "/tmp/signet-credential-lifecycle-XXXXXX.db";
  int fd = mkstemps(tmpl, 3);
  assert(fd >= 0);
  close(fd);
  unlink(tmpl);
  return g_strdup(tmpl);
}

static void keypair(char sk[65], char pk[65]) {
  char *generated = nostr_key_generate_private();
  assert(generated && strlen(generated) == 64);
  char *public_key = nostr_key_get_public(generated);
  assert(public_key && strlen(public_key) == 64);
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
  assert(hex32(sender_sk, sk) == 0);
  assert(hex32(bunker_pk, pk) == 0);
  char *encrypted = NULL;
  assert(nostr_nip44_encrypt_v2(sk, pk, (const uint8_t *)json,
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
  assert(f->keys);

  const char *const provisioners[] = { f->provisioner_pk };
  SignetMgmtHandlerConfig mcfg = {
    .provisioner_pubkeys = provisioners,
    .n_provisioner_pubkeys = 1,
    .bunker_secret_key_hex = f->bunker_sk,
    .bunker_pubkey_hex = f->bunker_pk,
  };
  f->mgmt = signet_mgmt_handler_new(f->keys, NULL, NULL, NULL, &mcfg);
  assert(f->mgmt);

  char agent_sk[65], agent_pk[65], out_pk[65] = {0};
  uint8_t raw[32];
  keypair(agent_sk, agent_pk);
  assert(hex32(agent_sk, raw) == 0);
  assert(signet_key_store_adopt_agent(
      f->keys, "owner", raw, agent_pk, "pairing-secret",
      f->bunker_pk, NULL, 0, out_pk, NULL) == SIGNET_ADOPT_OK);
  sodium_memzero(raw, sizeof(raw));
  sodium_memzero(agent_sk, sizeof(agent_sk));
}

static void teardown(Fixture *f) {
  signet_mgmt_handler_free(f->mgmt);
  signet_key_store_free(f->keys);
  signet_audit_logger_free(f->audit);
  unlink(f->db_path);
  g_free(f->db_path);
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
  assert(store);

  char *create = request_json("primary", "first-token-value");
  assert(handle(&f, SIGNET_MGMT_OP_CREATE_CREDENTIAL,
                create, "create-1") == 0);
  char primary_id[70];
  assert(signet_secret_id_generate("owner", SIGNET_SECRET_API_TOKEN,
                                   "primary", primary_id) == 0);

  SignetSecretMetadata metadata;
  memset(&metadata, 0, sizeof(metadata));
  assert(signet_store_get_secret_metadata(
      store, primary_id, 2000000000, &metadata) == SIGNET_SECRET_OK);
  assert(strcmp(metadata.provenance, "created") == 0);
  assert(strcmp(metadata.created_by, f.provisioner_pk) == 0);
  assert(strcmp(metadata.policy_id, "least-privilege") == 0);
  assert(metadata.status == SIGNET_SECRET_STATUS_ACTIVE);
  signet_secret_metadata_clear(&metadata);

  /* Same deterministic slot must refuse overwrite and preserve the payload. */
  char *duplicate = request_json("primary", "must-not-overwrite");
  assert(handle(&f, SIGNET_MGMT_OP_CREATE_CREDENTIAL,
                duplicate, "create-2") == -1);
  SignetSecretRecord record;
  memset(&record, 0, sizeof(record));
  assert(signet_store_get_secret_at(
      store, primary_id, 2000000001, &record) == SIGNET_SECRET_OK);
  assert(record.payload_len == strlen("first-token-value"));
  assert(memcmp(record.payload, "first-token-value", record.payload_len) == 0);
  signet_secret_record_clear(&record);

  /* Import is a distinct provenance path, but uses the same protected payload. */
  char *imported = request_json("imported", "imported-token");
  assert(handle(&f, SIGNET_MGMT_OP_IMPORT_CREDENTIAL,
                imported, "import-1") == 0);
  char imported_id[70];
  assert(signet_secret_id_generate("owner", SIGNET_SECRET_API_TOKEN,
                                   "imported", imported_id) == 0);
  assert(signet_store_get_secret_metadata(
      store, imported_id, 2000000000, &metadata) == SIGNET_SECRET_OK);
  assert(strcmp(metadata.provenance, "imported") == 0);
  signet_secret_metadata_clear(&metadata);

  char *rotate_b64 =
      g_base64_encode((const guchar *)"rotated-token", strlen("rotated-token"));
  char *rotate = g_strdup_printf(
      "{\"request_id\":\"r\",\"credential_id\":\"%s\","
      "\"expires_at\":2000000200,\"payload_b64\":\"%s\"}",
      primary_id, rotate_b64);
  sodium_memzero(rotate_b64, strlen(rotate_b64));
  g_free(rotate_b64);
  assert(handle(&f, SIGNET_MGMT_OP_ROTATE_CREDENTIAL,
                rotate, "rotate-1") == 0);
  assert(signet_store_secret_history_count(store, primary_id) == 1);
  assert(signet_store_get_secret_metadata(
      store, primary_id, 2000000000, &metadata) == SIGNET_SECRET_OK);
  assert(metadata.version == 2 && metadata.active_version == 2);
  signet_secret_metadata_clear(&metadata);

  char *inspect = g_strdup_printf(
      "{\"request_id\":\"r\",\"credential_id\":\"%s\"}", primary_id);
  assert(handle(&f, SIGNET_MGMT_OP_INSPECT_CREDENTIAL,
                inspect, "inspect-1") == 0);
  assert(handle(&f, SIGNET_MGMT_OP_LIST_CREDENTIALS,
                "{\"request_id\":\"r\",\"agent_id\":\"owner\"}",
                "list-1") == 0);

  assert(handle(&f, SIGNET_MGMT_OP_REVOKE_CREDENTIAL,
                inspect, "revoke-1") == 0);
  assert(signet_store_get_secret_at(
      store, primary_id, 2000000001, &record) == SIGNET_SECRET_REVOKED);
  assert(handle(&f, SIGNET_MGMT_OP_INSPECT_CREDENTIAL,
                inspect, "inspect-revoked") == 0);
  assert(handle(&f, SIGNET_MGMT_OP_DELETE_CREDENTIAL,
                inspect, "delete-1") == 0);
  assert(signet_store_get_secret_metadata(
      store, primary_id, 2000000001, &metadata) == SIGNET_SECRET_NOT_FOUND);

  g_free(create);
  g_free(duplicate);
  g_free(imported);
  g_free(rotate);
  g_free(inspect);
  teardown(&f);
  puts("test_encrypted_contextvm_lifecycle: PASS");
}

static void test_expiry_and_unauthorized(void) {
  Fixture f;
  setup(&f);
  SignetStore *store = signet_key_store_get_store(f.keys);
  assert(store);

  SignetSecretMetadata metadata;
  memset(&metadata, 0, sizeof(metadata));
  assert(signet_store_create_secret(
      store, "owner", SIGNET_SECRET_CREDENTIAL, "expires",
      (const uint8_t *)"value", 5, NULL, 2000000010,
      "created", f.provisioner_pk, 2000000000,
      &metadata) == SIGNET_SECRET_OK);
  char *expired_id = g_strdup(metadata.id);
  signet_secret_metadata_clear(&metadata);

  SignetSecretRecord record;
  memset(&record, 0, sizeof(record));
  assert(signet_store_get_secret_at(
      store, expired_id, 2000000009, &record) == SIGNET_SECRET_OK);
  signet_secret_record_clear(&record);
  assert(signet_store_get_secret_at(
      store, expired_id, 2000000010, &record) == SIGNET_SECRET_EXPIRED);
  assert(signet_store_rotate_secret_ex(
      store, expired_id, (const uint8_t *)"new", 3,
      false, 0, 2000000011, NULL) == SIGNET_SECRET_EXPIRED);
  assert(signet_store_rotate_secret_ex(
      store, expired_id, (const uint8_t *)"new", 3,
      true, 2000000100, 2000000011, NULL) == SIGNET_SECRET_OK);
  assert(signet_store_secret_history_count(store, expired_id) == 1);

  char attacker_sk[65], attacker_pk[65];
  keypair(attacker_sk, attacker_pk);
  char *request = request_json("attacker", "stolen");
  char *encrypted = encrypt_request(attacker_sk, f.bunker_pk, request);
  assert(signet_mgmt_handler_handle_request(
      f.mgmt, attacker_pk, encrypted, SIGNET_MGMT_OP_CREATE_CREDENTIAL,
      "unauthorized-1", 2000000000) == -1);
  char attacker_id[70];
  assert(signet_secret_id_generate("owner", SIGNET_SECRET_API_TOKEN,
                                   "attacker", attacker_id) == 0);
  assert(signet_store_get_secret_metadata(
      store, attacker_id, 2000000000, &metadata) == SIGNET_SECRET_NOT_FOUND);

  free(encrypted);
  g_free(request);
  g_free(expired_id);
  sodium_memzero(attacker_sk, sizeof(attacker_sk));
  teardown(&f);
  puts("test_expiry_and_unauthorized: PASS");
}

int main(void) {
  assert(sodium_init() >= 0);
  test_encrypted_contextvm_lifecycle();
  test_expiry_and_unauthorized();
  puts("credential lifecycle tests: ALL PASS");
  return 0;
}
