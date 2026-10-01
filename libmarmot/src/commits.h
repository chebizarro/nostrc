/*
 * libmarmot - Commit publication and ingestion (MIP-01/MIP-03, nostrc-9ata)
 *
 * Internal: shared by the Commit producers in groups.c and by
 * marmot_process_message() in messages.c.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef MARMOT_COMMITS_H
#define MARMOT_COMMITS_H

#include "marmot-internal.h"
#include "mls/mls_group.h"
#include <nostr-event.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* mls_kv label of the retained parent state (see marmot_commit_persist()). */
#define MARMOT_MLS_PARENT_LABEL "mls_group_parent"
/* mls_kv label of who removed our leaf (marmot_get_group_removal()). */
#define MARMOT_MLS_REMOVED_LABEL "mls_group_removed"

/**
 * W22 review B2: an event of the removed group that none of our exporter
 * secrets opens is of a later epoch -- the group moved on without us.  A
 * Commit that could still beat the removal is of the removal's epoch and
 * would open, so it is never counted.  Each such event (by id, once) is
 * counted in the removal record; after MARMOT_REMOVAL_FINAL_AFTER the removal
 * is final and the removed epoch's keys are deleted.  *out_final: final now.
 * In the caller's transaction.
 */
MarmotError marmot_commit_removal_note_later(Marmot *m, MarmotGroup *group,
                                             const char *event_id_hex, bool *out_final);

/**
 * The order in which marmot_clear_pending_commit() would replay the stored
 * pending Commit's deferred Commits (indices in arrival order), winner
 * first.  For tests.
 */
MarmotError marmot_commit_deferred_replay_order(Marmot *m, const MarmotGroupId *gid,
                                                size_t *order, size_t max, size_t *out_count);

/**
 * nostrc-qp24.5.1.3: the group an address it had before a routing rotation
 * names (marmot_get_group_routing()'s previous addresses).
 * MARMOT_ERR_GROUP_NOT_FOUND when none.
 */
MarmotError marmot_commit_find_group_by_alias(Marmot *m, const uint8_t nostr_group_id[32],
                                              MarmotGroup **out);

/** A Welcome made us a member again: forget an earlier removal. */
MarmotError marmot_commit_clear_removal(Marmot *m, const MarmotGroupId *gid);

/**
 * MarmotCommitKey:
 *
 * The authenticated ordering key of an applied Commit (Marmot convergence,
 * "Same-epoch races": CommitOrderingSuffix).  Lower wins: privileged before
 * ordinary, then the lower committer account key, then the lower digest.
 */
typedef struct {
    bool    privileged;       /**< committer had to be an admin (Add, Remove, GroupData change) */
    uint8_t committer[32];    /**< committer's Nostr account key (leaf credential) */
    uint8_t digest[32];       /**< SHA-256 of the Commit MLSMessage bytes */
    uint32_t committer_leaf;  /**< its leaf in the parent (marmot_commit_authorize()) */
} MarmotCommitKey;

/**
 * Validate the epoch transition `pre` -> `post` made by `committer_leaf`
 * (MIP-01): the committer is a member with a 32-byte account credential; a
 * Commit that adds or removes members or changes the GroupContext
 * extensions is privileged and needs a committer who is an admin of the
 * pre-Commit GroupData; the post-Commit GroupData is present exactly once,
 * well formed, and keeps nostr_group_id.
 *
 * Account binding (nostrc-7vyi): every leaf the Commit adds, or whose slot
 * now holds another account, carries a valid account-identity proof, else
 * MARMOT_ERR_KEY_PACKAGE_IDENTITY; with `allow_unproven`
 * (MarmotConfig.allow_unproven_members) one without any proof passes, in a
 * legacy-profile group only (`pre` and `post`; nostrc-6ukh).  A member's
 * replaced leaf (Update, UpdatePath) keeps its identity (the MLS layer pins
 * it) and may not drop a proof it had; outside the legacy profile it needs
 * one.  A proof that does not verify always fails.
 *
 * Fills `key` (all but digest) and returns the post-Commit GroupData in
 * *post_gde (caller frees).  Pure: touches no storage.
 */
