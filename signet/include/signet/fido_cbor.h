/* SPDX-License-Identifier: MIT */
/*
 * signet FIDO CBOR/COSE helpers.
 */
#ifndef SIGNET_FIDO_CBOR_H
#define SIGNET_FIDO_CBOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* authenticatorData flag bits (WebAuthn L3 §6.1). */
/**
 * SIGNET_FIDO_FLAG_UP:
 *
 * Authenticator-data flag indicating user presence.
 *
 * Since: 1.0
 */
#define SIGNET_FIDO_FLAG_UP 0x01 /* User Present */
/**
 * SIGNET_FIDO_FLAG_UV:
 *
 * Authenticator-data flag indicating user verification.
 *
 * Since: 1.0
 */
#define SIGNET_FIDO_FLAG_UV 0x04 /* User Verified */
/**
 * SIGNET_FIDO_FLAG_BE:
 *
 * Authenticator-data flag indicating backup eligibility.
 *
 * Since: 1.0
 */
#define SIGNET_FIDO_FLAG_BE 0x08 /* Backup Eligible */
/**
 * SIGNET_FIDO_FLAG_BS:
 *
 * Authenticator-data flag indicating backup state.
 *
 * Since: 1.0
 */
#define SIGNET_FIDO_FLAG_BS 0x10 /* Backup State */
/**
 * SIGNET_FIDO_FLAG_AT:
 *
 * Authenticator-data flag indicating attested credential data.
 *
 * Since: 1.0
 */
#define SIGNET_FIDO_FLAG_AT 0x40 /* Attested credential data included */

/* Encode a COSE_Key for an EC2 / P-256 / ES256 public key. 0 on success. */
/**
 * signet_cose_ec2_p256:
 * @x: (not nullable): x
 * @y: (not nullable): y
 * @out: (out) (transfer full) (not nullable): output record to populate
 * @out_len: (out) (not nullable): return location for len
 *
 * Encode a COSE_Key for an EC2 / P-256 / ES256 public key. 0 on success.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_cose_ec2_p256(const uint8_t x[32], const uint8_t y[32],
                         uint8_t **out, size_t *out_len);

/* Parse and validate a COSE_Key EC2 / P-256 / ES256 public key. 0 on success. */
/**
 * signet_cose_ec2_p256_parse:
 * @cose_key: (not nullable): cose key
 * @cose_key_len: length of @cose_key in bytes
 * @x: (not nullable): x
 * @y: (not nullable): y
 *
 * Parse and validate a COSE_Key EC2 / P-256 / ES256 public key. 0 on success.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_cose_ec2_p256_parse(const uint8_t *cose_key, size_t cose_key_len,
                               uint8_t x[32], uint8_t y[32]);

/* Build authenticatorData.
 * rp_id           : the relying-party id string (hashed with SHA-256 internally)
 * flags           : OR of SIGNET_FIDO_FLAG_*
 * sign_count      : reported counter (0 for synced credentials)
 * aaguid          : 16 bytes, used only when AT flag is set (may be NULL otherwise)
 * cred_id/len     : credential id, used only when AT flag is set
 * cose_key/len    : COSE public key, used only when AT flag is set
 * Result malloc'd into *out. 0 on success. */
