/* SPDX-License-Identifier: MIT */
/*
 * signet FIDO crypto — key-backend interface (Phase 0 spike).
 *
 * ES256 / COSE alg -7 = ECDSA over NIST P-256 with SHA-256, the mandatory
 * WebAuthn/FIDO2 algorithm. The interface is deliberately key-HANDLE based so a
 * future hardware/enclave backend can implement the same ops while keeping the
 * private key non-exportable (export_private simply fails for such backends).
 */
#ifndef SIGNET_FIDO_CRYPTO_H
#define SIGNET_FIDO_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque credential key handle. */
/**
 * signet_fido_key:
 * Opaque ES256 credential key handle.
 *
 * Since: 1.0
 */
typedef struct signet_fido_key signet_fido_key;

/* Generate a fresh ES256 (P-256) credential key. NULL on failure. */
/**
 * signet_fido_key_generate:
 *
 * Generate a fresh ES256 (P-256) credential key. NULL on failure.
 *
 * Returns: (transfer full) (nullable): a newly allocated object, or %NULL on failure
 *
 * Since: 1.0
 */
signet_fido_key *signet_fido_key_generate(void);

/* Export the public key as raw affine coordinates (32 bytes each). 0 on success. */
/**
 * signet_fido_key_public_xy:
 * @k: (not nullable): k
 * @x: (not nullable): x
 * @y: (not nullable): y
 *
 * Export the public key as raw affine coordinates (32 bytes each). 0 on success.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_fido_key_public_xy(const signet_fido_key *k, uint8_t x[32], uint8_t y[32]);

/* Export the private key as PKCS#8 DER (malloc'd into *out). 0 on success.
 * A hardware backend returns non-zero here — keys never leave the device. */
/**
 * signet_fido_key_export_private:
 * @k: (not nullable): k
 * @out: (out) (transfer full) (not nullable): output record to populate
 * @out_len: (out) (not nullable): return location for len
 *
 * Export the private key as PKCS#8 DER (malloc'd into *out). 0 on success. A hardware backend returns non-zero here — keys never leave the device.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_fido_key_export_private(const signet_fido_key *k, uint8_t **out, size_t *out_len);

/* Import a private key from PKCS#8 DER. NULL on failure. */
/**
 * signet_fido_key_import_private:
 * @der: (not nullable): der
 * @der_len: length of @der in bytes
 *
 * Import a private key from PKCS#8 DER. NULL on failure.
 *
 * Returns: (transfer full) (nullable): a newly allocated object, or %NULL on failure
 *
 * Since: 1.0
 */
signet_fido_key *signet_fido_key_import_private(const uint8_t *der, size_t der_len);

/* Sign msg with ECDSA P-256 + SHA-256; DER signature malloc'd into *sig. 0 on success. */
/**
 * signet_fido_key_sign:
 * @k: (not nullable): k
 * @msg: (not nullable): msg
 * @msg_len: length of @msg in bytes
 * @sig: (not nullable): sig
 * @sig_len: (not nullable): length of @sig in bytes
 *
 * Sign msg with ECDSA P-256 + SHA-256; DER signature malloc'd into *sig. 0 on success.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_fido_key_sign(const signet_fido_key *k, const uint8_t *msg, size_t msg_len,
                         uint8_t **sig, size_t *sig_len);

/* Standalone verify of a DER ECDSA-P256/SHA-256 signature against raw X/Y.
 * Used by the self-hosted RP verifier in tests. 1 = valid, 0 = invalid, <0 = error. */
/**
 * signet_fido_verify_p256:
 * @x: (not nullable): x
 * @y: (not nullable): y
 * @msg: (not nullable): msg
 * @msg_len: length of @msg in bytes
 * @sig: (not nullable): sig
 * @sig_len: length of @sig in bytes
 *
 * Standalone verify of a DER ECDSA-P256/SHA-256 signature against raw X/Y. Used by the self-hosted RP verifier in tests. 1 = valid, 0 = invalid, <0 = error.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_fido_verify_p256(const uint8_t x[32], const uint8_t y[32],
                            const uint8_t *msg, size_t msg_len,
                            const uint8_t *sig, size_t sig_len);

/**
 * signet_fido_key_free:
 * @k: (nullable): k
 *
 * signet fido key free.
 *
 * Since: 1.0
 */
void signet_fido_key_free(signet_fido_key *k);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_FIDO_CRYPTO_H */
