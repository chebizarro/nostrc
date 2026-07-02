/* SPDX-License-Identifier: MIT */
/*
 * fido.c - Signet software WebAuthn/FIDO2 authenticator service.
 */

#include "signet/fido.h"

#include "signet/audit_logger.h"
#include "signet/fido_cbor.h"
#include "signet/fido_crypto.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <glib.h>
#include <json-glib/json-glib.h>
#include <openssl/sha.h>
#include <sodium.h>

struct SignetFidoService {
  bool enabled;
  struct SignetStore *store;
  struct SignetAuditLogger *audit;
  uint8_t fleet_psk[SIGNET_PASSKEY_PSK_LEN];
  bool have_fleet_psk;
  uint8_t aaguid[SIGNET_FIDO_AAGUID_LEN];
  char *backend;
  char *attestation;
  bool allow_headless_uv;
};

typedef struct {
  uint8_t **items;
  size_t *lens;
  size_t count;
} BlobList;

static int64_t fido_now(void) {
  return (int64_t)time(NULL);
}

static void set_error(SignetFidoError *err, SignetFidoStatus status, const char *reason) {
  if (!err) return;
  err->status = status;
  err->reason = reason ? reason : signet_fido_status_string(status);
}

static SignetFidoStatus not_configured(SignetFidoError *err) {
  set_error(err, SIGNET_FIDO_ERR_NOT_CONFIGURED, "passkeys disabled or missing store/sync key");
  return SIGNET_FIDO_ERR_NOT_CONFIGURED;
}

static bool service_ready(SignetFidoService *svc) {
  return svc && svc->enabled && svc->store && svc->have_fleet_psk;
}

static uint8_t *dup_bytes(const uint8_t *p, size_t n) {
  if (!p || n == 0) return NULL;
  uint8_t *out = (uint8_t *)malloc(n);
  if (!out) return NULL;
  memcpy(out, p, n);
  return out;
}

static char *json_escape(const char *s) {
  if (!s) return g_strdup("");
  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "v");
  json_builder_add_string_value(b, s);
  json_builder_end_object(b);
  JsonNode *root = json_builder_get_root(b);
  g_autoptr(JsonGenerator) gen = json_generator_new();
  json_generator_set_root(gen, root);
  gchar *tmp = json_generator_to_data(gen, NULL);
  json_node_unref(root);
  if (!tmp) return g_strdup("");
  gchar *colon = strchr(tmp, ':');
  gchar *end = strrchr(tmp, '}');
  char *out = NULL;
  if (colon && end && end > colon + 1) out = g_strndup(colon + 1, (gsize)(end - colon - 1));
  g_free(tmp);
  return out ? out : g_strdup("\"\"");
}

static void bytes_hex(const uint8_t *bytes, size_t len, char *out_hex) {
  static const char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < len; i++) {
    out_hex[i * 2] = hex[(bytes[i] >> 4) & 0x0f];
    out_hex[i * 2 + 1] = hex[bytes[i] & 0x0f];
  }
  out_hex[len * 2] = '\0';
}

static void sha256_hex_blob(const uint8_t *bytes, size_t len,
                            char out_hex[SHA256_DIGEST_LENGTH * 2 + 1]) {
  uint8_t digest[SHA256_DIGEST_LENGTH];
  SHA256(bytes, len, digest);
  bytes_hex(digest, sizeof(digest), out_hex);
  sodium_memzero(digest, sizeof(digest));
}

static void audit_ceremony(SignetFidoService *svc,
                           const char *agent_id,
                           const char *method,
                           const char *rp_id,
                           const uint8_t *credential_id,
                           size_t credential_id_len,
                           const char *decision,
                           const char *reason,
                           SignetFidoUserVerification uv) {
  if (!svc || !svc->audit) return;

  char rp_hash_hex[SHA256_DIGEST_LENGTH * 2 + 1];
  char cred_hash_hex[SHA256_DIGEST_LENGTH * 2 + 1];
  rp_hash_hex[0] = '\0';
  cred_hash_hex[0] = '\0';
  if (rp_id) sha256_hex_blob((const uint8_t *)rp_id, strlen(rp_id), rp_hash_hex);
  if (credential_id && credential_id_len > 0) sha256_hex_blob(credential_id, credential_id_len, cred_hash_hex);

  const char *uv_mode = uv == SIGNET_FIDO_UV_REQUIRED ? "required" :
                        uv == SIGNET_FIDO_UV_PREFERRED ? "preferred" : "discouraged";

  char *detail = g_strdup_printf("{\"rp_id_hash\":\"%s\",\"credential_id_hash\":\"%s\",\"uv\":\"%s\"}",
                                 rp_hash_hex, cred_hash_hex, uv_mode);
  if (!detail) return;

  SignetAuditCommonFields f;
  memset(&f, 0, sizeof(f));
  f.identity = agent_id;
  f.method = method;
  f.decision = decision;
  f.reason_code = reason;
  (void)signet_audit_log_common(svc->audit, SIGNET_AUDIT_EVENT_PASSKEY_CEREMONY,
                                &f, detail);
  g_free(detail);
}

const char *signet_fido_status_string(SignetFidoStatus status) {
  switch (status) {
    case SIGNET_FIDO_OK: return "ok";
    case SIGNET_FIDO_ERR_NOT_CONFIGURED: return "not_configured";
    case SIGNET_FIDO_ERR_BAD_REQUEST: return "bad_request";
    case SIGNET_FIDO_ERR_UNSUPPORTED_ALGORITHM: return "unsupported_algorithm";
    case SIGNET_FIDO_ERR_EXCLUDED: return "excluded_credential";
    case SIGNET_FIDO_ERR_NOT_FOUND: return "credential_not_found";
    case SIGNET_FIDO_ERR_UV_REQUIRED: return "user_verification_required";
    case SIGNET_FIDO_ERR_INTERNAL: return "internal_error";
    default: return "unknown";
  }
}

