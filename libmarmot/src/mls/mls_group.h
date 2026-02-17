/*
 * libmarmot - MLS Group State Machine (RFC 9420 §11, §12)
 *
 * Manages the MLS group state: ratchet tree, key schedule, epoch secrets,
 * transcript hashes. Supports group creation, member addition/removal,
 * self-update, and application message encrypt/decrypt.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef MLS_GROUP_H
#define MLS_GROUP_H

#include "mls-internal.h"
#include "mls_tree.h"
#include "mls_key_schedule.h"
#include "mls_key_package.h"
#include "mls_framing.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ──────────────────────────────────────────────────────────────────────────
 * Proposal types (RFC 9420 §12.1)
 * ──────────────────────────────────────────────────────────────────────── */

#define MLS_PROPOSAL_ADD            1
#define MLS_PROPOSAL_UPDATE         2
#define MLS_PROPOSAL_REMOVE         3
#define MLS_PROPOSAL_PSK            4
#define MLS_PROPOSAL_REINIT         5
#define MLS_PROPOSAL_EXTERNAL_INIT  6
#define MLS_PROPOSAL_GROUP_CONTEXT_EXT 7

/* ──────────────────────────────────────────────────────────────────────────
 * MlsProposal - A single proposal within a Commit
 * ──────────────────────────────────────────────────────────────────────── */

typedef struct {
    uint16_t type;  /**< MLS_PROPOSAL_* */
    union {
        /** Add proposal: contains the KeyPackage to add */
        struct {
            MlsKeyPackage key_package;
        } add;

        /** Update proposal: new LeafNode for the sender */
        struct {
            MlsLeafNode leaf_node;
        } update;

        /** Remove proposal: leaf index to remove */
        struct {
            uint32_t removed_leaf;
        } remove;
    };
} MlsProposal;

/** Free proposal internals. */
void mls_proposal_clear(MlsProposal *p);

/* ──────────────────────────────────────────────────────────────────────────
 * MlsCommit - A Commit message (RFC 9420 §12.4)
 *
 * struct {
 *   Proposal proposals<V>;
 *   optional<UpdatePath> path;
 * } Commit;
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * MlsUpdatePathNode:
 *
 * A node in the UpdatePath, containing an HPKE-encrypted path secret
 * for each copath resolution member.
 */
typedef struct {
    uint8_t   encryption_key[MLS_KEM_PK_LEN]; /**< New HPKE public key */
    uint8_t  *encrypted_path_secrets;          /**< Serialized HPKECiphertext array */
    size_t    encrypted_path_secrets_len;
    uint32_t  secret_count;                    /**< Number of encrypted secrets */
} MlsUpdatePathNode;

/**
 * MlsUpdatePath:
 *
 * UpdatePath sent with a Commit to provide new keys along the committer's
 * direct path.
 */
typedef struct {
    MlsLeafNode       leaf_node;    /**< New leaf node for committer */
    MlsUpdatePathNode *nodes;       /**< Path nodes (one per filtered direct path) */
    size_t             node_count;
} MlsUpdatePath;

/** Free update path internals. */
void mls_update_path_clear(MlsUpdatePath *up);

/**
 * MlsCommit:
 *
 * A Commit message that applies proposals and optionally updates the
 * committer's path.
 */
typedef struct {
    MlsProposal *proposals;   /**< Inline proposals */
    size_t        proposal_count;
    bool          has_path;    /**< Whether an UpdatePath is present */
    MlsUpdatePath path;       /**< The update path (if has_path) */
} MlsCommit;

/** Free commit internals. */
void mls_commit_clear(MlsCommit *c);

/* ──────────────────────────────────────────────────────────────────────────
 * MlsGroup - The core group state machine
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * MlsGroup:
 *
 * Complete MLS group state for a single epoch. This struct is the
 * central data structure for group operations.
 */
typedef struct {
    /* ── Identity ─────────────────────────────────────────────────────── */
    uint8_t       *group_id;            /**< MLS group ID */
    size_t         group_id_len;
    uint64_t       epoch;               /**< Current epoch number */

    /* ── Ratchet tree ─────────────────────────────────────────────────── */
    MlsRatchetTree tree;                /**< The ratchet tree */

    /* ── Own state ────────────────────────────────────────────────────── */
    uint32_t       own_leaf_index;      /**< Our leaf index in the tree */
    uint8_t        own_signature_key[MLS_SIG_SK_LEN]; /**< Our Ed25519 private key */
    uint8_t        own_encryption_key[MLS_KEM_SK_LEN]; /**< Our X25519 encryption private key */

    /* ── Key schedule ─────────────────────────────────────────────────── */
    MlsEpochSecrets epoch_secrets;      /**< Derived epoch secrets */
    MlsSecretTree   secret_tree;        /**< Per-sender message key ratchets */

    /* ── Transcript hashes (RFC 9420 §8.2) ────────────────────────────── */
    uint8_t confirmed_transcript_hash[MLS_HASH_LEN];
    uint8_t interim_transcript_hash[MLS_HASH_LEN];

    /* ── Extensions ───────────────────────────────────────────────────── */
    uint8_t       *extensions_data;     /**< Serialized GroupContext extensions */
    size_t         extensions_len;

    /* ── Configuration ────────────────────────────────────────────────── */
    uint32_t max_forward_distance;      /**< Max forward ratchet for decryption */
} MlsGroup;

