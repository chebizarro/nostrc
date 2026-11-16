/* FSP sessions (upstream handlers/session.rs): the session table owns one XK
 * machine per remote address; SessionDatagrams for us are classified by the
 * strict FSP ingress gate and stepped through fips_fsp_rekey_receive or the
 * table's inbound answer; datagrams for others are routed by fips_router_route
 * and re-sealed on the next hop's link. Pending application datagrams wait,
 * bounded, for the first handshake or a coordinate lookup. */
#include "node/handlers/internal.h"

bool hs_session_init(handlers *h)
{
    fips_session_table_init(&h->sessions, h->session_entries, h->cfg.session_capacity);
    return true;
}
session_ext *hs_session_ext(handlers *h, const fips_session_entry *entry)
{
    if (!entry || entry < h->session_entries || entry >= h->session_entries + h->cfg.session_capacity) return NULL;
    return &h->session_ext[entry - h->session_entries];
}
static session_ext *ext_init(handlers *h, fips_session_entry *entry, const fips_node_addr *peer, bool initiator)
{
    session_ext *x = hs_session_ext(h, entry);
    pending_datagram *pending = x->pending; size_t cap = x->pending_capacity;
    memset(x, 0, sizeof *x);
    x->pending = pending; x->pending_capacity = cap;
    x->used = true; x->peer = *peer; x->is_initiator = initiator; x->created = hs_now(h); x->last_data = x->created;
    x->mmp_live = fips_mmp_session_init(&x->mmp, h->cfg.core.session_mmp_mode, h->cfg.core.session_mmp_log_interval_secs,
        h->cfg.core.session_mmp_window ? h->cfg.core.session_mmp_window : FIPS_MMP_DEFAULT_OWD_WINDOW_SIZE,
        initiator, x->owd, 64) == FIPS_OK;
    return x;
}
/* Destination coordinates: a direct peer's tree position, else the cache. */
static bool dest_coords(handlers *h, const fips_node_addr *dest, session_ext *x)
{
    const fips_tree_coordinate *c = hs_tree_peer_coords(h, dest);
    if (c && c->len) {
        size_t n = c->len < FIPSD_HANDLERS_COORD_ADDRS ? c->len : FIPSD_HANDLERS_COORD_ADDRS;
        for (size_t i = 0; i < n; ++i) x->dest_coords[i] = c->entries[i].node_addr;
        x->dest_count = n; x->has_dest_coords = true; return true;
    }
    uint16_t mtu;
    if (hs_cache_lookup(h, dest, x->dest_coords, FIPSD_HANDLERS_COORD_ADDRS, &x->dest_count, &mtu)) { x->has_dest_coords = true; return true; }
    return false;
}
static bool peer_key_for(handlers *h, const fips_node_addr *dest, fips_peer_key *out)
{
    fipsd_active_peer *p = fipsd_peer_table_find(h->peers->table, dest);
    if (p) { *out = fips_peer_key_from_xonly(&fipsd_active_peer_info(p)->identity.pubkey); return true; }
    const fips_peer_identity *id = fipsd_identity_cache_touch(&h->identities, fips_node_addr_prefix(dest), hs_wall_ms(h));
    if (!id) return false;
    *out = fips_peer_key_from_xonly(&id->pubkey);
    return true;
}
static fips_rekey_route_handle route_of(handlers *h, peer_rec *rec) { return rec ? (fips_rekey_route_handle)(rec - h->recs) + 1 : 0; }

