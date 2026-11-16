#include "ipv6/link.h"
#include "log.h"
#include <fips/proto/framing.h>
#include <stdlib.h>
#include <string.h>
#define MOD "ipv6/link"
static void set_limit(fipsd_ipv6_mesh_state *s, uint16_t limit)
{
    atomic_store_explicit(&s->node_limit, limit, memory_order_relaxed);
    atomic_store_explicit(&s->ceiling, fipsd_ipv6_tcp_mss(limit), memory_order_relaxed);
}
int fipsd_ipv6_mesh_state_init(fipsd_ipv6_mesh_state *s, size_t key_capacity, size_t path_capacity)
{
    memset(s, 0, sizeof *s);
    s->key_capacity = key_capacity ? key_capacity : 1024;
    s->keys = calloc(s->key_capacity, sizeof *s->keys);
    if (!s->keys || fipsd_ipv6_paths_init(&s->paths, path_capacity) < 0) { free(s->keys); s->keys = NULL; return -1; }
    set_limit(s, fips_framing_payload_limit(FIPSD_IPV6_MIN_MTU));
    return 0;
}
void fipsd_ipv6_mesh_state_destroy(fipsd_ipv6_mesh_state *s)
{
    fipsd_ipv6_paths_destroy(&s->paths); free(s->keys); s->keys = NULL; s->key_count = 0;
}
static fipsd_ipv6_known *find(fipsd_ipv6_mesh_state *s, const fips_node_prefix *prefix)
{
    for (size_t i = 0; i < s->key_capacity; ++i)
        if (s->keys[i].used && !memcmp(s->keys[i].prefix.bytes, prefix->bytes, 15)) return &s->keys[i];
    return NULL;
}
/* keys_mu held. Replacing a prefix changes its key; a full table evicts the
 * entry inserted longest ago (upstream's map is unbounded). */