void signet_fido_error_clear(SignetFidoError *err) {
  if (!err) return;
  err->status = SIGNET_FIDO_OK;
  err->reason = NULL;
}

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + c - 'a';
  if (c >= 'A' && c <= 'F') return 10 + c - 'A';
  return -1;
}

int signet_fido_parse_aaguid(const char *uuid, uint8_t out[SIGNET_FIDO_AAGUID_LEN]) {
  if (!uuid || !out) return -1;
  char hex[33];
  size_t n = 0;
  for (const char *p = uuid; *p; p++) {
    if (*p == '-') continue;
    if (n >= 32 || hexval(*p) < 0) return -1;
    hex[n++] = *p;
  }
  if (n != 32) return -1;
  for (size_t i = 0; i < 16; i++) {
    int hi = hexval(hex[i * 2]);
    int lo = hexval(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) return -1;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return 0;
}

void signet_fido_format_aaguid(const uint8_t aaguid[SIGNET_FIDO_AAGUID_LEN],
                               char out[37]) {
  char hex[33];
  bytes_hex(aaguid, SIGNET_FIDO_AAGUID_LEN, hex);
  snprintf(out, 37, "%.8s-%.4s-%.4s-%.4s-%.12s",
           hex, hex + 8, hex + 12, hex + 16, hex + 20);
}

SignetFidoService *signet_fido_service_new(const SignetFidoServiceConfig *cfg) {
  SignetFidoService *svc = (SignetFidoService *)calloc(1, sizeof(*svc));
  if (!svc) return NULL;

  svc->enabled = cfg ? cfg->enabled : false;
  svc->store = cfg ? cfg->store : NULL;
  svc->audit = cfg ? cfg->audit : NULL;
  svc->backend = g_strdup((cfg && cfg->backend) ? cfg->backend : "software-openssl");
  svc->attestation = g_strdup((cfg && cfg->attestation) ? cfg->attestation : "none");
  svc->allow_headless_uv = cfg ? cfg->allow_headless_uv : false;

  if (cfg && cfg->fleet_psk && cfg->fleet_psk_len == SIGNET_PASSKEY_PSK_LEN) {
    memcpy(svc->fleet_psk, cfg->fleet_psk, SIGNET_PASSKEY_PSK_LEN);
    svc->have_fleet_psk = true;
  }
  if (cfg) memcpy(svc->aaguid, cfg->aaguid, SIGNET_FIDO_AAGUID_LEN);
  else (void)signet_fido_parse_aaguid(SIGNET_FIDO_DEFAULT_AAGUID, svc->aaguid);

  if (!svc->backend || !svc->attestation) {
    signet_fido_service_free(svc);
    return NULL;
  }
  return svc;
}

void signet_fido_service_free(SignetFidoService *svc) {
  if (!svc) return;
  sodium_memzero(svc->fleet_psk, sizeof(svc->fleet_psk));
  g_free(svc->backend);
  g_free(svc->attestation);
  free(svc);
}

bool signet_fido_service_is_enabled(const SignetFidoService *svc) {
  return svc && svc->enabled;
}

static bool has_es256(const SignetFidoMakeCredentialRequest *req) {
  if (!req || !req->pub_key_cred_params || req->pub_key_cred_param_count == 0) return false;
  for (size_t i = 0; i < req->pub_key_cred_param_count; i++) {
    if (req->pub_key_cred_params[i] == SIGNET_PASSKEY_COSE_ALG_ES256) return true;
  }
  return false;
}

static bool validate_uv(SignetFidoService *svc,
                        SignetFidoUserVerification uv,
                        SignetFidoError *err) {
  if (uv == SIGNET_FIDO_UV_REQUIRED && (!svc || !svc->allow_headless_uv)) {
    set_error(err, SIGNET_FIDO_ERR_UV_REQUIRED,
              "userVerification required but headless UV is disabled");
    return false;
  }
  return true;
}

SignetFidoStatus signet_fido_make_credential(SignetFidoService *svc,
                                             const char *agent_id,
                                             const SignetFidoMakeCredentialRequest *req,
                                             SignetFidoMakeCredentialResult *out,
                                             SignetFidoError *err) {
  if (out) memset(out, 0, sizeof(*out));
  signet_fido_error_clear(err);
  if (!service_ready(svc)) return not_configured(err);
  if (!agent_id || !agent_id[0] || !req || !out ||
      !req->rp_id || !req->rp_id[0] ||
      !req->client_data_hash || req->client_data_hash_len != SIGNET_FIDO_CLIENT_DATA_HASH_LEN ||
      !req->user_handle || req->user_handle_len == 0) {
    audit_ceremony(svc, agent_id, "make_credential", req ? req->rp_id : NULL,
                   NULL, 0, "deny", "bad_request", req ? req->user_verification : 0);
    set_error(err, SIGNET_FIDO_ERR_BAD_REQUEST, "invalid makeCredential inputs");
    return SIGNET_FIDO_ERR_BAD_REQUEST;
  }
  if (!has_es256(req)) {
    audit_ceremony(svc, agent_id, "make_credential", req->rp_id,
                   NULL, 0, "deny", "unsupported_algorithm", req->user_verification);
    set_error(err, SIGNET_FIDO_ERR_UNSUPPORTED_ALGORITHM, "ES256 (-7) required");
    return SIGNET_FIDO_ERR_UNSUPPORTED_ALGORITHM;
  }
  if (!validate_uv(svc, req->user_verification, err)) {
    audit_ceremony(svc, agent_id, "make_credential", req->rp_id,
                   NULL, 0, "deny", err ? err->reason : "uv_required", req->user_verification);
    return SIGNET_FIDO_ERR_UV_REQUIRED;
  }

  bool has_excluded = false;
  if (req->exclude_credential_count > 0) {
    if (signet_store_passkey_has_excluded(svc->store, agent_id, req->rp_id,
                                          (const uint8_t *const *)req->exclude_credential_ids,
                                          req->exclude_credential_id_lens,
                                          req->exclude_credential_count,
                                          &has_excluded) != 0) {
      set_error(err, SIGNET_FIDO_ERR_INTERNAL, "excludeCredentials lookup failed");
      return SIGNET_FIDO_ERR_INTERNAL;
    }
    if (has_excluded) {
      audit_ceremony(svc, agent_id, "make_credential", req->rp_id,
                     req->exclude_credential_ids[0], req->exclude_credential_id_lens[0],
                     "deny", "excluded_credential", req->user_verification);
      set_error(err, SIGNET_FIDO_ERR_EXCLUDED, "excluded credential already exists");
      return SIGNET_FIDO_ERR_EXCLUDED;
    }
  }

  SignetFidoStatus status = SIGNET_FIDO_ERR_INTERNAL;
  signet_fido_key *key = NULL;
  uint8_t *priv_der = NULL;
  size_t priv_der_len = 0;
  uint8_t *cose = NULL;
  size_t cose_len = 0;
  uint8_t *auth_data = NULL;
  size_t auth_data_len = 0;
  uint8_t *att_obj = NULL;
  size_t att_obj_len = 0;
  uint8_t cred_id[32];
  uint8_t x[32], y[32];

  randombytes_buf(cred_id, sizeof(cred_id));
  key = signet_fido_key_generate();
  if (!key ||
      signet_fido_key_public_xy(key, x, y) != 0 ||
      signet_cose_ec2_p256(x, y, &cose, &cose_len) != 0 ||
      signet_fido_key_export_private(key, &priv_der, &priv_der_len) != 0) {
    set_error(err, SIGNET_FIDO_ERR_INTERNAL, "key generation failed");
    goto cleanup;
  }

  uint8_t flags = SIGNET_FIDO_FLAG_UP | SIGNET_FIDO_FLAG_BE |
                  SIGNET_FIDO_FLAG_BS | SIGNET_FIDO_FLAG_AT;
  if (req->user_verification == SIGNET_FIDO_UV_REQUIRED && svc->allow_headless_uv)
    flags |= SIGNET_FIDO_FLAG_UV;

  if (signet_fido_auth_data(req->rp_id, flags, 0, svc->aaguid,
                            cred_id, sizeof(cred_id), cose, cose_len,
                            &auth_data, &auth_data_len) != 0 ||
      signet_fido_attestation_none(auth_data, auth_data_len, &att_obj, &att_obj_len) != 0) {
    set_error(err, SIGNET_FIDO_ERR_INTERNAL, "authenticator data encoding failed");
    goto cleanup;
  }

  SignetPasskeyCreate create = {
    .credential_id = cred_id,
    .credential_id_len = sizeof(cred_id),
    .agent_id = agent_id,
    .rp_id = req->rp_id,
    .user_handle = req->user_handle,
    .user_handle_len = req->user_handle_len,
    .aaguid = svc->aaguid,
    .discoverable = req->discoverable,
    .created_at = req->now > 0 ? req->now : fido_now(),
    .backend_id = svc->backend,
    .cose_alg = SIGNET_PASSKEY_COSE_ALG_ES256,
    .key_blob = priv_der,
    .key_blob_len = priv_der_len,
    .cose_public_key = cose,
    .cose_public_key_len = cose_len,
    .user_name = req->user_name,
    .user_display_name = req->user_display_name,
  };

  int rc = signet_store_passkey_create(svc->store, &create,
                                       svc->fleet_psk, sizeof(svc->fleet_psk));
  if (rc != 0) {
    set_error(err, rc == 1 ? SIGNET_FIDO_ERR_EXCLUDED : SIGNET_FIDO_ERR_INTERNAL,
              rc == 1 ? "credential id collision" : "credential persistence failed");
    goto cleanup;
  }

  out->credential_id = dup_bytes(cred_id, sizeof(cred_id));
  out->credential_id_len = sizeof(cred_id);
  out->auth_data = auth_data;
  out->auth_data_len = auth_data_len;
  out->attestation_object = att_obj;
  out->attestation_object_len = att_obj_len;
  out->cose_public_key = cose;
  out->cose_public_key_len = cose_len;
  out->sign_count = 0;
  if (!out->credential_id) {
    out->auth_data = NULL;
    out->attestation_object = NULL;
    out->cose_public_key = NULL;
    set_error(err, SIGNET_FIDO_ERR_INTERNAL, "out of memory");
    goto cleanup;
  }
  auth_data = NULL;
  att_obj = NULL;
  cose = NULL;
  status = SIGNET_FIDO_OK;
  audit_ceremony(svc, agent_id, "make_credential", req->rp_id,
                 out->credential_id, out->credential_id_len, "allow", "ok",
                 req->user_verification);

cleanup:
  if (status != SIGNET_FIDO_OK) {
    audit_ceremony(svc, agent_id, "make_credential", req ? req->rp_id : NULL,
                   NULL, 0, "error", err && err->reason ? err->reason : "internal",
                   req ? req->user_verification : 0);
  }
  if (priv_der) {
    sodium_memzero(priv_der, priv_der_len);
    free(priv_der);
  }
  free(cose);
  free(auth_data);
  free(att_obj);
  signet_fido_key_free(key);
  sodium_memzero(cred_id, sizeof(cred_id));
  sodium_memzero(x, sizeof(x));
  sodium_memzero(y, sizeof(y));
  return status;
}

static int select_allowed_credential(SignetFidoService *svc,
                                     const char *agent_id,
                                     const char *rp_id,
                                     const SignetFidoGetAssertionRequest *req,
                                     SignetPasskeyCredential *out) {
  memset(out, 0, sizeof(*out));
  if (req->allow_credential_count > 0) {
    for (size_t i = 0; i < req->allow_credential_count; i++) {
      SignetPasskeyCredential rec;
      int rc = signet_store_passkey_find_by_credential_id(svc->store,
          req->allow_credential_ids[i], req->allow_credential_id_lens[i],
          svc->fleet_psk, sizeof(svc->fleet_psk), &rec);
      if (rc == 0) {
        if (rec.agent_id && strcmp(rec.agent_id, agent_id) == 0 &&
            rec.rp_id && strcmp(rec.rp_id, rp_id) == 0) {
          *out = rec;
          return 0;
        }
        signet_passkey_credential_clear(&rec);
      } else if (rc < 0) {
        return -1;
      }
    }
    return 1;
  }

  SignetPasskeyCredential *records = NULL;
  size_t count = 0;
  int rc = signet_store_passkey_find_by_agent_rp(svc->store, agent_id, rp_id,
      svc->fleet_psk, sizeof(svc->fleet_psk), &records, &count);
  if (rc != 0) return -1;
  if (count == 0) return 1;
  *out = records[0];
  for (size_t i = 1; i < count; i++) signet_passkey_credential_clear(&records[i]);
  free(records);
  return 0;
}

SignetFidoStatus signet_fido_get_assertion(SignetFidoService *svc,
                                           const char *agent_id,
                                           const SignetFidoGetAssertionRequest *req,
                                           SignetFidoGetAssertionResult *out,
                                           SignetFidoError *err) {
  if (out) memset(out, 0, sizeof(*out));
  signet_fido_error_clear(err);
  if (!service_ready(svc)) return not_configured(err);
  if (!agent_id || !agent_id[0] || !req || !out ||
      !req->rp_id || !req->rp_id[0] ||
      !req->client_data_hash || req->client_data_hash_len != SIGNET_FIDO_CLIENT_DATA_HASH_LEN) {
    set_error(err, SIGNET_FIDO_ERR_BAD_REQUEST, "invalid getAssertion inputs");
    return SIGNET_FIDO_ERR_BAD_REQUEST;
  }
  if (!validate_uv(svc, req->user_verification, err)) {
    audit_ceremony(svc, agent_id, "get_assertion", req->rp_id,
                   NULL, 0, "deny", err ? err->reason : "uv_required", req->user_verification);
    return SIGNET_FIDO_ERR_UV_REQUIRED;
  }

  SignetPasskeyCredential rec;
  int sel = select_allowed_credential(svc, agent_id, req->rp_id, req, &rec);
  if (sel != 0) {
    set_error(err, sel > 0 ? SIGNET_FIDO_ERR_NOT_FOUND : SIGNET_FIDO_ERR_INTERNAL,
              sel > 0 ? "credential not found" : "credential lookup failed");
    audit_ceremony(svc, agent_id, "get_assertion", req->rp_id,
                   NULL, 0, sel > 0 ? "deny" : "error", err->reason,
                   req->user_verification);
    return sel > 0 ? SIGNET_FIDO_ERR_NOT_FOUND : SIGNET_FIDO_ERR_INTERNAL;
  }

  SignetFidoStatus status = SIGNET_FIDO_ERR_INTERNAL;
  uint8_t *auth_data = NULL;
  size_t auth_data_len = 0;
  uint8_t *to_sign = NULL;
  uint8_t *sig = NULL;
  size_t sig_len = 0;
  signet_fido_key *key = NULL;

  uint8_t flags = SIGNET_FIDO_FLAG_UP | SIGNET_FIDO_FLAG_BE | SIGNET_FIDO_FLAG_BS;
  if (req->user_verification == SIGNET_FIDO_UV_REQUIRED && svc->allow_headless_uv)
    flags |= SIGNET_FIDO_FLAG_UV;

  if (signet_fido_auth_data(req->rp_id, flags, 0, NULL,
                            NULL, 0, NULL, 0, &auth_data, &auth_data_len) != 0) {
    set_error(err, SIGNET_FIDO_ERR_INTERNAL, "authenticator data encoding failed");
    goto cleanup;
  }

  if (auth_data_len > SIZE_MAX - SIGNET_FIDO_CLIENT_DATA_HASH_LEN) {
    set_error(err, SIGNET_FIDO_ERR_INTERNAL, "assertion message too large");
    goto cleanup;
  }
  to_sign = (uint8_t *)malloc(auth_data_len + SIGNET_FIDO_CLIENT_DATA_HASH_LEN);
  if (!to_sign) {
    set_error(err, SIGNET_FIDO_ERR_INTERNAL, "out of memory");
    goto cleanup;
  }
  memcpy(to_sign, auth_data, auth_data_len);
  memcpy(to_sign + auth_data_len, req->client_data_hash, SIGNET_FIDO_CLIENT_DATA_HASH_LEN);

  key = signet_fido_key_import_private(rec.key_blob, rec.key_blob_len);
  if (!key || signet_fido_key_sign(key, to_sign,
                                   auth_data_len + SIGNET_FIDO_CLIENT_DATA_HASH_LEN,
                                   &sig, &sig_len) != 0) {
    set_error(err, SIGNET_FIDO_ERR_INTERNAL, "assertion signature failed");
    goto cleanup;
  }

  out->credential_id = dup_bytes(rec.credential_id, rec.credential_id_len);
  out->credential_id_len = rec.credential_id_len;
  out->auth_data = auth_data;
  out->auth_data_len = auth_data_len;
  out->signature_der = sig;
  out->signature_der_len = sig_len;
  out->user_handle = dup_bytes(rec.user_handle, rec.user_handle_len);
  out->user_handle_len = rec.user_handle_len;
  out->sign_count = 0;
  if (!out->credential_id || !out->user_handle) {
    out->auth_data = NULL;
    out->signature_der = NULL;
    set_error(err, SIGNET_FIDO_ERR_INTERNAL, "out of memory");
    goto cleanup;
  }
  auth_data = NULL;
  sig = NULL;
  status = SIGNET_FIDO_OK;
  audit_ceremony(svc, agent_id, "get_assertion", req->rp_id,
                 out->credential_id, out->credential_id_len, "allow", "ok",
                 req->user_verification);

cleanup:
  if (status != SIGNET_FIDO_OK) {
    audit_ceremony(svc, agent_id, "get_assertion", req ? req->rp_id : NULL,
                   rec.credential_id, rec.credential_id_len, "error",
                   err && err->reason ? err->reason : "internal",
                   req ? req->user_verification : 0);
  }
  signet_fido_key_free(key);
  free(auth_data);
  free(sig);
  free(to_sign);
  signet_passkey_credential_clear(&rec);
  return status;
}

void signet_fido_make_credential_result_clear(SignetFidoMakeCredentialResult *r) {
  if (!r) return;
  free(r->credential_id);
  free(r->auth_data);
  free(r->attestation_object);
  free(r->cose_public_key);
  memset(r, 0, sizeof(*r));
}

void signet_fido_get_assertion_result_clear(SignetFidoGetAssertionResult *r) {
  if (!r) return;
  free(r->credential_id);
  free(r->auth_data);
  free(r->signature_der);
  free(r->user_handle);
  memset(r, 0, sizeof(*r));
}

static JsonNode *parse_json_root(const char *json) {
  if (!json) return NULL;
  g_autoptr(JsonParser) p = json_parser_new();
  if (!json_parser_load_from_data(p, json, -1, NULL)) return NULL;
  JsonNode *root = json_parser_get_root(p);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) return NULL;
  return json_node_copy(root);
}