MarmotError marmot_commit_authorize(const MlsGroup *pre, const MlsGroup *post,
                                    uint32_t committer_leaf, bool allow_unproven,
                                    MarmotCommitKey *key,
                                    MarmotGroupDataExtension **post_gde);

/**
 * marmot_commit_authorize() knowing what the Commit did with departure
 * requests (`departures`, from the MLS layer; NULL: nothing known, as
 * before 0.12.0).  nostrc-2um6: a SelfRemove's sender may not be an admin
 * of `pre` (marmot_policy_is_admin(); MARMOT_ERR_ADMIN_CANNOT_LEAVE), and a
 * Commit of SelfRemove proposals only is ordinary -- any member may commit
 * it -- while every other change still makes it privileged.
 */
MarmotError marmot_commit_authorize_ex(const MlsGroup *pre, const MlsGroup *post,
                                       uint32_t committer_leaf, bool allow_unproven,
                                       const MlsCommitSummary *departures,
                                       MarmotCommitKey *key,
                                       MarmotGroupDataExtension **post_gde);

/**
 * Persist an applied epoch transition: the exporter secret of post->epoch,
 * the retained history (`pre`, the Commit's ordering key and its MLSMessage
 * become the newest entry: commits.c, "Retained history"), the new MLS
 * state, and `group` (updated from `post_gde` and post->epoch).  It runs
 * inside the operation's storage transaction (nostrc-qp24.7), which makes
 * the four writes atomic; for backends without transactions a failed write
 * also restores every record already written, so on error the stored state
 * is as it was.  marmot_commit_persist() does not know the Commit's bytes:
 * a later reorg cannot retain that Commit as a candidate.
 */
MarmotError marmot_commit_persist(Marmot *m, const MlsGroup *pre,
                                  const MlsGroup *post,
                                  const MarmotCommitKey *key,
                                  const MarmotGroupDataExtension *post_gde,
                                  MarmotGroup *group);

/** marmot_commit_persist() of the Commit `commit` (its MLSMessage; ours
 *  when `own`), kept in the retained history (nostrc-w1m0). */
MarmotError marmot_commit_persist_ex(Marmot *m, const MlsGroup *pre,
                                     const MlsGroup *post,
                                     const MarmotCommitKey *key,
                                     const MarmotGroupDataExtension *post_gde,
                                     MarmotGroup *group,
                                     const uint8_t *commit, size_t commit_len, bool own);

/**
 * A late application message (MLS PrivateMessage `msg`) of `epoch`, an
 * epoch before the current one: decrypt it with the retained canonical
 * state of that epoch (the last five are retained: the app-payload window,
 * nostrc-w1m0) and store that state's advanced ratchet.  *out_tag: that
 * state's confirmed transcript hash (for marmot_commit_note_witness()).
 * MARMOT_ERR_OWN_MESSAGE for our own message; MARMOT_ERR_STORAGE_NOT_FOUND
 * when no state of that epoch is retained; MARMOT_ERR_MLS when it does not
 * decrypt.
 *
 * On success *out_replaced holds the retained-parent record as it was
 * before (caller wipes and frees it): the caller writes it back under
 * MARMOT_MLS_PARENT_LABEL if a later write of the same operation fails, so
 * on a storage without transactions the ratchet step does not outlive a
 * message that was never stored (nostrc-ai04).  *out_sender_identity is the
 * sender leaf's account identity in that state (nostrc-we6g).
 */
MarmotError marmot_commit_decrypt_late(Marmot *m, const MarmotGroupId *gid,
                                       uint64_t epoch,
                                       const uint8_t *msg, size_t msg_len,
                                       uint8_t **out_plaintext, size_t *out_len,
                                       uint32_t *out_sender,
                                       uint8_t out_sender_identity[32],
                                       uint8_t out_tag[32],
                                       uint8_t **out_replaced, size_t *out_replaced_len);

