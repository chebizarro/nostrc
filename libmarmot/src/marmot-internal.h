/*
 * libmarmot - Internal header (not part of public API)
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef MARMOT_INTERNAL_H
#define MARMOT_INTERNAL_H

#include <marmot/marmot.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ──────────────────────────────────────────────────────────────────────────
 * Marmot instance (opaque in public header)
 * ──────────────────────────────────────────────────────────────────────── */

struct Marmot {
    MarmotStorage *storage;     /* Owned — freed on marmot_free */
    MarmotConfig   config;

    /*
     * MLS crypto identity. Generated lazily on first key-package creation.
     * ed25519_sk[64] = (scalar[32] || public[32]) in libsodium format.
     * hpke_sk[32]    = X25519 private key (derived from ed25519).
     */
    uint8_t  ed25519_sk[64];
    uint8_t  ed25519_pk[32];
    uint8_t  hpke_sk[32];       /* X25519 private key */
    uint8_t  hpke_pk[32];       /* X25519 public key  */
    bool     identity_ready;

    /* marmot.member.account-identity-proof.v2 binding ed25519_pk (the leaf
     * key of groups this instance creates, and of KeyPackages made without
     * the account key) to account_proof_owner (nostrc-7vyi). Set by
     * marmot_set_account_proof(), or by marmot_create_key_package() with the
     * account key. In memory only, like the key it binds. */
    bool     account_proof_ready;
    uint8_t  account_proof_owner[32];
    uint8_t  account_proof[104];

    /* One storage transaction per public operation (nostrc-qp24.7). */
    unsigned txn_depth;         /* public calls in progress (never > 1 in practice) */
    bool     txn_keep;          /* commit even though the operation returns an error */
};

/* ──────────────────────────────────────────────────────────────────────────
 * Internal helpers
 * ──────────────────────────────────────────────────────────────────────── */

/** Hex-encode raw bytes. Caller frees result. Returns NULL on OOM. */
char *marmot_hex_encode(const uint8_t *data, size_t len);

/** Hex-decode string into out. Returns 0 on success, -1 on invalid input. */
int marmot_hex_decode(const char *hex, uint8_t *out, size_t out_len);

/** Constant-time comparison for n bytes. */
int marmot_constant_time_eq(const uint8_t *a, const uint8_t *b, size_t n);

/** Get current UNIX timestamp. */
int64_t marmot_now(void);

/** The created_at of the next kind:445 event we publish to this group: now,
 * or a second after the group's newest event we published or applied,
 * within a bounded lead over our clock (nostrc-2lrz; see marmot.c).  An
 * application message (`commit` false) is dated after the Commits it
 * follows, at the 60 s bound at most in the same second; a Commit, after
 * every event.  Reserved in storage: call it inside
 * the operation's storage transaction, which undoes the reservation if the
 * operation fails.  MARMOT_ERR_EVENT_RATE (Commits only): the bound is
 * reached, retry in a second. */
MarmotError marmot_next_group_event_time(Marmot *m, const uint8_t nostr_group_id[32],
                                         bool commit, int64_t *created_at);

/** Another member's Commit, or the Welcome that added us, has this
 * created_at: our next event follows it (up to a minute ahead of our clock). */
MarmotError marmot_observe_group_event_time(Marmot *m, const uint8_t nostr_group_id[32],
                                            int64_t created_at);

/** Decode GroupData we stored ourselves: MIP-01, or the layout libmarmot
 * 0.10.0 and older wrote (nostrc-c7ho).  Never for a Welcome's or a peer
 * Commit's GroupData: marmot_group_data_extension_deserialize() is for those. */
MarmotGroupDataExtension *marmot_group_data_extension_deserialize_stored(
    const uint8_t *data, size_t len);

/** Ensure MLS identity is initialized. Returns 0 on success, -1 on error. */
int marmot_ensure_identity(Marmot *m);

/** The account proof of ed25519_pk for `owner` (104 bytes into `out`, which
 *  may be NULL), if this instance holds one (nostrc-7vyi). */
bool marmot_account_proof_lookup(const Marmot *m, const uint8_t owner[32], uint8_t out[104]);

/* ── Storage transactions (nostrc-qp24.7) ─────────────────────────────────
 * Every public operation that may write runs between marmot_txn_begin() and
 * marmot_txn_end(): one storage transaction when the backend has the hooks
 * (MarmotStorage.begin/commit/rollback), a no-op otherwise.  Nested calls
 * join the outermost one.  The transaction commits when the operation
 * succeeds and rolls back on any error -- unless the operation called
 * marmot_txn_keep(): its writes are then its deliberate outcome although it
 * reports an error (a superseded pending Commit dropped, a losing inbound
 * Commit deferred, a duplicate Welcome retired, a Welcome recorded as
 * failed).  A failed commit returns its error: the caller must then discard
 * every output of the operation, which was rolled back. */

/** Begin (or join) the operation's transaction. */
MarmotError marmot_txn_begin(Marmot *m);

/** Commit the operation's writes even though it returns an error. */
void marmot_txn_keep(Marmot *m);

/** End the operation's transaction; returns `result`, or the commit error. */
MarmotError marmot_txn_end(Marmot *m, MarmotError result);

#ifdef __cplusplus
}
#endif

#endif /* MARMOT_INTERNAL_H */