static const char *obj_string(JsonObject *obj, const char *name) {
  if (!json_object_has_member(obj, name)) return NULL;
  JsonNode *n = json_object_get_member(obj, name);
  return JSON_NODE_HOLDS_VALUE(n) ? json_object_get_string_member(obj, name) : NULL;
}

static bool obj_bool_default(JsonObject *obj, const char *name, bool defv) {
  if (!json_object_has_member(obj, name)) return defv;
  return json_object_get_boolean_member(obj, name);
}

static uint8_t *b64_decode_member(JsonObject *obj, const char *name, size_t *out_len) {
  if (out_len) *out_len = 0;
  const char *s = obj_string(obj, name);
  if (!s) return NULL;
  gsize len = 0;
  guchar *raw = g_base64_decode(s, &len);
  if (!raw) return NULL;
  uint8_t *out = dup_bytes(raw, (size_t)len);
  g_free(raw);
  if (out_len) *out_len = out ? (size_t)len : 0;
  return out;
}

static SignetFidoUserVerification parse_uv(const char *s) {
  if (!s) return SIGNET_FIDO_UV_PREFERRED;
  if (strcmp(s, "required") == 0) return SIGNET_FIDO_UV_REQUIRED;
  if (strcmp(s, "discouraged") == 0) return SIGNET_FIDO_UV_DISCOURAGED;
  return SIGNET_FIDO_UV_PREFERRED;
}

