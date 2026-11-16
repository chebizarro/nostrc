/* Ordered output executor for link (FMP/IK) and session (FSP/XK) machines,
 * mirroring upstream handlers/rekey.rs: Send/Prompt go to the wire, Bind/Unbind
 * maintain the receive-index map, Release returns leases, Installed/Retired/
 * PeerRestarted apply settlement, Deliver is handed to the link/session message
 * dispatch AFTER the machine's output list has been executed in full. */
#include "node/handlers/internal.h"
#include "fiber_hooks.h"

static uint16_t jitter_draw(void *ctx, uint16_t bound)
{
    handlers *h = ctx;
    if (!bound) return 0;
    uint16_t v;
    h->entropy.fill(h->entropy.ctx, (uint8_t *)&v, sizeof v);
    return (uint16_t)(v % bound);
}
static void replay_mark_observed(void *ctx)
{
    fipsd_dataplane *d = ctx;
    if (gof_hook_current() == d->dispatcher_fiber) atomic_fetch_add(&d->stats.replay_marks, 1);
    else atomic_fetch_add(&d->stats.off_dispatcher_replay_marks, 1);
}
void hs_env(handlers *h, fips_rekey_env *env, fips_rekey_transport transport)
{
    if (h->dataplane && !h->dataplane->dispatcher_fiber) h->dataplane->dispatcher_fiber = gof_hook_current();
    *env = (fips_rekey_env){.indices = fips_rekey_index_source_from_allocator(&h->indices),
        .transport = transport, .tags = &h->tags, .workers = false, .jitter_draw = jitter_draw,
        .jitter_ctx = h, .local = &h->local, .scratch = h->scratch, .scratch_capacity = sizeof h->scratch,
        .replay_mark_observer = h->dataplane ? replay_mark_observed : NULL, .replay_mark_ctx = h->dataplane};
}
/* Receive-index map (upstream peers_by_index): one node-wide allocator makes
 * the index unique. Transport is retained as binding provenance, not a second
 * session identity (upstream rekey.rs run_link_outputs). */
void hs_index_bind(handlers *h, const fips_node_addr *peer, fips_rekey_transport transport, uint32_t index)
{
    index_entry *slot = NULL;
    for (size_t i = 0; i < h->index_map_capacity; ++i) {
        index_entry *e = &h->index_map[i];
        if (e->used && e->index == index) { e->peer = *peer; return; }
        if (!e->used && !slot) slot = e;
    }
    if (slot) *slot = (index_entry){.used = true, .index = index, .transport = transport, .peer = *peer};
    else FIPSD_ERROR(HMOD, "receive index map full");
}
void hs_index_unbind(handlers *h, fips_rekey_transport transport, uint32_t index)
{
    (void)transport;
    for (size_t i = 0; i < h->index_map_capacity; ++i) {
        index_entry *e = &h->index_map[i];
        if (e->used && e->index == index) e->used = false;
    }
}
void hs_index_forget_peer(handlers *h, const fips_node_addr *peer)
{
    for (size_t i = 0; i < h->index_map_capacity; ++i)
        if (h->index_map[i].used && !memcmp(h->index_map[i].peer.bytes, peer->bytes, 16)) h->index_map[i].used = false;
}
peer_rec *hs_index_lookup(handlers *h, fips_rekey_transport transport, uint32_t index)
{
    for (size_t i = 0; i < h->index_map_capacity; ++i) {
        index_entry *e = &h->index_map[i];
        if (!e->used || e->index != index) continue;
        peer_rec *rec = hs_rec_find(h, &e->peer);
        /* Resolve by index alone for a multipath Bundle, as upstream's
         * encrypted ingress does. This selects a session, not an authenticated
         * peer/path: hs_link_frame must still open and replay-check the frame. */
        if (e->transport == transport ||
            (rec && rec->path_live && rec->path->params.cfg.enabled)) return rec;
        return NULL;
    }
    return NULL;
}

typedef struct exec_ctx {
    handlers *h;
    peer_rec *rec;                 /* link execution */
    fips_session_entry *entry;     /* session execution */
    session_ext *ext;
    const fips_transport_addr *arrival;
    link_exec_result *result;
    bool session_teardown, session_installed, session_first;
} exec_ctx;

