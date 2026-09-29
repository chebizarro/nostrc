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
} MarmotCommitKey;

/**
 * Validate the epoch transition `pre` -> `post` made by `committer_leaf`
 * (MIP-01): the committer is a member with a 32-byte account credential; a
 * Commit that adds or removes members or changes the GroupContext
 * extensions is privileged and needs a committer who is an admin of the
 * pre-Commit GroupData; the post-Commit GroupData is present exactly once,
 * well formed, and keeps nostr_group_id.
 *
 * Fills `key` (all but digest) and returns the post-Commit GroupData in
 * *post_gde (caller frees).  Pure: touches no storage.
 */
MarmotError marmot_commit_authorize(const MlsGroup *pre, const MlsGroup *post,
                                    uint32_t committer_leaf,
                                    MarmotCommitKey *key,
                                    MarmotGroupDataExtension **post_gde);

/**
 * Persist an applied epoch transition: the exporter secret of post->epoch,
 * the retained parent (`pre` plus the Commit's ordering key, used to judge a
 * competing Commit for the same epoch), the new MLS state, and `group`
 * (updated from `post_gde` and post->epoch).  Storage has no transactions
 * (nostrc-qp24.7), so a failed write restores every record already written:
 * on error the stored state is as it was.
 */
MarmotError marmot_commit_persist(Marmot *m, const MlsGroup *pre,
                                  const MlsGroup *post,
                                  const MarmotCommitKey *key,
                                  const MarmotGroupDataExtension *post_gde,
                                  MarmotGroup *group);

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
 * Crash recovery (review N2): marmot_commit_persist() stores the MLS state
 * before the group record, so after an interrupted transition the record's
 * epoch lags the state.  Bring `group` (epoch and GroupData fields) up to
 * the stored MLS state and save it.  A no-op when they agree or the group
 * has no MLS state.
 */
MarmotError marmot_group_reconcile(Marmot *m, MarmotGroup *group);

/**
 * Publish-before-merge (MIP-03): a local Commit is staged, not applied.
 * marmot_commit_stage_pending() authorizes `pre` -> `post` like a receiver
 * and stores `post` and the Commit's ordering key as the group's pending
 * Commit (label "mls_group_pending"); the live state stays at `pre`.
 */
MarmotError marmot_commit_stage_pending(Marmot *m, const MlsGroup *pre,
                                        const MlsGroup *post,
                                        const uint8_t *commit, size_t commit_len);

/** Whether the group has a pending local Commit. */
MarmotError marmot_commit_has_pending(Marmot *m, const MarmotGroupId *gid, bool *out);

/**
 * Apply the pending Commit (a relay accepted it): persist it as
 * marmot_commit_persist() does and drop the pending record.
 * MARMOT_ERR_STORAGE_NOT_FOUND without a pending Commit;
 * MARMOT_ERR_WRONG_EPOCH (record dropped) when a competing Commit won while
 * it was pending.  Idempotent after a crash between persisting and dropping.
 */
MarmotError marmot_commit_merge_pending(Marmot *m, MarmotGroup *group);

/**
 * Discard the pending Commit (no relay accepted it) and re-process the
 * inbound Commits that were deferred because they lost to it.  MARMOT_OK
 * when there is nothing pending.
 */
MarmotError marmot_commit_clear_pending(Marmot *m, MarmotGroup *group);

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

#ifdef __cplusplus
}
#endif

#endif /* MARMOT_COMMITS_H */
