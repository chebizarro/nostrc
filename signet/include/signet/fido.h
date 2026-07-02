/* SPDX-License-Identifier: MIT */
/*
 * fido.h - Signet software WebAuthn/FIDO2 authenticator service.
 */
#ifndef SIGNET_FIDO_H
#define SIGNET_FIDO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "signet/store_passkeys.h"

#ifdef __cplusplus
extern "C" {
#endif

struct SignetAuditLogger;
struct SignetStore;

typedef struct SignetFidoService SignetFidoService;

#define SIGNET_FIDO_AAGUID_LEN 16
#define SIGNET_FIDO_CLIENT_DATA_HASH_LEN 32
#define SIGNET_FIDO_DEFAULT_AAGUID "80c64041-9927-4901-957f-e0032db96bee"

typedef enum {
  SIGNET_FIDO_OK = 0,
  SIGNET_FIDO_ERR_NOT_CONFIGURED,
  SIGNET_FIDO_ERR_BAD_REQUEST,
  SIGNET_FIDO_ERR_UNSUPPORTED_ALGORITHM,
  SIGNET_FIDO_ERR_EXCLUDED,
  SIGNET_FIDO_ERR_NOT_FOUND,
  SIGNET_FIDO_ERR_UV_REQUIRED,
  SIGNET_FIDO_ERR_INTERNAL
} SignetFidoStatus;

typedef struct {
  SignetFidoStatus status;
  const char *reason;
} SignetFidoError;

typedef enum {
  SIGNET_FIDO_UV_DISCOURAGED = 0,
  SIGNET_FIDO_UV_PREFERRED,
  SIGNET_FIDO_UV_REQUIRED
} SignetFidoUserVerification;

typedef struct {
  bool enabled;
  struct SignetStore *store;
  struct SignetAuditLogger *audit;
  const uint8_t *fleet_psk;
  size_t fleet_psk_len;
  uint8_t aaguid[SIGNET_FIDO_AAGUID_LEN];
  const char *backend;
  const char *attestation;
  bool allow_headless_uv;
} SignetFidoServiceConfig;

typedef struct {
  const char *rp_id;
  const uint8_t *client_data_hash;
  size_t client_data_hash_len;
  const uint8_t *user_handle;
  size_t user_handle_len;
  const char *user_name;
  const char *user_display_name;
  bool discoverable;
  SignetFidoUserVerification user_verification;
  const int *pub_key_cred_params;
  size_t pub_key_cred_param_count;
  const uint8_t *const *exclude_credential_ids;
  const size_t *exclude_credential_id_lens;
  size_t exclude_credential_count;
  int64_t now;
} SignetFidoMakeCredentialRequest;

typedef struct {
  uint8_t *credential_id;
  size_t credential_id_len;
  uint8_t *auth_data;
  size_t auth_data_len;
  uint8_t *attestation_object;
  size_t attestation_object_len;
  uint8_t *cose_public_key;
  size_t cose_public_key_len;
  uint32_t sign_count;
} SignetFidoMakeCredentialResult;

typedef struct {
  const char *rp_id;
  const uint8_t *client_data_hash;
  size_t client_data_hash_len;
  SignetFidoUserVerification user_verification;
  const uint8_t *const *allow_credential_ids;
  const size_t *allow_credential_id_lens;
  size_t allow_credential_count;
} SignetFidoGetAssertionRequest;

typedef struct {
  uint8_t *credential_id;
  size_t credential_id_len;
  uint8_t *auth_data;
  size_t auth_data_len;
  uint8_t *signature_der;
  size_t signature_der_len;
  uint8_t *user_handle;
  size_t user_handle_len;
  uint32_t sign_count;
} SignetFidoGetAssertionResult;

SignetFidoService *signet_fido_service_new(const SignetFidoServiceConfig *cfg);
void signet_fido_service_free(SignetFidoService *svc);

bool signet_fido_service_is_enabled(const SignetFidoService *svc);
const char *signet_fido_status_string(SignetFidoStatus status);
void signet_fido_error_clear(SignetFidoError *err);

SignetFidoStatus signet_fido_get_info_json(SignetFidoService *svc,
                                           const char *agent_id,
                                           char **out_json,
                                           SignetFidoError *err);

SignetFidoStatus signet_fido_make_credential(SignetFidoService *svc,
                                             const char *agent_id,
                                             const SignetFidoMakeCredentialRequest *req,
                                             SignetFidoMakeCredentialResult *out,
                                             SignetFidoError *err);

SignetFidoStatus signet_fido_get_assertion(SignetFidoService *svc,
                                           const char *agent_id,
                                           const SignetFidoGetAssertionRequest *req,
                                           SignetFidoGetAssertionResult *out,
                                           SignetFidoError *err);

SignetFidoStatus signet_fido_make_credential_json(SignetFidoService *svc,
                                                  const char *agent_id,
                                                  const char *request_json,
                                                  char **out_json,
                                                  SignetFidoError *err);

SignetFidoStatus signet_fido_get_assertion_json(SignetFidoService *svc,
                                                const char *agent_id,
                                                const char *request_json,
                                                char **out_json,
                                                SignetFidoError *err);

SignetFidoStatus signet_fido_export_credential_json(SignetFidoService *svc,
                                                    const char *agent_id,
                                                    const char *request_json,
                                                    char **out_json,
                                                    SignetFidoError *err);

SignetFidoStatus signet_fido_import_credential_json(SignetFidoService *svc,
                                                    const char *agent_id,
                                                    const char *request_json,
                                                    char **out_json,
                                                    SignetFidoError *err);

void signet_fido_make_credential_result_clear(SignetFidoMakeCredentialResult *r);
void signet_fido_get_assertion_result_clear(SignetFidoGetAssertionResult *r);

int signet_fido_parse_aaguid(const char *uuid, uint8_t out[SIGNET_FIDO_AAGUID_LEN]);
void signet_fido_format_aaguid(const uint8_t aaguid[SIGNET_FIDO_AAGUID_LEN],
                               char out[37]);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_FIDO_H */
