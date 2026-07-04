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

/**
 * SignetFidoService:
 * Opaque software FIDO2/WebAuthn authenticator service.
 *
 * Since: 1.0
 */
typedef struct SignetFidoService SignetFidoService;

/**
 * SIGNET_FIDO_AAGUID_LEN:
 *
 * Length in bytes of a FIDO authenticator AAGUID.
 *
 * Since: 1.0
 */
#define SIGNET_FIDO_AAGUID_LEN 16
/**
 * SIGNET_FIDO_CLIENT_DATA_HASH_LEN:
 *
 * Length in bytes of a WebAuthn clientDataHash.
 *
 * Since: 1.0
 */
#define SIGNET_FIDO_CLIENT_DATA_HASH_LEN 32
/**
 * SIGNET_FIDO_DEFAULT_AAGUID:
 *
 * Default software-authenticator AAGUID string.
 *
 * Since: 1.0
 */
#define SIGNET_FIDO_DEFAULT_AAGUID "80c64041-9927-4901-957f-e0032db96bee"

/**
 * SignetFidoStatus:
 * @SIGNET_FIDO_OK: signet fido ok
 * @SIGNET_FIDO_ERR_NOT_CONFIGURED: signet fido err not configured
 * @SIGNET_FIDO_ERR_BAD_REQUEST: signet fido err bad request
 * @SIGNET_FIDO_ERR_UNSUPPORTED_ALGORITHM: signet fido err unsupported algorithm
 * @SIGNET_FIDO_ERR_EXCLUDED: signet fido err excluded
 * @SIGNET_FIDO_ERR_NOT_FOUND: signet fido err not found
 * @SIGNET_FIDO_ERR_UV_REQUIRED: signet fido err uv required
 * @SIGNET_FIDO_ERR_INTERNAL: signet fido err internal
 *
 * Status codes returned by FIDO2/WebAuthn operations.
 *
 * Since: 1.0
 */
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

/**
 * SignetFidoError:
 * @status: status value.
 * @reason: reason value.
 *
 * Optional FIDO status detail returned to callers.
 *
 * Since: 1.0
 */
typedef struct {
  SignetFidoStatus status;
  const char *reason;
} SignetFidoError;

/**
 * SignetFidoUserVerification:
 * @SIGNET_FIDO_UV_DISCOURAGED: signet fido uv discouraged
 * @SIGNET_FIDO_UV_PREFERRED: signet fido uv preferred
 * @SIGNET_FIDO_UV_REQUIRED: signet fido uv required
 *
 * User-verification policy requested by a FIDO ceremony.
 *
 * Since: 1.0
 */
typedef enum {
  SIGNET_FIDO_UV_DISCOURAGED = 0,
  SIGNET_FIDO_UV_PREFERRED,
  SIGNET_FIDO_UV_REQUIRED
} SignetFidoUserVerification;

/**
 * SignetFidoServiceConfig:
 * @enabled: whether the service is enabled.
 * @store: borrowed store dependency.
 * @audit: borrowed audit logger dependency.
 * @fleet_psk: fleet psk value.
 * @fleet_psk_len: fleet psk len value.
 * @aaguid: authenticator AAGUID bytes.
 * @backend: backend name.
 * @attestation: attestation mode string.
 * @allow_headless_uv: allow headless uv value.
 *
 * Configuration for the FIDO2/WebAuthn service.
 *
 * Since: 1.0
 */
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

/**
 * SignetFidoMakeCredentialRequest:
 * @rp_id: WebAuthn relying-party identifier.
 * @client_data_hash: WebAuthn clientDataHash bytes.
 * @client_data_hash_len: length of @client_data_hash in bytes.
 * @user_handle: WebAuthn user handle bytes.
 * @user_handle_len: length of @user_handle in bytes.
 * @user_name: optional user name.
 * @user_display_name: optional display name.
 * @discoverable: whether the credential is discoverable.
 * @user_verification: user verification value.
 * @pub_key_cred_params: pub key cred params value.
 * @pub_key_cred_param_count: pub key cred param count value.
 * @exclude_credential_ids: exclude credential ids value.
 * @exclude_credential_id_lens: exclude credential id lens value.
 * @exclude_credential_count: exclude credential count value.
 * @now: now value.
 *
 * Decoded WebAuthn makeCredential request.
 *
 * Since: 1.0
 */
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

/**
 * SignetFidoMakeCredentialResult:
 * @credential_id: credential identifier bytes.
 * @credential_id_len: length of @credential_id in bytes.
 * @auth_data: authenticatorData bytes.
 * @auth_data_len: length of @auth_data in bytes.
 * @attestation_object: attestation object value.
 * @attestation_object_len: attestation object len value.
 * @cose_public_key: cose public key value.
 * @cose_public_key_len: cose public key len value.
 * @sign_count: signature counter value.
 *
 * Result of a WebAuthn makeCredential ceremony.
 *
 * Ownership: clear instances with the corresponding *_clear() function to release heap data and wipe secrets where applicable.
 *
 * Since: 1.0
 */
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