static int *parse_algorithms(JsonObject *obj, size_t *out_count) {
  *out_count = 0;
  if (!json_object_has_member(obj, "pubKeyCredParams")) return NULL;
  JsonNode *n = json_object_get_member(obj, "pubKeyCredParams");
  if (!JSON_NODE_HOLDS_ARRAY(n)) return NULL;
  JsonArray *arr = json_node_get_array(n);
  guint len = json_array_get_length(arr);
  int *algs = (int *)calloc(len ? len : 1, sizeof(int));
  if (!algs) return NULL;
  for (guint i = 0; i < len; i++) {
    JsonNode *e = json_array_get_element(arr, i);
    if (JSON_NODE_HOLDS_VALUE(e)) algs[*out_count] = (int)json_node_get_int(e);
    else if (JSON_NODE_HOLDS_OBJECT(e)) {
      JsonObject *eo = json_node_get_object(e);
      if (json_object_has_member(eo, "alg")) algs[*out_count] = (int)json_object_get_int_member(eo, "alg");
      else continue;
    } else continue;
    (*out_count)++;
  }
  return algs;
}

static BlobList parse_blob_array(JsonObject *obj, const char *name) {
  BlobList list;
  memset(&list, 0, sizeof(list));
  if (!json_object_has_member(obj, name)) return list;
  JsonNode *n = json_object_get_member(obj, name);
  if (!JSON_NODE_HOLDS_ARRAY(n)) return list;
  JsonArray *arr = json_node_get_array(n);
  guint len = json_array_get_length(arr);
  list.items = (uint8_t **)calloc(len ? len : 1, sizeof(uint8_t *));
  list.lens = (size_t *)calloc(len ? len : 1, sizeof(size_t));
  if (!list.items || !list.lens) return list;
  for (guint i = 0; i < len; i++) {
    JsonNode *e = json_array_get_element(arr, i);
    const char *b64 = NULL;
    if (JSON_NODE_HOLDS_VALUE(e)) b64 = json_node_get_string(e);
    else if (JSON_NODE_HOLDS_OBJECT(e)) {
      JsonObject *eo = json_node_get_object(e);
      b64 = obj_string(eo, "id");
      if (!b64) b64 = obj_string(eo, "credentialId");
    }
    if (b64) {
      gsize raw_len = 0;
      guchar *raw = g_base64_decode(b64, &raw_len);
      if (raw && raw_len > 0) {
        list.items[list.count] = dup_bytes(raw, (size_t)raw_len);
        list.lens[list.count] = (size_t)raw_len;
        if (list.items[list.count]) list.count++;
      }
      g_free(raw);
    }
  }
  return list;
}

