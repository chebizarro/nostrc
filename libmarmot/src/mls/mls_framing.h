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

/* ──────────────────────────────────────────────────────────────────────────
 * FramedContent (RFC 9420 §6.1)
 *
 * struct {
 *   opaque group_id<V>;
 *   uint64 epoch;
 *   Sender sender;
 *   opaque authenticated_data<V>;
 *   ContentType content_type;
 *   select (ContentType) {
 *     case application: opaque application_data<V>;
 *     case proposal: Proposal proposal;
 *     case commit: Commit commit;
 *   };
 * } FramedContent;
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * MlsSender:
 *
 * Identifies the sender of a message. For member senders, includes
 * the leaf index in the ratchet tree.
 */
typedef struct {
    uint8_t  sender_type;     /**< MLS_SENDER_TYPE_* */
    uint32_t leaf_index;      /**< Only valid for MEMBER type */
} MlsSender;

/**
 * MlsFramedContent:
 *
 * The inner envelope for MLS messages carrying the actual content
 * along with group context (group_id, epoch, sender).
 */
typedef struct {
    uint8_t  *group_id;
    size_t    group_id_len;
    uint64_t  epoch;
    MlsSender sender;
    uint8_t  *authenticated_data;
    size_t    authenticated_data_len;
    uint8_t   content_type;       /**< MLS_CONTENT_TYPE_* */
    uint8_t  *content;            /**< Serialized content body */
    size_t    content_len;
} MlsFramedContent;

/** Free a FramedContent's heap data. */
void mls_framed_content_clear(MlsFramedContent *fc);

/** Serialize FramedContent to TLS wire format. */
int mls_framed_content_serialize(const MlsFramedContent *fc, MlsTlsBuf *buf);

/** Deserialize FramedContent from TLS wire format. */
int mls_framed_content_deserialize(MlsTlsReader *reader, MlsFramedContent *fc);

/* ──────────────────────────────────────────────────────────────────────────
 * FramedContentTBS (RFC 9420 §6.1)
 *
 * The to-be-signed structure for content authentication.
 *
 * struct {
 *   ProtocolVersion version = mls10;
 *   WireFormat wire_format;
 *   FramedContent content;
 *   select (Sender.sender_type) {
 *     case member:
 *       GroupContext context;
 *   };
 * } FramedContentTBS;
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * Serialize FramedContentTBS for signing.
 *
 * @param fc                 The FramedContent to sign
 * @param wire_format        Wire format (MLS_WIRE_FORMAT_PUBLIC_MESSAGE or _PRIVATE_MESSAGE)
 * @param group_context      Serialized GroupContext (required for member senders)
 * @param group_context_len  Length of group_context
 * @param out                Output buffer (caller frees)
 * @param out_len            Output length
 * @return 0 on success
 */
int mls_framed_content_tbs_serialize(const MlsFramedContent *fc,
                                      uint16_t wire_format,
                                      const uint8_t *group_context, size_t group_context_len,
                                      uint8_t **out, size_t *out_len);

/* ──────────────────────────────────────────────────────────────────────────
 * FramedContentAuthData (RFC 9420 §6.1)
 *
 * struct {
 *   opaque signature<V>;
 *   select (content_type) {
 *     case commit: MAC confirmation_tag;
 *   };
 * } FramedContentAuthData;
 * ──────────────────────────────────────────────────────────────────────── */

typedef struct {
    uint8_t  signature[MLS_SIG_LEN]; /**< Ed25519 signature */
    bool     has_confirmation_tag;   /**< true for commit content type */
    uint8_t  confirmation_tag[MLS_HASH_LEN]; /**< MAC for commit messages */
} MlsFramedContentAuthData;

/**
 * Sign FramedContent.
 *
 * Computes SignWithLabel(signature_key, "FramedContentTBS", FramedContentTBS).
 *
 * @param fc                 Content to sign
 * @param wire_format        Wire format
 * @param group_context      Serialized GroupContext (for member senders)
 * @param group_context_len  Length of group_context
 * @param signature_key      Ed25519 private key
 * @param auth               Output auth data
 * @return 0 on success
 */
int mls_framed_content_sign(const MlsFramedContent *fc,
                             uint16_t wire_format,
                             const uint8_t *group_context, size_t group_context_len,
                             const uint8_t signature_key[MLS_SIG_SK_LEN],
                             MlsFramedContentAuthData *auth);