/**
 * SignetFidoGetAssertionRequest:
 * @rp_id: WebAuthn relying-party identifier.
 * @client_data_hash: WebAuthn clientDataHash bytes.
 * @client_data_hash_len: length of @client_data_hash in bytes.
 * @user_verification: user verification value.
 * @allow_credential_ids: allow credential ids value.
 * @allow_credential_id_lens: allow credential id lens value.
 * @allow_credential_count: allow credential count value.
 *
 * Decoded WebAuthn getAssertion request.
 *
 * Since: 1.0
 */
typedef struct {
  const char *rp_id;
  const uint8_t *client_data_hash;
  size_t client_data_hash_len;
  SignetFidoUserVerification user_verification;
  const uint8_t *const *allow_credential_ids;
  const size_t *allow_credential_id_lens;
  size_t allow_credential_count;
} SignetFidoGetAssertionRequest;

/**
 * SignetFidoGetAssertionResult:
 * @credential_id: credential identifier bytes.
 * @credential_id_len: length of @credential_id in bytes.
 * @auth_data: authenticatorData bytes.
 * @auth_data_len: length of @auth_data in bytes.
 * @signature_der: DER-encoded ECDSA signature.
 * @signature_der_len: length of @signature_der in bytes.
 * @user_handle: WebAuthn user handle bytes.
 * @user_handle_len: length of @user_handle in bytes.
 * @sign_count: signature counter value.
 *
 * Result of a WebAuthn getAssertion ceremony.
 *
 * Ownership: clear instances with the corresponding *_clear() function to release heap data and wipe secrets where applicable.
 *
 * Since: 1.0
 */
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

/**
 * signet_fido_service_new:
 * @cfg: (nullable): configuration to use
 *
 * Creates a software FIDO2/WebAuthn authenticator service.
 *
 * Returns: (transfer full) (nullable): a newly allocated object, or %NULL on failure
 *
 * Since: 1.0
 */
SignetFidoService *signet_fido_service_new(const SignetFidoServiceConfig *cfg);
/**
 * signet_fido_service_free:
 * @svc: (nullable): a #SignetFidoService
 *
 * signet fido service free.
 *
 * Since: 1.0
 */
void signet_fido_service_free(SignetFidoService *svc);

/**
 * signet_fido_service_is_enabled:
 * @svc: (not nullable): a #SignetFidoService
 *
 * signet fido service is enabled.
 *
 * Returns: %true if the condition is met, otherwise %false
 *
 * Since: 1.0
 */
bool signet_fido_service_is_enabled(const SignetFidoService *svc);

/* True if the service is configured to approve UV-required ceremonies headlessly
 * (allow_headless_uv). The CTAP-HID layer uses this to decide whether a uv=true
 * request can be honored rather than rejected outright. */
/**
 * signet_fido_service_allows_headless_uv:
 * @svc: (not nullable): a #SignetFidoService
 *
 * True if the service is configured to approve UV-required ceremonies headlessly (allow_headless_uv). The CTAP-HID layer uses this to decide whether a uv=true request can be honored rather than rejected outright.
 *
 * Returns: %true if the condition is met, otherwise %false
 *
 * Since: 1.0
 */
bool signet_fido_service_allows_headless_uv(const SignetFidoService *svc);
/**
 * signet_fido_status_string:
 * @status: status
 *
 * Returns a static string for a FIDO status code.
 *
 * Returns: (transfer none) (nullable): a borrowed pointer owned by the callee
 *
 * Since: 1.0
 */
const char *signet_fido_status_string(SignetFidoStatus status);
/**
 * signet_fido_error_clear:
 * @err: (nullable): optional FIDO error detail
 *
 * signet fido error clear.
 *
 * Since: 1.0
 */
void signet_fido_error_clear(SignetFidoError *err);

