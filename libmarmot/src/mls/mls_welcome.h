/*
 * libmarmot - MLS Welcome Message (RFC 9420 §12.4.3.1)
 *
 * A Welcome message allows a new member to join a group. It contains
 * the GroupInfo (encrypted) and the joiner_secret encrypted to the
 * new member's KeyPackage init_key.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef MLS_WELCOME_H
#define MLS_WELCOME_H

#include "mls-internal.h"
#include "mls_group.h"
#include "mls_key_package.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ──────────────────────────────────────────────────────────────────────────
 * EncryptedGroupSecrets (RFC 9420 §12.4.3.1)
 *
 * struct {
 *   KeyPackageRef new_member;
 *   HPKECiphertext encrypted_group_secrets;
 * } EncryptedGroupSecrets;
 * ──────────────────────────────────────────────────────────────────────── */

typedef struct {
    uint8_t  key_package_ref[MLS_HASH_LEN];  /**< Identifies target KeyPackage */
    uint8_t  kem_output[MLS_KEM_ENC_LEN];    /**< HPKE encap output */
    uint8_t *encrypted_joiner_secret;         /**< AEAD ciphertext */
    size_t   encrypted_joiner_secret_len;
} MlsEncryptedGroupSecrets;

/** Free EncryptedGroupSecrets internals. */
void mls_encrypted_group_secrets_clear(MlsEncryptedGroupSecrets *egs);

/* ──────────────────────────────────────────────────────────────────────────
 * Welcome (RFC 9420 §12.4.3.1)
 *
 * struct {
 *   CipherSuite cipher_suite;
 *   EncryptedGroupSecrets secrets<V>;
 *   opaque encrypted_group_info<V>;
 * } Welcome;
 * ──────────────────────────────────────────────────────────────────────── */

typedef struct {
    uint16_t cipher_suite;

    MlsEncryptedGroupSecrets *secrets;
    size_t                     secret_count;

    uint8_t *encrypted_group_info;
    size_t   encrypted_group_info_len;
} MlsWelcome;

/** Free Welcome internals. */
void mls_welcome_clear(MlsWelcome *w);

/* ──────────────────────────────────────────────────────────────────────────
 * Serialization
 * ──────────────────────────────────────────────────────────────────────── */

/** Serialize a Welcome to TLS wire format. */
int mls_welcome_serialize(const MlsWelcome *w, MlsTlsBuf *buf);

/** Deserialize a Welcome from TLS wire format. */
int mls_welcome_deserialize(MlsTlsReader *reader, MlsWelcome *w);

/* ──────────────────────────────────────────────────────────────────────────
 * Welcome processing (joining a group)
 *
 * A new member processes a Welcome by:
 *   1. Finding their EncryptedGroupSecrets entry (by KeyPackageRef)
 *   2. Decapsulating the HPKE ciphertext using their init_key private key
 *   3. Decrypting the joiner_secret
 *   4. Deriving the welcome_secret from the joiner_secret
 *   5. Decrypting the GroupInfo using the welcome_secret
 *   6. Initializing their group state from the GroupInfo
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * Process a Welcome message and initialize group state.
 *
 * @param welcome_data    Serialized Welcome message
 * @param welcome_len     Length of welcome data
 * @param kp              The KeyPackage that was used in the Welcome
 * @param kp_priv         Private keys from the KeyPackage
 * @param ratchet_tree    Optional: the ratchet tree (serialized), if provided
 *                        out-of-band. NULL to expect it in extensions.
 * @param tree_len        Length of ratchet tree data
 * @param group_out       Output: initialized group state
 * @return 0 on success
 */
int mls_welcome_process(const uint8_t *welcome_data, size_t welcome_len,
                        const MlsKeyPackage *kp,
                        const MlsKeyPackagePrivate *kp_priv,
                        const uint8_t *ratchet_tree, size_t tree_len,
                        MlsGroup *group_out);

/**
 * Process a Welcome that has already been deserialized.
 *
 * Same as mls_welcome_process but takes a parsed MlsWelcome struct.
 */
int mls_welcome_process_parsed(const MlsWelcome *welcome,
                               const MlsKeyPackage *kp,
                               const MlsKeyPackagePrivate *kp_priv,
                               const uint8_t *ratchet_tree, size_t tree_len,
                               MlsGroup *group_out);

#ifdef __cplusplus
}
#endif

#endif /* MLS_WELCOME_H */