static void blob_list_clear(BlobList *list) {
  if (!list) return;
  for (size_t i = 0; i < list->count; i++) free(list->items[i]);
  free(list->items);
  free(list->lens);
  memset(list, 0, sizeof(*list));
}

static char *b64(const uint8_t *p, size_t n) {
  return (char *)g_base64_encode(p, n);
}

SignetFidoStatus signet_fido_get_info_json(SignetFidoService *svc,
                                           const char *agent_id,
                                           char **out_json,
                                           SignetFidoError *err) {
  (void)agent_id;
  signet_fido_error_clear(err);
  if (out_json) *out_json = NULL;
  if (!out_json) {
    set_error(err, SIGNET_FIDO_ERR_BAD_REQUEST, "out_json is required");
    return SIGNET_FIDO_ERR_BAD_REQUEST;
  }
  if (!service_ready(svc)) return not_configured(err);

  char aaguid[37];
  signet_fido_format_aaguid(svc->aaguid, aaguid);
  char *backend = json_escape(svc->backend);
  char *att = json_escape(svc->attestation);
  *out_json = g_strdup_printf("{\"versions\":[\"FIDO_2_0\"],\"aaguid\":\"%s\",\"options\":{\"rk\":true,\"up\":true,\"uv\":%s,\"credMgmt\":false},\"algorithms\":[-7],\"backend\":%s,\"attestation\":%s,\"maxCredentialCountInList\":64}",
                              aaguid, svc->allow_headless_uv ? "true" : "false",
                              backend, att);
  g_free(backend);
  g_free(att);
  if (!*out_json) {
    set_error(err, SIGNET_FIDO_ERR_INTERNAL, "out of memory");
    return SIGNET_FIDO_ERR_INTERNAL;
  }
  audit_ceremony(svc, agent_id, "get_info", NULL, NULL, 0, "allow", "ok",
                 SIGNET_FIDO_UV_PREFERRED);
  return SIGNET_FIDO_OK;
}