/**
 * nostrc-w1m0 (convergence.md "App-payload witnesses"): an application
 * message of `epoch`, from the account `sender`, decrypted on the state
 * whose confirmed transcript hash is `tag` and passed every payload check
 * (the inner author is the MLS sender); `canonical`: that state is the
 * canonical one of its epoch.  The witness is recorded once per
 * (state, account), up to the quorum; when it may change the selected
 * branch (a candidate state's, or an earlier epoch's, while branches are
 * retained) the retained branches are resolved again: a change is reported
 * in result->convergence and `group` follows it.  On a change of the
 * record *out_replaced holds it as it was (caller wipes and frees it, and
 * writes it back if a later write of the operation fails).  A storage
 * error fails the operation; an unreadable record is left alone.
 */
MarmotError marmot_commit_note_witness(Marmot *m, MarmotGroup *group, uint64_t epoch,
                                       const uint8_t tag[32], const uint8_t sender[32],
                                       bool canonical,
                                       uint8_t **out_replaced, size_t *out_replaced_len,
                                       MarmotMessageResult *result);

/**
 * The exporter secret of a retained candidate (non-canonical) state: its
 * kind:445 events peel with it (inbound-processing.md "Transport-deferred
 * input").  `wants_witness`: its witness count is below the quorum, so an
 * application message of it is still worth reading.
 */
typedef struct {
    uint64_t epoch;
    uint8_t  tag[32];        /* the state's confirmed transcript hash */
    uint8_t  exporter[32];
    bool     wants_witness;
} MarmotBranchSecret;

/** The retained candidate states' exporter secrets of `gid` (*out NULL when
 *  none).  Free with marmot_branch_secrets_free(). */
MarmotError marmot_commit_branch_secrets(Marmot *m, const MarmotGroupId *gid,
                                         MarmotBranchSecret **out, size_t *out_count);
void marmot_branch_secrets_free(MarmotBranchSecret *list, size_t count);

/** Whether the state `tag` of `epoch` is canonical now (the tip or a
 *  retained canonical epoch). */
bool marmot_commit_state_canonical(Marmot *m, const MarmotGroupId *gid, uint64_t epoch,
                                   const uint8_t tag[32]);

/**
 * An application message `msg` of the candidate state `tag` of `epoch`
 * (peeled with its branch secret): decrypt it on that state, rebuilt by
 * replaying its branch (nothing kept is spent).  *out_sender_identity: the
 * sender leaf's account.  MARMOT_ERR_STORAGE_NOT_FOUND when that state is
 * no longer a candidate; MARMOT_ERR_OWN_MESSAGE; MARMOT_ERR_MLS.
 */
MarmotError marmot_commit_branch_decrypt(Marmot *m, MarmotGroup *group, uint64_t epoch,
                                         const uint8_t tag[32], const uint8_t *msg,
                                         size_t msg_len, uint8_t **out_plaintext,
                                         size_t *out_len, uint8_t out_sender_identity[32]);

/**
 * nostrc-7vyi (joining.md step 5): every member leaf of `g` is bound to the
 * account its credential names -- by a valid account-identity proof, or it
 * is `g`'s own leaf or the `exempt` leaf (UINT32_MAX: none).  A leaf without
 * any proof passes only with `allow_unproven` (MarmotConfig.
 * allow_unproven_members); a proof that does not verify never does.  Both
 * `exempt` and `allow_unproven` apply only to a legacy-profile `g`
 * (nostrc-6ukh).  MARMOT_ERR_KEY_PACKAGE_IDENTITY otherwise.
 * The joiner applies it to a Welcome's tree (exempting the GroupInfo signer
 * when it sent the Welcome), the inviter to the tree its Welcome carries.
 */
MarmotError marmot_tree_members_bound(const MlsGroup *g, uint32_t exempt, bool allow_unproven);

