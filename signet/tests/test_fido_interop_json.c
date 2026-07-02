/* SPDX-License-Identifier: MIT */
/*
 * Phase 3 interop emitter: drive SignetFidoService through the wired JSON
 * entrypoints used by D-Bus net.signet.Passkeys and NIP-46 webauthn_* handlers,
 * then emit artifacts for independent python-fido2 verification.
 */

#include "signet/audit_logger.h"
#include "signet/fido.h"
#include "signet/fido_cbor.h"
#include "store_passkeys_schema.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <json-glib/json-glib.h>
#include <sqlite3.h>
#include <sodium.h>

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

static char *hex_encode(const uint8_t *bytes, size_t len) {
  static const char hexdigits[] = "0123456789abcdef";
  char *out = (char *)calloc(len * 2 + 1, 1);
  if (!out) return NULL;
  for (size_t i = 0; i < len; i++) {
    out[i * 2] = hexdigits[bytes[i] >> 4];
    out[i * 2 + 1] = hexdigits[bytes[i] & 0x0f];
  }
  return out;
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

static int json_get_int_member(const char *json, const char *member, gint64 *out) {
  JsonParser *parser = json_parser_new();
  if (!parser) return -1;
  int rc = -1;
  if (json_parser_load_from_data(parser, json, -1, NULL)) {
    JsonNode *root = json_parser_get_root(parser);
    if (root && JSON_NODE_HOLDS_OBJECT(root)) {
      JsonObject *obj = json_node_get_object(root);
      if (json_object_has_member(obj, member)) {
        *out = json_object_get_int_member(obj, member);
        rc = 0;
      }
    }
  }
  g_object_unref(parser);
  return rc;
}

static uint8_t *json_get_b64_dup(const char *json, const char *member, size_t *out_len) {
  if (out_len) *out_len = 0;
  char *s = json_get_string_dup(json, member);
  if (!s) return NULL;
  gsize len = 0;
  guchar *raw = g_base64_decode(s, &len);
  g_free(s);
  if (!raw) return NULL;
  uint8_t *out = (uint8_t *)malloc((size_t)len ? (size_t)len : 1);
  if (out) {
    memcpy(out, raw, (size_t)len);
    if (out_len) *out_len = (size_t)len;
  }
  g_free(raw);
  return out;
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

  uint8_t client_hash_reg[32];
  uint8_t client_hash_assert[32];
  uint8_t user_handle[8];
  fill_bytes(client_hash_reg, sizeof(client_hash_reg), 0x20);
  fill_bytes(client_hash_assert, sizeof(client_hash_assert), 0xa0);
  fill_bytes(user_handle, sizeof(user_handle), 0x60);

  char *reg_hash_b64 = (char *)g_base64_encode(client_hash_reg, sizeof(client_hash_reg));
  char *assert_hash_b64 = (char *)g_base64_encode(client_hash_assert, sizeof(client_hash_assert));
  char *user_handle_b64 = (char *)g_base64_encode(user_handle, sizeof(user_handle));
  OK(reg_hash_b64 && assert_hash_b64 && user_handle_b64, "base64 inputs");

  char *mk_req = g_strdup_printf("{\"rpId\":\"example.com\",\"clientDataHash\":\"%s\",\"userHandle\":\"%s\",\"userName\":\"interop-agent\",\"userDisplayName\":\"Interop Agent\",\"discoverable\":true,\"userVerification\":\"preferred\",\"pubKeyCredParams\":[-7]}",
                                 reg_hash_b64, user_handle_b64);
  OK(mk_req != NULL, "makeCredential JSON request");

  char *mk_json = NULL;
  SignetFidoError err;
  OK(signet_fido_make_credential_json(svc, "agent-interop", mk_req, &mk_json, &err) == SIGNET_FIDO_OK,
     "MakeCredential JSON entrypoint");

  gint64 mk_sign_count = -1;
  OK(json_get_int_member(mk_json, "signCount", &mk_sign_count) == 0 && mk_sign_count == 0,
     "MakeCredential signCount zero in JSON response");

  size_t cred_len = 0, att_len = 0, cose_len = 0;
  uint8_t *cred = json_get_b64_dup(mk_json, "credentialId", &cred_len);
  uint8_t *att = json_get_b64_dup(mk_json, "attestationObject", &att_len);
  uint8_t *cose = json_get_b64_dup(mk_json, "publicKeyCose", &cose_len);
  OK(cred && cred_len == 32, "credentialId from JSON response");
  OK(att && att_len > 0, "attestationObject from JSON response");
  OK(cose && cose_len == 77, "COSE public key from JSON response");
  OK(cose[0] == 0xa5 && cose[8] == 0x58 && cose[9] == 0x20 &&
     cose[42] == 0x22 && cose[43] == 0x58 && cose[44] == 0x20,
     "COSE public key has expected ES256 shape");

  char *cred_b64 = (char *)g_base64_encode(cred, cred_len);
  OK(cred_b64 != NULL, "credential allow-list base64");
  char *ga_req = g_strdup_printf("{\"rpId\":\"example.com\",\"clientDataHash\":\"%s\",\"userVerification\":\"preferred\",\"allowCredentials\":[\"%s\"]}",
                                 assert_hash_b64, cred_b64);
  OK(ga_req != NULL, "GetAssertion JSON request");

  char *ga_json = NULL;
  OK(signet_fido_get_assertion_json(svc, "agent-interop", ga_req, &ga_json, &err) == SIGNET_FIDO_OK,
     "GetAssertion JSON entrypoint");
  gint64 ga_sign_count = -1;
  OK(json_get_int_member(ga_json, "signCount", &ga_sign_count) == 0 && ga_sign_count == 0,
     "GetAssertion signCount zero in JSON response");

  size_t auth_assert_len = 0, sig_len = 0;
  uint8_t *auth_assert = json_get_b64_dup(ga_json, "authData", &auth_assert_len);
  uint8_t *sig = json_get_b64_dup(ga_json, "signature", &sig_len);
  OK(auth_assert && auth_assert_len == 37, "assertion authData from JSON response");
  OK(sig && sig_len > 0, "assertion signature from JSON response");
  OK(auth_assert[32] == (SIGNET_FIDO_FLAG_UP | SIGNET_FIDO_FLAG_BE | SIGNET_FIDO_FLAG_BS),
     "assertion flags UP+BE+BS via JSON entrypoint");
  OK(memcmp(auth_assert + 33, "\0\0\0\0", 4) == 0, "assertion signCount bytes zero");

  char *att_hex = hex_encode(att, att_len);
  char *cred_hex = hex_encode(cred, cred_len);
  char *auth_assert_hex = hex_encode(auth_assert, auth_assert_len);
  char *assert_hash_hex = hex_encode(client_hash_assert, sizeof(client_hash_assert));
  char *sig_hex = hex_encode(sig, sig_len);
  char *pub_x_hex = hex_encode(cose + 10, 32);
  char *pub_y_hex = hex_encode(cose + 45, 32);
  OK(att_hex && cred_hex && auth_assert_hex && assert_hash_hex && sig_hex && pub_x_hex && pub_y_hex,
     "hex encode artifacts");

  printf("{\n");
  printf("  \"rpId\": \"example.com\",\n");
  printf("  \"credentialId\": \"%s\",\n", cred_hex);
  printf("  \"attestationObject\": \"%s\",\n", att_hex);
  printf("  \"authDataAssert\": \"%s\",\n", auth_assert_hex);
  printf("  \"clientDataHashAssert\": \"%s\",\n", assert_hash_hex);
  printf("  \"signature\": \"%s\",\n", sig_hex);
  printf("  \"pubX\": \"%s\",\n", pub_x_hex);
  printf("  \"pubY\": \"%s\"\n", pub_y_hex);
  printf("}\n");

  free(pub_y_hex);
  free(pub_x_hex);
  free(sig_hex);
  free(assert_hash_hex);
  free(auth_assert_hex);
  free(cred_hex);
  free(att_hex);
  free(sig);
  free(auth_assert);
  g_free(ga_json);
  g_free(ga_req);
  g_free(cred_b64);
  free(cose);
  free(att);
  free(cred);
  g_free(mk_json);
  g_free(mk_req);
  g_free(user_handle_b64);
  g_free(assert_hash_b64);
  g_free(reg_hash_b64);
  signet_fido_service_free(svc);
  sqlite3_close(db);
  return 0;
}
