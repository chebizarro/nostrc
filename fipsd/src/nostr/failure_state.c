#include "nostr/failure_state.h"
#include <stdlib.h>
#include <string.h>
static uint64_t add(uint64_t a, uint64_t b) { return UINT64_MAX - a < b ? UINT64_MAX : a + b; }
bool fipsd_nostr_failure_init(fipsd_nostr_failure_state *s, size_t cap,
    uint32_t threshold, uint64_t cooldown, uint64_t warn)
{
    if (!s || !cap || cap > 65536) return false;
    *s = (fipsd_nostr_failure_state){.entries = calloc(cap, sizeof(*s->entries)),
        .capacity = cap, .threshold = threshold, .cooldown_ms = cooldown, .warn_interval_ms = warn};
    return s->entries != NULL;
}
void fipsd_nostr_failure_clear(fipsd_nostr_failure_state *s) { free(s->entries); *s = (fipsd_nostr_failure_state){0}; }
const fipsd_nostr_failure *fipsd_nostr_failure_get(const fipsd_nostr_failure_state *s, const char *peer)
{
    for (size_t i = 0; i < s->count; ++i) if (!strcmp(s->entries[i].peer, peer)) return &s->entries[i];
    return NULL;
}
static fipsd_nostr_failure *entry(fipsd_nostr_failure_state *s, const char *peer, uint64_t now)
{
    if (!peer || strlen(peer) != 64 || !s->capacity) return NULL;
    for (size_t i = 0; i < s->count; ++i) if (!strcmp(s->entries[i].peer, peer)) return &s->entries[i];
    size_t i = s->count;
    if (i == s->capacity) {
        i = 0;
        for (size_t j = 1; j < s->count; ++j) if (s->entries[j].touched_ms < s->entries[i].touched_ms) i = j;
    } else ++s->count;
    s->entries[i] = (fipsd_nostr_failure){.touched_ms = now};
    memcpy(s->entries[i].peer, peer, 65);
    return &s->entries[i];
}
fipsd_nostr_failure_decision fipsd_nostr_failure_record(fipsd_nostr_failure_state *s, const char *peer, uint64_t now)
{
    fipsd_nostr_failure *e = entry(s, peer, now);
    if (!e) return (fipsd_nostr_failure_decision){0};
    if (e->streak < UINT32_MAX) ++e->streak;
    e->touched_ms = now;
    bool warn = !e->warned || (now >= e->warn_ms && now - e->warn_ms >= s->warn_interval_ms);
    if (warn) { e->warned = true; e->warn_ms = now; }
    if (e->streak >= s->threshold) e->cooldown_ms = add(now, s->cooldown_ms);
    return (fipsd_nostr_failure_decision){e->streak, warn, e->streak == s->threshold, e->cooldown_ms};
}
void fipsd_nostr_failure_refresh(fipsd_nostr_failure_state *s, const char *peer)
{
    for (size_t i = 0; i < s->count; ++i) if (!strcmp(s->entries[i].peer, peer)) {
        s->entries[i].streak = 0; s->entries[i].cooldown_ms = 0; s->entries[i].mismatch_ms = 0; return;
    }
}
void fipsd_nostr_failure_success(fipsd_nostr_failure_state *s, const char *peer, uint64_t now)
{
    fipsd_nostr_failure_refresh(s, peer);
    for (size_t i = 0; i < s->count; ++i) if (!strcmp(s->entries[i].peer, peer)) s->entries[i].touched_ms = now;
}
void fipsd_nostr_failure_skew(fipsd_nostr_failure_state *s, const char *peer, int64_t skew, uint64_t now)
{
    fipsd_nostr_failure *e = entry(s, peer, now);
    if (e) { e->has_skew = true; e->skew_ms = skew; }
}
bool fipsd_nostr_failure_mismatch(fipsd_nostr_failure_state *s, const char *peer, uint64_t now, uint64_t cooldown)
{
    fipsd_nostr_failure *e = entry(s, peer, now);
    if (!e) return false;
    if (e->streak < s->threshold) e->streak = s->threshold;
    e->touched_ms = now;
    if (e->mismatch_ms > now) {
        if (e->cooldown_ms < e->mismatch_ms) e->cooldown_ms = e->mismatch_ms;
        return false;
    }
    e->cooldown_ms = e->mismatch_ms = add(now, cooldown);
    return true;
}
