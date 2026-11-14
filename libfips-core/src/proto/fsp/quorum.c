#include <fips/proto/fsp/quorum.h>
#include <string.h>
#include <stdbool.h>
_Static_assert(FIPS_FSP_QUORUM_LINKS == 2, "one retained vote per destination");
static bool equal(const fips_node_addr *a, const fips_node_addr *b) { return memcmp(a->bytes, b->bytes, 16) == 0; }
static uint64_t elapsed(uint64_t now, uint64_t at) { return now > at ? now - at : 0; }
static bool live(uint64_t at, uint64_t now) { return elapsed(now, at) <= FIPS_FSP_QUORUM_WINDOW_MS; }
static void remove_at(fips_fsp_link_quorum *q, size_t i)
{
    if (i + 1 < q->len) memmove(q->items + i, q->items + i + 1, (q->len - i - 1) * sizeof(*q->items));
    --q->len;
}
void fips_fsp_link_quorum_init(fips_fsp_link_quorum *q, fips_fsp_quorum_entry *s, size_t n)
{ *q = (fips_fsp_link_quorum){s, 0, n, 0}; }
void fips_fsp_link_quorum_clear(fips_fsp_link_quorum *q, const fips_node_addr *dest)
{
    for (size_t i = 0; i < q->len; ++i) if (equal(&q->items[i].dest, dest)) { remove_at(q, i); return; }
}
fips_status fips_fsp_link_quorum_record(fips_fsp_link_quorum *q, fips_node_addr dest, fips_node_addr peer, uint64_t now, fips_fsp_quorum_verdict *out)
{
    bool sweep = elapsed(now, q->last_sweep_ms) >= FIPS_FSP_QUORUM_WINDOW_MS;
    bool room = q->len < q->capacity;
    for (size_t i = 0; i < q->len; ++i)
        if (equal(&q->items[i].dest, &dest) || (sweep && !live(q->items[i].first_seen_ms, now))) room = true;
    if (!room) return FIPS_ERR_RANGE;
    if (sweep) {
        q->last_sweep_ms = now;
        for (size_t i = 0; i < q->len;) {
            if (!live(q->items[i].first_seen_ms, now)) remove_at(q, i);
            else ++i;
        }
    }
    for (size_t i = 0; i < q->len; ++i) {
        fips_fsp_quorum_entry *entry = q->items + i;
        if (!equal(&entry->dest, &dest)) continue;
        if (!live(entry->first_seen_ms, now)) { *entry = (fips_fsp_quorum_entry){dest, peer, now}; break; }
        if (!equal(&entry->link_peer, &peer)) {
            remove_at(q, i); *out = (fips_fsp_quorum_verdict){FIPS_FSP_Reached, FIPS_FSP_QUORUM_LINKS}; return FIPS_OK;
        }
        /* Repeat leaves first-seen time unchanged. */
        *out = (fips_fsp_quorum_verdict){FIPS_FSP_Below, 1}; return FIPS_OK;
    }
    bool found = false;
    for (size_t i = 0; i < q->len; ++i) if (equal(&q->items[i].dest, &dest)) found = true;
    if (!found) q->items[q->len++] = (fips_fsp_quorum_entry){dest, peer, now};
    *out = (fips_fsp_quorum_verdict){FIPS_FSP_Below, 1}; return FIPS_OK;
}
