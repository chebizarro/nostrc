#include "node/handlers/internal.h"
#include "host_os.h"

static uint64_t hash(uint64_t v, const void *p, size_t n)
{
    const uint8_t *b = p;
    while (n--) { v ^= *b++; v *= UINT64_C(1099511628211); }
    return v;
}
static uint64_t coord_hash(uint64_t v, const fips_tree_coordinate *c)
{
    if (!c) return hash(v, "none", 4);
    v = hash(v, &c->len, sizeof c->len);
    return hash(v, c->entries, c->len * sizeof c->entries[0]);
}
static GoRefPtr coord_copy(const fips_tree_coordinate *c, GoRefPtr old)
{
    if (!c) return (GoRefPtr){0};
    if (old.ptr && fips_tree_coordinate_equal(c, old.ptr)) { go_refptr_retain(&old); return old; }
    fips_tree_coordinate *p = calloc(1, sizeof *p);
    if (!p) return (GoRefPtr){0};
    p->len = c->len; memcpy(p->entries, c->entries, c->len * sizeof c->entries[0]);
    return make_go_refptr(p, free);
}
static void snapshot_free(void *value)
{
    fipsd_dp_snapshot *s = value;
    go_refptr_release(&s->my_coords);
    for (size_t i = 0; i < s->peer_count; ++i) {
        go_refptr_release(&s->peers[i].coords); go_refptr_release(&s->peers[i].filter);
    }
    for (size_t i = 0; i < s->epoch_count; ++i) fips_cipher_state_destroy(&s->epochs[i].recv);
    fipsd_dp_cache_release(s->cache);
    free(s->peers); free(s->epochs); free(s);
}
static uint64_t machine_hash(uint64_t v, const fips_rekey *m)
{
    for (int slot = FIPS_REKEY_Current; slot <= FIPS_REKEY_Previous; ++slot) {
        fips_rekey_epoch_tag tag = 0;
        bool present = fips_rekey_tag(m, (fips_rekey_slot)slot, &tag);
        v = hash(v, &present, sizeof present);
        v = hash(v, &tag, sizeof tag);
        if (present) {
            fips_rekey_index ours = fips_rekey_ours(m, (fips_rekey_slot)slot);
            v = hash(v, &ours.value, sizeof ours.value);
            const fips_noise_session *keys = fips_rekey_keys(m, (fips_rekey_slot)slot);
            v = hash(v, keys->recv_cipher.key.bytes, sizeof keys->recv_cipher.key.bytes);
        }
    }
    return v;
}
static void epochs(fipsd_dp_snapshot *s, const fips_rekey *m, const fips_node_addr *peer,
                   bool session, fips_transport_id transport, bool multipath)
{
    for (int slot = FIPS_REKEY_Current; slot <= FIPS_REKEY_Previous; ++slot) {
        fips_rekey_epoch_tag tag;
        if (!fips_rekey_tag(m, (fips_rekey_slot)slot, &tag)) continue;
        const fips_noise_session *keys = fips_rekey_keys(m, (fips_rekey_slot)slot);
        fipsd_dp_epoch *e = &s->epochs[s->epoch_count];
        if (!keys || fips_noise_session_recv_cipher_clone(keys, &e->recv) != FIPS_NOISE_OK) continue;
        fips_rekey_view view = fips_rekey_get_view(m);
        const fips_rekey_epoch_view *ev = slot == FIPS_REKEY_Current ? &view.current :
            slot == FIPS_REKEY_Pending ? &view.pending : &view.previous;
        *e = (fipsd_dp_epoch){.peer = *peer, .session = session, .tag = tag, .k = ev->k,
            .index = fips_rekey_ours(m, (fips_rekey_slot)slot).value, .transport = transport,
            .multipath = multipath, .recv = e->recv};
        ++s->epoch_count;
    }
}
bool fipsd_dp_publish(handlers *h)
{
    fipsd_dataplane *d = h->dataplane;
    if (!d || !h->tree) return false;
    fipsd_dp_snapshot *old = d->current.ptr;
    if (old && !d->needs_publish) return true;
    uint64_t fp = coord_hash(UINT64_C(1469598103934665603), fips_tree_state_my_coords(h->tree));
    for (size_t i = 0; i < h->cfg.peer_capacity; ++i) {
        peer_rec *r = &h->recs[i];
        if (!r->used) continue;
        const fipsd_peer_info *info = fipsd_active_peer_info(r->peer);
        fp = hash(fp, &r->addr, sizeof r->addr);
        fp = hash(fp, &info->addr, sizeof info->addr);
        fp = hash(fp, &info->transport, sizeof info->transport);
        fp = coord_hash(fp, hs_tree_peer_coords(h, &r->addr));
        const fips_bloom_filter *f = hs_bloom_peer_filter(h, &r->addr);
        if (f) { fp = hash(fp, &f->num_bits, sizeof f->num_bits);
            fp = hash(fp, &f->hash_count, sizeof f->hash_count); fp = hash(fp, f->bits, f->num_bits / 8); }
        fp = machine_hash(fp, fipsd_active_peer_link_machine(r->peer));
        bool multipath = hs_rec_multipath(r); fp = hash(fp, &multipath, sizeof multipath);
        uint16_t mtu = fipsd_dp_link_mtu(h, r, info->transport, &info->addr);
        fp = hash(fp, &mtu, sizeof mtu);
        double cost = hs_mmp_link_cost(h, r); fp = hash(fp, &cost, sizeof cost);
    }
    for (size_t i = 0; i < h->sessions.capacity; ++i) if (h->session_entries[i].occupied) {
        fp = hash(fp, &h->session_entries[i].peer, sizeof(fips_node_addr));
        fp = machine_hash(fp, &h->session_entries[i].machine);
    }
    /* The cache section (nostrc-x345n): every resident entry, expired or not
     * (forwarding applies the expiry), brought up to the core's generation
     * from the slots it reports changed. Nothing to do while the generation
     * stands, and a change rebuilds only the parts its rows hash to; the
     * section, and every part it did not touch, are shared by reference. */
    if (!fipsd_dp_cache_view_update(&d->cache_view, &h->cache)) return false;
    fp = hash(fp, &d->cache_view.version, sizeof d->cache_view.version);
    if (old && old->fingerprint == fp) { d->needs_publish = false; return true; }
    fipsd_dp_snapshot *s = calloc(1, sizeof *s);
    if (!s) return false;
    s->me = h->me; s->origin = h->origin; s->fingerprint = fp;
    s->peers = calloc(h->cfg.peer_capacity, sizeof *s->peers);
    s->epochs = calloc(3 * (h->cfg.peer_capacity + h->cfg.session_capacity), sizeof *s->epochs);
    s->cache = fipsd_dp_cache_retain(d->cache_view.section);
    s->my_coords = coord_copy(fips_tree_state_my_coords(h->tree), old ? old->my_coords : (GoRefPtr){0});
    if (!s->peers || !s->epochs || !s->my_coords.ptr) { snapshot_free(s); return false; }
    for (size_t i = 0; i < h->cfg.peer_capacity; ++i) {
        peer_rec *r = &h->recs[i]; if (!r->used) continue;
        const fipsd_peer_info *info = fipsd_active_peer_info(r->peer);
        fipsd_dp_peer *p = &s->peers[s->peer_count++];
        fipsd_dp_peer *prev = NULL;
        if (old) for (size_t j = 0; j < old->peer_count; ++j)
            if (!memcmp(old->peers[j].addr.bytes, r->addr.bytes, 16)) { prev = &old->peers[j]; break; }
        p->addr = r->addr; p->transport = info->transport; p->endpoint = info->addr;
        p->cost = hs_mmp_link_cost(h, r);
        p->mtu = fipsd_dp_link_mtu(h, r, info->transport, &info->addr);
        const fips_tree_coordinate *c = hs_tree_peer_coords(h, &r->addr);
        p->coords = coord_copy(c, prev ? prev->coords : (GoRefPtr){0});
        if (c && !p->coords.ptr) { snapshot_free(s); return false; }
        const fips_bloom_filter *f = hs_bloom_peer_filter(h, &r->addr);
        if (f) {
            if (prev && prev->filter.ptr && fips_bloom_equal(f, prev->filter.ptr)) {
                p->filter = prev->filter; go_refptr_retain(&p->filter);
            } else {
                fips_bloom_filter *copy = malloc(sizeof *copy);
                if (!copy) { snapshot_free(s); return false; }
                *copy = *f; p->filter = make_go_refptr(copy, free);
            }
        }
        epochs(s, fipsd_active_peer_link_machine(r->peer), &r->addr, false, info->transport, hs_rec_multipath(r));
    }
    for (size_t i = 0; i < h->sessions.capacity; ++i) if (h->session_entries[i].occupied)
        epochs(s, &h->session_entries[i].machine, &h->session_entries[i].peer, true, (fips_transport_id){0}, false);
    if (!go_swap_store(d->tables, s)) { snapshot_free(s); return false; }
    go_refptr_release(&d->current);
    d->current = go_swap_load(d->tables); /* once per changed generation, NOT a packet read */
    d->needs_publish = false;
    return true;
}
bool fipsd_dp_init(handlers *h)
{
    fipsd_dataplane *d = calloc(1, sizeof *d);
    if (!d) return false;
    h->dataplane = d; d->needs_publish = true; d->owner = h;
    fipsd_dp_cache_view_init(&d->cache_view, h->entropy.fill ? h->entropy : fipsd_host_os_entropy());
    d->packet_count = FIPSD_DP_PACKETS;
    d->clamp_capacity = h->cfg.session_capacity + h->cfg.peer_capacity;
    d->packets = calloc(d->packet_count, sizeof *d->packets);
    d->clamps = calloc(d->clamp_capacity, sizeof *d->clamps);
    d->coord_scratch = calloc(1, sizeof *d->coord_scratch);
    d->route_peers = calloc(h->cfg.peer_capacity, sizeof *d->route_peers);
    d->tables = go_swap_create(NULL, snapshot_free);
    return d->packets && d->clamps && d->coord_scratch && d->route_peers && d->tables;
}
void fipsd_dp_destroy(fipsd_dataplane *d)
{
    if (!d) return;
    /* Runtime joined AND result queues drained before handlers are destroyed. */
    go_refptr_release(&d->current); go_swap_free(d->tables);
    fipsd_dp_cache_view_free(&d->cache_view);
    free(d->coord_scratch); free(d->route_peers); free(d->clamps); free(d->packets); free(d);
}