/**
 * Verify FramedContent signature.
 *
 * @param fc                 Content to verify
 * @param auth               Auth data containing signature
 * @param wire_format        Wire format
 * @param group_context      Serialized GroupContext (for member senders)
 * @param group_context_len  Length of group_context
 * @param verification_key   Ed25519 public key
 * @return 0 on valid signature
 */
int mls_framed_content_verify(const MlsFramedContent *fc,
                               const MlsFramedContentAuthData *auth,
                               uint16_t wire_format,
                               const uint8_t *group_context, size_t group_context_len,
                               const uint8_t verification_key[MLS_SIG_PK_LEN]);

/* ──────────────────────────────────────────────────────────────────────────
 * PublicMessage (RFC 9420 §6.2)
 *
 * struct {
 *   FramedContent content;
 *   FramedContentAuthData auth;
 *   select (content.sender.sender_type) {
 *     case member: MAC membership_tag;
 *   };
 * } PublicMessage;
 * ──────────────────────────────────────────────────────────────────────── */

typedef struct {
    MlsFramedContent         content;
    MlsFramedContentAuthData auth;
    bool                     has_membership_tag;
    uint8_t                  membership_tag[MLS_HASH_LEN];
} MlsPublicMessage;

/** Free a PublicMessage's heap data. */
void mls_public_message_clear(MlsPublicMessage *msg);

/** Serialize a PublicMessage to TLS wire format. */
int mls_public_message_serialize(const MlsPublicMessage *msg, MlsTlsBuf *buf);

/** Deserialize a PublicMessage from TLS wire format. */
int mls_public_message_deserialize(MlsTlsReader *reader, MlsPublicMessage *msg);

/**
 * Compute the membership tag for a PublicMessage.
 *
 * membership_tag = MAC(membership_key, AuthenticatedContentTBM)
 *
 * @param msg              The PublicMessage (content + auth must be filled)
 * @param membership_key   From epoch secrets
 * @param group_context    Serialized GroupContext
 * @param group_context_len Length of group_context
 * @return 0 on success (fills msg->membership_tag)
 */
int mls_public_message_compute_membership_tag(MlsPublicMessage *msg,
                                               const uint8_t membership_key[MLS_HASH_LEN],
                                               const uint8_t *group_context, size_t group_context_len);

/**
 * Verify the membership tag on a PublicMessage.
 */
int mls_public_message_verify_membership_tag(const MlsPublicMessage *msg,
                                              const uint8_t membership_key[MLS_HASH_LEN],
                                              const uint8_t *group_context, size_t group_context_len);

/* ──────────────────────────────────────────────────────────────────────────
 * Confirmation tag (RFC 9420 §8.1)
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * Compute confirmation tag for a Commit message.
 *
 * confirmation_tag = MAC(confirmation_key, confirmed_transcript_hash)
 *
 * @param confirmation_key  From epoch secrets
 * @param confirmed_transcript_hash Current transcript hash
 * @param out               Output tag
 * @return 0 on success
 */
int mls_compute_confirmation_tag(const uint8_t confirmation_key[MLS_HASH_LEN],
                                  const uint8_t confirmed_transcript_hash[MLS_HASH_LEN],
                                  uint8_t out[MLS_HASH_LEN]);

/* ──────────────────────────────────────────────────────────────────────────
 * MLSMessage (RFC 9420 §6)
 *
 * Top-level container for all MLS messages on the wire.
 *
 * struct {
 *   ProtocolVersion version = mls10;
 *   CipherSuite cipher_suite;
 *   WireFormat wire_format;
 *   select (wire_format) {
 *     case mls_public_message:  PublicMessage;
 *     case mls_private_message: PrivateMessage;
 *   };
 * } MLSMessage;
 * ──────────────────────────────────────────────────────────────────────── */

typedef struct {
    uint16_t wire_format;     /**< MLS_WIRE_FORMAT_* */
    uint16_t cipher_suite;    /**< 0x0001 for our suite */
    union {
        MlsPublicMessage  public_message;
        MlsPrivateMessage private_message;
    };
} MlsMLSMessage;

/** Free an MLSMessage's heap data. */
void mls_message_clear(MlsMLSMessage *msg);

/** Serialize an MLSMessage to TLS wire format. */
int mls_message_serialize(const MlsMLSMessage *msg, MlsTlsBuf *buf);

/** Deserialize an MLSMessage from TLS wire format. */
int mls_message_deserialize(MlsTlsReader *reader, MlsMLSMessage *msg);

#ifdef __cplusplus
}
#endif

#endif /* MLS_FRAMING_H */
