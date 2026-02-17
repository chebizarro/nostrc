/*
 * libmarmot - MLS KeyPackage (RFC 9420 §10)
 *
 * A KeyPackage is a signed object that a client uses to advertise its
 * ability to join a group. It contains the client's HPKE init key,
 * leaf node (signing identity + encryption key), and extensions.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef MLS_KEY_PACKAGE_H
#define MLS_KEY_PACKAGE_H

#include "mls-internal.h"
#include "mls_tree.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ──────────────────────────────────────────────────────────────────────────
 * KeyPackage (RFC 9420 §10.1)
 *
 * struct {
 *   ProtocolVersion version;
 *   CipherSuite cipher_suite;
 *   HPKEPublicKey init_key;
 *   LeafNode leaf_node;
 *   Extension extensions<V>;
 *   opaque signature<V>;
 * } KeyPackage;
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * MlsKeyPackage:
 *
 * A signed key package advertising a client's ability to join a group.
 */
typedef struct {
    uint16_t    version;           /**< ProtocolVersion (= 1 for mls10) */
    uint16_t    cipher_suite;      /**< CipherSuite (= 0x0001) */
    uint8_t     init_key[MLS_KEM_PK_LEN]; /**< HPKE init public key (X25519) */
    MlsLeafNode leaf_node;         /**< The leaf node for the tree */
    uint8_t    *extensions_data;   /**< Serialized extensions */
    size_t      extensions_len;

    /* Signature over KeyPackageTBS */
    uint8_t     signature[MLS_SIG_LEN];
    size_t      signature_len;
} MlsKeyPackage;

/**
 * MlsKeyPackagePrivate:
 *
 * Private keys associated with a KeyPackage. Held by the creator,
 * consumed when processing a Welcome.
 */
typedef struct {
    uint8_t init_key_private[MLS_KEM_SK_LEN]; /**< HPKE init private key */
    uint8_t encryption_key_private[MLS_KEM_SK_LEN]; /**< Leaf HPKE private key */
    uint8_t signature_key_private[MLS_SIG_SK_LEN]; /**< Ed25519 signing key */
} MlsKeyPackagePrivate;

/* ──────────────────────────────────────────────────────────────────────────
 * Lifecycle
 * ──────────────────────────────────────────────────────────────────────── */

/** Free internal resources of a KeyPackage (but not the struct itself). */
void mls_key_package_clear(MlsKeyPackage *kp);

/** Free internal resources of a KeyPackagePrivate. */
void mls_key_package_private_clear(MlsKeyPackagePrivate *priv);

/* ──────────────────────────────────────────────────────────────────────────
 * Creation
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * Create a new KeyPackage.
 *
 * Generates fresh HPKE init, encryption, and signing keypairs.
 * The credential_identity is the Nostr pubkey (32 bytes).
 *
 * @param kp          Output KeyPackage (public portion)
 * @param priv_out    Output private keys (caller must protect/clear)
 * @param credential_identity  The identity for the BasicCredential (e.g., 32-byte nostr pubkey)
 * @param credential_identity_len  Length of identity
 * @param extensions_data  Serialized extensions (can be NULL)
 * @param extensions_len   Length of extensions
 * @return 0 on success
 */
int mls_key_package_create(MlsKeyPackage *kp,
                            MlsKeyPackagePrivate *priv_out,
                            const uint8_t *credential_identity,
                            size_t credential_identity_len,
                            const uint8_t *extensions_data,
                            size_t extensions_len);

/* ──────────────────────────────────────────────────────────────────────────
 * Serialization (TLS format)
 * ──────────────────────────────────────────────────────────────────────── */

/** Serialize a KeyPackage to TLS wire format. */
int mls_key_package_serialize(const MlsKeyPackage *kp, MlsTlsBuf *buf);

/** Deserialize a KeyPackage from TLS wire format. */
int mls_key_package_deserialize(MlsTlsReader *reader, MlsKeyPackage *kp);

/* ──────────────────────────────────────────────────────────────────────────
 * KeyPackageTBS serialization (for signing/verification)
 *
 * struct {
 *   ProtocolVersion version;
 *   CipherSuite cipher_suite;
 *   HPKEPublicKey init_key;
 *   LeafNode leaf_node;
 *   Extension extensions<V>;
 * } KeyPackageTBS;
 * ──────────────────────────────────────────────────────────────────────── */

/** Serialize KeyPackageTBS (everything except the signature). */
int mls_key_package_tbs_serialize(const MlsKeyPackage *kp, MlsTlsBuf *buf);

/* ──────────────────────────────────────────────────────────────────────────
 * Validation
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * Validate a KeyPackage.
 *
 * Checks:
 *   - version == mls10
 *   - cipher_suite == 0x0001
 *   - signature verifies against leaf_node.signature_key
 *   - leaf_node has valid content
 *
 * @return 0 if valid, error code otherwise
 */
int mls_key_package_validate(const MlsKeyPackage *kp);

/* ──────────────────────────────────────────────────────────────────────────
 * KeyPackageRef (RFC 9420 §5.3.1)
 *
 * KeyPackageRef = RefHash("MLS 1.0 KeyPackage Reference", serialized_kp)
 * Used to uniquely identify a key package.
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * Compute the KeyPackageRef for a key package.
 *
 * @param kp   The key package
 * @param out  Output hash (MLS_HASH_LEN bytes)
 * @return 0 on success
 */
int mls_key_package_ref(const MlsKeyPackage *kp, uint8_t out[MLS_HASH_LEN]);

#ifdef __cplusplus
}
#endif

#endif /* MLS_KEY_PACKAGE_H */