SignetFidoStatus signet_fido_make_credential_json(SignetFidoService *svc,
                                                  const char *agent_id,
                                                  const char *request_json,
                                                  char **out_json,
                                                  SignetFidoError *err) {
  if (out_json) *out_json = NULL;
  signet_fido_error_clear(err);
  if (!out_json) {
    set_error(err, SIGNET_FIDO_ERR_BAD_REQUEST, "out_json is required");
    return SIGNET_FIDO_ERR_BAD_REQUEST;
  }
  JsonNode *root = parse_json_root(request_json);
  if (!root) {
    set_error(err, SIGNET_FIDO_ERR_BAD_REQUEST, "request must be a JSON object");
    return SIGNET_FIDO_ERR_BAD_REQUEST;
  }
  JsonObject *obj = json_node_get_object(root);
  size_t cdh_len = 0, uh_len = 0;
  uint8_t *cdh = b64_decode_member(obj, "clientDataHash", &cdh_len);
  uint8_t *uh = b64_decode_member(obj, "userHandle", &uh_len);
  size_t alg_count = 0;
  int *algs = parse_algorithms(obj, &alg_count);
  BlobList exclude = parse_blob_array(obj, "excludeCredentials");

  SignetFidoMakeCredentialRequest req = {
    .rp_id = obj_string(obj, "rpId"),
    .client_data_hash = cdh,
    .client_data_hash_len = cdh_len,
    .user_handle = uh,
    .user_handle_len = uh_len,
    .user_name = obj_string(obj, "userName"),
    .user_display_name = obj_string(obj, "userDisplayName"),
    .discoverable = obj_bool_default(obj, "discoverable", true),
    .user_verification = parse_uv(obj_string(obj, "userVerification")),
    .pub_key_cred_params = algs,
    .pub_key_cred_param_count = alg_count,
    .exclude_credential_ids = (const uint8_t *const *)exclude.items,
    .exclude_credential_id_lens = exclude.lens,
    .exclude_credential_count = exclude.count,
    .now = fido_now(),
  };

  SignetFidoMakeCredentialResult res;
  SignetFidoStatus st = signet_fido_make_credential(svc, agent_id, &req, &res, err);
  if (st == SIGNET_FIDO_OK) {
    char *cred = b64(res.credential_id, res.credential_id_len);
    char *auth = b64(res.auth_data, res.auth_data_len);
    char *att = b64(res.attestation_object, res.attestation_object_len);
    char *cose = b64(res.cose_public_key, res.cose_public_key_len);
    *out_json = g_strdup_printf("{\"credentialId\":\"%s\",\"authData\":\"%s\",\"attestationObject\":\"%s\",\"publicKeyCose\":\"%s\",\"fmt\":\"none\",\"signCount\":0}",
                                cred ? cred : "", auth ? auth : "", att ? att : "", cose ? cose : "");
    g_free(cred); g_free(auth); g_free(att); g_free(cose);
    signet_fido_make_credential_result_clear(&res);
    if (!*out_json) {
      set_error(err, SIGNET_FIDO_ERR_INTERNAL, "out of memory");
      st = SIGNET_FIDO_ERR_INTERNAL;
    }
  }

  free(cdh);
  free(uh);
  free(algs);
  blob_list_clear(&exclude);
  json_node_unref(root);
  return st;
}

