/* SPDX-License-Identifier: MIT */
/*
 * Phase 2 SignetFidoService test: MakeCredential -> GetAssertion happy path,
 * WebAuthn flags/signCount semantics, excludeCredentials, and UV-required
 * default-deny. Standalone: uses an in-memory SQLite SignetStore shim.
 */

#include "signet/audit_logger.h"
#include "signet/fido.h"
#include "signet/fido_cbor.h"
#include "signet/fido_crypto.h"
#include "store_passkeys_schema.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>
#include <sodium.h>
#include <glib.h>
#include <json-glib/json-glib.h>

#define OK(cond, msg) do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); return 1; } } while (0)

struct SignetStore {
  sqlite3 *db;
};

sqlite3 *signet_store_get_db(SignetStore *store) {
  return store ? store->db : NULL;
}

int signet_audit_log_common(SignetAuditLogger *l,
                            SignetAuditEventType type,
                            const SignetAuditCommonFields *fields,
                            const char *details_json_object) {
  (void)l;
  (void)type;
  (void)fields;
  (void)details_json_object;
  return 0;
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

static void fill_bytes(uint8_t *dst, size_t len, uint8_t seed) {
  for (size_t i = 0; i < len; i++) dst[i] = (uint8_t)(seed + i);
}

static char *json_get_string_dup(const char *json, const char *member) {
  JsonParser *parser = json_parser_new();
  if (!parser) return NULL;
  if (!json_parser_load_from_data(parser, json, -1, NULL)) {
    g_object_unref(parser);
    return NULL;
  }
  JsonNode *root = json_parser_get_root(parser);
  char *out = NULL;
  if (root && JSON_NODE_HOLDS_OBJECT(root)) {
    JsonObject *obj = json_node_get_object(root);
    if (json_object_has_member(obj, member))
      out = g_strdup(json_object_get_string_member(obj, member));
  }
  g_object_unref(parser);
  return out;
}

static int verify_cose_es256_assertion(const uint8_t *cose, size_t cose_len,
                                       const uint8_t *auth_data, size_t auth_data_len,
                                       const uint8_t client_hash[32],
                                       const uint8_t *sig, size_t sig_len) {
  if (!cose || !auth_data || !sig) return -1;
  uint8_t x[32], y[32];
  if (signet_cose_ec2_p256_parse(cose, cose_len, x, y) != 0) return -1;
  uint8_t *msg = (uint8_t *)malloc(auth_data_len + 32);
  if (!msg) return -1;
  memcpy(msg, auth_data, auth_data_len);
  memcpy(msg + auth_data_len, client_hash, 32);
  int ok = signet_fido_verify_p256(x, y, msg, auth_data_len + 32, sig, sig_len);
  free(msg);
  return ok == 1 ? 0 : -1;
}

static bool has_flag(uint8_t flags, uint8_t flag) {
  return (flags & flag) == flag;
}

static int verify_attestation_none_structure(const uint8_t *att, size_t att_len,
                                             const uint8_t *auth_data,
                                             size_t auth_data_len) {
  if (!att || !auth_data || auth_data_len > 0xff || att_len != auth_data_len + 30)
    return -1;

  size_t i = 0;
  if (att[i++] != 0xa3) return -1;                         /* map(3) */
  if (att[i++] != 0x63 || memcmp(att + i, "fmt", 3) != 0) return -1;
  i += 3;
  if (att[i++] != 0x64 || memcmp(att + i, "none", 4) != 0) return -1;
  i += 4;
  if (att[i++] != 0x67 || memcmp(att + i, "attStmt", 7) != 0) return -1;
  i += 7;
  if (att[i++] != 0xa0) return -1;                         /* empty map */
  if (att[i++] != 0x68 || memcmp(att + i, "authData", 8) != 0) return -1;
  i += 8;
  if (att[i++] != 0x58 || att[i++] != auth_data_len) return -1;
  if (memcmp(att + i, auth_data, auth_data_len) != 0) return -1;
  i += auth_data_len;
  return i == att_len ? 0 : -1;
}

int main(void) {
  OK(sodium_init() >= 0, "sodium_init");

  sqlite3 *db = NULL;
  OK(sqlite3_open(":memory:", &db) == SQLITE_OK, "open sqlite");
  OK(exec_sql(db, SIGNET_PASSKEY_CREDENTIALS_SCHEMA_SQL) == 0, "passkey schema");

  SignetStore store = { db };
  uint8_t psk[SIGNET_PASSKEY_PSK_LEN];
  randombytes_buf(psk, sizeof(psk));

  uint8_t aaguid[SIGNET_FIDO_AAGUID_LEN];
  OK(signet_fido_parse_aaguid(SIGNET_FIDO_DEFAULT_AAGUID, aaguid) == 0,
     "parse default AAGUID");

  SignetFidoServiceConfig cfg = {
    .enabled = true,
    .store = &store,
    .audit = NULL,
    .fleet_psk = psk,
    .fleet_psk_len = sizeof(psk),
    .backend = "software-openssl",
    .attestation = "none",
    .allow_headless_uv = false,
  };
  memcpy(cfg.aaguid, aaguid, sizeof(aaguid));
  SignetFidoService *svc = signet_fido_service_new(&cfg);
  OK(svc != NULL, "service new");

  uint8_t client_hash[32];
  uint8_t assert_hash[32];
  uint8_t user_handle[8];
  fill_bytes(client_hash, sizeof(client_hash), 0x10);
  fill_bytes(assert_hash, sizeof(assert_hash), 0x80);
  fill_bytes(user_handle, sizeof(user_handle), 0x40);
  int algs[1] = { SIGNET_PASSKEY_COSE_ALG_ES256 };

  SignetFidoMakeCredentialRequest mk = {
    .rp_id = "example.com",
    .client_data_hash = client_hash,
    .client_data_hash_len = sizeof(client_hash),
    .user_handle = user_handle,
    .user_handle_len = sizeof(user_handle),
    .user_name = "agent",
    .user_display_name = "Agent",
    .discoverable = true,
    .user_verification = SIGNET_FIDO_UV_PREFERRED,
    .pub_key_cred_params = algs,
    .pub_key_cred_param_count = 1,
    .now = 1234,
  };
  SignetFidoMakeCredentialResult mkres;
  SignetFidoError err;
  OK(signet_fido_make_credential(svc, "agent-alpha", &mk, &mkres, &err) == SIGNET_FIDO_OK,
     "make credential happy path");
  OK(mkres.credential_id_len == 32, "credential id length");
  OK(mkres.sign_count == 0, "make signCount zero");
  OK(mkres.auth_data_len > 37 && mkres.auth_data[32] ==
     (SIGNET_FIDO_FLAG_UP | SIGNET_FIDO_FLAG_BE | SIGNET_FIDO_FLAG_BS | SIGNET_FIDO_FLAG_AT),
     "make flags UP+BE+BS+AT");
  OK(has_flag(mkres.auth_data[32], SIGNET_FIDO_FLAG_BE), "make BE bit set explicitly");
  OK(has_flag(mkres.auth_data[32], SIGNET_FIDO_FLAG_BS), "make BS bit set explicitly");
  OK(memcmp(mkres.auth_data + 33, "\0\0\0\0", 4) == 0, "make signCount bytes zero");
  OK(memcmp(mkres.auth_data + 37, aaguid, sizeof(aaguid)) == 0, "frozen AAGUID in authData");
  OK(mkres.cose_public_key_len == 77, "COSE_Key length");
  OK(verify_attestation_none_structure(mkres.attestation_object, mkres.attestation_object_len,
                                       mkres.auth_data, mkres.auth_data_len) == 0,
     "attestation object is CBOR none with embedded authData");

  const uint8_t *exclude_ids[1] = { mkres.credential_id };
  size_t exclude_lens[1] = { mkres.credential_id_len };
  mk.exclude_credential_ids = exclude_ids;
  mk.exclude_credential_id_lens = exclude_lens;
  mk.exclude_credential_count = 1;
  SignetFidoMakeCredentialResult dupres;
  OK(signet_fido_make_credential(svc, "agent-alpha", &mk, &dupres, &err) == SIGNET_FIDO_ERR_EXCLUDED,
     "excludeCredentials rejects duplicate");

  SignetFidoGetAssertionRequest ga = {
    .rp_id = "example.com",
    .client_data_hash = assert_hash,
    .client_data_hash_len = sizeof(assert_hash),
    .user_verification = SIGNET_FIDO_UV_PREFERRED,
    .allow_credential_ids = exclude_ids,
    .allow_credential_id_lens = exclude_lens,
    .allow_credential_count = 1,
  };
  SignetFidoGetAssertionResult gares;
  OK(signet_fido_get_assertion(svc, "agent-alpha", &ga, &gares, &err) == SIGNET_FIDO_OK,
     "get assertion happy path");
  OK(gares.sign_count == 0, "assert signCount zero");
  OK(gares.auth_data_len == 37, "assert authData has no AT data");
  OK(gares.auth_data[32] == (SIGNET_FIDO_FLAG_UP | SIGNET_FIDO_FLAG_BE | SIGNET_FIDO_FLAG_BS),
     "assert flags UP+BE+BS no AT");
  OK(has_flag(gares.auth_data[32], SIGNET_FIDO_FLAG_BE), "assert BE bit set explicitly");
  OK(has_flag(gares.auth_data[32], SIGNET_FIDO_FLAG_BS), "assert BS bit set explicitly");
  OK(!has_flag(gares.auth_data[32], SIGNET_FIDO_FLAG_AT), "assert AT bit not set");
  OK(memcmp(gares.auth_data + 33, "\0\0\0\0", 4) == 0, "assert signCount bytes zero");
  OK(gares.user_handle_len == sizeof(user_handle) &&
     memcmp(gares.user_handle, user_handle, sizeof(user_handle)) == 0,
     "user handle round-trip");
  OK(verify_cose_es256_assertion(mkres.cose_public_key, mkres.cose_public_key_len,
                                 gares.auth_data, gares.auth_data_len,
                                 assert_hash, gares.signature_der,
                                 gares.signature_der_len) == 0,
     "assertion signature verifies");

  ga.user_verification = SIGNET_FIDO_UV_REQUIRED;
  SignetFidoGetAssertionResult denied;
  OK(signet_fido_get_assertion(svc, "agent-alpha", &ga, &denied, &err) == SIGNET_FIDO_ERR_UV_REQUIRED,
     "UV-required default-deny");
  ga.user_verification = SIGNET_FIDO_UV_PREFERRED;

  char *cred_b64 = (char *)g_base64_encode(mkres.credential_id, mkres.credential_id_len);
  OK(cred_b64 != NULL, "credential id base64");
  char *export_req = g_strdup_printf("{\"credentialId\":\"%s\"}", cred_b64);
  OK(export_req != NULL, "export request JSON");
  char *export_json = NULL;
  OK(signet_fido_export_credential_json(svc, "agent-alpha", export_req, &export_json, &err) == SIGNET_FIDO_OK,
     "export credential JSON");
  char *container_b64 = json_get_string_dup(export_json, "container");
  OK(container_b64 != NULL && container_b64[0] != '\0', "export response container");
  char *import_req = g_strdup_printf("{\"container\":\"%s\"}", container_b64);
  OK(import_req != NULL, "import request JSON");

  sqlite3 *wrong_db = NULL;
  OK(sqlite3_open(":memory:", &wrong_db) == SQLITE_OK, "open wrong-psk sqlite");
  OK(exec_sql(wrong_db, SIGNET_PASSKEY_CREDENTIALS_SCHEMA_SQL) == 0, "wrong-psk schema");
  SignetStore wrong_store = { wrong_db };
  uint8_t wrong_psk[SIGNET_PASSKEY_PSK_LEN];
  randombytes_buf(wrong_psk, sizeof(wrong_psk));
  SignetFidoServiceConfig wrong_cfg = cfg;
  wrong_cfg.store = &wrong_store;
  wrong_cfg.fleet_psk = wrong_psk;
  wrong_cfg.fleet_psk_len = sizeof(wrong_psk);
  SignetFidoService *wrong_svc = signet_fido_service_new(&wrong_cfg);
  OK(wrong_svc != NULL, "wrong-psk service new");
  char *wrong_import_json = NULL;
  OK(signet_fido_import_credential_json(wrong_svc, "agent-alpha", import_req,
                                        &wrong_import_json, &err) == SIGNET_FIDO_ERR_BAD_REQUEST,
     "import rejects wrong fleet PSK");
  g_free(wrong_import_json);
  signet_fido_service_free(wrong_svc);
  sqlite3_close(wrong_db);

  sqlite3 *db2 = NULL;
  OK(sqlite3_open(":memory:", &db2) == SQLITE_OK, "open second sqlite");
  OK(exec_sql(db2, SIGNET_PASSKEY_CREDENTIALS_SCHEMA_SQL) == 0, "second schema");
  SignetStore store2 = { db2 };
  SignetFidoServiceConfig cfg2 = cfg;
  cfg2.store = &store2;
  SignetFidoService *svc2 = signet_fido_service_new(&cfg2);
  OK(svc2 != NULL, "second service new");
  char *import_json = NULL;
  OK(signet_fido_import_credential_json(svc2, "agent-alpha", import_req,
                                        &import_json, &err) == SIGNET_FIDO_OK,
     "import credential with same fleet PSK");

  SignetFidoGetAssertionResult gares2;
  OK(signet_fido_get_assertion(svc2, "agent-alpha", &ga, &gares2, &err) == SIGNET_FIDO_OK,
     "imported credential is usable in second store");
  OK(verify_cose_es256_assertion(mkres.cose_public_key, mkres.cose_public_key_len,
                                 gares2.auth_data, gares2.auth_data_len,
                                 assert_hash, gares2.signature_der,
                                 gares2.signature_der_len) == 0,
     "imported assertion signature verifies");

  signet_fido_get_assertion_result_clear(&gares2);
  g_free(import_json);
  signet_fido_service_free(svc2);
  sqlite3_close(db2);
  g_free(import_req);
  g_free(container_b64);
  g_free(export_json);
  g_free(export_req);
  g_free(cred_b64);
  signet_fido_get_assertion_result_clear(&gares);
  signet_fido_make_credential_result_clear(&mkres);
  signet_fido_service_free(svc);
  sqlite3_close(db);
  printf("PASS test_fido_service (MakeCredential->GetAssertion; flags; signCount=0; excludeCredentials; UV-required deny; export/import portability)\n");
  return 0;
}
