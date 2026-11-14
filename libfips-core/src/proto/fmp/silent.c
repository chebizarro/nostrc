#include <fips/proto/fmp/silent.h>
#include <string.h>

static uint64_t add(uint64_t a, uint64_t b) { return b > UINT64_MAX - a ? UINT64_MAX : a + b; }
static size_t position(const fips_fmp_silent_sessions *s, const fips_node_addr *peer)
{
    size_t lo = 0, hi = s->len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (memcmp(s->records[mid].peer.bytes, peer->bytes, sizeof(peer->bytes)) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}
static bool found(const fips_fmp_silent_sessions *s, size_t p, const fips_node_addr *peer)
{ return p < s->len && memcmp(s->records[p].peer.bytes, peer->bytes, sizeof(peer->bytes)) == 0; }
void fips_fmp_silent_sessions_init(fips_fmp_silent_sessions *s, fips_fmp_silent_record *storage, size_t capacity, fips_instant origin)
{ *s = (fips_fmp_silent_sessions){storage, 0, capacity, origin}; }
fips_status fips_fmp_silent_sessions_ended(fips_fmp_silent_sessions *s, fips_node_addr peer,
    fips_fmp_epoch epoch, const fips_fmp_ephemeral_digest *setup, fips_instant now, bool *started, fips_fmp_refusal_start *out)
{
    uint64_t ms = fips_fmp_elapsed_ms(now, s->origin);
    size_t live = 0;
    for (size_t i = 0; i < s->len; ++i) {
        const fips_fmp_silent_record *r = &s->records[i];
        uint64_t from = r->has_until ? r->until_ms : r->last_end_ms;
        if (ms < add(from, FIPS_FMP_SILENT_BACKOFF_CAP_MS)) s->records[live++] = *r;
    }
    s->len = live;
    size_t p = position(s, &peer);
    if (!found(s, p, &peer)) {
        if (s->len == s->capacity) return FIPS_ERR_RANGE;
        memmove(s->records + p + 1, s->records + p, (s->len - p) * sizeof(*s->records));
        s->records[p] = (fips_fmp_silent_record){.peer = peer, .epoch = epoch};
        ++s->len;
    }
    fips_fmp_silent_record *r = &s->records[p];
    if (fips_fmp_epochs_differ(r->epoch, epoch)) *r = (fips_fmp_silent_record){.peer = peer, .epoch = epoch};
    if (!r->epoch.present) r->epoch = epoch;
    *started = false;
    if (setup) {
        for (size_t i = 0; i < r->counted_len; ++i)
            if (!memcmp(r->counted[i].bytes, setup->bytes, sizeof(setup->bytes))) return FIPS_OK;
        if (r->counted_len == FIPS_FMP_COUNTED_SETUP_RECORD) {
            memmove(r->counted, r->counted + 1, (FIPS_FMP_COUNTED_SETUP_RECORD - 1) * sizeof(*r->counted));
            --r->counted_len;
        }
        r->counted[r->counted_len++] = *setup;
    }
    if (r->silent != UINT32_MAX) ++r->silent;
    r->last_end_ms = ms;
    if (r->silent < FIPS_FMP_SILENT_SESSION_LIMIT) return FIPS_OK;
    uint64_t length = fips_fmp_backoff_ms(r->silent - FIPS_FMP_SILENT_SESSION_LIMIT,
        FIPS_FMP_SILENT_BACKOFF_BASE_MS, FIPS_FMP_SILENT_BACKOFF_CAP_MS);
    r->has_until = true; r->until_ms = add(ms, length);
    *out = (fips_fmp_refusal_start){{r->epoch, r->silent, length}, r->refused};
    r->refused = 0; *started = true;
    return FIPS_OK;
}
uint32_t fips_fmp_silent_sessions_heard(fips_fmp_silent_sessions *s, const fips_node_addr *peer)
{
    size_t p = position(s, peer);
    if (!found(s, p, peer)) return 0;
    uint32_t refused = s->records[p].refused;
    --s->len;
    memmove(s->records + p, s->records + p + 1, (s->len - p) * sizeof(*s->records));
    return refused;
}
bool fips_fmp_silent_sessions_note_refused(fips_fmp_silent_sessions *s, const fips_node_addr *peer, uint32_t *out)
{
    size_t p = position(s, peer);
    if (!found(s, p, peer)) return false;
    if (s->records[p].refused != UINT32_MAX) ++s->records[p].refused;
    *out = s->records[p].refused;
    return true;
}
bool fips_fmp_silent_sessions_refusing(const fips_fmp_silent_sessions *s, const fips_node_addr *peer,
    fips_instant now, fips_fmp_active_backoff *out)
{
    size_t p = position(s, peer);
    if (!found(s, p, peer)) return false;
    const fips_fmp_silent_record *r = &s->records[p];
    uint64_t ms = fips_fmp_elapsed_ms(now, s->origin);
    if (!r->has_until || r->until_ms <= ms) return false;
    *out = (fips_fmp_active_backoff){r->epoch, r->silent, r->until_ms - ms};
    return true;
}
bool fips_fmp_silent_sessions_count(const fips_fmp_silent_sessions *s, const fips_node_addr *peer, uint32_t *out)
{
    size_t p = position(s, peer);
    if (!found(s, p, peer)) return false;
    *out = s->records[p].silent;
    return true;
}