static bool coords(void *c, fips_rekey_route route, fips_rekey_route_handle handle, fips_fsp_coords *src, fips_fsp_coords *dest)
{
    exec_ctx *x = c; handlers *h = x->h;
    (void)route; (void)handle;
    if (!x->ext || !h->my_coord_count || !x->ext->has_dest_coords || !x->ext->dest_count) return false;
    *src = (fips_fsp_coords){h->my_coord_addrs[0].bytes, h->my_coord_count};
    *dest = (fips_fsp_coords){x->ext->dest_coords[0].bytes, x->ext->dest_count};
    return true;
}
static fips_status send_wire(void *c, const fips_rekey_output *o, const uint8_t *bytes, size_t len)
{
    exec_ctx *x = c; handlers *h = x->h;
    h->dataplane->needs_publish = true; /* handshake Send can install pending keys */
    if (x->rec) {
        const fipsd_peer_info *info = fipsd_active_peer_info(x->rec->peer);
        fipsd_transport *t = hs_transport(h, info->transport);
        const fips_transport_addr *to = o->route == FIPS_REKEY_ROUTE_Arrival && x->arrival ? x->arrival : &info->addr;
        if (!t || t->ops->send(t, to, bytes, len) != 0) { ++h->counters.send_failed; return FIPS_ERR_RANGE; }
        ++h->counters.frames_sent; ++x->rec->frames_out; x->rec->bytes_out += len;
        return FIPS_OK;
    }
    if (x->ext) {
        fips_session_datagram dg;
        fips_session_datagram_init(&dg, h->me, x->ext->peer, bytes, len);
        return hs_route_datagram(h, &dg, NULL, false) ? FIPS_OK : FIPS_ERR_RANGE;
    }
    return FIPS_ERR_RANGE;
}
static void link_installed(handlers *h, peer_rec *rec, const fips_rekey_output *o)
{
    fips_mmp_peer_state *mmp = fipsd_active_peer_mmp(rec->peer);
    if (mmp) fips_mmp_peer_reset_for_rekey(mmp, hs_now_ms(h));
    fips_rekey_view v = fipsd_active_peer_link_view(rec->peer);
    if (v.current.present && v.current.tag != 0) {
        ++h->counters.rekey_installed;
        if (o->role == FIPS_REKEY_Responder) {
            fips_bloom_peer *bp = fips_bloom_state_peer(&h->bloom, &rec->addr);
            if (bp) fips_bloom_state_peer_restarted(&h->bloom, bp);
        }
        fipsd_node_notice n = {.kind = FIPSD_NOTICE_LINK_REKEYED, .peer = rec->addr, .k = v.current.k, .value = v.current.tag};
        hs_notice_full(h, &n, NULL, 0);
    }
    hs_mark_dirty(h);
}
static void link_restarted(handlers *h, peer_rec *rec)
{
    /* Restart settlement: stale tree/filter state is discarded and re-learned. */
    ++h->counters.rekey_restart;
    fipsd_dp_mtu_release(h->dataplane, &rec->addr); /* peer restart releases stale clamp */
    fips_bloom_peer *bp = fips_bloom_state_peer(&h->bloom, &rec->addr);
    if (bp) fips_bloom_state_peer_restarted(&h->bloom, bp);
    fips_tree_state_remove_peer(h->tree, rec->addr);
    fipsd_active_peer_data(rec->peer)->has_tree = false;
    rec->tree_pending = true; rec->filter_pending = true;
    hs_mark_dirty(h);
}
static void effect(void *c, const fips_rekey_output *o, const uint8_t *plaintext, size_t len)
{
    exec_ctx *x = c; handlers *h = x->h;
    (void)plaintext;
    if (o->kind == FIPS_REKEY_OUT_Bind || o->kind == FIPS_REKEY_OUT_Unbind ||
        o->kind == FIPS_REKEY_OUT_Installed || o->kind == FIPS_REKEY_OUT_Retired ||
        o->kind == FIPS_REKEY_OUT_Reverted || o->kind == FIPS_REKEY_OUT_PeerRestarted ||
        o->kind == FIPS_REKEY_OUT_Teardown)
        h->dataplane->needs_publish = true;
    switch (o->kind) {
    case FIPS_REKEY_OUT_Deliver:
        if (x->result) { x->result->delivered = true; x->result->begin = o->begin; x->result->end = o->end; }
        if (x->ext) { x->session_installed = x->session_installed; }
        if (x->entry && x->result) { x->result->delivered = true; x->result->begin = o->begin; x->result->end = o->end; }
        (void)len; break;
    case FIPS_REKEY_OUT_Bind:
        if (x->rec) hs_index_bind(h, &x->rec->addr, o->transport, o->index); break;
    case FIPS_REKEY_OUT_Unbind:
        if (x->rec) hs_index_unbind(h, o->transport, o->index); break;
    case FIPS_REKEY_OUT_Installed:
        if (x->rec) link_installed(h, x->rec, o);
        else x->session_installed = true;
        break;
    case FIPS_REKEY_OUT_PeerRestarted:
        if (x->rec) link_restarted(h, x->rec);
        break;
    case FIPS_REKEY_OUT_ReplaceRequested:
        if (x->result) x->result->replace = true; break;
    case FIPS_REKEY_OUT_Teardown:
        ++h->counters.rekey_teardown;
        if (x->result) x->result->teardown = true;
        x->session_teardown = true; break;
    case FIPS_REKEY_OUT_Abandoned: ++h->counters.rekey_abandoned; break;
    case FIPS_REKEY_OUT_Reject:
        ++h->counters.rekey_rejected;
        if (o->reason == FIPS_REKEY_REASON_Replay) ++h->counters.replay;
        if (o->reason == FIPS_REKEY_REASON_FrameUnauthenticated) ++h->counters.decrypt_failed;
        break;
    case FIPS_REKEY_OUT_Rekeyed: if (x->ext) ++x->ext->rekeys; ++h->counters.sess_rekeys; break;
    case FIPS_REKEY_OUT_Delegate:
        /* .7 jobs clone bare keys from snapshots, NEVER a replay window.
         * env.workers stays false: replay authority remains on this dispatcher. */
        FIPSD_WARN(HMOD, "unexpected replay-authority delegation in snapshot dataplane"); break;
    default: break;
    }
}
/* .7: preserve executor ordering while Prompt's packet AEAD is asynchronous.
 * Handshake state-machine crypto remains on its single control-plane owner. */