/* ──────────────────────────────────────────────────────────────────────────
 * Lifecycle
 * ──────────────────────────────────────────────────────────────────────── */

/** Free all internal resources of an MlsGroup (but not the struct itself). */
void mls_group_free(MlsGroup *g);

/* ──────────────────────────────────────────────────────────────────────────
 * Group creation (RFC 9420 §11)
 *
 * Creates a new single-member group. The creator becomes leaf 0.
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * Create a new MLS group with the caller as the sole member.
 *
 * @param group               Output group state
 * @param group_id            Group identifier
 * @param group_id_len        Length of group_id
 * @param credential_identity Identity for BasicCredential (e.g. nostr pubkey)
 * @param credential_identity_len Length of credential_identity
 * @param signature_key_private  Caller's Ed25519 signing key (64 bytes, libsodium format)
 * @param extensions_data     GroupContext extensions (can be NULL)
 * @param extensions_len      Length of extensions
 * @return 0 on success, negative error code on failure
 */
int mls_group_create(MlsGroup *group,
                     const uint8_t *group_id, size_t group_id_len,
                     const uint8_t *credential_identity, size_t credential_identity_len,
                     const uint8_t signature_key_private[MLS_SIG_SK_LEN],
                     const uint8_t *extensions_data, size_t extensions_len);

/* ──────────────────────────────────────────────────────────────────────────
 * Add member (Commit + Welcome)
 *
 * Adds a member by their KeyPackage. Produces:
 *   1) A Commit (serialized for broadcast)
 *   2) A Welcome (for the new member)
 *   3) Updated group state
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * MlsAddResult:
 *
 * Result of adding a member to the group.
 */
typedef struct {
    uint8_t *commit_data;       /**< Serialized Commit message */
    size_t   commit_len;
    uint8_t *welcome_data;      /**< Serialized Welcome message */
    size_t   welcome_len;
} MlsAddResult;

/** Free add result internals. */
void mls_add_result_clear(MlsAddResult *r);

/**
 * Add a member to the group.
 *
 * This creates a Commit containing an Add proposal for the given
 * KeyPackage, generates an UpdatePath, derives a new epoch, and
 * produces a Welcome for the new member.
 *
 * On success, the group state is advanced to the new epoch.
 *
 * @param group     The group state (modified on success)
 * @param kp        KeyPackage of the member to add
 * @param result    Output commit + welcome
 * @return 0 on success
 */
int mls_group_add_member(MlsGroup *group,
                         const MlsKeyPackage *kp,
                         MlsAddResult *result);

/* ──────────────────────────────────────────────────────────────────────────
 * Remove member
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * MlsCommitResult:
 *
 * Result of a Commit operation (remove, self-update).
 */
typedef struct {
    uint8_t *commit_data;     /**< Serialized Commit message */
    size_t   commit_len;
} MlsCommitResult;

/** Free commit result internals. */
void mls_commit_result_clear(MlsCommitResult *r);

/**
 * Remove a member from the group.
 *
 * Creates a Commit containing a Remove proposal. On success, the group
 * state is advanced to the new epoch with the member's leaf blanked.
 *
 * @param group       The group state (modified on success)
 * @param leaf_index  Leaf index of the member to remove
 * @param result      Output serialized commit
 * @return 0 on success
 */
int mls_group_remove_member(MlsGroup *group,
                            uint32_t leaf_index,
                            MlsCommitResult *result);

/* ──────────────────────────────────────────────────────────────────────────
 * Self-update
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * Perform a self-update of the committer's leaf node.
 *
 * Creates a Commit with an Update proposal containing a new leaf node
 * (fresh encryption key). Advances the group to a new epoch.
 *
 * @param group   The group state (modified on success)
 * @param result  Output serialized commit
 * @return 0 on success
 */
int mls_group_self_update(MlsGroup *group,
                          MlsCommitResult *result);