static void insert(fipsd_ipv6_mesh_state *s, const fips_node_addr *peer, uint64_t learned)
{
    fips_node_prefix prefix = fips_node_addr_prefix(peer);
    fipsd_ipv6_known *e = find(s, &prefix), *oldest = NULL;
    for (size_t i = 0; !e && i < s->key_capacity; ++i) {
        if (!s->keys[i].used) { e = &s->keys[i]; ++s->key_count; }
        else if (!oldest || s->keys[i].order < oldest->order) oldest = &s->keys[i];
    }
    if (!e) { e = oldest; FIPSD_WARN(MOD, "key table full (%zu); evicting the oldest key", s->key_capacity); }
    e->used = true; e->prefix = prefix; e->peer = *peer; e->learned = learned; e->order = ++s->next_order;
}
fipsd_ipv6_applied fipsd_ipv6_mesh_state_apply(fipsd_ipv6_mesh_state *s, const fipsd_ipv6_from_mesh *m, fips_ipv6_address *attached)
{
    switch (m->kind) {
    case FIPSD_IPV6_MESH_ATTACHED:
        /* An attach replays the per-peer limits in full: anything held from
         * an earlier attach is dropped first. */
        fipsd_ipv6_paths_clear(&s->paths);
        nsync_mu_lock(&s->keys_mu);
        for (size_t i = 0; i < s->key_capacity; ++i) s->keys[i].used = false;
        s->key_count = 0;
        nsync_mu_unlock(&s->keys_mu);
        set_limit(s, m->payload_limit);
        if (attached) *attached = fipsd_ipv6_peer_address(&m->peer);
        return FIPSD_IPV6_APPLIED_ATTACHED;
    case FIPSD_IPV6_MESH_SYNCED: return FIPSD_IPV6_APPLIED_SYNCED;
    case FIPSD_IPV6_MESH_NODE_MTU: set_limit(s, m->payload_limit); break;
    case FIPSD_IPV6_MESH_PATH_MTU: {
        fips_ipv6_address addr = fipsd_ipv6_peer_address(&m->peer);
        if (m->has_limit) (void)fipsd_ipv6_paths_set(&s->paths, addr.bytes, m->payload_limit);
        else fipsd_ipv6_paths_remove(&s->paths, addr.bytes);
        break;
    }
    case FIPSD_IPV6_MESH_KNOWN_KEY: {
        nsync_mu_lock(&s->keys_mu);
        fips_node_prefix prefix = fips_node_addr_prefix(&m->peer);
        fipsd_ipv6_known *e = find(s, &prefix);
        uint64_t learned = e && !memcmp(e->peer.bytes, m->peer.bytes, 16) ? e->learned : 0;
        insert(s, &m->peer, learned);
        nsync_mu_unlock(&s->keys_mu);
        break;
    }
    case FIPSD_IPV6_MESH_FORGOT_KEY: {
        nsync_mu_lock(&s->keys_mu);
        fips_node_prefix prefix = fips_node_addr_prefix(&m->peer);
        fipsd_ipv6_known *e = find(s, &prefix);
        /* A key the host handed out after the node decided this stays. */
        if (e && !memcmp(e->peer.bytes, m->peer.bytes, 16) && e->learned <= m->learned) { e->used = false; --s->key_count; }
        nsync_mu_unlock(&s->keys_mu);
        break;
    }
    }
    return FIPSD_IPV6_APPLIED_STATE;
}
fipsd_ipv6_learned fipsd_ipv6_mesh_state_learn(fipsd_ipv6_mesh_state *s, const fips_node_addr *peer)
{
    uint64_t seq = atomic_fetch_add_explicit(&s->learned, 1, memory_order_relaxed) + 1;
    nsync_mu_lock(&s->keys_mu); insert(s, peer, seq); nsync_mu_unlock(&s->keys_mu);
    return (fipsd_ipv6_learned){*peer, seq};
}
fipsd_ipv6_learned fipsd_ipv6_mesh_state_report(fipsd_ipv6_mesh_state *s, const fips_node_addr *peer)
{
    return (fipsd_ipv6_learned){*peer, atomic_fetch_add_explicit(&s->learned, 1, memory_order_relaxed) + 1};
}
bool fipsd_ipv6_mesh_state_key(fipsd_ipv6_mesh_state *s, const fips_node_prefix *prefix, fips_node_addr *out)
{
    nsync_mu_lock(&s->keys_mu);
    fipsd_ipv6_known *e = find(s, prefix);
    if (e && out) *out = e->peer;
    nsync_mu_unlock(&s->keys_mu);
    return e != NULL;
}
size_t fipsd_ipv6_mesh_state_keys(fipsd_ipv6_mesh_state *s, fips_node_addr *out, size_t cap)
{
    size_t n = 0;
    nsync_mu_lock(&s->keys_mu);
    for (size_t i = 0; i < s->key_capacity; ++i) if (s->keys[i].used) { if (n < cap) out[n] = s->keys[i].peer; ++n; }
    nsync_mu_unlock(&s->keys_mu);
    return n;
}
uint16_t fipsd_ipv6_mesh_state_node_limit(const fipsd_ipv6_mesh_state *s) { return atomic_load_explicit(&s->node_limit, memory_order_relaxed); }
uint16_t fipsd_ipv6_mesh_state_ipv6_mtu(const fipsd_ipv6_mesh_state *s) { return fipsd_ipv6_mtu_of_limit(fipsd_ipv6_mesh_state_node_limit(s)); }
uint16_t fipsd_ipv6_mesh_state_ceiling(const fipsd_ipv6_mesh_state *s) { return atomic_load_explicit(&s->ceiling, memory_order_relaxed); }
fipsd_ipv6_paths *fipsd_ipv6_mesh_state_paths(fipsd_ipv6_mesh_state *s) { return &s->paths; }
fips_ipv6_address fipsd_ipv6_peer_address(const fips_node_addr *peer) { return fips_ipv6_address_from_node_addr(peer); }
fips_node_prefix fipsd_ipv6_destination_prefix(const uint8_t *packet)
{
    fips_node_prefix p; memcpy(p.bytes, packet + 25, 15); return p;
}
void fipsd_ipv6_host_link_send(const fipsd_ipv6_host_link *link, const fipsd_ipv6_from_mesh *message)
{
    if (link && link->state) (void)fipsd_ipv6_mesh_state_apply(link->state, message, NULL);
}
