/*
 * libmarmot - MLS Message Framing (RFC 9420 §6)
 *
 * Handles PrivateMessage encryption/decryption (the main message format)
 * and the sender data encryption that protects sender identity.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef MLS_FRAMING_H
#define MLS_FRAMING_H

#include "mls-internal.h"
#include "mls_key_schedule.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ──────────────────────────────────────────────────────────────────────────
 * Wire format constants
 * ──────────────────────────────────────────────────────────────────────── */

#define MLS_WIRE_FORMAT_PUBLIC_MESSAGE   0x0001
#define MLS_WIRE_FORMAT_PRIVATE_MESSAGE  0x0002
#define MLS_WIRE_FORMAT_WELCOME          0x0003
#define MLS_WIRE_FORMAT_GROUP_INFO       0x0004
#define MLS_WIRE_FORMAT_KEY_PACKAGE      0x0005

#define MLS_CONTENT_TYPE_APPLICATION  1
#define MLS_CONTENT_TYPE_PROPOSAL     2
#define MLS_CONTENT_TYPE_COMMIT       3

#define MLS_SENDER_TYPE_MEMBER          1
#define MLS_SENDER_TYPE_EXTERNAL        2
#define MLS_SENDER_TYPE_NEW_MEMBER_PROP 3
#define MLS_SENDER_TYPE_NEW_MEMBER_COMMIT 4

/* ──────────────────────────────────────────────────────────────────────────
 * Sender data (RFC 9420 §6.3.1)
 *
 * The sender data in a PrivateMessage is encrypted to hide which member
 * sent the message.
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * MlsSenderData:
 *
 * The plaintext sender data that gets encrypted in a PrivateMessage.
 */
typedef struct {
    uint32_t leaf_index;       /**< Sender's leaf index */
    uint32_t generation;       /**< Message generation number */
    uint8_t  reuse_guard[4];   /**< Random bytes to prevent nonce reuse */
} MlsSenderData;

/**
 * Encrypt sender data.
 *
 * sender_data_key = ExpandWithLabel(sender_data_secret, "key", ciphertext_sample, key_len)
 * sender_data_nonce = ExpandWithLabel(sender_data_secret, "nonce", ciphertext_sample, nonce_len)
 * encrypted_sender_data = AEAD.Seal(key, nonce, "", sender_data)
 *
 * @param sender_data_secret  From epoch secrets
 * @param ciphertext_sample   First AEAD_KEY_LEN bytes of the ciphertext
 * @param sender_data         Plaintext sender data to encrypt
 * @param out                 Output buffer (at least 12 + AEAD_TAG_LEN bytes)
 * @param out_len             Output length
 * @return 0 on success
 */
int mls_sender_data_encrypt(const uint8_t sender_data_secret[MLS_HASH_LEN],
                             const uint8_t *ciphertext_sample, size_t sample_len,
                             const MlsSenderData *sender_data,
                             uint8_t *out, size_t *out_len);

/**
 * Decrypt sender data.
 */
int mls_sender_data_decrypt(const uint8_t sender_data_secret[MLS_HASH_LEN],
                             const uint8_t *ciphertext_sample, size_t sample_len,
                             const uint8_t *encrypted, size_t encrypted_len,
                             MlsSenderData *out);

/* ──────────────────────────────────────────────────────────────────────────
 * PrivateMessage (RFC 9420 §6.3.2)
 *
 * The main encrypted message format for both handshake and application
 * messages within a group.
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * MlsPrivateMessage:
 *
 * A serialized PrivateMessage containing encrypted content.
 */
typedef struct {
    uint8_t *group_id;
    size_t   group_id_len;
    uint64_t epoch;
    uint8_t  content_type;     /**< MLS_CONTENT_TYPE_* */
    uint8_t *authenticated_data;
    size_t   authenticated_data_len;
    uint8_t *encrypted_sender_data;
    size_t   encrypted_sender_data_len;
    uint8_t *ciphertext;
    size_t   ciphertext_len;
} MlsPrivateMessage;

