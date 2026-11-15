#include "peer/data.h"
#include <string.h>
void fipsd_peer_data_init(fipsd_peer_data *d, uint64_t wall_ms, fips_instant now, bool heard)
{
    memset(d, 0, sizeof(*d));
    d->tree_announce_min_interval_ms = 500;
    d->pending_filter_update = true;
    d->authenticated_at = d->last_seen = wall_ms;
    d->promoted_at = d->heard_at = now;
    d->heard = heard;
}
bool fipsd_peer_data_tree(fipsd_peer_data *d, const fips_parent_declaration *decl,
    const fips_coord_entry *ancestry, size_t count)
{
    if (!decl) { d->has_tree = false; d->ancestry_count = 0; return true; }
    if (!count || count > FIPSD_PEER_COORD_CAPACITY || !ancestry) return false;
    d->declaration = *decl;
    memcpy(d->ancestry, ancestry, count * sizeof(*ancestry));
    d->ancestry_count = count; d->has_tree = true;
    return true;
}
bool fipsd_peer_data_tree_due(const fipsd_peer_data *d, uint64_t ms)
{
    return !d->has_tree_announce_sent ||
        (ms >= d->last_tree_announce_sent_ms && ms - d->last_tree_announce_sent_ms >= d->tree_announce_min_interval_ms);
}
void fipsd_peer_data_tree_sent(fipsd_peer_data *d, uint64_t ms)
{
    d->has_tree_announce_sent = true; d->last_tree_announce_sent_ms = ms;
    d->pending_tree_announce = false;
}
void fipsd_peer_data_filter(fipsd_peer_data *d, const fips_bloom_filter *f, uint64_t seq, uint64_t ms)
{
    d->has_filter = f != NULL;
    if (f) d->inbound_filter = *f;
    else memset(&d->inbound_filter, 0, sizeof(d->inbound_filter));
    d->filter_sequence = f ? seq : 0;
    d->filter_received_at = f ? ms : 0;
}
bool fipsd_peer_data_filter_stale(const fipsd_peer_data *d, uint64_t ms, uint64_t threshold)
{
    return !d->filter_received_at || (ms >= d->filter_received_at && ms - d->filter_received_at > threshold);
}
