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
#include "mls_app_data_update.h"
#include "mls_app_components.h"
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
#define MLS_PROPOSAL_APP_DATA_UPDATE  8
/* draft-ietf-mls-extensions SelfRemove (Marmot registry 0x000a; empty
 * body, the sender leaves).  Never inline: it is sent as a standalone
 * PublicMessage and committed by reference by another member.  Since
 * 0.12.0 (nostrc-2um6). */
#define MLS_PROPOSAL_SELF_REMOVE   0x000A

#define MLS_RESUMPTION_PSK_CACHE_SIZE 8
#define MLS_OWN_PATH_KEY_CACHE_SIZE 128

typedef struct {
    bool     valid;
    uint64_t epoch;
    uint8_t  psk[MLS_HASH_LEN];
} MlsResumptionPskCacheEntry;

typedef struct {
    bool     valid;
    uint32_t node;
    uint8_t  sk[MLS_KEM_SK_LEN];
    uint8_t  pk[MLS_KEM_PK_LEN];
} MlsOwnPathKeyCacheEntry;

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

        /** PreSharedKey proposal: external or resumption PSK id plus nonce. */
        struct {
            uint8_t  psk_type;  /* 1 = external, 2 = resumption */

            uint8_t *psk_id;
            size_t   psk_id_len;

            uint8_t  resumption_usage;
            uint8_t *resumption_group_id;
            size_t   resumption_group_id_len;
            uint64_t resumption_epoch;

            uint8_t *psk_nonce;
            size_t   psk_nonce_len;
        } psk;

        /** GroupContextExtensions proposal: serialized Extension vector. */
        struct {
            uint8_t *extensions;
            size_t   extensions_len;
        } group_context_extensions;

        /** MLS extensions draft AppDataUpdate; parsed but not yet applied. */
        MlsAppDataUpdate app_data_update;
    };

    /** Target leaf for an Update proposal.  An Update replaces the LeafNode of
     *  the member that *sent* the proposal, not the committer; it is taken
     *  from the framing of a by-reference Update.  UINT32_MAX (an inline
     *  Update, i.e. one the committer generated) is rejected on processing
     *  (RFC 9420 §12.2): the committer updates itself via the UpdatePath. */
    uint32_t update_leaf_index;

    /** The member leaf that sent a by-reference proposal (from its framing,
     *  once resolved); UINT32_MAX for an inline one, whose sender is the
     *  committer.  A SelfRemove removes this leaf. */
    uint32_t sender_leaf;

    /** True when this slot is a referenced proposal (ProposalOrRef type 2)
     *  that has not yet been resolved against an external proposal store. */
    bool     is_ref;
    uint8_t  ref[MLS_HASH_LEN];  /**< ProposalRef hash when is_ref is true */
    size_t   ref_len;

    /** True when the proposal type is recognized but not supported for
     *  processing (ReInit, ExternalInit, AppDataUpdate).
     *  Such a commit is rejected with MARMOT_ERR_UNSUPPORTED. */
    bool     unsupported;
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
    MlsOwnPathKeyCacheEntry own_path_keys[MLS_OWN_PATH_KEY_CACHE_SIZE];

    /* ── Key schedule ─────────────────────────────────────────────────── */
    MlsEpochSecrets epoch_secrets;      /**< Derived epoch secrets */
    MlsSecretTree   secret_tree;        /**< Per-sender message key ratchets */
    MlsResumptionPskCacheEntry resumption_psk_cache[MLS_RESUMPTION_PSK_CACHE_SIZE];

    /* ── Transcript hashes (RFC 9420 §8.2) ────────────────────────────── */
    uint8_t confirmed_transcript_hash[MLS_HASH_LEN];
    uint8_t interim_transcript_hash[MLS_HASH_LEN];

    /* ── Extensions ───────────────────────────────────────────────────── */
    uint8_t       *extensions_data;     /**< Serialized GroupContext extensions */
    size_t         extensions_len;

    /* ── Wire profile (nostrc-qp24.5.1) ───────────────────────────────── */
    /** Classified from the GroupContext when the group is created, joined
     *  or loaded, persisted with the state (serial version 4, as a private
     *  format constant, not this enum's value) and never changed: an
     *  ADOPTED state must always pass mls_group_profile_check(), and every
     *  epoch it enters mls_group_profile_check_entered() -- which is why
     *  every Commit producer in mls_group.c installs its stage through
     *  group_install_checked(). */
    MarmotGroupProfile profile;

    /* ── Configuration ────────────────────────────────────────────────── */
    uint32_t max_forward_distance;      /**< Max forward ratchet for decryption */
} MlsGroup;

