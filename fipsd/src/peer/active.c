#include "peer/active.h"
#include "peer/link.h"
#include "swap.h"
#include <stdlib.h>
#include <string.h>

struct fipsd_active_peer {
    fipsd_peer_info info;
    fipsd_peer_machine machine;
    GoSwap *published;
    bool link_live;
    fips_rekey link;
    fips_mmp_peer_state mmp;
    fips_mmp_owd_sample samples[64];
};
fipsd_active_peer *fipsd_active_peer_create(const fips_peer_identity *id,
    fips_transport_id transport, const fips_transport_addr *addr, fips_link_id link,
    fips_link_direction direction, uint64_t wall_ms, fips_instant now, fipsd_peer_timing timing)
{
    if (addr->len > FIPS_TRANSPORT_ADDR_CAPACITY || !fipsd_peer_timing_valid(timing)) return NULL;
    fipsd_active_peer *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->info.identity = *id; p->info.transport = transport; p->info.addr = *addr;
    p->info.link = link; p->info.direction = direction;
    fipsd_peer_data_init(&p->info.data, wall_ms, now, direction == FIPS_LINK_OUTBOUND);
    fipsd_peer_machine_init(&p->machine, link, direction, id,
        (fips_fmp_leg_stamp){wall_ms, now}, timing);
    p->machine.state = p->info.state = FIPSD_PEER_ESTABLISHED;
    p->machine.liveness_at = fips_deadline(now, fips_duration_from_millis(timing.heartbeat_ms));
    p->published = go_swap_create(NULL, free);
    if (!p->published || !fipsd_active_peer_publish(p)) {
        go_swap_free(p->published); free(p); return NULL;
    }
    return p;
}
bool fipsd_active_peer_destroy(fipsd_active_peer *p)
{
    if (!p) return true;
    if (p->link_live) return false;
    go_swap_free(p->published); free(p); return true;
}
const fipsd_peer_info *fipsd_active_peer_info(const fipsd_active_peer *p) { return &p->info; }
fipsd_peer_data *fipsd_active_peer_data(fipsd_active_peer *p) { return &p->info.data; }
void fipsd_active_peer_set_endpoint(fipsd_active_peer *p, fips_transport_id t, const fips_transport_addr *a)
{ p->info.transport = t; p->info.addr = *a; }
void fipsd_active_peer_step(fipsd_active_peer *p, fips_instant now,
    const fipsd_peer_event *e, fipsd_peer_actions *out)
{
    fipsd_peer_machine_step(&p->machine, now, e, out);
    p->info.state = p->machine.state;
    if (e->kind == FIPSD_PEER_HEARD && fipsd_peer_machine_established(&p->machine)) {
        p->info.data.heard = p->info.data.frame_heard = true;
        p->info.data.heard_at = now;
        p->info.data.last_seen = e->wall_ms;
        fips_link_stats_record_recv(&p->info.data.link_stats, e->authenticated_bytes, e->wall_ms);
    }
}
fips_instant fipsd_active_peer_next_wake(const fipsd_active_peer *p)
{ return fipsd_peer_machine_next_wake(&p->machine); }
bool fipsd_active_peer_store_msg1(fipsd_active_peer *p, const uint8_t *wire, size_t len)
{ return fips_fmp_connection_set_handshake_msg1(&p->machine.conn, wire, len) == FIPS_OK; }
bool fipsd_active_peer_publish(fipsd_active_peer *p)
{
    fipsd_peer_info *copy = malloc(sizeof(*copy));
    if (!copy) return false;
    if (p->info.has_mmp) p->info.metrics = p->mmp.metrics;
    *copy = p->info;
    if (!go_swap_store(p->published, copy)) { free(copy); return false; }
    return true;
}
bool fipsd_active_peer_mmp_init(fipsd_active_peer *p, fips_mmp_mode mode)
{
    if (p->info.has_mmp) return false;
    if (fips_mmp_peer_init(&p->mmp, mode, 60, 64, p->info.direction == FIPS_LINK_OUTBOUND,
            p->samples, 64) != FIPS_OK) return false;
    p->info.has_mmp = true; return true;
}
fips_mmp_peer_state *fipsd_active_peer_mmp(fipsd_active_peer *p)
{ return p->info.has_mmp ? &p->mmp : NULL; }
GoRefPtr fipsd_active_peer_snapshot_load(fipsd_active_peer *p) { return go_swap_load(p->published); }
bool fipsd_active_peer_link_attached(const fipsd_active_peer *p) { return p->link_live; }
fips_status fipsd_active_peer_link_established(fipsd_active_peer *p,
    const fips_rekey_profile *profile, fips_rekey_params params, fips_node_addr me,
    fips_rekey_completed *first, fips_rekey_ids *ids, fips_rekey_setup_facts *facts,
    fips_instant now, fips_rekey_env *env, fips_rekey_outputs *out)
{
    if (p->link_live || !profile || profile->layer != FIPS_REKEY_LAYER_Fmp ||
        env->transport != p->info.transport.value) return FIPS_ERR_RANGE;
    fips_peer_key key;
    memcpy(key.bytes, p->info.identity.pubkey.bytes, sizeof(key.bytes));
    fips_rekey_bond bond = {.kind = FIPS_REKEY_BOND_Key, .key = key};
    fips_status s = fips_rekey_established(&p->link, profile, params, bond, me,
        first, ids, facts, now, env, out);
    if (s == FIPS_OK) p->link_live = true;
    return s;
}
fips_status fipsd_active_peer_link_step(fipsd_active_peer *p, fips_instant now,
    fips_rekey_input in, fips_rekey_env *env, fips_rekey_outputs *out)
{
    if (!p->link_live || env->transport != p->info.transport.value) return FIPS_ERR_RANGE;
    return fips_rekey_step(&p->link, now, in, env, out);
}
fips_instant fipsd_active_peer_link_next_wake(const fipsd_active_peer *p)
{ return p->link_live ? fips_rekey_next_wake(&p->link) : (fips_instant){0}; }
fips_rekey_seal_refusal fipsd_active_peer_link_seal(fipsd_active_peer *p,
    fips_instant now, fips_rekey_sealed *out)
{ return p->link_live ? fips_rekey_seal(&p->link, now, out) : FIPS_REKEY_SEAL_NotEstablished; }
fips_rekey_view fipsd_active_peer_link_view(const fipsd_active_peer *p)
{ return p->link_live ? fips_rekey_get_view(&p->link) : (fips_rekey_view){0}; }
void fipsd_active_peer_link_close(fipsd_active_peer *p, fips_rekey_close reason,
    fips_rekey_outputs *out, fips_rekey_carry *carry)
{
    if (!p->link_live) return;
    (void)fips_rekey_close_machine(&p->link, reason, out, carry);
    fips_rekey_destroy(&p->link); p->link_live = false;
}