/** Free a PrivateMessage's heap data. */
void mls_private_message_clear(MlsPrivateMessage *msg);

/**
 * Encrypt a PrivateMessage.
 *
 * @param group_id            Group ID
 * @param group_id_len        Length of group ID
 * @param epoch               Current epoch
 * @param content_type        MLS_CONTENT_TYPE_APPLICATION or _PROPOSAL or _COMMIT
 * @param authenticated_data  AAD (can be NULL/0)
 * @param aad_len             Length of AAD
 * @param plaintext           The FramedContent plaintext
 * @param plaintext_len       Length of plaintext
 * @param sender_data_secret  Sender data secret from epoch
 * @param message_keys        Message keys from secret tree
 * @param sender_leaf_index   Sender's leaf index
 * @param reuse_guard         4 random bytes
 * @param out                 Output PrivateMessage (caller must clear)
 * @return 0 on success
 */
int mls_private_message_encrypt(const uint8_t *group_id, size_t group_id_len,
                                 uint64_t epoch,
                                 uint8_t content_type,
                                 const uint8_t *authenticated_data, size_t aad_len,
                                 const uint8_t *plaintext, size_t plaintext_len,
                                 const uint8_t sender_data_secret[MLS_HASH_LEN],
                                 const MlsMessageKeys *message_keys,
                                 uint32_t sender_leaf_index,
                                 const uint8_t reuse_guard[4],
                                 MlsPrivateMessage *out);

/**
 * Decrypt a PrivateMessage.
 *
 * @param msg                 The PrivateMessage to decrypt
 * @param sender_data_secret  Sender data secret from epoch
 * @param st                  Secret tree for key derivation
 * @param max_forward_distance Maximum forward ratchet distance
 * @param out_plaintext       Output decrypted content (caller frees)
 * @param out_pt_len          Output length
 * @param out_sender          Output sender data (leaf_index, generation)
 * @return 0 on success
 */
int mls_private_message_decrypt(const MlsPrivateMessage *msg,
                                 const uint8_t sender_data_secret[MLS_HASH_LEN],
                                 MlsSecretTree *st,
                                 uint32_t max_forward_distance,
                                 uint8_t **out_plaintext, size_t *out_pt_len,
                                 MlsSenderData *out_sender);

/* ──────────────────────────────────────────────────────────────────────────
 * PrivateMessage TLS serialization
 * ──────────────────────────────────────────────────────────────────────── */

/** Serialize a PrivateMessage to TLS wire format. */
int mls_private_message_serialize(const MlsPrivateMessage *msg, MlsTlsBuf *buf);

/** Deserialize a PrivateMessage from TLS wire format. */
int mls_private_message_deserialize(MlsTlsReader *reader, MlsPrivateMessage *msg);

/* ──────────────────────────────────────────────────────────────────────────
 * Content AAD (RFC 9420 §6.3.2)
 *
 * The AAD for PrivateMessage AEAD is:
 *   PrivateContentAAD = group_id || epoch || content_type
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * Build the content AAD for a PrivateMessage.
 * Caller frees *out.
 */
int mls_build_content_aad(const uint8_t *group_id, size_t group_id_len,
                           uint64_t epoch, uint8_t content_type,
                           const uint8_t *authenticated_data, size_t aad_len,
                           uint8_t **out, size_t *out_len);

/* ──────────────────────────────────────────────────────────────────────────
 * Nonce reuse guard (RFC 9420 §6.3.2)
 *
 * The nonce for PrivateMessage encryption is XORed with the reuse_guard
 * from the sender data to prevent nonce reuse across epochs.
 * ──────────────────────────────────────────────────────────────────────── */

/** Apply reuse guard to a nonce: XOR the first 4 bytes. */
void mls_apply_reuse_guard(uint8_t nonce[MLS_AEAD_NONCE_LEN],
                           const uint8_t reuse_guard[4]);

#ifdef __cplusplus
}
#endif

#endif /* MLS_FRAMING_H */
