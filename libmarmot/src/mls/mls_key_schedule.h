/*
 * libmarmot - MLS Key Schedule (RFC 9420 §8)
 *
 * Derives epoch secrets from init_secret + commit_secret + GroupContext.
 * Also provides the secret tree for per-sender message keys.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef MLS_KEY_SCHEDULE_H
#define MLS_KEY_SCHEDULE_H

#include "mls-internal.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ──────────────────────────────────────────────────────────────────────────
 * Epoch secrets (RFC 9420 §8)
 *
 * All secrets are MLS_HASH_LEN (32) bytes for ciphersuite 0x0001.
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * MlsEpochSecrets:
 *
 * All secrets derived from the key schedule for a single epoch.
 */
typedef struct {
    uint8_t sender_data_secret[MLS_HASH_LEN];
    uint8_t encryption_secret[MLS_HASH_LEN];
    uint8_t exporter_secret[MLS_HASH_LEN];
    uint8_t external_secret[MLS_HASH_LEN];
    uint8_t confirmation_key[MLS_HASH_LEN];
    uint8_t membership_key[MLS_HASH_LEN];
    uint8_t resumption_psk[MLS_HASH_LEN];
    uint8_t epoch_authenticator[MLS_HASH_LEN];
    uint8_t init_secret[MLS_HASH_LEN];       /* init_secret for NEXT epoch */

    /** Welcome secret (derived from joiner_secret, used for Welcome) */
    uint8_t welcome_secret[MLS_HASH_LEN];

    /** The joiner secret (needed for Welcome construction) */
    uint8_t joiner_secret[MLS_HASH_LEN];
} MlsEpochSecrets;

/**
 * mls_key_schedule_derive:
 *
 * Derive all epoch secrets from the key schedule inputs.
 *
 * @param init_secret_prev  Init secret from previous epoch (32 bytes, or NULL for epoch 0)
 * @param commit_secret     Commit secret for this epoch (32 bytes)
 * @param group_context     Serialized GroupContext (TLS encoded)
 * @param group_context_len Length of group_context
 * @param psk_secret        PSK secret (32 bytes, or NULL for all-zero)
 * @param out               Output epoch secrets
 * @return 0 on success
 */
int mls_key_schedule_derive(const uint8_t *init_secret_prev,
                             const uint8_t commit_secret[MLS_HASH_LEN],
                             const uint8_t *group_context, size_t group_context_len,
                             const uint8_t *psk_secret,
                             MlsEpochSecrets *out);

/* ──────────────────────────────────────────────────────────────────────────
 * Secret tree & message keys (RFC 9420 §9)
 *
 * The secret tree derives per-sender encryption keys from the
 * encryption_secret. It uses the same left-balanced binary tree
 * structure as the ratchet tree.
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * MlsSenderRatchet:
 *
 * Per-sender ratchet state for deriving message keys.
 * Each sender gets a handshake and application key chain.
 */
typedef struct {
    uint8_t  handshake_secret[MLS_HASH_LEN];
    uint8_t  application_secret[MLS_HASH_LEN];
    uint32_t handshake_generation;
    uint32_t application_generation;
} MlsSenderRatchet;

/**
 * MlsSecretTree:
 *
 * Manages per-sender ratchets for message key derivation.
 *
 * NOTE: This structure is NOT thread-safe. If used in a multi-threaded
 * environment, external synchronization is required.
 */
typedef struct {
    uint8_t (*tree_secrets)[MLS_HASH_LEN]; /**< Secrets for each node */
    uint32_t n_leaves;
    MlsSenderRatchet *senders;             /**< Per-leaf sender ratchets */
    bool *sender_initialized;              /**< Whether sender ratchet is initialized */
} MlsSecretTree;

/**
 * Initialize a secret tree from the encryption_secret.
 *
 * @param st                Output secret tree
 * @param encryption_secret The encryption secret from the epoch
 * @param n_leaves          Number of leaves (= number of group members)
 * @return 0 on success
 */
int mls_secret_tree_init(MlsSecretTree *st,
                          const uint8_t encryption_secret[MLS_HASH_LEN],
                          uint32_t n_leaves);

/** Free secret tree resources. */
void mls_secret_tree_free(MlsSecretTree *st);

/**
 * MlsMessageKeys:
 *
 * The key and nonce for encrypting/decrypting a single message.
 */
typedef struct {
    uint8_t  key[MLS_AEAD_KEY_LEN];
    uint8_t  nonce[MLS_AEAD_NONCE_LEN];
    uint32_t generation;
} MlsMessageKeys;

/**
 * Derive message keys for a sender at the given generation.
 *
 * For encryption: call with is_handshake=false for application messages.
 * The generation counter is automatically advanced.
 *
 * @param st           The secret tree
 * @param leaf_index   Sender's leaf index
 * @param is_handshake Whether this is a handshake (proposal/commit) or application message
 * @param out          Output message keys
 * @return 0 on success
 */
int mls_secret_tree_derive_keys(MlsSecretTree *st, uint32_t leaf_index,
                                 bool is_handshake, MlsMessageKeys *out);

/**
 * Derive message keys for decrypting a message at a specific generation.
 *
 * Advances the ratchet forward if needed (up to max_forward_distance).
 *
 * @param st                  The secret tree
 * @param leaf_index          Sender's leaf index
 * @param is_handshake        Handshake or application message
 * @param generation          The generation number from the message
 * @param max_forward_distance Maximum generations to advance
 * @param out                 Output message keys
 * @return 0 on success
 */
int mls_secret_tree_get_keys_for_generation(MlsSecretTree *st, uint32_t leaf_index,
                                             bool is_handshake, uint32_t generation,
                                             uint32_t max_forward_distance,
                                             MlsMessageKeys *out);

/* ──────────────────────────────────────────────────────────────────────────
 * MLS Exporter (RFC 9420 §8.5)
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * Derive an exported secret from the exporter_secret.
 *
 * MLS-Exporter(label, context, length) =
 *   ExpandWithLabel(DeriveSecret(exporter_secret, label),
 *                   "exported", Hash(context), length)
 *
 * Marmot uses this for NIP-44 conversation keys (MIP-03).
 */
int mls_exporter(const uint8_t exporter_secret[MLS_HASH_LEN],
                 const char *label,
                 const uint8_t *context, size_t context_len,
                 uint8_t *out, size_t out_len);

/* ──────────────────────────────────────────────────────────────────────────
 * GroupContext serialization (RFC 9420 §8.1)
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * Serialize a GroupContext to TLS format.
 *
 * @param group_id          Group ID bytes
 * @param group_id_len      Length of group ID
 * @param epoch             Current epoch number
 * @param tree_hash         Tree hash (MLS_HASH_LEN bytes)
 * @param confirmed_transcript_hash  Confirmed transcript hash (MLS_HASH_LEN bytes)
 * @param extensions_data   Serialized extensions (or NULL)
 * @param extensions_len    Length of extensions
 * @param out_data          Output buffer (caller frees)
 * @param out_len           Output length
 * @return 0 on success
 */
int mls_group_context_serialize(const uint8_t *group_id, size_t group_id_len,
                                 uint64_t epoch,
                                 const uint8_t tree_hash[MLS_HASH_LEN],
                                 const uint8_t confirmed_transcript_hash[MLS_HASH_LEN],
                                 const uint8_t *extensions_data, size_t extensions_len,
                                 uint8_t **out_data, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* MLS_KEY_SCHEDULE_H */
