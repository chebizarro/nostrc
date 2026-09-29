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
 * MARMOT_ERR_KEY_PACKAGE_IDENTITY; with `allow_unproven` (legacy mode) one
 * without any proof passes.  A member's replaced leaf (Update, UpdatePath)
 * keeps its identity (the MLS layer pins it) and may not drop a proof it
 * had.  A proof that does not verify always fails.
 *
 * Fills `key` (all but digest) and returns the post-Commit GroupData in
 * *post_gde (caller frees).  Pure: touches no storage.
 */
MarmotError marmot_commit_authorize(const MlsGroup *pre, const MlsGroup *post,
                                    uint32_t committer_leaf, bool allow_unproven,
                                    MarmotCommitKey *key,
                                    MarmotGroupDataExtension **post_gde);

/**
 * Persist an applied epoch transition: the exporter secret of post->epoch,
 * the retained parent (`pre` plus the Commit's ordering key, used to judge a
 * competing Commit for the same epoch and to read late application messages
 * of `pre`'s epoch; reduced to the late-message part at once when no member
 * could publish a winning competitor, nostrc-yuj2), the new MLS state, and
 * `group` (updated from `post_gde` and post->epoch).  It runs inside the operation's storage transaction
 * (nostrc-qp24.7), which makes the four writes atomic; for backends without
 * transactions a failed write also restores every record already written,
 * so on error the stored state is as it was.
 */
MarmotError marmot_commit_persist(Marmot *m, const MlsGroup *pre,
                                  const MlsGroup *post,
                                  const MarmotCommitKey *key,
                                  const MarmotGroupDataExtension *post_gde,
                                  MarmotGroup *group);

/**
 * A late application message (MLS PrivateMessage `msg`) of `epoch`, the
 * epoch before the current one: decrypt it with the retained parent state
 * (the state the last applied Commit was built on, kept for one epoch) and
 * store that state's advanced ratchet.  The retention horizon is libmarmot's
 * one-epoch rewind: older messages cannot be read.  MARMOT_ERR_OWN_MESSAGE
 * for our own message; MARMOT_ERR_STORAGE_NOT_FOUND when no state of that
 * epoch is retained; MARMOT_ERR_MLS when it does not decrypt.
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
                                       uint8_t **out_replaced, size_t *out_replaced_len);

/**
 * nostrc-yuj2: an application message from `sender_leaf` decrypted and
 * authenticated in `cur`'s epoch.  If the retained parent still keeps its
 * full state waiting for that member (it could have published a winning
 * competing Commit), it is off the list now; when nobody is left, the parent
 * is reduced to what reads late messages (mls_group_strip_to_reader()).  On
 * a change *out_replaced holds the record as it was (caller wipes and frees
 * it, and writes it back if a later write of the operation fails).  A
 * storage error fails the operation; an unreadable record is left alone.
 */
MarmotError marmot_commit_note_witness(Marmot *m, const MlsGroup *cur, uint32_t sender_leaf,
                                       uint8_t **out_replaced, size_t *out_replaced_len);

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
 * Returns NULL on failure.
 */
char *marmot_commit_build_event(const uint8_t *commit_msg, size_t commit_len,
                                const uint8_t source_exporter[32],
                                const uint8_t nostr_group_id[32]);

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

/* NIP-44 layer of kind:445 events keyed by an epoch exporter secret
 * (messages.c). */
int marmot_group_event_encrypt(const uint8_t exporter_secret[32],
                               const uint8_t *plaintext, size_t plaintext_len,
                               char **out_base64);
int marmot_group_event_decrypt(const uint8_t exporter_secret[32],
                               const char *base64_payload,
                               uint8_t **out_plaintext, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* MARMOT_COMMITS_H */
