#include "node/handlers/internal.h"
#include <string.h>
/* The IPv6 shim keeps a replica of these clamps; it learns of a change (and
 * only a change) through a notice carrying the new MTU, 0 when released. The
 * notice channel is bounded and never blocks the dispatcher: one it cannot
 * take is counted and left pending on the clamp, and re-sent at the next set
 * for that peer (every MMP PathMtuNotification applies one). Not
 * control-visible state, so no snapshot is published for it. */
static bool notify(fipsd_dataplane *d, const fips_node_addr *peer, uint16_t mtu)
{
    handlers *h = d->owner;
    if (!h || !h->notices) return true;
    fipsd_node_notice *n = malloc(sizeof *n);
    if (!n) { ++h->counters.notices_dropped; return false; }
    *n = (fipsd_node_notice){.kind = FIPSD_NOTICE_PATH_MTU, .peer = *peer, .value = mtu};
    if (go_channel_try_send(h->notices, n) != 0) { fipsd_node_notice_free(n); ++h->counters.notices_dropped; return false; }
    return true;
}
void fipsd_dp_mtu_set(fipsd_dataplane *d, const fips_node_addr *peer, uint16_t mtu)
{
    if (!d || !mtu || mtu == UINT16_MAX) return;
    size_t vacant = SIZE_MAX;
    for (size_t i = 0; i < d->clamp_capacity; ++i) {
        if (d->clamps[i].used && !memcmp(d->clamps[i].peer.bytes, peer->bytes, 16)) {
            if (d->clamps[i].mtu != mtu || d->clamps[i].notify_pending) {
                d->clamps[i].mtu = mtu; d->clamps[i].notify_pending = !notify(d, peer, mtu);
            }
            return;
        }
        if (!d->clamps[i].used) vacant = i;
    }
    if (vacant != SIZE_MAX) {
        d->clamps[vacant].used = true; d->clamps[vacant].peer = *peer; d->clamps[vacant].mtu = mtu;
        d->clamps[vacant].notify_pending = !notify(d, peer, mtu);
    }
}
uint16_t fipsd_dp_mtu_get(const fipsd_dataplane *d, const fips_node_addr *peer)
{
    if (d) for (size_t i = 0; i < d->clamp_capacity; ++i)
        if (d->clamps[i].used && !memcmp(d->clamps[i].peer.bytes, peer->bytes, 16)) return d->clamps[i].mtu;
    return 0;
}
void fipsd_dp_mtu_release(fipsd_dataplane *d, const fips_node_addr *peer)
{
    if (d) for (size_t i = 0; i < d->clamp_capacity; ++i)
        if (d->clamps[i].used && !memcmp(d->clamps[i].peer.bytes, peer->bytes, 16)) {
            d->clamps[i].used = false; d->clamps[i].notify_pending = false; ++d->stats.mtu_releases;
            (void)notify(d, peer, 0); /* a dropped release leaves the smaller limit in place until the next set */
        }
}

uint16_t fipsd_dp_link_mtu(handlers *h, const peer_rec *rec, fips_transport_id transport, const fips_transport_addr *to)
{
    fipsd_transport *t = hs_transport(h, transport);
    size_t mtu = t ? t->ops->mtu(t, to) : UINT16_MAX;
    if (t && !t->ops->connect_async && hs_rec_multipath(rec)) {
        size_t index = fips_path_set_find(&rec->path->set, transport, to);
        const fips_peer_path *path = fips_path_set_get(&rec->path->set, index);
        if (path) mtu = fips_path_carried(mtu, path->live.has_mtu, path->live.mtu, false);
    }
    return mtu > UINT16_MAX ? UINT16_MAX : (uint16_t)mtu;
}
