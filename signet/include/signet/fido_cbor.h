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
#define SIGNET_FIDO_FLAG_UP 0x01 /* User Present */
#define SIGNET_FIDO_FLAG_UV 0x04 /* User Verified */
#define SIGNET_FIDO_FLAG_BE 0x08 /* Backup Eligible */
#define SIGNET_FIDO_FLAG_BS 0x10 /* Backup State */
#define SIGNET_FIDO_FLAG_AT 0x40 /* Attested credential data included */

/* Encode a COSE_Key for an EC2 / P-256 / ES256 public key. 0 on success. */
int signet_cose_ec2_p256(const uint8_t x[32], const uint8_t y[32],
                         uint8_t **out, size_t *out_len);

/* Parse and validate a COSE_Key EC2 / P-256 / ES256 public key. 0 on success. */
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
int signet_fido_auth_data(const char *rp_id, uint8_t flags, uint32_t sign_count,
                          const uint8_t aaguid[16],
                          const uint8_t *cred_id, size_t cred_id_len,
                          const uint8_t *cose_key, size_t cose_key_len,
                          uint8_t **out, size_t *out_len);

/* Build a CBOR "none" attestation object: {fmt:"none", attStmt:{}, authData:<bytes>}. */
int signet_fido_attestation_none(const uint8_t *auth_data, size_t auth_data_len,
                                 uint8_t **out, size_t *out_len);

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
int signet_fido_cbor_decode_make_credential(const uint8_t *cbor, size_t cbor_len,
                                            SignetFidoCborMakeCredential *out);
int signet_fido_cbor_decode_get_assertion(const uint8_t *cbor, size_t cbor_len,
                                          SignetFidoCborGetAssertion *out);

void signet_fido_cbor_make_credential_clear(SignetFidoCborMakeCredential *d);
void signet_fido_cbor_get_assertion_clear(SignetFidoCborGetAssertion *d);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_FIDO_CBOR_H */