/**
 * The Marmot account identity (32-byte Nostr public key) the credential of
 * `g`'s leaf `leaf` binds, into `out`.  -1 for a blank leaf or a credential
 * that is not a 32-byte identity (nostrc-we6g).
 */
int marmot_mls_sender_identity(const MlsGroup *g, uint32_t leaf, uint8_t out[32]);

/** Mirror the committed GroupData (name, description, admins) into `group`. */
MarmotError marmot_group_apply_group_data(MarmotGroup *group,
                                          const MarmotGroupDataExtension *gde);

/**
 * Build the kind:445 event carrying a Commit MLSMessage: the bytes are
 * NIP-44-encrypted with the exporter secret of the epoch the Commit was
 * created in (`source_exporter`), exactly like application messages
 * (MIP-03), routed by the `h` tag, and signed by a fresh ephemeral key.
 * Dated `created_at`, which the caller reserves with
 * marmot_next_group_event_time() (nostrc-2lrz).  Returns NULL on failure.
 */
char *marmot_commit_build_event(const uint8_t *commit_msg, size_t commit_len,
                                const uint8_t source_exporter[32],
                                const uint8_t nostr_group_id[32], int64_t created_at);

/**
 * Sign `event` with a freshly generated secp256k1 key that is wiped
 * afterwards (MIP-03: every kind:445 has its own ephemeral author, never the
 * account key).  Sets pubkey, id and sig.  Returns 0 on success.
 */
int marmot_sign_ephemeral(NostrEvent *event);

/**
 * Crash recovery (review N2) for storage without transactions:
 * marmot_commit_persist() stores the MLS state before the group record, so
 * after an interrupted transition the record's epoch lags the state (with
 * the transaction hooks, nostrc-qp24.7, that cannot happen).  Bring `group` (epoch and GroupData fields) up to
 * the stored MLS state and save it.  A no-op when they agree or the group
 * has no MLS state.
 */
MarmotError marmot_group_reconcile(Marmot *m, MarmotGroup *group);

/**
 * Publish-before-merge (MIP-03): a local Commit is staged, not applied.
 * marmot_commit_stage_pending() authorizes `pre` -> `post` like a receiver
 * and stores, as the group's pending Commit (label "mls_group_pending"):
 * the exact parent it was built on (epoch and confirmed transcript hash),
 * `post`, the ordering key, the signed `event_json` (to republish after a
 * restart) and the Add's Welcomes with their recipients.  The live state
 * stays at `pre`.
 */
MarmotError marmot_commit_stage_pending(Marmot *m, const MlsGroup *pre,
                                        const MlsGroup *post,
                                        const uint8_t *commit, size_t commit_len,
                                        const char *event_json,
                                        const MarmotUnsentWelcome *welcomes,
                                        size_t welcome_count);

/** marmot_commit_stage_pending() of a Commit of departures (nostrc-2um6):
 *  `departures` is authorized with it and kept in the pending record, so
 *  the merge authorizes it the same way. */
MarmotError marmot_commit_stage_pending_ex(Marmot *m, const MlsGroup *pre,
                                           const MlsGroup *post,
                                           const uint8_t *commit, size_t commit_len,
                                           const char *event_json,
                                           const MarmotUnsentWelcome *welcomes,
                                           size_t welcome_count,
                                           const MlsCommitSummary *departures);

/**
 * The group's pending Commit: its signed event (NULL when there is none) and
 * whether it is still built on the current state (`*out_live`; otherwise a
 * competing Commit replaced that state and merging will fail).  A record
 * whose Commit is already applied (crash before its removal) is finished
 * here and reported as none.
 */
MarmotError marmot_commit_get_pending(Marmot *m, MarmotGroup *group,
                                      char **out_event_json, bool *out_live);

/** Whether the group has a pending local Commit (live or superseded). */
MarmotError marmot_commit_has_pending(Marmot *m, MarmotGroup *group, bool *out);

