#include <fips/node/rate_limit.h>
#include <math.h>
#include <float.h>
#include <string.h>

static double elapsed_secs(fips_instant now, fips_instant earlier)
{
    fips_duration d = now.ops->saturating_duration_since(now, earlier);
    return (double)d.secs + (double)d.nanos / 1e9;
}

void fips_token_bucket_init(fips_token_bucket *b, fips_bucket_params p, fips_instant now)
{
    *b = (fips_token_bucket){p.burst, (double)p.burst, p.rate, now};
}

double fips_token_bucket_tokens(fips_token_bucket *b, fips_instant now)
{
    b->tokens += elapsed_secs(now, b->last_refill) * b->refill_rate;
    if (b->tokens > (double)b->capacity) b->tokens = (double)b->capacity;
    b->last_refill = now;
    return b->tokens;
}

bool fips_token_bucket_acquire(fips_token_bucket *b, uint32_t n, fips_instant now)
{
    if (fips_token_bucket_tokens(b, now) < (double)n) return false;
    b->tokens -= (double)n;
    return true;
}

void fips_token_bucket_reset(fips_token_bucket *b, fips_instant now)
{
    b->tokens = (double)b->capacity;
    b->last_refill = now;
}

/* Rust Duration::from_secs_f64 rounds the exact binary fraction to nearest
 * nanoseconds, ties to even. A floating multiply by 1e9 can double-round;
 * multiply the 53-bit significand into two integer limbs instead. */
static uint32_t fractional_nanos(double fraction)
{
    _Static_assert(sizeof(double) == sizeof(uint64_t) && DBL_MANT_DIG == 53 &&
                   DBL_MAX_EXP == 1024, "FIPS requires IEEE binary64 doubles");
    uint64_t bits;
    memcpy(&bits, &fraction, sizeof(bits));
    unsigned int exponent = (unsigned int)((bits >> 52) & 2047);
    if (exponent == 0) return 0; /* Zero/subnormal is far below half a ns. */
    unsigned int shift = 1075 - exponent; /* fraction < 1 implies shift >= 53 */
    if (shift > 83) return 0;
    uint64_t mantissa = (bits & UINT64_C(0x000fffffffffffff)) | (UINT64_C(1) << 52);
    uint64_t lo = (mantissa & UINT32_MAX) * UINT64_C(1000000000);
    uint64_t mid = (mantissa >> 32) * UINT64_C(1000000000) + (lo >> 32);
    lo = (mid << 32) | (lo & UINT32_MAX);
    uint64_t hi = mid >> 32;
    uint64_t n, remainder, half;
    bool above, tie;
    if (shift < 64) {
        n = (lo >> shift) | (hi << (64 - shift));
        remainder = lo & ((UINT64_C(1) << shift) - 1);
        half = UINT64_C(1) << (shift - 1);
        above = remainder > half;
        tie = remainder == half;
    } else if (shift == 64) {
        n = hi;
        half = UINT64_C(1) << 63;
        above = lo > half;
        tie = lo == half;
    } else {
        n = hi >> (shift - 64);
        remainder = hi & ((UINT64_C(1) << (shift - 64)) - 1);
        half = UINT64_C(1) << (shift - 65);
        above = remainder > half || (remainder == half && lo != 0);
        tie = remainder == half && lo == 0;
    }
    return (uint32_t)(n + (above || (tie && (n & 1) != 0) ? 1 : 0));
}

fips_status fips_token_bucket_wait(fips_token_bucket *b, fips_instant now, fips_duration *out)
{
    double tokens = fips_token_bucket_tokens(b, now);
    if (tokens >= 1.0) { *out = (fips_duration){0, 0}; return FIPS_OK; }
    double seconds = (1.0 - tokens) / b->refill_rate;
    if (!isfinite(seconds) || seconds >= 0x1p64) return FIPS_ERR_RANGE;
    uint64_t secs = (uint64_t)seconds;
    uint32_t nanos = fractional_nanos(seconds - (double)secs);
    if (nanos == 1000000000) {
        if (secs == UINT64_MAX) return FIPS_ERR_RANGE;
        ++secs;
        nanos = 0;
    }
    *out = (fips_duration){secs, nanos};
    return FIPS_OK;
}

fips_bucket_params fips_derive_established_bucket(size_t peers, uint64_t period,
                                                  uint32_t resends, fips_bucket_params stranger)
{
    if (peers == 0) return stranger;
    double rate = (double)peers / (double)(period == 0 ? 1 : period) * (1.0 + (double)resends);
    return (fips_bucket_params){peers > UINT32_MAX ? UINT32_MAX : (uint32_t)peers,
                               fmax(rate, FIPS_ESTABLISHED_RATE_FLOOR)};
}

const char *fips_msg1_result_name(fips_msg1_result r)
{
    switch (r) {
    case FIPS_MSG1_ADMIT: return "admit";
    case FIPS_MSG1_PENDING_LIMIT: return "pending_limit";
    case FIPS_MSG1_RATE_LIMIT: return "rate_limit";
    }
    return "unknown";
}

static fips_token_bucket *bucket_for(fips_handshake_rate_limiter *l, fips_msg1_class c)
{
    return c == FIPS_MSG1_STRANGER ? &l->stranger : &l->established;
}

