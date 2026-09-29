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

/** A single external PSK input for RFC 9420 §8.4 psk_secret computation. */
typedef struct {
    /** 0 or 1 = external PSK, 2 = resumption PSK. */
    uint8_t psk_type;

    const uint8_t *psk_id;
    size_t psk_id_len;

    uint8_t resumption_usage;
    const uint8_t *resumption_group_id;
    size_t resumption_group_id_len;
    uint64_t resumption_epoch;

    const uint8_t *psk;
    size_t psk_len;
    const uint8_t *psk_nonce;
    size_t psk_nonce_len;
} MlsPskInput;

/**
 * Compute psk_secret from an ordered list of external PSKs (RFC 9420 §8.4).
 *
 * This implements the PSK extraction chain used as input to the epoch key
 * schedule.  With psk_count == 0, out is the all-zero KDF.Nh vector.
 */
int mls_psk_secret_compute(const MlsPskInput *psks, size_t psk_count,
                           uint8_t out[MLS_HASH_LEN]);

/* ──────────────────────────────────────────────────────────────────────────
 * Secret tree & message keys (RFC 9420 §9)
 *
 * The secret tree derives per-sender encryption keys from the
 * encryption_secret. It uses the same left-balanced binary tree
 * structure as the ratchet tree.
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * The out-of-order window of every sender ratchet chain (RFC 9420 §15.3
 * policy, nostrc-ai04).  The keys of skipped generations are retained only
 * while they are among the MLS_SECRET_TREE_MAX_SKIPPED_MESSAGE_KEYS
 * generations just below the newest generation read from that chain, and
 * only until used: a message older than that, or one whose key was
 * consumed, fails closed.  The cache holds at most this many keys per chain,
 * persisted with the group state.
 */
#define MLS_SECRET_TREE_MAX_SKIPPED_MESSAGE_KEYS 32

/**
 * Generations the group's own sender ratchets are moved forward when a state
 * persisted without them (MlsGroup serial format 1 or 2) is loaded: past any
 * generation such a state may already have used, and within the forward
 * distance receivers accept (libmarmot 0.8.0 and later, OpenMLS and MDK
 * default to 1000; libmarmot 0.7.0 and earlier read no generation above 32
 * at all).  Through the public API those formats never sent at a generation
 * above 0 (the ratchet restarted on every load, nostrc-ai04).
 */
#define MLS_SECRET_TREE_LEGACY_OWN_STRIDE 512

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

/** A cached skipped message key, consumed at most once. */
typedef struct {
    bool valid;
    MlsMessageKeys keys;
} MlsSkippedMessageKey;

/**
 * MlsSenderRatchet:
 *
 * Per-sender ratchet state for deriving message keys.
 * Each sender gets a handshake and application key chain plus bounded
 * skipped-key caches for out-of-order decryption.
 */
typedef struct {
    uint8_t  handshake_secret[MLS_HASH_LEN];
    uint8_t  application_secret[MLS_HASH_LEN];
    uint32_t handshake_generation;
    uint32_t application_generation;
    MlsSkippedMessageKey handshake_skipped[MLS_SECRET_TREE_MAX_SKIPPED_MESSAGE_KEYS];
    MlsSkippedMessageKey application_skipped[MLS_SECRET_TREE_MAX_SKIPPED_MESSAGE_KEYS];
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

/* A sender ratchet chain gives out generations 0 .. MLS_RATCHET_GENERATION_MAX;
 * then it is exhausted (a generation is never reused, RFC 9420 §9.1). */
#define MLS_RATCHET_GENERATION_MAX (UINT32_MAX - 1)

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
 * Advances the ratchet forward if needed (up to max_forward_distance),
 * retaining skipped intervening keys in a bounded cache for out-of-order
 * delivery. Past cached keys are consumed once; replays fail.
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

/**
 * Move both of `leaf_index`'s chains `generations` steps forward, deleting
 * every key and secret passed (none is cached).  Used to put the own sender
 * past generations a legacy state may have used.  0 on success.
 */
int mls_secret_tree_skip(MlsSecretTree *st, uint32_t leaf_index, uint32_t generations);

/**
 * A copy of one sender's ratchet state, to undo a key derivation whose
 * message then fails to authenticate (a failed decryption consumes nothing,
 * RFC 9420 §9.2).  Holds secrets: always end it with
 * mls_secret_tree_sender_restore() or mls_secret_tree_sender_discard().
 */
typedef struct {
    uint32_t         leaf_index;
    bool             initialized;
    uint8_t          leaf_secret[MLS_HASH_LEN];
    MlsSenderRatchet ratchet;
} MlsSenderSnapshot;

int  mls_secret_tree_sender_save(const MlsSecretTree *st, uint32_t leaf_index,
                                 MlsSenderSnapshot *out);
void mls_secret_tree_sender_restore(MlsSecretTree *st, MlsSenderSnapshot *snap);
void mls_secret_tree_sender_discard(MlsSenderSnapshot *snap);

/**
 * Persisted form of a secret tree (MlsGroup serial format 3, nostrc-ai04):
 *
 *   u32 n_leaves
 *   per leaf:
 *     u8 0 (ratchets not started): [Nh] leaf secret
 *     u8 1 (ratchets started):     handshake chain, application chain
 *   chain: u32 next generation, [Nh] ratchet secret of that generation,
 *          u8 skipped count (<= MLS_SECRET_TREE_MAX_SKIPPED_MESSAGE_KEYS),
 *          per skipped key: u32 generation, [Nk] key, [Nn] nonce
 *
 * Only unconsumed values are written (RFC 9420 §9.2): the leaf secret of a
 * sender whose ratchets started, every ratchet secret below the next
 * generation and every used key are gone, and cannot be derived from this
 * tree.  The encryption_secret and internal node secrets are never stored.
 * (The group's retained parent state plus the public Commit can derive the
 * whole epoch again; see the format notes in mls_group.c, nostrc-yuj2.)
 */
int mls_secret_tree_serialize(const MlsSecretTree *st, MlsTlsBuf *buf);

/**
 * Read a secret tree written by mls_secret_tree_serialize() for a group of
 * `n_leaves`.  Fails closed (and leaves `st` empty) on anything malformed:
 * another leaf count, an unknown leaf kind, a skipped key outside its
 * chain's out-of-order window or given twice.
 */
int mls_secret_tree_deserialize(MlsTlsReader *reader, uint32_t n_leaves,
                                MlsSecretTree *st);

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

/**
 * MLS-Exporter with binary label (RFC 9420 §8.5).
 *
 * Same as mls_exporter but accepts a raw binary label with explicit length,
 * needed for RFC 9420 test vector validation where labels are arbitrary bytes.
 */
int mls_exporter_raw(const uint8_t exporter_secret[MLS_HASH_LEN],
                     const uint8_t *label, size_t label_len,
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
