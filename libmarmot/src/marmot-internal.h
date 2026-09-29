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

/** Ensure MLS identity is initialized. Returns 0 on success, -1 on error. */
int marmot_ensure_identity(Marmot *m);

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