void fips_handshake_init(fips_handshake_rate_limiter *l, fips_token_bucket stranger,
                         fips_token_bucket established, size_t max_pending)
{
    *l = (fips_handshake_rate_limiter){stranger, established, 0, max_pending};
}

bool fips_handshake_can_start(fips_handshake_rate_limiter *l, fips_msg1_class c, fips_instant now)
{
    return fips_token_bucket_tokens(bucket_for(l, c), now) >= 1.0 && l->pending < l->max_pending;
}

fips_msg1_result fips_handshake_start(fips_handshake_rate_limiter *l, fips_msg1_class c,
                                      fips_instant now, fips_pending_handshake *slot)
{
    if (l->pending >= l->max_pending) return FIPS_MSG1_PENDING_LIMIT;
    if (!fips_token_bucket_acquire(bucket_for(l, c), 1, now)) return FIPS_MSG1_RATE_LIMIT;
    ++l->pending;
    slot->owner = l;
    return FIPS_MSG1_ADMIT;
}

void fips_pending_handshake_release(fips_pending_handshake *slot)
{
    if (slot->owner != NULL) {
        if (slot->owner->pending != 0) --slot->owner->pending;
        slot->owner = NULL;
    }
}

void fips_handshake_reset(fips_handshake_rate_limiter *l, fips_instant now)
{
    fips_token_bucket_reset(&l->stranger, now);
    fips_token_bucket_reset(&l->established, now);
    l->pending = 0;
}

/* The address is the first field of both sorted entry types. */
static size_t position(const void *entries, size_t len, size_t stride, const uint8_t addr[16])
{
    const unsigned char *p = entries;
    size_t lo = 0, hi = len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (memcmp(p + mid * stride, addr, 16) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

void fips_session_setup_init(fips_session_setup_limiter *l, fips_setup_entry *entries,
    size_t capacity, fips_bucket_params stranger, fips_bucket_params established)
{
    *l = (fips_session_setup_limiter){entries, 0, capacity, stranger, established};
}

void fips_session_setup_cleanup(fips_session_setup_limiter *l, fips_instant now)
{
    size_t n = 0;
    for (size_t i = 0; i < l->len; ++i)
        if (now.ops->saturating_duration_since(now, l->entries[i].seen).secs < FIPS_SETUP_BUCKET_IDLE_SECS)
            l->entries[n++] = l->entries[i];
    l->len = n;
}

fips_status fips_session_setup_admit(fips_session_setup_limiter *l, const uint8_t addr[16],
                                    fips_msg1_class c, fips_instant now, bool *out)
{
    size_t i = position(l->entries, l->len, sizeof(l->entries[0]), addr);
    if (i == l->len || memcmp(l->entries[i].addr, addr, 16) != 0) {
        if (l->len == l->capacity) return FIPS_ERR_RANGE;
        memmove(l->entries + i + 1, l->entries + i, (l->len - i) * sizeof(l->entries[0]));
        ++l->len;
        memcpy(l->entries[i].addr, addr, 16);
        fips_token_bucket_init(&l->entries[i].stranger, l->stranger, now);
        fips_token_bucket_init(&l->entries[i].established, l->established, now);
    }
    fips_setup_entry *entry = &l->entries[i];
    entry->seen = now;
    *out = fips_token_bucket_acquire(c == FIPS_MSG1_STRANGER ? &entry->stranger : &entry->established,
                                   1, now);
    if (*out) fips_session_setup_cleanup(l, now);
    return FIPS_OK;
}

void fips_lookup_sign_init(fips_lookup_sign_limiter *l, fips_sign_entry *entries,
                           size_t capacity, double burst, double rate)
{
    *l = (fips_lookup_sign_limiter){entries, 0, capacity, burst, rate};
}

void fips_lookup_sign_cleanup(fips_lookup_sign_limiter *l, fips_instant now)
{
    size_t n = 0;
    for (size_t i = 0; i < l->len; ++i)
        if (now.ops->saturating_duration_since(now, l->entries[i].updated).secs < FIPS_SIGN_MAX_AGE_SECS)
            l->entries[n++] = l->entries[i];
    l->len = n;
}

fips_status fips_lookup_should_sign(fips_lookup_sign_limiter *l, const uint8_t addr[16],
                                    fips_instant now, bool *out)
{
    if (l->burst <= 0.0) { *out = true; return FIPS_OK; }
    size_t i = position(l->entries, l->len, sizeof(l->entries[0]), addr);
    if (i == l->len || memcmp(l->entries[i].addr, addr, 16) != 0) {
        if (l->len == l->capacity) return FIPS_ERR_RANGE;
        memmove(l->entries + i + 1, l->entries + i, (l->len - i) * sizeof(l->entries[0]));
        ++l->len;
        memcpy(l->entries[i].addr, addr, 16);
        l->entries[i].tokens = l->burst;
        l->entries[i].updated = now;
    }
    fips_sign_entry *entry = &l->entries[i];
    entry->tokens = fmin(entry->tokens + elapsed_secs(now, entry->updated) * l->rate, l->burst);
    entry->updated = now;
    *out = entry->tokens >= 1.0;
    if (*out) {
        entry->tokens -= 1.0;
        fips_lookup_sign_cleanup(l, now);
    }
    return FIPS_OK;
}