/**
 * Apply the pending Commit (a relay accepted it), move its Welcomes to the
 * unsent-Welcome outbox and drop the record.
 * - MARMOT_ERR_STORAGE_NOT_FOUND without a pending Commit.
 * - MARMOT_ERR_WRONG_EPOCH (record dropped) when the state it was built on
 *   was replaced by a competing Commit (review R1).
 * - An already applied Commit (crash, or our relay echo) is finished: OK.
 * - A Commit that can no longer pass authorization is dropped (its error);
 *   a storage error keeps it (merge again, or clear).
 */
MarmotError marmot_commit_merge_pending(Marmot *m, MarmotGroup *group);

/**
 * Discard the pending Commit (no relay accepted it) and re-process the
 * inbound Commits that were deferred because they lost to it.  MARMOT_OK
 * when there is nothing pending; an already applied Commit is kept (its
 * Welcomes go to the outbox).
 */
MarmotError marmot_commit_clear_pending(Marmot *m, MarmotGroup *group);

/** The unsent-Welcome outbox (Welcomes of merged Adds; append-only,
 *  entries removed by id once sent). */
MarmotError marmot_commit_get_unsent_welcomes(Marmot *m, const MarmotGroupId *gid,
                                              MarmotUnsentWelcome **out,
                                              size_t *out_count);
MarmotError marmot_commit_mark_welcomes_sent(Marmot *m, const MarmotGroupId *gid,
                                             const uint8_t (*ids)[32], size_t id_count);

/* Tests and diagnostics: how a Commit (MLSMessage bytes) of the stored
 * current epoch of `gid` would be judged -- the specific refusal of
 * staging and authorization (and of a removal of our leaf), before any
 * routing-record check and without MARMOT_ERR_COMMIT_REFUSED's mapping.
 * Nothing is stored. */
MarmotError marmot_commit_judge(Marmot *m, const MarmotGroupId *gid, const uint8_t *msg,
                                size_t msg_len);

/**
 * Apply a received Commit (`msg`, an MLSMessage PublicMessage already
 * recovered from the kind:445 NIP-44 layer with the exporter secret of
 * `outer_epoch`).  See marmot_process_message() for the epoch rules.  On
 * success fills result (MARMOT_RESULT_COMMIT with the updated group, or
 * MARMOT_RESULT_OWN_MESSAGE for an already-applied Commit).  On failure
 * nothing is stored.
 */
MarmotError marmot_commit_process_inbound(Marmot *m, MarmotGroup *group,
                                          uint64_t outer_epoch,
                                          const uint8_t *msg, size_t msg_len,
                                          const char *event_id_hex,
                                          MarmotMessageResult *result);

/** marmot_commit_process_inbound() of a Commit whose kind:445 event the
 *  exporter secret of the candidate state `parent_tag` (NULL: the canonical
 *  state of `outer_epoch`) opened: that state is the Commit's parent
 *  (nostrc-w1m0). */
MarmotError marmot_commit_process_inbound_ex(Marmot *m, MarmotGroup *group,
                                             uint64_t outer_epoch, const uint8_t *parent_tag,
                                             const uint8_t *msg, size_t msg_len,
                                             const char *event_id_hex,
                                             MarmotMessageResult *result);

/* The content encryption of kind:445 events (MIP-03, messages.c): keyed by
 * an epoch's RFC 9420 exporter_secret (the key is MLS-Exporter("marmot",
 * "group-event", 32) of it), or by that key itself. */
int marmot_group_event_encrypt(const uint8_t exporter_secret[32],
                               const uint8_t *plaintext, size_t plaintext_len,
                               char **out_base64);
int marmot_group_event_decrypt(const uint8_t exporter_secret[32],
                               const char *base64_payload,
                               uint8_t **out_plaintext, size_t *out_len);
int marmot_group_event_encrypt_with_key(const uint8_t key[32],
                                        const uint8_t *plaintext, size_t plaintext_len,
                                        char **out_base64);
int marmot_group_event_decrypt_with_key(const uint8_t key[32], const char *base64_payload,
                                        uint8_t **out_plaintext, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* MARMOT_COMMITS_H */