/* ──────────────────────────────────────────────────────────────────────────
 * Lifecycle
 * ──────────────────────────────────────────────────────────────────────── */

/** Free all internal resources of an MlsGroup (but not the struct itself). */
void mls_group_free(MlsGroup *g);

/**
 * The structural invariants of @g's profile (nostrc-qp24.5.1): LEGACY needs
 * a GroupContext without an adopted marker; ADOPTED one that passes
 * mls_adopted_group_context_parse() and members that pass
 * mls_adopted_tree_check().  0, or the MARMOT_ERR_* of the first failure.
 * Account proofs are not verified cryptographically here.  Run on every
 * created, loaded or cloned state.
 */
int mls_group_profile_check(const MlsGroup *g);

/**
 * mls_group_profile_check() plus the resulting-epoch invariants of an epoch
 * the group enters (by Welcome or Commit): every admin is a member.
 */
int mls_group_profile_check_entered(const MlsGroup *g);

/**
 * Reduce `g` to what reading the rest of its epoch's application messages
 * needs (nostrc-yuj2; Marmot protocol-core/retained-history.md, "Retained
 * cryptographic material": the app-payload row without the candidate-
 * advancement row).  Kept: the group id, epoch, public tree, GroupContext
 * (transcript hashes, extensions), own leaf index, sender-data secret and
 * secret tree (only its unconsumed ratchets), forward distance.  Wiped: the
 * init secret, membership, confirmation, exporter, external, resumption and
 * epoch-authenticator secrets, the own signature and encryption private
 * keys, the path-key and resumption-PSK caches.  Such a state can no longer
 * process a Commit -- nor, with the Commit it produced, derive the next
 * epoch again -- or send.  mls_group_decrypt() still works; the state
 * serializes as before, with those fields zero.
 */
void mls_group_strip_to_reader(MlsGroup *g);

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

/**
 * mls_group_create() whose creator LeafNode also carries `leaf_extensions`
 * (a serialized Extension list, e.g. the account proof's
 * app_data_dictionary; nostrc-7vyi), covered by its signature.  Every type in
 * it must be one the Marmot leaf capabilities list.
 */
int mls_group_create_with_leaf_extensions(MlsGroup *group,
                                          const uint8_t *group_id, size_t group_id_len,
                                          const uint8_t *credential_identity,
                                          size_t credential_identity_len,
                                          const uint8_t signature_key_private[MLS_SIG_SK_LEN],
                                          const uint8_t *extensions_data,
                                          size_t extensions_len,
                                          const uint8_t *leaf_extensions,
                                          size_t leaf_extensions_len);

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
 * On success, the group state is advanced to the new epoch and the
 * committer keeps the private keys of the path nodes it installed.  The Commit
 * is built on a staged copy: on failure the group is unchanged.
 *
 * @param group     The group state (modified on success)
 * @param kp        KeyPackage of the member to add
 * @param result    Output commit + welcome
 * @return 0 on success
 */
int mls_group_add_member(MlsGroup *group,
                         const MlsKeyPackage *kp,
                         MlsAddResult *result);

/**
 * Add several members in one Commit (RFC 9420 §12.4: one Add proposal per
 * KeyPackage, applied in order, plus the UpdatePath) and one Welcome carrying
 * an EncryptedGroupSecrets entry -- with that joiner's LCA path secret -- per
 * KeyPackage.  At most 64 KeyPackages; the same member twice (duplicate leaf
 * keys) is MARMOT_ERR_INVALID_ARG.  On failure the group is unchanged.
 */
int mls_group_add_members(MlsGroup *group,
                          const MlsKeyPackage *const *kps, size_t kp_count,
                          MlsAddResult *result);

#ifdef MARMOT_TEST_HOOKS
/* Tests only (nostrc-zbmb), and only in builds with tests
 * (MARMOT_TEST_HOOKS): when true, our Adds skip the check that a joiner
 * supports what the group requires, so a test can make the Commit a
 * non-conforming client sends and check that receivers refuse it. */
extern bool mls_test_allow_unsupported_adds;
#endif

