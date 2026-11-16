#ifndef FIPSD_NOSTR_FAILURE_STATE_H
#define FIPSD_NOSTR_FAILURE_STATE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Single owner (the rendezvous actor); public snapshots are copied by callers. */
typedef struct {
    char peer[65];
    uint32_t streak;
    uint64_t touched_ms, warn_ms, cooldown_ms, mismatch_ms;
    int64_t skew_ms;
    bool warned, has_skew;
} fipsd_nostr_failure;
typedef struct {
    fipsd_nostr_failure *entries;
    size_t count, capacity;
    uint32_t threshold;
    uint64_t cooldown_ms, warn_interval_ms;
} fipsd_nostr_failure_state;
typedef struct {
    uint32_t streak;
    bool warn, crossed_threshold;
    uint64_t cooldown_ms;
} fipsd_nostr_failure_decision;
bool fipsd_nostr_failure_init(fipsd_nostr_failure_state *, size_t capacity,
    uint32_t threshold, uint64_t cooldown_ms, uint64_t warn_interval_ms);
void fipsd_nostr_failure_clear(fipsd_nostr_failure_state *);
const fipsd_nostr_failure *fipsd_nostr_failure_get(const fipsd_nostr_failure_state *, const char *peer);
fipsd_nostr_failure_decision fipsd_nostr_failure_record(fipsd_nostr_failure_state *, const char *, uint64_t now_ms);
void fipsd_nostr_failure_success(fipsd_nostr_failure_state *, const char *, uint64_t now_ms);
void fipsd_nostr_failure_refresh(fipsd_nostr_failure_state *, const char *);
void fipsd_nostr_failure_skew(fipsd_nostr_failure_state *, const char *, int64_t skew, uint64_t now_ms);
bool fipsd_nostr_failure_mismatch(fipsd_nostr_failure_state *, const char *, uint64_t now_ms, uint64_t cooldown_ms);
#endif