SignetFidoStatus signet_fido_get_assertion_json(SignetFidoService *svc,
                                                const char *agent_id,
                                                const char *request_json,
                                                char **out_json,
                                                SignetFidoError *err) {
  if (out_json) *out_json = NULL;
  signet_fido_error_clear(err);
  if (!out_json) {
    set_error(err, SIGNET_FIDO_ERR_BAD_REQUEST, "out_json is required");
    return SIGNET_FIDO_ERR_BAD_REQUEST;
  }
  JsonNode *root = parse_json_root(request_json);
  if (!root) {
    set_error(err, SIGNET_FIDO_ERR_BAD_REQUEST, "request must be a JSON object");
    return SIGNET_FIDO_ERR_BAD_REQUEST;
  }
  JsonObject *obj = json_node_get_object(root);
  size_t cdh_len = 0;
  uint8_t *cdh = b64_decode_member(obj, "clientDataHash", &cdh_len);
  BlobList allow = parse_blob_array(obj, "allowCredentials");

  SignetFidoGetAssertionRequest req = {
    .rp_id = obj_string(obj, "rpId"),
    .client_data_hash = cdh,
    .client_data_hash_len = cdh_len,
    .user_verification = parse_uv(obj_string(obj, "userVerification")),
    .allow_credential_ids = (const uint8_t *const *)allow.items,
    .allow_credential_id_lens = allow.lens,
    .allow_credential_count = allow.count,
  };

  SignetFidoGetAssertionResult res;
  SignetFidoStatus st = signet_fido_get_assertion(svc, agent_id, &req, &res, err);
  if (st == SIGNET_FIDO_OK) {
    char *cred = b64(res.credential_id, res.credential_id_len);
    char *auth = b64(res.auth_data, res.auth_data_len);
    char *sig = b64(res.signature_der, res.signature_der_len);
    char *uh = b64(res.user_handle, res.user_handle_len);
    *out_json = g_strdup_printf("{\"credentialId\":\"%s\",\"authData\":\"%s\",\"signature\":\"%s\",\"userHandle\":\"%s\",\"signCount\":0}",
                                cred ? cred : "", auth ? auth : "", sig ? sig : "", uh ? uh : "");
    g_free(cred); g_free(auth); g_free(sig); g_free(uh);
    signet_fido_get_assertion_result_clear(&res);
    if (!*out_json) {
      set_error(err, SIGNET_FIDO_ERR_INTERNAL, "out of memory");
      st = SIGNET_FIDO_ERR_INTERNAL;
    }
  }

  free(cdh);
  blob_list_clear(&allow);
  json_node_unref(root);
  return st;
}

SignetFidoStatus signet_fido_export_credential_json(SignetFidoService *svc,
                                                    const char *agent_id,
                                                    const char *request_json,
                                                    char **out_json,
                                                    SignetFidoError *err) {
  if (out_json) *out_json = NULL;
  signet_fido_error_clear(err);
  if (!out_json) {
    set_error(err, SIGNET_FIDO_ERR_BAD_REQUEST, "out_json is required");
    return SIGNET_FIDO_ERR_BAD_REQUEST;
  }
  if (!service_ready(svc)) return not_configured(err);

  JsonNode *root = parse_json_root(request_json);
  if (!root) {
    set_error(err, SIGNET_FIDO_ERR_BAD_REQUEST, "request must be a JSON object");
    return SIGNET_FIDO_ERR_BAD_REQUEST;
  }
  JsonObject *obj = json_node_get_object(root);
  size_t cred_len = 0;
  uint8_t *cred = b64_decode_member(obj, "credentialId", &cred_len);
  if (!cred || cred_len == 0) {
    free(cred);
    json_node_unref(root);
    audit_ceremony(svc, agent_id, "export_credential", NULL, NULL, 0,
                   "deny", "bad_request", SIGNET_FIDO_UV_DISCOURAGED);
    set_error(err, SIGNET_FIDO_ERR_BAD_REQUEST, "credentialId is required");
    return SIGNET_FIDO_ERR_BAD_REQUEST;
  }

  SignetPasskeyCredential rec;
  int rc = signet_store_passkey_find_by_credential_id(svc->store, cred, cred_len,
      svc->fleet_psk, sizeof(svc->fleet_psk), &rec);
  bool owned = (rc == 0 && rec.agent_id && strcmp(rec.agent_id, agent_id) == 0);
  if (rc != 0 || !owned) {
    audit_ceremony(svc, agent_id, "export_credential",
                   rc == 0 ? rec.rp_id : NULL, cred, cred_len,
                   rc == 0 || rc == 1 ? "deny" : "error",
                   rc == 0 || rc == 1 ? "credential_not_found" : "credential_lookup_failed",
                   SIGNET_FIDO_UV_DISCOURAGED);
    if (rc == 0) signet_passkey_credential_clear(&rec);
    free(cred);
    json_node_unref(root);
    set_error(err, rc == 0 || rc == 1 ? SIGNET_FIDO_ERR_NOT_FOUND : SIGNET_FIDO_ERR_INTERNAL,
              rc == 0 || rc == 1 ? "credential not found" : "credential lookup failed");
    return rc == 0 || rc == 1 ? SIGNET_FIDO_ERR_NOT_FOUND : SIGNET_FIDO_ERR_INTERNAL;
  }

  uint8_t *container = NULL;
  size_t container_len = 0;
  rc = signet_store_passkey_export_container(svc->store, agent_id, cred, cred_len,
      svc->fleet_psk, sizeof(svc->fleet_psk), &container, &container_len);
  if (rc != 0) {
    audit_ceremony(svc, agent_id, "export_credential", rec.rp_id, cred, cred_len,
                   rc == 1 ? "deny" : "error",
                   rc == 1 ? "credential_not_found" : "export_failed",
                   SIGNET_FIDO_UV_DISCOURAGED);
    signet_passkey_credential_clear(&rec);
    free(cred);
    json_node_unref(root);
    set_error(err, rc == 1 ? SIGNET_FIDO_ERR_NOT_FOUND : SIGNET_FIDO_ERR_INTERNAL,
              rc == 1 ? "credential not found" : "credential export failed");
    return rc == 1 ? SIGNET_FIDO_ERR_NOT_FOUND : SIGNET_FIDO_ERR_INTERNAL;
  }

  char *container_b64 = b64(container, container_len);
  char *cred_b64 = b64(rec.credential_id, rec.credential_id_len);
  *out_json = g_strdup_printf("{\"format\":\"signet-passkey-export\",\"formatVersion\":%d,\"credentialId\":\"%s\",\"container\":\"%s\"}",
                              SIGNET_PASSKEY_EXPORT_FORMAT_VERSION,
                              cred_b64 ? cred_b64 : "",
                              container_b64 ? container_b64 : "");
  g_free(container_b64);
  g_free(cred_b64);
  signet_passkey_export_container_free(container);
  if (!*out_json) {
    audit_ceremony(svc, agent_id, "export_credential", rec.rp_id, cred, cred_len,
                   "error", "out_of_memory", SIGNET_FIDO_UV_DISCOURAGED);
    signet_passkey_credential_clear(&rec);
    free(cred);
    json_node_unref(root);
    set_error(err, SIGNET_FIDO_ERR_INTERNAL, "out of memory");
    return SIGNET_FIDO_ERR_INTERNAL;
  }

  audit_ceremony(svc, agent_id, "export_credential", rec.rp_id,
                 rec.credential_id, rec.credential_id_len, "allow", "ok",
                 SIGNET_FIDO_UV_DISCOURAGED);
  signet_passkey_credential_clear(&rec);
  free(cred);
  json_node_unref(root);
  return SIGNET_FIDO_OK;
}