/**
 * mls_group_add_members() with a GroupContextExtensions proposal in the same
 * Commit (when `extensions` is not NULL): the group's extension list becomes
 * `extensions` (RFC 9420 §12.3 applies it before the Adds and evaluates them
 * against it; the resulting tree and GroupContext are the same in either
 * order), and the Welcome carries it.  Every member, the joiners included,
 * must support it
 * (MARMOT_ERR_UNSUPPORTED or MARMOT_ERR_INVALID_ARG otherwise).  libmarmot
 * re-encodes a libmarmot 0.10.0 group's GroupData this way when it adds
 * members (review W24 M1).
 */
int mls_group_add_members_with_extensions(MlsGroup *group,
                                          const MlsKeyPackage *const *kps, size_t kp_count,
                                          const uint8_t *extensions, size_t extensions_len,
                                          MlsAddResult *result);

/**
 * mls_group_add_members() whose Commit first removes `removes` (at most 64
 * occupied leaves, never our own): Remove proposals, then Adds, in one
 * Commit with one UpdatePath.  RFC 9420 applies Removes first and an Add
 * takes the leftmost blank leaf, so an Add may land in a removed member's
 * slot.  Marmot treats every such slot as a new member (admins only, a new
 * identity claim: marmot_commit_authorize()); libmarmot's tests use it to
 * build the slot takeover W24 review B1 found.  On failure the group is
 * unchanged.
 */
