#include "node/handlers/internal.h"
#include <string.h>
void fipsd_dp_mtu_set(fipsd_dataplane *d, const fips_node_addr *peer, uint16_t mtu)
{
    if (!d || !mtu || mtu == UINT16_MAX) return;
    size_t vacant = SIZE_MAX;
    for (size_t i = 0; i < d->clamp_capacity; ++i) {
        if (d->clamps[i].used && !memcmp(d->clamps[i].peer.bytes, peer->bytes, 16)) {
            d->clamps[i].mtu = mtu; return;
        }
        if (!d->clamps[i].used) vacant = i;
    }
    if (vacant != SIZE_MAX) {
        d->clamps[vacant].used = true; d->clamps[vacant].peer = *peer; d->clamps[vacant].mtu = mtu;
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
            d->clamps[i].used = false; ++d->stats.mtu_releases;
        }
}

uint16_t fipsd_dp_link_mtu(handlers *h, const peer_rec *rec, fips_transport_id transport, const fips_transport_addr *to)
{
    fipsd_transport *t = hs_transport(h, transport);
    size_t mtu = t ? t->ops->mtu(t, to) : UINT16_MAX;
    if (rec->path_live && rec->path) {
        size_t index = fips_path_set_find(&rec->path->set, transport, to);
        const fips_peer_path *path = fips_path_set_get(&rec->path->set, index);
        if (path) mtu = fips_path_liveness_size(&path->live, mtu);
    }
    return mtu > UINT16_MAX ? UINT16_MAX : (uint16_t)mtu;
}