SignetFidoStatus signet_fido_import_credential_json(SignetFidoService *svc,
                                                    const char *agent_id,
                                                    const char *request_json,
                                                    char **out_json,
                                                    SignetFidoError *err) {
  if (out_json) *out_json = NULL;
  signet_fido_error_clear(err);
  if (!out_json) {
    set_error(err, SIGNET_FIDO_ERR_BAD_REQUEST, "out_json is required");
    return SIGNET_FIDO_ERR_BAD_REQUEST;
  }
  if (!service_ready(svc)) return not_configured(err);

  JsonNode *root = parse_json_root(request_json);
  if (!root) {
    set_error(err, SIGNET_FIDO_ERR_BAD_REQUEST, "request must be a JSON object");
    return SIGNET_FIDO_ERR_BAD_REQUEST;
  }
  JsonObject *obj = json_node_get_object(root);
  size_t container_len = 0;
  uint8_t *container = b64_decode_member(obj, "container", &container_len);
  if (!container || container_len == 0) {
    free(container);
    json_node_unref(root);
    audit_ceremony(svc, agent_id, "import_credential", NULL, NULL, 0,
                   "deny", "bad_request", SIGNET_FIDO_UV_DISCOURAGED);
    set_error(err, SIGNET_FIDO_ERR_BAD_REQUEST, "container is required");
    return SIGNET_FIDO_ERR_BAD_REQUEST;
  }

  SignetPasskeyCredential rec;
  int rc = signet_store_passkey_import_container(svc->store, agent_id,
      container, container_len, svc->fleet_psk, sizeof(svc->fleet_psk),
      fido_now(), &rec);
  if (rc != 0) {
    audit_ceremony(svc, agent_id, "import_credential", NULL, NULL, 0,
                   rc == 1 ? "deny" : "error",
                   rc == 1 ? "credential_already_exists" : "invalid_container_or_wrong_psk",
                   SIGNET_FIDO_UV_DISCOURAGED);
    free(container);
    json_node_unref(root);
    set_error(err, rc == 1 ? SIGNET_FIDO_ERR_EXCLUDED : SIGNET_FIDO_ERR_BAD_REQUEST,
              rc == 1 ? "credential already exists" :
                         "container is invalid or does not decrypt under this fleet PSK");
    return rc == 1 ? SIGNET_FIDO_ERR_EXCLUDED : SIGNET_FIDO_ERR_BAD_REQUEST;
  }

  char aaguid[37];
  signet_fido_format_aaguid(rec.aaguid, aaguid);
  char *cred_b64 = b64(rec.credential_id, rec.credential_id_len);
  char *uh_b64 = b64(rec.user_handle, rec.user_handle_len);
  char *rp_json = json_escape(rec.rp_id);
  *out_json = g_strdup_printf("{\"credentialId\":\"%s\",\"rpId\":%s,\"userHandle\":\"%s\",\"discoverable\":%s,\"aaguid\":\"%s\",\"alg\":-7,\"formatVersion\":%d}",
                              cred_b64 ? cred_b64 : "",
                              rp_json ? rp_json : "\"\"",
                              uh_b64 ? uh_b64 : "",
                              rec.discoverable ? "true" : "false",
                              aaguid,
                              SIGNET_PASSKEY_EXPORT_FORMAT_VERSION);
  g_free(cred_b64);
  g_free(uh_b64);
  g_free(rp_json);
  if (!*out_json) {
    audit_ceremony(svc, agent_id, "import_credential", rec.rp_id,
                   rec.credential_id, rec.credential_id_len, "error",
                   "out_of_memory", SIGNET_FIDO_UV_DISCOURAGED);
    signet_passkey_credential_clear(&rec);
    free(container);
    json_node_unref(root);
    set_error(err, SIGNET_FIDO_ERR_INTERNAL, "out of memory");
    return SIGNET_FIDO_ERR_INTERNAL;
  }

  audit_ceremony(svc, agent_id, "import_credential", rec.rp_id,
                 rec.credential_id, rec.credential_id_len, "allow", "ok",
                 SIGNET_FIDO_UV_DISCOURAGED);
  signet_passkey_credential_clear(&rec);
  free(container);
  json_node_unref(root);
  return SIGNET_FIDO_OK;
}