static fips_status prompt_worker(void *c, fips_rekey *m, fips_instant now, const fips_rekey_output *o)
{
    exec_ctx *x = c;
    x->h->dataplane->needs_publish = true; /* pending prompt may be its first job */
    fips_rekey_sealed_prompt p = {0};
    if (fips_rekey_seal_prompt(m, now, o->tag, &p) != FIPS_REKEY_SEAL_OK) return FIPS_ERR_RANGE;
    fips_transport_id transport = {0}; const fips_transport_addr *to = NULL;
    if (x->rec) {
        const fipsd_peer_info *info = fipsd_active_peer_info(x->rec->peer);
        transport = info->transport; to = x->arrival ? x->arrival : &info->addr;
    }
    bool ok = fipsd_dp_seal(x->h, x->rec, x->entry, &p.sealed, p.plaintext.bytes,
        p.plaintext.len, true, transport, to, false);
    fips_rekey_sealed_prompt_clear(&p);
    return ok ? FIPS_OK : FIPS_ERR_RANGE;
}
static void release_index(void *c, uint32_t index)
{
    exec_ctx *x = c; handlers *h = x->h;
    (void)fips_index_allocator_free(&h->indices, (fips_session_index){index});
    for (size_t i = 0; i < h->index_map_capacity; ++i)
        if (h->index_map[i].used && h->index_map[i].index == index) h->index_map[i].used = false;
}

/* Outputs of a closed machine (close_machine): no sends or deliveries remain,
 * only retirements, unbinds and the linear leases that must return. */
