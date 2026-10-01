/*
 * libmarmot - Convergence state and branch selection (nostrc-w1m0)
 *
 * Marmot protocol-core/convergence.md and retained-history.md (adopted,
 * marmot-protocol/marmot 07da8ffb), as MDK v0.11.0's cgka-engine runs them
 * (convergence.rs, canonicalization.rs, openmls_projection.rs).
 *
 * Internal: the retained history of a group (states and Commits of the last
 * max_rewind_commits epochs), the Commits retained off the canonical branch
 * (candidates), app-payload witnesses and the exporter secrets of candidate
 * states, kept in the retained-parent record ("mls_group_parent"), and the
 * pure branch comparison.  commits.c replays and applies branches.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef MARMOT_CONVERGENCE_H
#define MARMOT_CONVERGENCE_H

#include "commits.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Convergence policy v1 (convergence.md, "Convergence policy") ────────
 * Protocol constants, never group-tunable.  The two pass timers
 * (settlement_quiescence_ms 1000, max_convergence_pass_ms 5000) are not
 * implemented: libmarmot resolves at once on every admitted input (see
 * commits.c, "Convergence"). */
#define CONV_MAX_REWIND_COMMITS          5
#define CONV_APP_PAYLOAD_PAST_EPOCH_LIMIT 5
#define CONV_WITNESS_QUORUM_SENDERS      2
#define CONV_WITNESS_QUORUM_EPOCHS       1
#define CONV_MAX_WITNESS_OVERRIDE_DEPTH  1

/* ── Local resource bounds (inbound-processing.md "resource_refused") ────
 * They bound the Commits retained off the canonical branch -- the losers
 * only: a Commit the selection makes canonical (a linear advance of the tip
 * included) is never refused for capacity.  Every input is admitted and
 * resolved first; then, over a bound, the losers least likely to win are
 * evicted (the lowest-scoring branch tip first: commits.c,
 * "conv_retained()").  When that is the arriving Commit itself it is
 * MARMOT_ERR_RESOURCE_REFUSED: not kept, never marked processed, eligible
 * again when offered again (transports/nostr.md). */
#define CONV_MAX_CANDIDATES       32   /* Commits retained off the canonical branch */
#define CONV_MAX_PER_COMMITTER     4   /* ...by one authenticated committer */
/* Room for the Commits admitted to one pass before the bounds run: the
 * arriving one, or -- our pending Commit merged -- the Commits that waited
 * on it (commits.c PENDING_MAX_DEFERRED, 16).  They are bounded with the
 * others afterwards; a stored record holds CONV_MAX_CANDIDATES at most. */
#define CONV_CANDIDATE_SLOTS (CONV_MAX_CANDIDATES + 17)
/* A Commit that no retained state authenticates (convergence.md "Candidate
 * branches": its parent identified by MLS authentication alone) is not
 * retained: its kind:445 opened only with an exporter secret we hold, so no
 * state we could still learn authenticates it.  MDK v0.11.0 drops such a
 * Commit too (InvalidAgainstCandidateState once every reachable parent was
 * tried, openmls_projection.rs). */
/* Each (epoch state, sender) pair counts once, and a state's score stops at
 * CONV_WITNESS_QUORUM_SENDERS: no more are ever needed. */
#define CONV_MAX_WITNESSES (CONV_WITNESS_QUORUM_SENDERS * \
                            (CONV_MAX_REWIND_COMMITS + 1 + CONV_MAX_CANDIDATES))

/* One canonical epoch of the retained history: the state S_e at the start
 * of epoch `epoch` (full: it authenticates and processes Commits from that
 * epoch and reads its late application messages) and the canonical Commit
 * that left it (S_e -> S_{e+1}). */
typedef struct {
    uint64_t        epoch;
    MarmotCommitKey key;          /* of the Commit that left this epoch */
    bool            own;          /* that Commit is ours */
    uint8_t        *commit;       /* its MLSMessage bytes (NULL: not known,
                                     a record of an earlier version) */
    size_t          commit_len;
    MlsGroup        state;
    bool            reader;       /* stripped (pre-W25 READER tier): reads
                                     late messages, judges nothing */
} ConvEntry;

/* A Commit retained off the canonical branch: one that lost a selection
 * (deferred while its branch can still win) or the losing side of a reorg.
 * Its parent is the retained state its MLS authentication succeeds against
 * (membership tag, sender data: convergence.md "Candidate branches"); the
 * state whose exporter secret sealed its kind:445 event, named by its
 * confirmed transcript hash, is only tried first.  It is staged
 * (authenticated and validated) at every pass.  Our own Commit cannot be
 * processed by us (its UpdatePath is not encrypted to its sender): its
 * resulting state and priority are kept instead, and its parent is named. */
typedef struct {
    uint64_t  source_epoch;      /* authenticated epoch of the MLSMessage */
    uint8_t   digest[32];        /* SHA-256 of the MLSMessage bytes */
    uint8_t  *msg;
    size_t    msg_len;
    char     *event_id;          /* hex, or NULL */
    uint8_t   parent_tag[32];    /* the state whose exporter secret sealed it
                                    (its confirmed transcript hash): tried first */
    bool      own;
    bool      own_privileged;    /* own: its ordering priority, as authorized */
    bool      own_unconfirmed;   /* own: a pending Commit superseded before any
                                    relay confirmed it -- not selectable as a
                                    branch tip until it shows it was published
                                    (its echo, a witness, a Commit on top) */
    uint8_t  *own_post;          /* own: mls_group_serialize() of the result */
    size_t    own_post_len;
} ConvCandidate;

