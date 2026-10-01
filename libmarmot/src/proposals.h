/*
 * libmarmot - Standalone proposals and leaving (nostrc-2um6)
 *
 * Internal: shared by commits.c (Commit processing, authorization), groups.c
 * (the producers) and messages.c (the Leaving gate).
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef MARMOT_PROPOSALS_H
#define MARMOT_PROPOSALS_H

#include "marmot-internal.h"
#include "mls/mls_group.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* mls_kv labels: the index of the group's kept proposals (key: group id;
 * the records themselves are "mls_group_proposal_slot", one per epoch and
 * sender, see proposals.c), and our leave request (marmot_self_remove()). */
#define MARMOT_MLS_PROPOSALS_LABEL "mls_group_proposals"
#define MARMOT_MLS_LEAVING_LABEL   "mls_group_leaving"
/* Records kept per sender and epoch (review M2): enough for a member's
 * distinct leave proposals of one epoch (member-departure.md keeps them
 * all), never another member's room. */
#define MARMOT_PROPOSALS_PER_SENDER 4
/* Slots per group (epochs x senders), a safety bound. */
#define MARMOT_PROPOSAL_SLOTS_MAX 4096

/** One opened standalone proposal, as stored. */
typedef struct {
    uint64_t epoch;          /* the epoch it was sent in */
    bool     own;            /* ours (our SelfRemove) */
    uint32_t sender_leaf;
    uint16_t type;           /* MLS_PROPOSAL_* */
    uint32_t target_leaf;    /* the leaf it removes, UINT32_MAX for other types */
    uint8_t  ref[32];        /* ProposalRef */
    uint8_t  digest[32];     /* SHA-256 of the complete MLSMessage (member-departure.md) */
    uint8_t  sender[32];     /* the sender leaf's account when it was opened */
    bool     has_target;
    uint8_t  target[32];     /* the target leaf's account */
    uint8_t *ac;             /* AuthenticatedContent */
    size_t   ac_len;
} MarmotStoredProposal;

/** A group's stored proposals, and views of one epoch's for the MLS layer. */
typedef struct {
    MarmotStoredProposal *items;
    size_t                count;
    const uint8_t       **acs;      /* the records of `epoch` (marmot_proposals_load_epoch()) */
    size_t               *ac_lens;
    size_t                ac_count;
} MarmotProposalSet;

void marmot_proposals_clear(MarmotProposalSet *set);

/** All of a group's stored proposals (none: an empty set). */
MarmotError marmot_proposals_load(Marmot *m, const uint8_t *gid, size_t gid_len,
                                  MarmotProposalSet *out);

/** The stored proposals with set->acs/ac_lens/ac_count viewing those of
 *  `epoch` (what a Commit of that epoch may reference). */
MarmotError marmot_proposals_load_epoch(Marmot *m, const uint8_t *gid, size_t gid_len,
                                        uint64_t epoch, MarmotProposalSet *out);

/** Drop proposals of epochs before `keep_from` (a Commit made them stale;
 *  the retained parent's epoch is kept for a competing Commit).  Best
 *  effort: a failure leaves records that the epoch checks ignore. */
void marmot_proposals_prune(Marmot *m, const uint8_t *gid, size_t gid_len, uint64_t keep_from);

/** Forget a group's proposals and leave request (removal final, left). */
void marmot_proposals_forget(Marmot *m, const uint8_t *gid, size_t gid_len);

/**
 * A standalone Proposal MLSMessage `msg` for `group` (routed: of this group
 * and of the epoch whose exporter secret opened it): authenticate it in the
 * current epoch, check it against the group's policy, and keep it.  On
 * success fills `result` (MARMOT_RESULT_PROPOSAL, or OWN_MESSAGE for our
 * own echo).  Errors as marmot_process_message(); nothing is stored then.
 */
MarmotError marmot_proposal_process_inbound(Marmot *m, MarmotGroup *group,
                                            const uint8_t *msg, size_t msg_len,
                                            const char *event_id_hex,
                                            MarmotMessageResult *result);

/**
 * The group's admin policy (a profile hook): whether `account` is an admin
 * in the authenticated state `g`.  Legacy (MIP-01) groups: listed in the
 * marmot_group_data 0xF2EE admins.  Adopted groups (a GroupContext
 * app_data_dictionary): listed in marmot.group.admin-policy.v1 (0x8003).
 * MARMOT_ERR_EXTENSION_FORMAT when the policy cannot be read (callers fail
 * closed).
 */
MarmotError marmot_policy_is_admin(const MlsGroup *g, const uint8_t account[32], bool *out);

/** Whether `g` is an adopted-profile group (its GroupContext carries an
 *  app_data_dictionary): there only SelfRemove may be a non-admin's
 *  standalone proposal (protocol-core/group-messaging.md). */
bool marmot_policy_is_adopted(const MlsGroup *g);

/** MARMOT_ERR_LEAVING while our leave request for the group stands. */
MarmotError marmot_leaving_gate(Marmot *m, const MarmotGroupId *gid);

/** The proposals of `cur`'s epoch marmot_commit_pending_proposals() commits
 *  (one per leaving member), as indices into `set`'s items.  *out (caller
 *  frees) may be empty. */
MarmotError marmot_proposals_select(const MlsGroup *cur, const MarmotProposalSet *set,
                                    size_t **out, size_t *out_count);

/** Whether stored proposal `p` is one marmot_proposals_select() may pick on
 *  `cur` (before the one-per-leaver choice). */
bool marmot_proposal_committable(const MlsGroup *cur, const MarmotStoredProposal *p);

/* The public operations (groups.c holds the Commit producer). */
MarmotError marmot_self_remove_impl(Marmot *m, const MarmotGroupId *gid, char **out_event_json);

#ifdef __cplusplus
}
#endif

#endif /* MARMOT_PROPOSALS_H */
