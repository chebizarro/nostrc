/* SPDX-License-Identifier: MIT */
/*
 * signet FIDO CBOR/COSE — minimal deterministic encoder (Phase 0 spike).
 *
 * NOTE: This is a spike-scoped encoder that emits exactly the shapes WebAuthn
 * needs (COSE_Key EC2, authenticatorData, "none" attestation object) in a
 * deterministic byte order. The production plan replaces it with libcbor
 * (docs/plans/signet-passkeys-fido-2026-07-02.md, Phase 0 item 1). The point of
 * the spike is to prove byte-correctness end to end, which a small controlled
 * encoder does directly.
 */
#ifndef SIGNET_FIDO_CBOR_H
#define SIGNET_FIDO_CBOR_H

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

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_FIDO_CBOR_H */