int mls_group_replace_members(MlsGroup *group,
                              const uint32_t *removes, size_t remove_count,
                              const MlsKeyPackage *const *kps, size_t kp_count,
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
 * state is advanced to the new epoch with the member's leaf blanked; on
 * failure it is unchanged.
 *
 * @param group       The group state (modified on success)
 * @param leaf_index  Leaf index of the member to remove
 * @param result      Output serialized commit
 * @return 0 on success
 */
int mls_group_remove_member(MlsGroup *group,
                            uint32_t leaf_index,
                            MlsCommitResult *result);

/**
 * Remove several members in one Commit (one Remove proposal each, plus the
 * UpdatePath).  Every leaf must be a current member other than us, named
 * once; otherwise MARMOT_ERR_INVALID_ARG.  On failure the group is unchanged.
 */
int mls_group_remove_members(MlsGroup *group,
                             const uint32_t *leaves, size_t leaf_count,
                             MlsCommitResult *result);

/* ──────────────────────────────────────────────────────────────────────────
 * Self-update
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * Perform a self-update of the committer's leaf node.
 *
 * Creates an empty Commit whose UpdatePath carries a new leaf node (fresh
 * encryption key) and new path keys, which the committer retains.  Advances
 * the group to a new epoch on success; on failure the group is unchanged.
 *
 * @param group   The group state (modified on success)
 * @param result  Output serialized commit
 * @return 0 on success
 */
int mls_group_self_update(MlsGroup *group,
                          MlsCommitResult *result);

/**
 * mls_group_self_update() whose UpdatePath leaf carries `leaf_ext` (a
 * serialized LeafNode Extension list, e.g. the account-identity proof's
 * app_data_dictionary; nostrc-rgb5) instead of the current leaf's
 * extensions.  The signature key and credential do not change.
 */
int mls_group_self_update_with_leaf_extensions(MlsGroup *group,
                                               const uint8_t *leaf_ext, size_t leaf_ext_len,
                                               MlsCommitResult *result);

/* ──────────────────────────────────────────────────────────────────────────
 * GroupContext extensions update
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * Replace the group's GroupContext extensions: commit a
 * GroupContextExtensions proposal (RFC 9420 §12.1.7) carrying `extensions`
 * (a serialized Extension list) together with the UpdatePath it requires.
 * Receivers apply it through mls_group_process_commit().
 *
 * The list must be well formed, free of duplicates and of extension types
 * libmarmot does not apply, and every member must support each extension
 * (and any required_capabilities); otherwise MARMOT_ERR_UNSUPPORTED (or
 * MARMOT_ERR_INVALID_ARG for a malformed list) is returned.  On success the
 * group is advanced to the new epoch; on failure it is unchanged.
 */
int mls_group_commit_extensions(MlsGroup *group,
                                const uint8_t *extensions, size_t extensions_len,
                                MlsCommitResult *result);

/* ──────────────────────────────────────────────────────────────────────────
 * Process incoming Commit
 *
 * Called by non-committing members when they receive a Commit.
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * Whether @commit_data is a Commit of this group and epoch that member
 * @sender_leaf signed (RFC 9420 sections 6.1-6.3: a PublicMessage's
 * signature and membership tag, or a PrivateMessage decrypted and its
 * signature checked), whatever its content: 0, or the error.  Nothing is
 * applied.
 */
int mls_group_commit_authentic(const MlsGroup *group, const uint8_t *commit_data,
                               size_t commit_len, uint32_t sender_leaf);

/**
 * Whether the Commit @commit_data from member @sender_leaf in @group's epoch
 * removes @group's own leaf (RFC 9420 §12.4.2; OpenMLS's self_removed).  A
 * removed member cannot process such a Commit: its UpdatePath is encrypted
 * to the remaining members only.  What it can check is checked: the
 * framing for this group and epoch, the committer's signature and, for a
 * PublicMessage, the membership tag (the same checks mls_group_process_commit()
 * makes first; a PrivateMessage is decrypted first, since 0.11.0), then that the proposal list is well formed, carries an UpdatePath
 * and that every inline Remove names an occupied leaf other than the
 * committer's.  Proposals by reference are not resolved and never count.
 *
 * @return 0 with *out_removed set; MARMOT_ERR_MLS_PROCESS_MESSAGE for a
 *   Commit that does not authenticate or is malformed (*out_removed false)
 */
int mls_group_commit_removes_self(const MlsGroup *group,
                                  const uint8_t *commit_data, size_t commit_len,
                                  uint32_t sender_leaf, bool *out_removed);

/**
 * The member leaf that sent the handshake MLSMessage @msg (a Proposal or
 * Commit) in @group's epoch: a PublicMessage names it in its FramedContent; a
 * PrivateMessage (RFC 9420 section 6.3.2, OpenMLS MIXED_CIPHERTEXT: how MDK
 * sends Commits) in its sender data, decrypted with @group's
 * sender_data_secret. No ratchet key is used. Only routes: the Commit is
 * authenticated when it is processed. Since 0.11.0.
 *
 * @return 0 with *out_leaf set; MARMOT_ERR_MLS_FRAMING or
 *   MARMOT_ERR_MLS_PROCESS_MESSAGE otherwise (another group or epoch, a
 *   sender data that does not decrypt, an empty leaf)
 */
int mls_group_handshake_sender(const MlsGroup *group, const uint8_t *msg, size_t msg_len,
                               uint32_t *out_leaf);

/**
 * The leaf the sender data of the application PrivateMessage @msg names,
 * decrypted with @sender_data_secret (RFC 9420 section 6.3.2) of the state
 * of @group_id at @epoch -- without that state at hand.  No ratchet key is
 * used.  Only routes: the message is authenticated when it is decrypted.
 * Since 0.12.0 (nostrc-w1m0: who sent a candidate branch's message).
 *
 * @return 0 with *out_leaf set; MARMOT_ERR_MLS_FRAMING or
 *   MARMOT_ERR_MLS_PROCESS_MESSAGE otherwise
 */
int mls_private_message_sender_leaf(const uint8_t sender_data_secret[32],
                                    const uint8_t *group_id, size_t group_id_len,
                                    uint64_t epoch, const uint8_t *msg, size_t msg_len,
                                    uint32_t *out_leaf);

/**
 * Process an incoming Commit message.
 *
 * Validates the commit, applies proposals, decrypts the UpdatePath
 * (if present), and advances the group to the new epoch.  A Commit without an
 * UpdatePath is rejected with MARMOT_ERR_MLS_PROCESS_MESSAGE when it covers no
 * proposals or any Update, Remove or GroupContextExtensions proposal (RFC 9420
 * §12.4; ExternalInit, which also requires a path, is rejected earlier as
 * unsupported).  A rejected Commit leaves the group unchanged.
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

/**
 * Process a Commit that may contain referenced proposals (ProposalOrRef
 * type 2, RFC 9420 §12.4).
 *
 * The referenced proposals are supplied as previously-received standalone
 * Proposal MLSMessages (PublicMessage wire format).  Each is parsed and its
 * ProposalRef = RefHash("MLS 1.0 Proposal Reference", AuthenticatedContent)
 * computed so the commit's references can be resolved.  Behaves exactly like
 * mls_group_process_commit() for inline proposals.
 *
 * Commits carrying proposal types that libmarmot does not yet apply
 * (currently ReInit / ExternalInit) are rejected with
 * MARMOT_ERR_UNSUPPORTED rather than a generic framing error.  Commits with
 * external PreSharedKey proposals require mls_group_process_commit_ex_with_psks()
 * so the application can supply the external PSK material.
 *
 * @param group           The group state (modified on success)
 * @param commit_data     Serialized Commit MLSMessage
 * @param commit_len      Length of commit data
 * @param sender_leaf     Leaf index of the committer
 * @param proposal_msgs   Array of serialized standalone Proposal MLSMessages
 * @param proposal_lens   Length of each proposal message
 * @param proposal_count  Number of referenced-proposal messages
 * @return 0 on success, MARMOT_ERR_UNSUPPORTED for unsupported proposal types
 */
int mls_group_process_commit_ex(MlsGroup *group,
                                const uint8_t *commit_data, size_t commit_len,
                                uint32_t sender_leaf,
                                const uint8_t *const *proposal_msgs,
                                const size_t *proposal_lens,
                                size_t proposal_count);

/**
 * Process a Commit with referenced proposals and externally supplied PSK
 * material.  PSK proposals are applied in commit order; external PSKs are
 * matched by psk_id and their proposal nonce is used for RFC 9420 §8.4
 * psk_secret computation.  Resumption PSKs for the current group/epoch are
 * resolved from group->epoch_secrets.resumption_psk.
 */
int mls_group_process_commit_ex_with_psks(MlsGroup *group,
                                          const uint8_t *commit_data,
                                          size_t commit_len,
                                          uint32_t sender_leaf,
                                          const uint8_t *const *proposal_msgs,
                                          const size_t *proposal_lens,
                                          size_t proposal_count,
                                          const MlsPskInput *external_psks,
                                          size_t external_psk_count);

/* ──────────────────────────────────────────────────────────────────────────
 * Standalone proposals and Commits by reference (since 0.12.0, nostrc-2um6)
 *
 * A standalone Proposal is authenticated once, in the epoch it was sent in,
 * and kept as its AuthenticatedContent (RFC 9420 section 6.1): a
 * PrivateMessage cannot be decrypted twice, and the bytes are what its
 * ProposalRef hashes.  A Commit of that epoch resolves its references
 * against those records; each record's signature is checked again then.
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * MlsOpenedProposal:
 *
 * A standalone Proposal authenticated in a group's epoch.
 */
typedef struct {
    uint8_t  *ac;                 /**< AuthenticatedContent bytes (caller frees) */
    size_t    ac_len;
    uint8_t   ref[MLS_HASH_LEN];  /**< ProposalRef */
    uint16_t  wire_format;        /**< it arrived as: MLS_WIRE_FORMAT_PUBLIC/PRIVATE_MESSAGE */
    uint32_t  sender_leaf;        /**< the member that sent it */
    uint16_t  type;               /**< MLS_PROPOSAL_* */
    uint32_t  target_leaf;        /**< Remove: removed leaf; SelfRemove: the sender; else UINT32_MAX */
    uint16_t  component_id;       /**< AppDataUpdate: the component it changes; else 0 */
} MlsOpenedProposal;

/** Free an opened proposal's bytes. */
void mls_opened_proposal_clear(MlsOpenedProposal *p);

/**
 * Authenticate the standalone Proposal MLSMessage @msg in @group's epoch:
 * a PublicMessage by its signature and membership tag, a PrivateMessage by
 * decrypting it with its sender's handshake ratchet (left as it was, as for
 * a Commit) and then its signature (wire format mls_private_message).
 *
 * Forward secrecy (review N2): putting the ratchet back means the sender's
 * generation key for a PrivateMessage proposal is not deleted before the
 * epoch ends (RFC 9420 section 9.2 asks for that), and a replayed
 * ciphertext decrypts again (it is deduplicated by ProposalRef and event
 * id). Accepted for MDK 0.8's PrivateMessage Remove-of-self: the content is
 * a public leave request, and the epoch's secrets stay stored until the
 * next Commit anyway. Our own PrivateMessage proposal (its key is spent) is
 * MARMOT_ERR_OWN_MESSAGE.  The
 * body must parse completely and be of a type a Commit can apply (Add,
 * Update, Remove, GroupContextExtensions, SelfRemove); a SelfRemove must come
 * as a PublicMessage (draft-ietf-mls-extensions, MIP-03), a Remove must name
 * an occupied leaf.
 *
 * @return 0; MARMOT_ERR_MLS_FRAMING (not a Proposal MLSMessage);
 *   MARMOT_ERR_WRONG_GROUP_ID; MARMOT_ERR_WRONG_EPOCH (another epoch);
 *   MARMOT_ERR_UNSUPPORTED (another proposal type);
 *   MARMOT_ERR_MLS_PROCESS_MESSAGE (does not authenticate, malformed)
 */
int mls_group_open_proposal(const MlsGroup *group, const uint8_t *msg, size_t msg_len,
                            MlsOpenedProposal *out);

/** Whether every member leaf of @group advertises proposal type @type in its
 *  capabilities (RFC 9420 section 12.2; a default type always counts). */
bool mls_group_members_support_proposal(const MlsGroup *group, uint16_t type);

/** Whether @group's GroupContext required_capabilities list proposal type
 *  @type (MDK 0.8 sends SelfRemove only then; review M1). */
bool mls_group_requires_proposal(const MlsGroup *group, uint16_t type);

/**
 * MlsCommitSummary:
 *
 * What a processed Commit did with the leaves of its members' own departure
 * requests, for the protocol layer's authorization.
 */
/* A deliberate bound (review N3): a Commit with more departures than this
 * (SelfRemoves, or Removes members sent for themselves) is refused, where
 * OpenMLS would accept it; it matters only for very large groups. */
#define MLS_COMMIT_SUMMARY_MAX 64
typedef struct {
    size_t   proposal_count;                       /**< all proposals of the Commit */
    size_t   self_remove_count;                    /**< SelfRemove proposals */
    uint32_t self_removed[MLS_COMMIT_SUMMARY_MAX]; /**< their senders, the leaves they removed */
    size_t   left_count;                           /**< by-reference Removes a member sent for itself */
    uint32_t left[MLS_COMMIT_SUMMARY_MAX];         /**< those leaves */

    /* The Commit's shape (nostrc-qp24.5.1.3), for the adopted profile's
     * authorization (group-messaging.md "Commit authorization"): set by the
     * Commit processor (shape_known), never stored with a pending Commit.
     * A producer's summary leaves it unknown and the Marmot layer derives
     * the shape of our own Commit from the states. */
    bool     shape_known;
    bool     has_path;                             /**< an UpdatePath */
    size_t   add_count, remove_count, update_count;
    size_t   gce_count;                            /**< GroupContextExtensions */
    size_t   adu_count;                            /**< AppDataUpdate (0x0008) */
    /* For MDK's lifecycle transition rules (validate_group_lifecycle_
     * transition; slice H review M1): AppDataUpdates of 0x800c, inline or by
     * reference, and inline AppDataUpdates of 0x0001 or 0x800c. */
    size_t   adu_lifecycle_count;
    size_t   adu_inline_enablement_count;
    size_t   other_count;                          /**< PSK, ReInit, ExternalInit, unknown */
    /* By-reference proposals other than SelfRemove: their senders (leaves of
     * the source epoch) and types; their authority is the Marmot layer's. */
    size_t   ref_count;
    uint32_t ref_sender[MLS_COMMIT_SUMMARY_MAX];
    uint16_t ref_type[MLS_COMMIT_SUMMARY_MAX];
    uint16_t ref_component[MLS_COMMIT_SUMMARY_MAX]; /**< an AppDataUpdate's component id */
} MlsCommitSummary;

/**
 * mls_group_process_commit() for a Commit whose references resolve against
 * @acs (AuthenticatedContent records of this epoch's opened proposals; each
 * is checked again: this group and epoch, a member sender, its signature).
 * A SelfRemove may only be referenced, never by its own sender, and only
 * when every member supports it; a leaf may be removed once.  @summary
 * (nullable) is filled on success.
 */
int mls_group_process_commit_by_ref(MlsGroup *group,
                                    const uint8_t *commit_data, size_t commit_len,
                                    uint32_t sender_leaf,
                                    const uint8_t *const *acs, const size_t *ac_lens,
                                    size_t ac_count, MlsCommitSummary *summary);

/**
 * mls_group_commit_removes_self() resolving references against @acs: a
 * referenced SelfRemove of our own leaf, or a referenced Remove of it,
 * counts too.  @summary (nullable) gets what the Commit did with
 * departure requests (filled when *out_removed).
 */
int mls_group_commit_removes_self_by_ref(const MlsGroup *group,
                                         const uint8_t *commit_data, size_t commit_len,
                                         uint32_t sender_leaf,
                                         const uint8_t *const *acs, const size_t *ac_lens,
                                         size_t ac_count, bool *out_removed,
                                         MlsCommitSummary *summary);

/**
 * The public result of a Commit from @sender_leaf (slice H review L1):
 * mls_group_process_commit_by_ref()'s every check up to, not including, the
 * decryption of the UpdatePath secret, then the resulting-epoch check
 * (mls_group_profile_check_entered()).  *out is a copy of @group with the
 * Commit's proposals applied, the committer's leaf replaced by the
 * UpdatePath's (parent nodes are not updated) and the epoch advanced: no
 * key schedule, no secrets of the new epoch; for judging (authorization,
 * leaves, GroupContext), never for installing.  A member the Commit removes
 * can compute it.  Free with mls_group_free().  @summary as for
 * mls_group_process_commit_by_ref().  @group is unchanged.
 */
int mls_group_commit_public_result_by_ref(const MlsGroup *group,
                                          const uint8_t *commit_data, size_t commit_len,
                                          uint32_t sender_leaf,
                                          const uint8_t *const *acs, const size_t *ac_lens,
                                          size_t ac_count, MlsGroup *out,
                                          MlsCommitSummary *summary);

/**
 * An adopted-profile Commit (nostrc-qp24.5.1.3) of Removes of @removes,
 * Adds of @kps and inline AppDataUpdate proposals @adus (applied to the
 * GroupContext app_data_dictionary as receivers apply them,
 * mls_app_data_update_apply()), with an UpdatePath; in that proposal order.
 * Any of the three may be empty, not all.  With Adds, result->welcome_data
 * is the Welcome.  Only for an adopted group (MARMOT_ERR_UNSUPPORTED
 * otherwise); installed through the resulting-epoch check like every
 * producer.  On failure the group is unchanged.
 */
int mls_group_commit_adopted(MlsGroup *group,
                             const uint32_t *removes, size_t remove_count,
                             const MlsKeyPackage *const *kps, size_t kp_count,
                             const MlsAppDataUpdate *adus, size_t adu_count,
                             MlsAddResult *result);

/**
 * Commit, by reference, the opened proposals @acs of this epoch (SelfRemove
 * and Remove only: the departures), with an UpdatePath.  Each must be from
 * another member and remove a different leaf, never ours; SelfRemove needs
 * every member's support.  On success the group advances (on failure it is
 * unchanged), as for mls_group_remove_members().
 *
 * @return 0; MARMOT_ERR_INVALID_ARG for a set that cannot be committed
 */
int mls_group_commit_by_ref(MlsGroup *group,
                            const uint8_t *const *acs, const size_t *ac_lens, size_t ac_count,
                            MlsCommitResult *result);

/**
 * A Remove of our own leaf for @group's epoch (MDK 0.8's leave where the
 * group does not require SelfRemove; review M1): a PrivateMessage (signed,
 * encrypted with our handshake ratchet, whose step is taken in @group: store
 * it), and *out_own kept as receivers keep it. An admin commits it.
 */
int mls_group_remove_self_proposal(MlsGroup *group, uint8_t **out_msg, size_t *out_len,
                                   MlsOpenedProposal *out_own);

/**
 * Our own SelfRemove proposal for @group's epoch: a PublicMessage (signed,
 * with the membership tag) in *out_msg, and *out_own opened as a receiver
 * would (to keep for the Commit that will reference it).  The group is not
 * changed.  MARMOT_ERR_UNSUPPORTED when some member does not support
 * SelfRemove.
 */
int mls_group_self_remove_proposal(const MlsGroup *group, uint8_t **out_msg, size_t *out_len,
                                   MlsOpenedProposal *out_own);

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

/* At most this many proposals in a Commit: one AppDataUpdate per component
 * id (MLS_APP_DATA_UPDATE_MAX), and 1024 for every other kind (the receive
 * path applies at most 64 Adds, 64 Updates and MLS_COMMIT_SUMMARY_MAX
 * departures of each kind, one GroupContextExtensions).  A longer one is
 * refused while it is parsed, before any proposal is sorted or applied
 * (slice H re-review R2). */
#define MLS_COMMIT_MAX_PROPOSALS ((size_t)MLS_APP_DATA_UPDATE_MAX + 1024u)

/** Deserialize a Commit from TLS wire format (at most
 *  MLS_COMMIT_MAX_PROPOSALS proposals). */
int mls_commit_deserialize(MlsTlsReader *reader, MlsCommit *commit);

/** Serialize an UpdatePath to TLS wire format. */
int mls_update_path_serialize(const MlsUpdatePath *up, MlsTlsBuf *buf);

/** Deserialize an UpdatePath from TLS wire format. */
int mls_update_path_deserialize(MlsTlsReader *reader, MlsUpdatePath *up);

/* ──────────────────────────────────────────────────────────────────────────
 * TreeKEM UpdatePath helpers
 * ──────────────────────────────────────────────────────────────────────── */

/** Derive commit_secret = DeriveSecret(root_path_secret, "path"). */
int mls_treekem_commit_secret_from_path_secret(const uint8_t root_path_secret[MLS_HASH_LEN],
                                               uint8_t out[MLS_HASH_LEN]);

/**
 * Decrypt one encrypted path secret from an UpdatePathNode for a node in the
 * copath resolution.  The caller provides the target node's HPKE private key;
 * the public key is read from the ratchet tree.
 */
int mls_treekem_update_path_decrypt_secret(const MlsRatchetTree *tree,
                                           const MlsUpdatePathNode *path_node,
                                           uint32_t copath_node_idx,
                                           uint32_t resolution_node_idx,
                                           const uint8_t *group_context,
                                           size_t group_context_len,
                                           const uint8_t node_enc_sk[MLS_KEM_SK_LEN],
                                           uint8_t out_path_secret[MLS_HASH_LEN]);

/**
 * Apply an UpdatePath to a ratchet tree and reconstruct parent hashes.
 * The path must carry exactly one node per filtered direct path node of
 * sender_leaf (RFC 9420 §7.6); parent_hash links follow the filtered path,
 * the topmost filtered node has an empty parent_hash, and the new leaf's
 * parent_hash must equal the parent hash of the bottom filtered node (empty
 * when the filtered path is empty) (RFC 9420 §7.9).
 * The tree is mutated in place and may be partially updated if validation
 * fails; callers that need rollback should apply to a staged copy.
 */
int mls_treekem_apply_update_path(MlsRatchetTree *tree,
                                  uint32_t sender_leaf,
                                  const MlsUpdatePath *path);

/**
 * Install the path keys a joiner learns from its Welcome (RFC 9420
 * §12.4.3.1, nostrc-il4i).  `path_secret` is GroupSecrets.path_secret: the
 * path secret of the lowest common ancestor of the joiner (group->
 * own_leaf_index) and the committer (`committer_leaf`, GroupInfo.signer).
 * The key pair of that node and of every node above it on the committer's
 * filtered direct path is derived; each public key must equal the node's key
 * in group->tree.  On success the private keys join group->own_path_keys; on
 * any mismatch MARMOT_ERR_WELCOME_INVALID is returned and the cache is left
 * as it was.
 */
int mls_group_welcome_install_path_secret(MlsGroup *group, uint32_t committer_leaf,
                                          const uint8_t path_secret[MLS_HASH_LEN]);

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
    uint8_t *group_info_extensions_data;     /**< Serialized GroupInfo extensions */
    size_t   group_info_extensions_len;
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

/* ──────────────────────────────────────────────────────────────────────────
 * Group state serialization (internal persistence)
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * Serialize an MlsGroup to a binary blob for storage.
 * Caller frees *out_data.
 *
 * @return 0 on success
 */
int mls_group_serialize(const MlsGroup *group,
                        uint8_t **out_data, size_t *out_len);

/**
 * Deserialize an MlsGroup from a binary blob.
 * On success, group is fully initialized (including secret tree).
 *
 * @return 0 on success
 */
int mls_group_deserialize(const uint8_t *data, size_t len,
                          MlsGroup *group);

#ifdef __cplusplus
}
#endif

#endif /* MLS_GROUP_H */