/**
 * signet_fido_auth_data:
 * @rp_id: (not nullable): rp id
 * @flags: flags
 * @sign_count: number of elements
 * @aaguid: (not nullable): aaguid
 * @cred_id: (not nullable): cred id
 * @cred_id_len: length of @cred_id in bytes
 * @cose_key: (not nullable): cose key
 * @cose_key_len: length of @cose_key in bytes
 * @out: (out) (transfer full) (not nullable): output record to populate
 * @out_len: (out) (not nullable): return location for len
 *
 * Build authenticatorData. rp_id           : the relying-party id string (hashed with SHA-256 internally) flags           : OR of SIGNET_FIDO_FLAG_* sign_count      : reported counter (0 for synced credentials) aaguid          : 16 bytes, used only when AT flag is set (may be NULL otherwise) cred_id/len     : credential id, used only when AT flag is set cose_key/len    : COSE public key, used only when AT flag is set Result malloc'd into *out. 0 on success.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_fido_auth_data(const char *rp_id, uint8_t flags, uint32_t sign_count,
                          const uint8_t aaguid[16],
                          const uint8_t *cred_id, size_t cred_id_len,
                          const uint8_t *cose_key, size_t cose_key_len,
                          uint8_t **out, size_t *out_len);

/* Build a CBOR "none" attestation object: {fmt:"none", attStmt:{}, authData:<bytes>}. */
/**
 * signet_fido_attestation_none:
 * @auth_data: (not nullable): auth data
 * @auth_data_len: length of @auth_data in bytes
 * @out: (out) (transfer full) (not nullable): output record to populate
 * @out_len: (out) (not nullable): return location for len
 *
 * Build a CBOR "none" attestation object: {fmt:"none", attStmt:{}, authData:<bytes>}.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_fido_attestation_none(const uint8_t *auth_data, size_t auth_data_len,
                                 uint8_t **out, size_t *out_len);

/**
 * SignetFidoCborMakeCredential:
 * @client_data_hash: WebAuthn clientDataHash bytes.
 * @client_data_hash_len: length of @client_data_hash in bytes.
 * @rp_id: WebAuthn relying-party identifier.
 * @user_handle: WebAuthn user handle bytes.
 * @user_handle_len: length of @user_handle in bytes.
 * @user_name: optional user name.
 * @user_display_name: optional display name.
 * @discoverable: whether the credential is discoverable.
 * @uv_required: uv required value.
 * @unsupported_pin_uv: unsupported pin uv value.
 * @algs: algs value.
 * @alg_count: alg count value.
 * @exclude_ids: exclude ids value.
 * @exclude_lens: exclude lens value.
 * @exclude_count: exclude count value.
 *
 * Decoded CTAP2 authenticatorMakeCredential request.
 *
 * Since: 1.0
 */
typedef struct {
  uint8_t *client_data_hash;
  size_t client_data_hash_len;
  char *rp_id;
  uint8_t *user_handle;
  size_t user_handle_len;
  char *user_name;
  char *user_display_name;
  bool discoverable;
  bool uv_required;
  bool unsupported_pin_uv;
  int *algs;
  size_t alg_count;
  uint8_t **exclude_ids;
  size_t *exclude_lens;
  size_t exclude_count;
} SignetFidoCborMakeCredential;

/**
 * SignetFidoCborGetAssertion:
 * @rp_id: WebAuthn relying-party identifier.
 * @client_data_hash: WebAuthn clientDataHash bytes.
 * @client_data_hash_len: length of @client_data_hash in bytes.
 * @uv_required: uv required value.
 * @unsupported_pin_uv: unsupported pin uv value.
 * @allow_ids: allow ids value.
 * @allow_lens: allow lens value.
 * @allow_count: allow count value.
 *
 * Decoded CTAP2 authenticatorGetAssertion request.
 *
 * Since: 1.0
 */
typedef struct {
  char *rp_id;
  uint8_t *client_data_hash;
  size_t client_data_hash_len;
  bool uv_required;
  bool unsupported_pin_uv;
  uint8_t **allow_ids;
  size_t *allow_lens;
  size_t allow_count;
} SignetFidoCborGetAssertion;

/* Decode CTAP2 authenticatorMakeCredential/getAssertion request maps. */
/**
 * signet_fido_cbor_decode_make_credential:
 * @cbor: (not nullable): cbor
 * @cbor_len: length of @cbor in bytes
 * @out: (out) (not nullable): output record to populate
 *
 * Decode CTAP2 authenticatorMakeCredential/getAssertion request maps.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_fido_cbor_decode_make_credential(const uint8_t *cbor, size_t cbor_len,
                                            SignetFidoCborMakeCredential *out);
/**
 * signet_fido_cbor_decode_get_assertion:
 * @cbor: (not nullable): cbor
 * @cbor_len: length of @cbor in bytes
 * @out: (out) (not nullable): output record to populate
 *
 * signet fido cbor decode get assertion.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_fido_cbor_decode_get_assertion(const uint8_t *cbor, size_t cbor_len,
                                          SignetFidoCborGetAssertion *out);

/**
 * signet_fido_cbor_make_credential_clear:
 * @d: (nullable): d
 *
 * signet fido cbor make credential clear.
 *
 * Since: 1.0
 */
void signet_fido_cbor_make_credential_clear(SignetFidoCborMakeCredential *d);
/**
 * signet_fido_cbor_get_assertion_clear:
 * @d: (nullable): d
 *
 * signet fido cbor get assertion clear.
 *
 * Since: 1.0
 */
void signet_fido_cbor_get_assertion_clear(SignetFidoCborGetAssertion *d);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_FIDO_CBOR_H */