/**
 * signet_fido_get_info_json:
 * @svc: (not nullable): a #SignetFidoService
 * @agent_id: (not nullable): agent identifier
 * @out_json: (out) (transfer full) (not nullable): return location for a newly allocated JSON string
 * @err: (nullable): optional FIDO error detail
 *
 * signet fido get info json.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
SignetFidoStatus signet_fido_get_info_json(SignetFidoService *svc,
                                           const char *agent_id,
                                           char **out_json,
                                           SignetFidoError *err);

/**
 * signet_fido_make_credential:
 * @svc: (not nullable): a #SignetFidoService
 * @agent_id: (not nullable): agent identifier
 * @req: (not nullable): request data
 * @out: (out) (not nullable): output record to populate
 * @err: (nullable): optional FIDO error detail
 *
 * signet fido make credential.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
SignetFidoStatus signet_fido_make_credential(SignetFidoService *svc,
                                             const char *agent_id,
                                             const SignetFidoMakeCredentialRequest *req,
                                             SignetFidoMakeCredentialResult *out,
                                             SignetFidoError *err);

/**
 * signet_fido_get_assertion:
 * @svc: (not nullable): a #SignetFidoService
 * @agent_id: (not nullable): agent identifier
 * @req: (not nullable): request data
 * @out: (out) (not nullable): output record to populate
 * @err: (nullable): optional FIDO error detail
 *
 * signet fido get assertion.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
SignetFidoStatus signet_fido_get_assertion(SignetFidoService *svc,
                                           const char *agent_id,
                                           const SignetFidoGetAssertionRequest *req,
                                           SignetFidoGetAssertionResult *out,
                                           SignetFidoError *err);

/**
 * signet_fido_make_credential_json:
 * @svc: (not nullable): a #SignetFidoService
 * @agent_id: (not nullable): agent identifier
 * @request_json: (not nullable): request JSON object string
 * @out_json: (out) (transfer full) (not nullable): return location for a newly allocated JSON string
 * @err: (nullable): optional FIDO error detail
 *
 * signet fido make credential json.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
SignetFidoStatus signet_fido_make_credential_json(SignetFidoService *svc,
                                                  const char *agent_id,
                                                  const char *request_json,
                                                  char **out_json,
                                                  SignetFidoError *err);

/**
 * signet_fido_get_assertion_json:
 * @svc: (not nullable): a #SignetFidoService
 * @agent_id: (not nullable): agent identifier
 * @request_json: (not nullable): request JSON object string
 * @out_json: (out) (transfer full) (not nullable): return location for a newly allocated JSON string
 * @err: (nullable): optional FIDO error detail
 *
 * signet fido get assertion json.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
SignetFidoStatus signet_fido_get_assertion_json(SignetFidoService *svc,
                                                const char *agent_id,
                                                const char *request_json,
                                                char **out_json,
                                                SignetFidoError *err);

/**
 * signet_fido_export_credential_json:
 * @svc: (not nullable): a #SignetFidoService
 * @agent_id: (not nullable): agent identifier
 * @request_json: (not nullable): request JSON object string
 * @out_json: (out) (transfer full) (not nullable): return location for a newly allocated JSON string
 * @err: (nullable): optional FIDO error detail
 *
 * signet fido export credential json.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
SignetFidoStatus signet_fido_export_credential_json(SignetFidoService *svc,
                                                    const char *agent_id,
                                                    const char *request_json,
                                                    char **out_json,
                                                    SignetFidoError *err);

/**
 * signet_fido_import_credential_json:
 * @svc: (not nullable): a #SignetFidoService
 * @agent_id: (not nullable): agent identifier
 * @request_json: (not nullable): request JSON object string
 * @out_json: (out) (transfer full) (not nullable): return location for a newly allocated JSON string
 * @err: (nullable): optional FIDO error detail
 *
 * signet fido import credential json.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
SignetFidoStatus signet_fido_import_credential_json(SignetFidoService *svc,
                                                    const char *agent_id,
                                                    const char *request_json,
                                                    char **out_json,
                                                    SignetFidoError *err);

/**
 * signet_fido_make_credential_result_clear:
 * @r: (nullable): r
 *
 * signet fido make credential result clear.
 *
 * Since: 1.0
 */
void signet_fido_make_credential_result_clear(SignetFidoMakeCredentialResult *r);
/**
 * signet_fido_get_assertion_result_clear:
 * @r: (nullable): r
 *
 * signet fido get assertion result clear.
 *
 * Since: 1.0
 */
void signet_fido_get_assertion_result_clear(SignetFidoGetAssertionResult *r);

/**
 * signet_fido_parse_aaguid:
 * @uuid: (not nullable): uuid
 * @out: (out) (not nullable): output record to populate
 *
 * signet fido parse aaguid.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_fido_parse_aaguid(const char *uuid, uint8_t out[SIGNET_FIDO_AAGUID_LEN]);
/**
 * signet_fido_format_aaguid:
 * @aaguid: (not nullable): aaguid
 * @out: (out) (not nullable): output record to populate
 *
 * signet fido format aaguid.
 *
 * Since: 1.0
 */
void signet_fido_format_aaguid(const uint8_t aaguid[SIGNET_FIDO_AAGUID_LEN],
                               char out[37]);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_FIDO_H */