bool hs_route_datagram(handlers *h, const fips_session_datagram *dg, const fips_node_addr *exclude, bool incoming_ce)
{
    fips_routing_peer peers[64]; size_t n = 0;
    peer_rec *direct = hs_rec_find(h, &dg->dest_addr);
    for (size_t i = 0; i < h->cfg.peer_capacity && n < 64; ++i) {
        peer_rec *rec = &h->recs[i];
        if (!rec->used || (exclude && !memcmp(rec->addr.bytes, exclude->bytes, 16))) continue;
        peers[n++] = (fips_routing_peer){.addr = rec->addr, .coords = hs_tree_peer_coords(h, &rec->addr),
            .filter = hs_bloom_peer_filter(h, &rec->addr), .link_cost = hs_mmp_link_cost(h, rec)};
    }
    fips_routing_view rv = {.peers = peers, .peer_count = n, .ctx = h, .cached_coords = hs_cache_coords};
    fips_next_hop hop = {0};
    if (direct) {
        const fipsd_peer_info *info = fipsd_active_peer_info(direct->peer);
        fipsd_transport *t = hs_transport(h, info->transport);
        hop = (fips_next_hop){direct->addr, t ? t->ops->mtu(t, &info->addr) : 1280};
    }
    fips_writer w; fips_writer_init(&w, h->route_buf, sizeof h->route_buf);
    fips_route_outcome out; fips_proto_error e;
    if (fips_router_route(&h->router, dg, &h->me, incoming_ce, direct ? &hop : NULL, &rv, &w, &out, &e) != FIPS_OK) {
        ++h->counters.fwd_decode_error_packets; return false;
    }
    bool originated = !memcmp(dg->src_addr.bytes, h->me.bytes, 16);
    switch (out.kind) {
    case FIPS_ROUTE_Forward: {
        peer_rec *next = hs_rec_find(h, &out.next_hop);
        if (!next) { ++h->counters.fwd_drop_no_route_packets; h->counters.fwd_drop_no_route_bytes += dg->payload_len; return false; }
        if (direct) ++h->counters.fwd_route_direct_peer; else ++h->counters.fwd_route_tree_up;
        bool ok = hs_link_send(h, next, h->route_buf, fips_writer_len(&w));
        if (ok) {
            if (originated) { ++h->counters.fwd_originated_packets; h->counters.fwd_originated_bytes += dg->payload_len; }
            else { ++h->counters.fwd_forwarded_packets; h->counters.fwd_forwarded_bytes += dg->payload_len; }
        }
        return ok;
    }
    case FIPS_ROUTE_DeliverLocal: return false; /* callers deliver before routing */
    case FIPS_ROUTE_Drop: ++h->counters.fwd_ttl_exhausted_packets; return false;
    case FIPS_ROUTE_NoRoute:
        ++h->counters.fwd_drop_no_route_packets; h->counters.fwd_drop_no_route_bytes += dg->payload_len;
        return false;
    }
    return false;
}
/* Seal one session message under the current epoch and route it. */
bool hs_session_send_message(handlers *h, fips_session_entry *entry, session_ext *x, uint8_t msg_type, const uint8_t *body, size_t len)
{
    fips_rekey_sealed s = {0};
    fips_rekey_seal_refusal refusal = fips_rekey_seal(&entry->machine, hs_now(h), &s);
    if (refusal != FIPS_REKEY_SEAL_OK) { ++h->counters.send_failed; return false; }
    uint32_t ts = (uint32_t)(s.elapsed.secs * 1000 + s.elapsed.nanos / 1000000);
    fips_writer w; fips_writer_init(&w, h->session_plain, sizeof h->session_plain);
    size_t n = 0;
    bool ok = fips_fsp_prepend_inner_header(ts, msg_type, 0, body, len, &w, NULL) == FIPS_OK &&
        fips_session_encode_sealed(&fips_fsp_rekey_profile, &s, h->session_plain, fips_writer_len(&w),
                                   h->session_wire, sizeof h->session_wire, &n) == FIPS_OK;
    uint64_t counter = s.counter;
    fips_rekey_sealed_clear(&s);
    if (!ok) { ++h->counters.send_failed; return false; }
    fips_session_datagram dg;
    fips_session_datagram_init(&dg, h->me, x->peer, h->session_wire, n);
    ok = hs_route_datagram(h, &dg, NULL, false);
    if (x->mmp_live) fips_mmp_sender_record_sent(&x->mmp.roles.sender, counter, ts, n);
    fips_rekey_outputs_clear(&h->session_out);
    fips_rekey_env env; hs_env(h, &env, 0);
    if (fips_rekey_step(&entry->machine, hs_now(h), (fips_rekey_input){.kind = FIPS_REKEY_IN_DataSent}, &env, &h->session_out) == FIPS_OK)
        (void)hs_session_execute(h, entry, NULL, 0);
    return ok;
}
static bool send_data(handlers *h, fips_session_entry *entry, session_ext *x, uint16_t sp, uint16_t dp, const uint8_t *payload, size_t len)
{
    uint8_t body[FIPSD_HANDLERS_DATAGRAM_MAX + 4];
    fips_writer w; fips_writer_init(&w, body, sizeof body);
    if (fips_fsp_port_header_encode((fips_fsp_port_header){sp, dp}, &w, NULL) != FIPS_OK ||
        fips_writer_write_bytes(&w, payload, len, NULL) != FIPS_OK) return false;
    if (!hs_session_send_message(h, entry, x, FIPS_SESSION_DataPacket, body, fips_writer_len(&w))) return false;
    ++x->sent; ++h->counters.sess_data_out; x->last_data = hs_now(h);
    fips_session_note_activity(entry, hs_now(h));
    return true;
}
static void flush_pending(handlers *h, fips_session_entry *entry, session_ext *x)
{
    size_t n = x->pending_len; x->pending_len = 0;
    for (size_t i = 0; i < n && entry->occupied && x->established; ++i) {
        pending_datagram *d = &x->pending[i];
        if (!send_data(h, entry, x, d->src_port, d->dst_port, d->bytes, d->len)) {
            fipsd_node_notice notice = {.kind = FIPSD_NOTICE_SEND_FAILED, .peer = x->peer};
            hs_notice_full(h, &notice, NULL, 0);
        }
    }
}
static void queue_pending(handlers *h, session_ext *x, uint16_t sp, uint16_t dp, const uint8_t *payload, size_t len)
{
    (void)h;
    if (x->pending_len == x->pending_capacity) { /* drop oldest, as push_bounded_pending */
        memmove(x->pending, x->pending + 1, (x->pending_len - 1) * sizeof *x->pending); --x->pending_len;
    }
    pending_datagram *d = &x->pending[x->pending_len++];
    d->src_port = sp; d->dst_port = dp; d->len = len;
    if (len) memcpy(d->bytes, payload, len);
}
static void hold(handlers *h, const fips_node_addr *dest, uint16_t sp, uint16_t dp, const uint8_t *payload, size_t len)
{
    size_t slot = SIZE_MAX;
    for (size_t i = 0; i < h->cfg.session_capacity; ++i) {
        if (h->held[i].used && !memcmp(h->held[i].dest.bytes, dest->bytes, 16)) { slot = i; break; }
        if (!h->held[i].used && slot == SIZE_MAX) slot = i;
    }
    if (slot == SIZE_MAX) return;
    if (!h->held[slot].used) {
        h->held[slot].items = h->held[slot].items ? h->held[slot].items : calloc(h->cfg.pending_per_destination, sizeof(pending_datagram));
        if (!h->held[slot].items) return;
        h->held[slot].used = true; h->held[slot].dest = *dest; h->held[slot].len = 0; h->held[slot].since = hs_now(h);
    }
    if (h->held[slot].len == h->cfg.pending_per_destination) {
        memmove(h->held[slot].items, h->held[slot].items + 1, (h->held[slot].len - 1) * sizeof(pending_datagram)); --h->held[slot].len;
    }
    pending_datagram *d = &h->held[slot].items[h->held[slot].len++];
    d->src_port = sp; d->dst_port = dp; d->len = len;
    if (len) memcpy(d->bytes, payload, len);
}
static fips_session_entry *open_session(handlers *h, const fips_node_addr *dest)
{
    fips_peer_key key;
    if (!peer_key_for(h, dest, &key)) return NULL;
    fips_rekey_env env; hs_env(h, &env, 0);
    fips_rekey_outputs_clear(&h->session_out);
    fips_session_entry *entry = NULL;
    if (fips_session_table_open(&h->sessions, key, h->me, h->session_params, hs_now(h), &env, &h->session_out, &entry) != FIPS_OK) {
        fips_rekey_outputs_clear(&h->session_out); return NULL;
    }
    session_ext *x = ext_init(h, entry, dest, true);
    (void)dest_coords(h, dest, x);
    (void)hs_session_execute(h, entry, NULL, 0);
    hs_mark_dirty(h);
    return entry->occupied ? entry : NULL;
}
bool hs_session_open(handlers *h, const fips_node_addr *dest)
{
    if (fips_session_table_by_peer(&h->sessions, *dest)) return true;
    return open_session(h, dest) != NULL;
}
bool hs_session_send(handlers *h, const fips_node_addr *dest, uint16_t sp, uint16_t dp, const uint8_t *payload, size_t len)
{
    if (!memcmp(dest->bytes, h->me.bytes, 16)) { hs_native_deliver(h, dest, sp, dp, payload, len); return true; }
    fips_session_entry *entry = fips_session_table_by_peer(&h->sessions, *dest);
    session_ext *x = entry ? hs_session_ext(h, entry) : NULL;
    if (entry && x->established) return send_data(h, entry, x, sp, dp, payload, len);
    if (!entry) {
        fips_peer_key key;
        if (!peer_key_for(h, dest, &key)) {
            fipsd_node_notice n = {.kind = FIPSD_NOTICE_SEND_FAILED, .peer = *dest};
            hs_notice_full(h, &n, NULL, 0); return false;
        }
        session_ext probe = {0};
        if (!dest_coords(h, dest, &probe)) {
            hold(h, dest, sp, dp, payload, len);
            (void)hs_lookup_initiate(h, dest);
            return true;
        }
        entry = open_session(h, dest);
        if (!entry) { fipsd_node_notice n = {.kind = FIPSD_NOTICE_SEND_FAILED, .peer = *dest}; hs_notice_full(h, &n, NULL, 0); return false; }
        x = hs_session_ext(h, entry);
        if (x->established) return send_data(h, entry, x, sp, dp, payload, len);
    }
    queue_pending(h, x, sp, dp, payload, len);
    return true;
}
void hs_session_coords_resolved(handlers *h, const fips_node_addr *dest)
{
    fips_session_entry *entry = fips_session_table_by_peer(&h->sessions, *dest);
    if (entry) {
        session_ext *x = hs_session_ext(h, entry);
        if (!x->has_dest_coords) (void)dest_coords(h, dest, x);
        if (x->established) flush_pending(h, entry, x);
    }
    for (size_t i = 0; i < h->cfg.session_capacity; ++i) {
        if (!h->held[i].used || memcmp(h->held[i].dest.bytes, dest->bytes, 16)) continue;
        size_t n = h->held[i].len; h->held[i].used = false; h->held[i].len = 0;
        for (size_t j = 0; j < n; ++j) {
            pending_datagram *d = &h->held[i].items[j];
            (void)hs_session_send(h, dest, d->src_port, d->dst_port, d->bytes, d->len);
        }
    }
}
static void setup_received(handlers *h, peer_rec *rec, const fips_session_datagram *dg, uint8_t *frame, size_t len)
{
    ++h->counters.sess_setup_received;
    fips_session_entry *entry = fips_session_table_by_peer(&h->sessions, dg->src_addr);
    bool admitted = false;
    if (fips_session_setup_admit(&h->setup_limiter, rec->addr.bytes, entry ? FIPS_MSG1_ESTABLISHED : FIPS_MSG1_STRANGER, hs_now(h), &admitted) != FIPS_OK || !admitted) {
        ++h->counters.sess_setup_rate_limited; return;
    }
    fips_reader r; fips_reader_init(&r, frame + FIPS_FRAMING_FSP_COMMON_PREFIX_SIZE, len - FIPS_FRAMING_FSP_COMMON_PREFIX_SIZE);
    fips_fsp_session_setup msg;
    if (fips_fsp_session_setup_decode_strict(&r, &msg, NULL) != FIPS_OK || msg.src_coords.count == 0 ||
        msg.src_coords.count > FIPSD_HANDLERS_COORD_ADDRS) { ++h->counters.sess_decode_error; return; }
    fips_rekey_env env; hs_env(h, &env, 0);
    if (entry) {
        /* Overdue work first, then re-admit the setup to the surviving entry. */
        fips_instant wake = fips_rekey_next_wake(&entry->machine);
        if (wake.ops && wake.ops->compare(hs_now(h), wake) >= 0) {
            fips_rekey_outputs_clear(&h->session_out);
            if (fips_rekey_step(&entry->machine, hs_now(h), (fips_rekey_input){.kind = FIPS_REKEY_IN_Poll}, &env, &h->session_out) == FIPS_OK)
                (void)hs_session_execute(h, entry, NULL, 0);
            entry = fips_session_table_by_peer(&h->sessions, dg->src_addr);
        }
    }
    if (entry) {
        session_ext *x = hs_session_ext(h, entry);
        x->carrier = rec->addr;
        fips_rekey_outputs_clear(&h->session_out);
        if (fips_fsp_rekey_receive(&entry->machine, hs_now(h), rec->addr, route_of(h, rec), frame, len, &env, &h->session_out) == FIPS_OK)
            (void)hs_session_execute(h, entry, frame, len);
        else fips_rekey_outputs_clear(&h->session_out);
        return;
    }
    fips_rekey_input in = {.kind = FIPS_REKEY_IN_Setup, .bytes = msg.handshake_payload.data, .len = msg.handshake_payload.len,
        .carrier = rec->addr, .route = route_of(h, rec)};
    fips_rekey_outputs_clear(&h->session_out);
    if (fips_session_table_answer(&h->sessions, dg->src_addr, h->me, h->session_params, hs_now(h), in, &env, &h->session_out, &entry) != FIPS_OK) {
        fips_rekey_outputs_clear(&h->session_out); return;
    }
    session_ext *x = ext_init(h, entry, &dg->src_addr, false);
    x->carrier = rec->addr;
    for (size_t i = 0; i < msg.src_coords.count; ++i) memcpy(x->dest_coords[i].bytes, msg.src_coords.addrs + 16 * i, 16);
    x->dest_count = msg.src_coords.count; x->has_dest_coords = true;
    (void)hs_session_execute(h, entry, frame, len);
    hs_mark_dirty(h);
}
void hs_session_datagram_received(handlers *h, peer_rec *rec, const uint8_t *body, size_t len)
{
    ++h->counters.fwd_received_packets; h->counters.fwd_received_bytes += len;
    fips_reader r; fips_reader_init(&r, body, len);
    fips_session_datagram dg;
    if (fips_session_datagram_decode(&r, &dg, NULL) != FIPS_OK) { ++h->counters.fwd_decode_error_packets; return; }
    if (memcmp(dg.dest_addr.bytes, h->me.bytes, 16)) {
        (void)hs_route_datagram(h, &dg, &rec->addr, false);
        return;
    }
    ++h->counters.fwd_delivered_packets; h->counters.fwd_delivered_bytes += dg.payload_len;
    if (dg.payload_len > sizeof h->session_frame) { ++h->counters.sess_decode_error; return; }
    memcpy(h->session_frame, dg.payload, dg.payload_len);
    uint8_t *frame = h->session_frame; size_t flen = dg.payload_len;
    fips_fsp_action action; fips_fsp_action_list list = {&action, 0, 1};
    if (fips_fsp_receive(dg.src_addr, frame, flen, &list) != FIPS_OK || action.kind == FIPS_FSP_DropFrame) { ++h->counters.sess_decode_error; return; }
    if (action.kind == FIPS_FSP_ReceiveRoutingSignal) { hs_routing_signal(h, rec, &dg.src_addr, frame, flen); return; }
    if (action.kind == FIPS_FSP_ReceiveSetup) { setup_received(h, rec, &dg, frame, flen); return; }
    fips_session_entry *entry = fips_session_table_by_peer(&h->sessions, dg.src_addr);
    if (!entry) { ++h->counters.sess_decode_error; return; }
    session_ext *x = hs_session_ext(h, entry);
    x->carrier = rec->addr;
    fips_rekey_env env; hs_env(h, &env, 0);
    fips_rekey_outputs_clear(&h->session_out);
    if (fips_fsp_rekey_receive(&entry->machine, hs_now(h), rec->addr, route_of(h, rec), frame, flen, &env, &h->session_out) == FIPS_OK)
        (void)hs_session_execute(h, entry, frame, flen);
    else fips_rekey_outputs_clear(&h->session_out);
}
void hs_session_message(handlers *h, fips_session_entry *entry, const uint8_t *plaintext, size_t len)
{
    session_ext *x = hs_session_ext(h, entry);
    fips_fsp_inner_header inner; fips_fsp_bytes payload;
    if (!fips_fsp_strip_inner_header(plaintext, len, &inner, &payload)) return;
    if (x && x->mmp_live) {
        /* Session-layer receive accounting rides the authenticated frame. */
        fips_mmp_receiver_record_frame(&x->mmp.roles.receiver, x->received, inner.timestamp_ms, len, false, hs_now_ms(h), true);
    }
    if (inner.msg_type == FIPS_SESSION_DataPacket) {
        fips_reader r; fips_reader_init(&r, payload.data, payload.len);
        fips_fsp_port_header ports;
        if (fips_fsp_port_header_decode(&r, &ports, NULL) != FIPS_OK) { ++h->counters.sess_decode_error; return; }
        if (x) { ++x->received; x->last_data = hs_now(h); }
        ++h->counters.sess_data_in;
        fips_session_note_activity(entry, hs_now(h));
        const uint8_t *data = fips_reader_rest(&r);
        size_t data_len = fips_reader_remaining(&r);
        hs_native_deliver(h, &entry->peer, ports.src_port, ports.dst_port, data, data_len);
        return;
    }
    if (x) hs_mmp_session_message(h, entry, x, inner.msg_type, payload.data, payload.len);
}
void hs_routing_signal(handlers *h, peer_rec *rec, const fips_node_addr *src, const uint8_t *frame, size_t len)
{
    if (len < FIPS_FRAMING_FSP_COMMON_PREFIX_SIZE + 1) return;
    fips_routing_signal_type type;
    if (!fips_routing_signal_type_from_byte(frame[FIPS_FRAMING_FSP_COMMON_PREFIX_SIZE], &type)) return;
    fips_reader r; fips_reader_init(&r, frame + FIPS_FRAMING_FSP_COMMON_PREFIX_SIZE + 1, len - FIPS_FRAMING_FSP_COMMON_PREFIX_SIZE - 1);
    fips_fsp_action items[4]; fips_fsp_action_list actions = {items, 0, 4};
    (void)src;
    if (type == FIPS_ROUTING_CoordsRequired) {
        fips_coords_required m;
        if (fips_coords_required_decode(&r, &m, NULL) != FIPS_OK) return;
        bool cached = fipsd_identity_cache_touch(&h->identities, fips_node_addr_prefix(&m.dest_addr), hs_wall_ms(h)) != NULL;
        (void)fips_fsp_plan_coords_required_lookup(m.dest_addr, cached, &actions);
    } else if (type == FIPS_ROUTING_PathBroken) {
        fips_path_broken m;
        if (fips_path_broken_decode(&r, &m, h->coord_scratch, NULL) != FIPS_OK) return;
        const fips_cache_entry *e = fips_coord_cache_get_entry(&h->cache, m.dest_addr.bytes);
        bool verified = e && fips_cache_entry_is_verified(e, fips_coord_cache_millis(&h->cache, hs_now(h)));
        /* Quorum votes are keyed on the authenticated link peer, never the body. */
        fips_fsp_quorum_entry storage[8]; fips_fsp_link_quorum q; fips_fsp_link_quorum_init(&q, storage, 8);
        fips_fsp_quorum_verdict v = {FIPS_FSP_Below, 1};
        (void)fips_fsp_link_quorum_record(&q, m.dest_addr, rec->addr, hs_now_ms(h), &v);
        (void)fips_fsp_plan_path_broken(m.dest_addr, verified, v, &actions);
    } else {
        fips_mtu_exceeded m;
        if (fips_mtu_exceeded_decode(&r, &m, NULL) != FIPS_OK) return;
        const fips_cache_entry *e = fips_coord_cache_get_entry(&h->cache, m.dest_addr.bytes);
        if (e && fips_fsp_should_apply_path_mtu(e->has_path_mtu, e->path_mtu, m.mtu))
            (void)fips_coord_cache_insert_verified_with_path_mtu(&h->cache, m.dest_addr.bytes, &e->coords, hs_now(h), m.mtu);
        return;
    }
    for (size_t i = 0; i < actions.len; ++i) {
        const fips_fsp_action *a = &items[i];
        switch (a->kind) {
        case FIPS_FSP_InvalidateCoords: (void)fips_coord_cache_remove(&h->cache, a->dest.bytes, NULL); break;
        case FIPS_FSP_DemoteCoords: (void)fips_coord_cache_demote(&h->cache, a->dest.bytes); break;
        case FIPS_FSP_InitiateLookup: (void)hs_lookup_initiate(h, &a->dest); break;
        default: break;
        }
    }
}
static void close_entry(handlers *h, fips_session_entry *entry, fips_rekey_close why)
{
    session_ext *x = hs_session_ext(h, entry);
    fips_node_addr peer = entry->peer;
    fips_rekey_carry carry = {0};
    fips_rekey_outputs_clear(&h->session_out);
    if (fips_session_table_remove(&h->sessions, entry, why, &h->session_out, &carry)) {
        /* The entry is wiped; its released leases still need executing. */
        for (size_t i = 0; i < h->session_out.count; ++i)
            if (h->session_out.items[i].kind == FIPS_REKEY_OUT_Release) {
                uint32_t index;
                if (fips_rekey_index_lease_release(&h->session_out.items[i].lease, &index)) (void)fips_index_allocator_free(&h->indices, (fips_session_index){index});
            }
    }
    fips_rekey_outputs_clear(&h->session_out);
    if (x) { x->used = false; x->established = false; x->pending_len = 0; }
    ++h->counters.sess_closed;
    hs_notice(h, FIPSD_NOTICE_SESSION_DOWN, &peer, NULL, 0);
    hs_mark_dirty(h);
}
uint64_t hs_session_wait_ms(handlers *h, fips_instant now)
{
    uint64_t wait = UINT64_MAX;
    for (size_t i = 0; i < h->cfg.session_capacity; ++i)
        if (h->session_entries[i].occupied && h->session_entries[i].machine.live)
            wait = hs_min_wait(wait, fips_rekey_next_wake(&h->session_entries[i].machine), now);
    return wait;
}
void hs_session_poll(handlers *h, fips_instant now)
{
    fips_rekey_env env; hs_env(h, &env, 0);
    for (size_t i = 0; i < h->cfg.session_capacity; ++i) {
        fips_session_entry *entry = &h->session_entries[i];
        if (!entry->occupied || !entry->machine.live) continue;
        fips_instant wake = fips_rekey_next_wake(&entry->machine);
        if (!wake.ops || wake.ops->compare(now, wake) < 0) continue;
        fips_rekey_outputs_clear(&h->session_out);
        if (fips_rekey_step(&entry->machine, now, (fips_rekey_input){.kind = FIPS_REKEY_IN_Poll}, &env, &h->session_out) == FIPS_OK)
            (void)hs_session_execute(h, entry, NULL, 0);
        else fips_rekey_outputs_clear(&h->session_out);
    }
}
void hs_session_tick(handlers *h, fips_instant now)
{
    fips_rekey_env env; hs_env(h, &env, 0);
    for (size_t i = 0; i < h->cfg.session_capacity; ++i) {
        fips_session_entry *entry = &h->session_entries[i];
        if (!entry->occupied || !entry->machine.live) continue;
        session_ext *x = hs_session_ext(h, entry);
        /* Route facts gate own rekeys: a reachable destination and the session SRTT. */
        session_ext probe = {0};
        bool reachable = hs_rec_find(h, &entry->peer) != NULL || dest_coords(h, &entry->peer, &probe);
        fips_rekey_input facts = {.kind = FIPS_REKEY_IN_PathFacts, .reachable = reachable};
        double srtt;
        if (x && x->mmp_live && fips_mmp_metrics_srtt_ms(&x->mmp.roles.metrics, &srtt)) {
            facts.has_srtt = true; facts.srtt = fips_duration_from_millis((uint64_t)srtt);
        }
        fips_rekey_outputs_clear(&h->session_out);
        if (fips_rekey_step(&entry->machine, now, facts, &env, &h->session_out) == FIPS_OK) (void)hs_session_execute(h, entry, NULL, 0);
        else fips_rekey_outputs_clear(&h->session_out);
        if (!entry->occupied) continue;
        if (x && x->established && h->cfg.session_idle_timeout_ms && hs_elapsed_ms(now, x->last_data) > h->cfg.session_idle_timeout_ms)
            close_entry(h, entry, FIPS_REKEY_CLOSE_IdlePurge);
    }
    /* Held datagrams whose lookup never resolved are released after the ladder. */
    uint64_t ladder_ms = 0;
    for (size_t i = 0; i < h->cfg.lookup_attempt_count; ++i) ladder_ms += h->cfg.lookup_attempt_timeouts_secs[i] * 1000;
    for (size_t i = 0; i < h->cfg.session_capacity; ++i)
        if (h->held[i].used && hs_elapsed_ms(now, h->held[i].since) > ladder_ms + h->cfg.timing.handshake_timeout_ms + h->cfg.tick_ms) {
            fipsd_node_notice n = {.kind = FIPSD_NOTICE_SEND_FAILED, .peer = h->held[i].dest, .value = h->held[i].len};
            hs_notice_full(h, &n, NULL, 0);
            h->held[i].used = false; h->held[i].len = 0;
        }
}
void hs_session_close_all(handlers *h)
{
    for (size_t i = 0; i < h->cfg.session_capacity; ++i)
        if (h->session_entries[i].occupied) close_entry(h, &h->session_entries[i], FIPS_REKEY_CLOSE_Shutdown);
}
void hs_session_peer_removed(handlers *h, const fips_node_addr *peer) { (void)h; (void)peer; }