/* An app-payload witness (convergence.md, "App-payload witnesses"): an
 * application message of `epoch` that decrypted on the state whose confirmed
 * transcript hash is `tag`, sent by the account `sender` from its leaf
 * `leaf`, and passed every payload check.  The witnesses of a branch that
 * lost a reorg stay (they are retained authenticated input, and that branch
 * may be contested again); MDK v0.11.0 no longer re-admits the messages it
 * invalidated as witnesses, so a later contest of the same old branch can
 * score differently there. */
typedef struct {
    uint64_t epoch;
    uint8_t  tag[32];
    uint8_t  sender[32];
    uint32_t leaf;
} ConvWitness;

/* The exporter secret of a candidate (non-canonical) epoch state, so its
 * kind:445 events can be peeled (inbound-processing.md "Transport-deferred
 * input": a retained candidate state can make them decryptable). */
typedef struct {
    uint64_t epoch;
    uint8_t  tag[32];
    uint8_t  exporter[32];
    uint8_t  sender_data[32];   /* its sender-data secret: who sent a message
                                   of it, without rebuilding it (M1) */
} ConvBranchSecret;

/* The retained-parent record, whole. */
typedef struct {
    ConvEntry        entries[CONV_MAX_REWIND_COMMITS];  /* [0]: tip-1, [1]: tip-2, ... */
    size_t           n_entries;
    ConvCandidate    cands[CONV_CANDIDATE_SLOTS];   /* stored: CONV_MAX_CANDIDATES */
    size_t           n_cands;
    ConvWitness      wits[CONV_MAX_WITNESSES];
    size_t           n_wits;
    ConvBranchSecret secrets[CONV_MAX_CANDIDATES];
    size_t           n_secrets;
} ConvHistory;

void conv_history_clear(ConvHistory *h);
void conv_candidate_clear(ConvCandidate *c);
void conv_entry_clear(ConvEntry *e);

/* Encode `h` as the retained-parent record.  The record keeps the layout
 * libmarmot 0.10.0 and later read -- u8 version 1, u64 parent epoch, the
 * applied Commit's key, the parent state, u8 tier, u32 pending count -- for
 * entries[0], with the rest after it.  -1 on failure (or no entry). */
int conv_history_encode(const ConvHistory *h, uint8_t **out, size_t *out_len);

/* Decode a retained-parent record of any version; -1 when it is not one.
 * A record without the W25 trailer yields its one entry, without Commit
 * bytes (and `reader` when it was stripped). */
int conv_history_decode(const uint8_t *data, size_t len, ConvHistory *out);

/* The entry of `epoch`, or NULL. */
ConvEntry *conv_history_entry(ConvHistory *h, uint64_t epoch);

/* Whether `h` already has the witness (epoch, tag, sender); whether
 * (epoch, tag) has CONV_WITNESS_QUORUM_SENDERS distinct senders already. */
bool conv_witness_known(const ConvHistory *h, uint64_t epoch, const uint8_t tag[32],
                        const uint8_t sender[32]);
bool conv_witness_full(const ConvHistory *h, uint64_t epoch, const uint8_t tag[32]);
/* Whether the leaf `leaf` already witnesses (epoch, tag). */
bool conv_witness_leaf_known(const ConvHistory *h, uint64_t epoch, const uint8_t tag[32],
                             uint32_t leaf);
/* Add a witness: 1 added, 0 known or not needed, -1 no room. */
int conv_witness_add(ConvHistory *h, uint64_t epoch, const uint8_t tag[32],
                     const uint8_t sender[32], uint32_t leaf);
/* Distinct senders recorded for (epoch, tag), capped at the quorum. */
size_t conv_witness_count(const ConvHistory *h, uint64_t epoch, const uint8_t tag[32]);

/* ── Branch comparison (convergence.md, "Branch selection") ──────────── */

typedef struct {
    uint64_t fork_epoch;              /* the pass's replay start */
    uint64_t tip_epoch;
    bool     quorum;                  /* witness_quorum_met */
    size_t   witness_score;           /* app_witness_score */
    bool     privileged;              /* tip_priority */
    uint8_t  committer[32];           /* tip_committer */
    uint8_t  digest[32];              /* tip_digest */
} ConvBranchScore;

/* One state of a branch, for its witness score: its epoch and confirmed
 * transcript hash (`tag` NULL: a state we cannot hold, a removal of our
 * leaf). */
typedef struct {
    uint64_t       epoch;
    const uint8_t *tag;
} ConvPathState;

/* The witness part of the score of the branch whose states are `path`
 * (any order) from `s->fork_epoch` to `s->tip_epoch`: per branch epoch after
 * the fork and inside the app-payload window of the branch's own tip
 * (retained-history.md: tip - epoch <= app_payload_past_epoch_limit),
 * min(distinct senders, quorum senders) summed; quorum when
 * CONV_WITNESS_QUORUM_EPOCHS epochs reach it. */
void conv_score_witnesses(const ConvHistory *h, const ConvPathState *path, size_t n_path,
                          ConvBranchScore *s);

/* effective_commit_depth: raw depth plus the bounded witness boost. */
uint64_t conv_effective_depth(const ConvBranchScore *s);

/* > 0 when `a` beats `b`, < 0 when `b` beats `a`, 0 only for the same tip.
 * Higher effective depth, quorum over none, higher witness score,
 * privileged before ordinary, lower committer, lower digest. */
int conv_branch_cmp(const ConvBranchScore *a, const ConvBranchScore *b);

#ifdef __cplusplus
}
#endif

#endif /* MARMOT_CONVERGENCE_H */
