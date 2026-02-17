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

#ifdef __cplusplus
}
#endif

#endif /* MARMOT_INTERNAL_H */