/* ──────────────────────────────────────────────────────────────────────────
 * Process incoming Commit
 *
 * Called by non-committing members when they receive a Commit.
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * Process an incoming Commit message.
 *
 * Validates the commit, applies proposals, decrypts the UpdatePath
 * (if present), and advances the group to the new epoch.
 *
 * @param group        The group state (modified on success)
 * @param commit_data  Serialized Commit message
 * @param commit_len   Length of commit data
 * @param sender_leaf  Leaf index of the committer
 * @return 0 on success
 */
int mls_group_process_commit(MlsGroup *group,
                             const uint8_t *commit_data, size_t commit_len,
                             uint32_t sender_leaf);

/* ──────────────────────────────────────────────────────────────────────────
 * Application messages
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * Encrypt an application message.
 *
 * @param group          The group state
 * @param plaintext      Application data to encrypt
 * @param plaintext_len  Length of plaintext
 * @param out_data       Output encrypted PrivateMessage (caller frees)
 * @param out_len        Output length
 * @return 0 on success
 */
int mls_group_encrypt(MlsGroup *group,
                      const uint8_t *plaintext, size_t plaintext_len,
                      uint8_t **out_data, size_t *out_len);

/**
 * Decrypt an application message.
 *
 * @param group          The group state
 * @param ciphertext     Serialized PrivateMessage
 * @param ciphertext_len Length of ciphertext
 * @param out_plaintext  Output decrypted data (caller frees)
 * @param out_pt_len     Output length
 * @param out_sender_leaf Output sender's leaf index (can be NULL)
 * @return 0 on success
 */
int mls_group_decrypt(MlsGroup *group,
                      const uint8_t *ciphertext, size_t ciphertext_len,
                      uint8_t **out_plaintext, size_t *out_pt_len,
                      uint32_t *out_sender_leaf);

/* ──────────────────────────────────────────────────────────────────────────
 * GroupContext helpers
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * Compute the serialized GroupContext for the current epoch.
 * Caller frees *out_data.
 */
int mls_group_context_build(const MlsGroup *group,
                            uint8_t **out_data, size_t *out_len);

/**
 * Compute the tree hash of the current ratchet tree.
 */
int mls_group_tree_hash(const MlsGroup *group, uint8_t out[MLS_HASH_LEN]);

/* ──────────────────────────────────────────────────────────────────────────
 * Commit serialization
 * ──────────────────────────────────────────────────────────────────────── */

/** Serialize a Commit to TLS wire format. */
int mls_commit_serialize(const MlsCommit *commit, MlsTlsBuf *buf);

/** Deserialize a Commit from TLS wire format. */
int mls_commit_deserialize(MlsTlsReader *reader, MlsCommit *commit);

/** Serialize an UpdatePath to TLS wire format. */
int mls_update_path_serialize(const MlsUpdatePath *up, MlsTlsBuf *buf);

/** Deserialize an UpdatePath from TLS wire format. */
int mls_update_path_deserialize(MlsTlsReader *reader, MlsUpdatePath *up);

/* ──────────────────────────────────────────────────────────────────────────
 * Group info (for Welcome construction)
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * MlsGroupInfo:
 *
 * GroupInfo is included in Welcome messages so joiners can initialize
 * their group state.
 */
typedef struct {
    /* GroupContext fields */
    uint8_t *group_id;
    size_t   group_id_len;
    uint64_t epoch;
    uint8_t  tree_hash[MLS_HASH_LEN];
    uint8_t  confirmed_transcript_hash[MLS_HASH_LEN];
    uint8_t *extensions_data;
    size_t   extensions_len;

    /* GroupInfo-specific fields */
    uint8_t  confirmation_tag[MLS_HASH_LEN]; /**< MAC over confirmed_transcript_hash */
    uint32_t signer_leaf;                    /**< Which leaf signed this GroupInfo */
    uint8_t  signature[MLS_SIG_LEN];
    size_t   signature_len;
} MlsGroupInfo;

/** Free GroupInfo internals. */
void mls_group_info_clear(MlsGroupInfo *gi);

/** Serialize GroupInfo to TLS wire format. */
int mls_group_info_serialize(const MlsGroupInfo *gi, MlsTlsBuf *buf);

/** Deserialize GroupInfo from TLS wire format. */
int mls_group_info_deserialize(MlsTlsReader *reader, MlsGroupInfo *gi);

/**
 * Build GroupInfo from the current group state.
 */
int mls_group_info_build(const MlsGroup *group, MlsGroupInfo *gi);

#ifdef __cplusplus
}
#endif

#endif /* MLS_GROUP_H */
