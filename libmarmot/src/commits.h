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
 * (MIP-03), and routed by the `h` tag.  Unsigned; the caller signs with an
 * ephemeral key.  Returns NULL on failure.
 */
char *marmot_commit_build_event(const uint8_t *commit_msg, size_t commit_len,
                                const uint8_t source_exporter[32],
                                const uint8_t nostr_group_id[32]);

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