void hs_release_outputs(handlers *h, const fips_node_addr *peer, fips_rekey_outputs *out)
{
    for (size_t i = 0; i < out->count; ++i) {
        fips_rekey_output *o = &out->items[i];
        if (o->kind == FIPS_REKEY_OUT_Unbind) hs_index_unbind(h, o->transport, o->index);
        else if (o->kind == FIPS_REKEY_OUT_Release) {
            uint32_t index;
            if (fips_rekey_index_lease_release(&o->lease, &index)) {
                (void)fips_index_allocator_free(&h->indices, (fips_session_index){index});
                for (size_t j = 0; j < h->index_map_capacity; ++j)
                    if (h->index_map[j].used && h->index_map[j].index == index) h->index_map[j].used = false;
            }
        } else if (o->kind == FIPS_REKEY_OUT_Abandoned) ++h->counters.rekey_abandoned;
        else if (o->kind == FIPS_REKEY_OUT_Send || o->kind == FIPS_REKEY_OUT_Prompt || o->kind == FIPS_REKEY_OUT_Deliver)
            FIPSD_WARN(HMOD, "wire output discarded while closing a link");
    }
    if (peer) hs_index_forget_peer(h, peer);
    fips_rekey_outputs_clear(out);
}
bool hs_link_execute(handlers *h, peer_rec *rec, const fips_transport_addr *arrival, uint8_t *input, size_t len, link_exec_result *result)
{
    fips_rekey *m = fipsd_active_peer_link_machine(rec->peer);
    if (!m) { hs_release_outputs(h, &rec->addr, &h->link_out); return false; }
    exec_ctx x = {.h = h, .rec = rec, .arrival = arrival, .result = result};
    fips_session_executor exec = {.ctx = &x, .coords = coords, .send = send_wire, .effect = effect,
        .release = release_index, .prompt = prompt_worker, .wire = h->link_exec_wire, .capacity = sizeof h->link_exec_wire};
    return fips_session_execute(m, hs_now(h), &h->link_out, input, len, &exec) == FIPS_OK;
}
bool hs_link_step(handlers *h, peer_rec *rec, fips_rekey_input input, const fips_transport_addr *arrival, uint8_t *frame, size_t len, link_exec_result *result)
{
    const fipsd_peer_info *info = fipsd_active_peer_info(rec->peer);
    fips_rekey_env env; hs_env(h, &env, info->transport.value);
    fips_rekey_outputs_clear(&h->link_out);
    fips_status s = fipsd_active_peer_link_step(rec->peer, hs_now(h), input, &env, &h->link_out);
    if (s != FIPS_OK) { fips_rekey_outputs_clear(&h->link_out); return false; }
    return hs_link_execute(h, rec, arrival, frame, len, result);
}
/* .7: this seam never performs packet crypto on the dispatcher. */
bool hs_link_frame(handlers *h, peer_rec *rec, fipsd_transport *t, const fips_transport_addr *from, uint8_t *frame, size_t len)
{
    fips_fmp_encrypted_header header;
    if (!fips_fmp_encrypted_header_parse(frame, len, &header)) { ++h->counters.frames_dropped; return false; }
    link_exec_result r = {.counter = header.counter};
    fips_rekey_input in = {.kind = FIPS_REKEY_IN_Frame, .bytes = frame, .buf = frame, .len = len,
        .hint = {header.counter, (header.flags & FIPS_FMP_FLAG_KEY_EPOCH) != 0, header.receiver_idx.value,
                 FIPS_FRAMING_FMP_ESTABLISHED_HEADER_SIZE}};
    bool ok = hs_link_step(h, rec, in, from, frame, len, &r);
    if (r.teardown) { hs_remove_peer(h, rec, FIPS_REKEY_CLOSE_Admin, false, FIPS_FMP_ProtocolError); return ok; }
    if (!r.delivered) return ok;
    if (r.end > len || r.begin > r.end) return false;
    uint32_t timestamp = 0; const uint8_t *message = NULL; size_t message_len = 0;
    if (!fips_fmp_strip_inner_header(frame + r.begin, r.end - r.begin, &timestamp, &message, &message_len)) return false;
    ++h->counters.frames_delivered;
    link_heard(h, rec, len);
    if (!rec->used) return true; /* heard effects may have removed the peer */
    hs_mmp_frame_received(h, rec, header.counter, timestamp, len, (header.flags & FIPS_FMP_FLAG_CE) != 0);
    if (message_len) hs_path_account(h, rec, t, from, len, message[0]);
    /* The message borrows the frame buffer; copy so nested sends can reuse it. */
    if (message_len > sizeof h->msg_buf) return false;
    memcpy(h->msg_buf, message, message_len);
    hs_link_message(h, rec, t, from, timestamp, h->msg_buf, message_len);
    return true;
}
{ return fipsd_dp_open(h, rec, NULL, t->id, from, frame, len); }
bool hs_link_send(handlers *h, peer_rec *rec, const uint8_t *message, size_t len)
{
    const fipsd_peer_info *info = fipsd_active_peer_info(rec->peer);
    return hs_link_send_to(h, rec, info->transport, &info->addr, message, len);
}
bool hs_link_send_to(handlers *h, peer_rec *rec, fips_transport_id transport, const fips_transport_addr *to, const uint8_t *message, size_t len)
{
    return fipsd_dp_link_send(h, rec, transport, to, message, len);
}
void hs_link_message(handlers *h, peer_rec *rec, fipsd_transport *t, const fips_transport_addr *from, uint32_t timestamp_ms, const uint8_t *message, size_t len)

