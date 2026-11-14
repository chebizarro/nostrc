#ifndef FIPS_HOST_H
#define FIPS_HOST_H

#include <fips/export.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Ownership convention for libfips-core APIs:
 * - Buffers and vtable contexts belong to the caller; calls borrow them only.
 *   A retained context/vtable must outlive every core object using it. Copying
 *   a vtable never transfers ownership. The caller provides synchronization.
 * - Results use caller-provided buffers with explicit capacities/lengths.
 *   Future allocating constructors must have explicit matching destroy functions;
 *   never free a core object with the host allocator (or vice versa).
 * - Fallible operations return status codes and write out-parameters only on
 *   success. Caller misuse (NULL required pointers, invalid durations, mixed
 *   clock domains) is outside the contract, not an input-validation strategy.
 * - These interfaces allocate nothing. Entropy fill and host clock reads are
 *   infallible, like Rust's traits: a host must not return partial/random-looking
 *   bytes or a fabricated timestamp on failure. Terminate at the host boundary
 *   if it cannot fulfill that contract. The core never calls clock.now itself.
 */
typedef enum fips_status {
    FIPS_OK = 0,
    FIPS_ERR_RANGE = 1
} fips_status;

/** Rust Duration range/precision. Always normalized: nanos < 1,000,000,000. */
typedef struct fips_duration {
    uint64_t secs;
    uint32_t nanos;
} fips_duration;

#define FIPS_DEADLINE_CEILING_SECS UINT64_C(3153600000)

typedef struct fips_instant_ops fips_instant_ops;

/** Copyable monotonic value; only its host adapter interprets opaque words.
 * Operations require instants from the same clock domain and ops table.
 * The immutable ops table must outlive all copies (normally static storage).
 */
typedef struct fips_instant {
    uint64_t opaque[2];
    const fips_instant_ops *ops;
} fips_instant;

struct fips_instant_ops {
    /** Negative, zero, positive for a < b, a == b, a > b. */
    int (*compare)(fips_instant a, fips_instant b);
    fips_duration (*saturating_duration_since)(fips_instant a, fips_instant earlier);
    fips_status (*checked_add)(fips_instant a, fips_duration d, fips_instant *out);
    fips_status (*checked_sub)(fips_instant a, fips_duration d, fips_instant *out);
};

typedef struct fips_clock {
    /** Host-only: successive reads must never go backwards. */
    fips_instant (*now)(void *ctx);
    void *ctx;
} fips_clock;

typedef struct fips_entropy {
    /** Fill ALL n bytes with cryptographically secure entropy. n == 0 permits
     * buf == NULL and must not consume entropy. Test fakes are NOT secure. */
    void (*fill)(void *ctx, uint8_t *buf, size_t n);
    void *ctx;
} fips_entropy;

FIPS_CORE_API fips_duration fips_duration_from_millis(uint64_t ms);
/** now + min(d, 100 years), halving the step until checked_add succeeds. */
FIPS_CORE_API fips_instant fips_deadline(fips_instant now, fips_duration d);
/** min(base * factor, cap); factor = 2^exponent for exponent < 32, else
 * UINT32_MAX. The multiplication saturates at Rust's Duration::MAX. */
FIPS_CORE_API fips_duration fips_backoff(uint32_t exponent, fips_duration base,
                                      fips_duration cap);

#ifdef __cplusplus
}
#endif

#endif /* FIPS_HOST_H */