void hs_link_message(handlers *h, peer_rec *rec, const fips_transport_addr *from, uint32_t timestamp_ms, const uint8_t *message, size_t len)
{
    (void)timestamp_ms;
    if (!len) return;
    fips_link_message_type type;
    if (fips_link_message_type_from_u8(message[0], &type, NULL) != FIPS_OK) { ++h->counters.frames_dropped; return; }
    const uint8_t *body = message + 1; size_t body_len = len - 1;
    /* Path messages belong to the authenticated frame's ingress transport,
     * not the peer's active send path (which may be a different binding). */
    switch (type) {
    case FIPS_LINK_SessionDatagram: hs_session_datagram_received(h, rec, body, body_len); break;
    case FIPS_LINK_SenderReport:
    case FIPS_LINK_ReceiverReport: hs_mmp_report_received(h, rec, type, body, body_len); break;
    case FIPS_LINK_TreeAnnounce: hs_tree_announce_received(h, rec, body, body_len); break;
    case FIPS_LINK_FilterAnnounce: hs_bloom_filter_received(h, rec, body, body_len); break;
    case FIPS_LINK_LookupRequest: hs_lookup_request_received(h, rec, body, body_len); break;
    case FIPS_LINK_LookupResponse: hs_lookup_response_received(h, rec, body, body_len); break;
    case FIPS_LINK_Disconnect: {
        fips_fmp_disconnect d = {FIPS_FMP_Other};
        fips_reader r; fips_reader_init(&r, body, body_len);
        (void)fips_fmp_disconnect_decode(&r, &d, NULL);
        FIPSD_INFO(HMOD, "peer disconnect: %s", fips_fmp_disconnect_reason_name(d.reason));
        hs_remove_peer(h, rec, FIPS_REKEY_CLOSE_Disconnect, false, d.reason);
        break;
    }
    case FIPS_LINK_Heartbeat: break;
    case FIPS_LINK_PathProbe:
    case FIPS_LINK_PathAck:
    case FIPS_LINK_PathClose: if (t) hs_path_message(h, rec, t, from, type, body, body_len); break;
    }
}
/* Session machines: same executor, FSP framing by fips_session_execute. */
bool hs_session_execute(handlers *h, fips_session_entry *entry, const uint8_t *input, size_t len)
{
    session_ext *ext = hs_session_ext(h, entry);
    link_exec_result r = {0};
    exec_ctx x = {.h = h, .entry = entry, .ext = ext, .result = &r};
    fips_session_executor exec = {.ctx = &x, .coords = coords, .send = send_wire, .effect = effect,
        .release = release_index, .prompt = prompt_worker, .wire = h->session_exec_wire, .capacity = sizeof h->session_exec_wire};
    fips_status s = fips_session_execute(&entry->machine, hs_now(h), &h->session_out, input, len, &exec);
    if (x.session_installed && ext) {
        fips_rekey_view v = fips_rekey_get_view(&entry->machine);
        if (!ext->established) {
            ext->established = true; ++h->counters.sess_established;
            hs_notice(h, FIPSD_NOTICE_SESSION_UP, &ext->peer, NULL, 0);
            if (ext->mmp_live) fips_mmp_session_reset_for_rekey(&ext->mmp, hs_now_ms(h));
            hs_session_coords_resolved(h, &ext->peer);
        } else {
            if (ext->mmp_live) fips_mmp_session_reset_for_rekey(&ext->mmp, hs_now_ms(h));
            fipsd_node_notice n = {.kind = FIPSD_NOTICE_SESSION_REKEYED, .peer = ext->peer, .k = v.current.k, .value = v.current.tag};
            hs_notice_full(h, &n, NULL, 0);
        }
        hs_mark_dirty(h);
    }
    if (r.delivered && entry->occupied && input && r.end <= len && r.begin <= r.end) {
        if (r.end - r.begin <= sizeof h->msg_buf) {
            memcpy(h->msg_buf, input + r.begin, r.end - r.begin);
            hs_session_message(h, entry, h->msg_buf, r.end - r.begin);
        }
    }
    if (x.session_teardown && entry->occupied) {
        fips_node_addr peer = entry->peer;
        fipsd_dp_mtu_release(h->dataplane, &peer);
        fips_rekey_carry carry = {0};
        fips_rekey_outputs_clear(&h->session_out);
        if (fips_session_table_remove(&h->sessions, entry, FIPS_REKEY_CLOSE_OpeningTeardown, &h->session_out, &carry)) {
            exec_ctx y = {.h = h, .ext = ext};
            fips_session_executor exec2 = {.ctx = &y, .coords = coords, .send = send_wire, .effect = effect,
                .release = release_index, .prompt = prompt_worker, .wire = h->session_exec_wire, .capacity = sizeof h->session_exec_wire};
            (void)fips_session_execute(&entry->machine, hs_now(h), &h->session_out, NULL, 0, &exec2);
        }
        if (ext) { ext->used = false; ext->established = false; ext->pending_len = 0; }
        ++h->counters.sess_closed;
        hs_notice(h, FIPSD_NOTICE_SESSION_DOWN, &peer, NULL, 0);
        hs_mark_dirty(h);
    }
    return s == FIPS_OK;
}
