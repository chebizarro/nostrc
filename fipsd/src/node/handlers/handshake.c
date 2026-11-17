/* FMP link handshake (upstream handlers/handshake.rs + lifecycle dial/msg1
 * preparation): pending legs are fipsd_peer_machine state plus the IK profile's
 * handshake owner; msg1/msg2 are authenticated by the production IK profile,
 * classified by fips_fmp_establish_{inbound,outbound}, and promotion imports the
 * completed Noise session into the link machine through peer/link.h. */
#include "node/handlers/internal.h"

static fips_fmp_epoch epoch_of(const uint8_t bytes[8])
{ fips_fmp_epoch e = {.present = true}; memcpy(e.bytes, bytes, 8); return e; }
static fips_fmp_epoch link_epoch(peer_rec *rec)
{
    fips_rekey_view v = fipsd_active_peer_link_view(rec->peer);
    fips_fmp_epoch e = {.present = v.has_peer_epoch};
    if (v.has_peer_epoch) memcpy(e.bytes, v.peer_epoch, 8);
    return e;
}
/* H1 restart admission. IK authenticates the recorded identity, not freshness,
 * and FMP classifies any differing known epoch as a restart. A replayed msg1
 * from an earlier epoch therefore used to evict the healthy link here. Now the
 * candidate is held (own receive index, msg2 answered, bounded hold) and the
 * incumbent closes only in hs_restart_result, once a frame authenticates under
 * keys derived from our fresh msg2 ephemeral: a recording cannot produce one.
 *
 * Supplementary bounded memory, never a freshness oracle: forgotten or unseen
 * recordings still have to answer a fresh msg2 before replacing a live link. */
static void restart_remember(handlers *h, const fips_node_addr *peer, const uint8_t *value, bool epoch, fips_instant until)
{
    size_t i = h->retired_next++ % FIPSD_RESTART_HISTORY;
    h->retired[i].used = true; h->retired[i].has_epoch = epoch; h->retired[i].peer = *peer; h->retired[i].until = until;
    memset(h->retired[i].value, 0, sizeof h->retired[i].value);
    memcpy(h->retired[i].value, value, epoch ? 8 : 32);
}
static bool restart_seen(const handlers *h, const fips_node_addr *peer, const fips_fmp_ik_msg1 *token, fips_instant now)
{
    for (size_t i = 0; i < FIPSD_RESTART_HISTORY; ++i) {
        if (!h->retired[i].used || memcmp(h->retired[i].peer.bytes, peer->bytes, 16)) continue;
        if (h->retired[i].until.ops && now.ops->compare(now, h->retired[i].until) >= 0) continue;
        bool epoch = h->retired[i].has_epoch;
        if (!memcmp(h->retired[i].value, epoch ? token->proof.epoch : token->proof.ephemeral, epoch ? 8 : 32)) return true;
    }
    return false;
}
/* An expired candidate's msg1 is refused for one more hold window, so one
 * recording cannot re-occupy the slot the instant it lapses, yet an initiator
 * whose own window outlives ours is challenged again rather than tombstoned. */
static void restart_clear(handlers *h, peer_rec *rec, bool remember, fips_instant now)
{
    restart_candidate *c = &rec->restart;
    if (!c->used) return;
    if (remember)
        restart_remember(h, &rec->addr, c->ephemeral, false,
            fips_deadline(now, fips_duration_from_millis(h->cfg.timing.handshake_timeout_ms)));
    if (c->lease.live) {
        uint32_t index;
        if (fips_rekey_index_lease_release(&c->lease, &index))
            (void)fips_index_allocator_free(&h->indices, (fips_session_index){index});
    }
    fips_rekey_completed_clear(&c->done);
    sodium_memzero(c, sizeof *c);
    --h->restart_pending;
}
static bool restart_send(handlers *h, const restart_candidate *c, const fips_transport_addr *to)
{
    fipsd_transport *t = hs_transport(h, c->transport);
    bool ok = t && t->ops->send(t, to, c->msg2.bytes, c->msg2.len) == 0;
    if (ok) ++h->counters.frames_sent; else ++h->counters.send_failed;
    return ok;
}
static bool restart_hold(handlers *h, peer_rec *rec, fipsd_transport *t,
    const fips_transport_addr *from, const fips_fmp_ik_msg1 *token,
    const uint8_t *frame, size_t len, bool replace)
{
    restart_candidate *c = &rec->restart;
    fips_instant now = hs_now(h);
    if (restart_seen(h, &rec->addr, token, now)) { ++h->counters.hs_rejected; return false; }
    /* One candidate per live peer and max_pending node-wide. A different msg1
     * while one is held waits for its confirmation or the hold deadline. */
    if (c->used || h->restart_pending >= h->hs_limiter.max_pending) { ++h->counters.hs_rate_limited; return false; }
    if (len > FIPS_REKEY_WIRE_MAX) { ++h->counters.hs_failed; return false; }
    c->used = true; ++h->restart_pending;
    c->transport = t->id; c->addr = *from; c->replace = replace;
    memcpy(c->msg1.bytes, frame, len); c->msg1.len = len;
    memcpy(c->ephemeral, token->proof.ephemeral, 32);
    /* The hold spans the initiator's own handshake window (the same timing
     * knob), so its identical retransmits are answered from here instead of
     * being remembered as stale once the hold lapses. */
    c->deadline = fips_deadline(now, fips_duration_from_millis(h->cfg.timing.handshake_timeout_ms));
    fips_rekey_env env; hs_env(h, &env, t->id.value);
    if (!fips_rekey_allocate(env.indices, &c->lease) ||
        !fips_fmp_rekey_profile.msg1_answer(token, &h->local,
            (fips_rekey_index){c->lease.index, true}, &c->done, &c->msg2)) {
        ++h->counters.hs_failed; restart_clear(h, rec, false, now); return false;
    }
    c->link = (fips_link_id){h->next_link_id++};
    /* The lease is not bound to the index map: no peer, endpoint, tree/session
     * state, live keys, counters or replay windows change until confirmation. */
    (void)restart_send(h, c, &c->addr); /* exact retransmits recover a failed send */
    return true;
}
static bool leg_send_wrap(handlers *h, peer_rec *rec, const uint8_t *bytes, size_t len);
static configured_peer *configured_find(handlers *h, const fips_node_addr *addr)
{
    for (size_t i = 0; i < h->cfg.peer_capacity; ++i)
        if (h->configured[i].used && !memcmp(h->configured[i].identity.node_addr.bytes, addr->bytes, 16)) return &h->configured[i];
    return NULL;
}
static pending_leg *leg_find(handlers *h, const fips_node_addr *peer)
{
    for (size_t i = 0; i < h->cfg.peer_capacity; ++i)
        if (h->legs[i].used && !memcmp(h->legs[i].expected.node_addr.bytes, peer->bytes, 16)) return &h->legs[i];
    return NULL;
}
static void leg_free(handlers *h, pending_leg *leg)
{
    if (leg->hs_live) { fips_rekey_hs_clear(&leg->hs); leg->hs_live = false; }
    if (leg->lease.live) {
        uint32_t index;
        if (fips_rekey_index_lease_release(&leg->lease, &index))
            (void)fips_index_allocator_free(&h->indices, (fips_session_index){index});
    }
    if (leg->has_ticket) { fipsd_peer_budget_release(&leg->ticket); leg->has_ticket = false; }
    configured_peer *c = configured_find(h, &leg->expected.node_addr);
    if (c) c->dialing = false;
    sodium_memzero(leg, sizeof *leg);
    hs_mark_dirty(h);
}
static bool leg_send(handlers *h, pending_leg *leg, const uint8_t *bytes, size_t len)
{
    fipsd_transport *t = hs_transport(h, leg->transport);
    bool ok = t && t->ops->send(t, &leg->addr, bytes, len) == 0;
    if (ok) ++h->counters.frames_sent; else ++h->counters.send_failed;
    return ok;
}
static void handshake_failed(handlers *h, pending_leg *leg, bool link_dead)
{
    ++h->counters.hs_failed;
    configured_peer *c = configured_find(h, &leg->expected.node_addr);
    if (c) {
        fipsd_peer_retry_policy p = {5000, 300000, h->cfg.timing.handshake_timeout_ms, 5};
        (void)fipsd_peer_retry_failed(&c->retry, p, hs_now(h), link_dead, true, true, false, false);
    }
    hs_notice(h, FIPSD_NOTICE_HANDSHAKE_FAILED, &leg->expected.node_addr, NULL, 0);
}
bool hs_leg_effects(handlers *h, pending_leg *leg, const fipsd_peer_actions *actions)
{
    bool teardown = false, lost = false;
    for (size_t i = 0; i < actions->count; ++i) {
        const fipsd_peer_action *a = &actions->items[i];
        switch (a->kind) {
        case FIPSD_PEER_OPEN_TRANSPORT: {
            fipsd_transport *t = hs_transport(h, leg->transport);
            if (!t || !t->ops->connect_async || t->ops->connect_async(t, &leg->addr) != 0) {
                fipsd_peer_event e = {.kind = FIPSD_PEER_CONNECT_FAILED};
                fipsd_peer_actions more;
                fipsd_peer_machine_step(&leg->machine, hs_now(h), &e, &more);
                return hs_leg_effects(h, leg, &more);
            }
            break;
        }
        case FIPSD_PEER_START_HANDSHAKE:
            if (!leg_send(h, leg, leg->machine.conn.handshake_msg1, leg->machine.conn.handshake_msg1_len)) {
                fipsd_peer_event e = {.kind = FIPSD_PEER_SEND_FAILED};
                fipsd_peer_actions more;
                fipsd_peer_machine_step(&leg->machine, hs_now(h), &e, &more);
            }
            break;
        case FIPSD_PEER_SEND_HANDSHAKE: {
            bool ok = leg_send(h, leg, a->bytes.data, a->bytes.len);
            fipsd_peer_event e = {.kind = FIPSD_PEER_RESEND_RESULT, .success = ok};
            fipsd_peer_actions more;
            fipsd_peer_machine_step(&leg->machine, hs_now(h), &e, &more);
            break;
        }
        case FIPSD_PEER_TEARDOWN: teardown = true; break;
        case FIPSD_PEER_LOST_HANDSHAKE: lost = true; ++h->counters.hs_timeouts; break;
        default: break;
        }
    }
    if (teardown && lost) {
        /* Tear down first so the failure notice follows a snapshot without the leg. */
        pending_leg copy = *leg; copy.hs_live = false; copy.lease = (fips_rekey_index_lease){0}; copy.has_ticket = false;
        leg_free(h, leg);
        handshake_failed(h, &copy, false);
        sodium_memzero(&copy, sizeof copy);
    } else {
        if (lost) handshake_failed(h, leg, false);
        if (teardown) leg_free(h, leg);
    }
    return true;
}
bool hs_dial(handlers *h, const fips_peer_identity *peer, fips_transport_id transport, const fips_transport_addr *addr, bool configured)
{
    if (!memcmp(peer->node_addr.bytes, h->me.bytes, 16)) return false;
    /* Upstream OutboundConnect gate: before reserve, index allocation or DH. */
    if (!hs_acl_authorize(h, peer, "outbound_connect")) {
        hs_notice(h, FIPSD_NOTICE_HANDSHAKE_FAILED, &peer->node_addr, NULL, 0); return false;
    }
    if (hs_rec_find(h, &peer->node_addr) || leg_find(h, &peer->node_addr)) return true;
    fipsd_transport *t = hs_transport(h, transport);
    if (!t) { hs_notice(h, FIPSD_NOTICE_HANDSHAKE_FAILED, &peer->node_addr, NULL, 0); return false; }
    pending_leg *leg = NULL;
    for (size_t i = 0; i < h->cfg.peer_capacity && !leg; ++i) if (!h->legs[i].used) leg = &h->legs[i];
    if (!leg) return false;
    memset(leg, 0, sizeof *leg);
    leg->used = true; leg->configured = configured; leg->expected = *peer; leg->transport = transport; leg->addr = *addr;
    leg->started = hs_now(h);
    if (fipsd_peer_table_reserve(h->peers->table, transport, &leg->ticket) != FIPSD_ADMIT_OK) {
        FIPSD_WARN(HMOD, "dial refused by admission caps");
        leg->used = false; hs_notice(h, FIPSD_NOTICE_HANDSHAKE_FAILED, &peer->node_addr, NULL, 0); return false;
    }
    leg->has_ticket = true;
    fips_rekey_env env; hs_env(h, &env, transport.value);
    if (!fips_rekey_allocate(env.indices, &leg->lease)) { leg_free(h, leg); return false; }
    fips_peer_key key = fips_peer_key_from_xonly(&peer->pubkey);
    fips_rekey_wire msg1 = {0};
    if (!fips_fmp_rekey_profile.initiate(&h->local, &key, (fips_rekey_index){leg->lease.index, true}, &leg->hs, &msg1)) {
        FIPSD_WARN(HMOD, "IK msg1 construction failed");
        leg_free(h, leg); return false;
    }
    leg->hs_live = true;
    leg->link = (fips_link_id){h->next_link_id++};
    fipsd_peer_machine_init(&leg->machine, leg->link, FIPS_LINK_OUTBOUND, peer,
        (fips_fmp_leg_stamp){hs_wall_ms(h), hs_now(h)}, h->cfg.timing);
    (void)fips_fmp_connection_set_handshake_msg1(&leg->machine.conn, msg1.bytes, msg1.len);
    leg->machine.conn.transport_id = transport; leg->machine.conn.has_transport_id = true;
    leg->machine.conn.source_addr = *addr; leg->machine.conn.has_source_addr = true;
    sodium_memzero(&msg1, sizeof msg1);
    configured_peer *c = configured_find(h, &peer->node_addr);
    if (c) c->dialing = true;
    fipsd_peer_event e = {.kind = FIPSD_PEER_DIAL, .connection_oriented = t->ops->connect_async != NULL};
    fipsd_peer_actions actions;
    fipsd_peer_machine_step(&leg->machine, hs_now(h), &e, &actions);
    hs_mark_dirty(h);
    return hs_leg_effects(h, leg, &actions);
}
/* Shared post-promotion effects for both directions. */
static void promoted(handlers *h, peer_rec *rec)
{
    ++h->counters.hs_promoted;
    rec->established_at = rec->last_heard = hs_now(h);
    rec->tree_pending = true; rec->filter_pending = true;
    configured_peer *c = configured_find(h, &rec->addr);
    if (c) { fipsd_peer_retry_init(&c->retry); c->dialing = false; }
    hs_mmp_peer_added(h, rec);
    hs_path_peer_added(h, rec);
    hs_tree_peer_added(h, rec);
    hs_bloom_peer_added(h, rec);
    char hex[33]; (void)fips_node_addr_format(&rec->addr, hex, sizeof hex);
    FIPSD_INFO(HMOD, "link established with %s (%s)", hex,
        fipsd_active_peer_info(rec->peer)->direction == FIPS_LINK_OUTBOUND ? "outbound" : "inbound");
    hs_notice(h, FIPSD_NOTICE_LINK_UP, &rec->addr, NULL, 0);
    hs_mark_dirty(h);
}
/* Import a completed first handshake into a new ActivePeer and the table. The
 * lease and completed session are consumed on success and released otherwise. */
static peer_rec *establish(handlers *h, const fips_peer_identity *identity, fipsd_transport *t,
    const fips_transport_addr *addr, fips_link_id link, fips_link_direction direction,
    fips_rekey_completed *done, fips_rekey_index_lease *lease, fips_rekey_index theirs,
    const fips_rekey_carry *carry, fipsd_peer_ticket *ticket)
{
    fips_instant now = hs_now(h);
    fipsd_active_peer *peer = fipsd_active_peer_create(identity, t->id, addr, link, direction, hs_wall_ms(h), now, h->cfg.timing);
    if (!peer) return NULL;
    fips_rekey_env env; hs_env(h, &env, t->id.value);
    fips_rekey_ids ids = {.lease = *lease, .theirs = theirs, .transport = t->id.value};
    fips_rekey_setup_facts facts = *carry;
    if (!facts.has_peer_epoch) { memcpy(facts.peer_epoch, done->epoch, 8); facts.has_peer_epoch = true; }
    if (!facts.link_since.ops) facts.link_since = now;
    if (!facts.started.ops) facts.started = now;
    if (!facts.last_rx.ops) facts.last_rx = now;
    fips_rekey_outputs_clear(&h->link_out);
    fips_status s = fipsd_active_peer_link_established(peer, &fips_fmp_rekey_profile, h->link_params, h->me,
        done, &ids, &facts, now, &env, &h->link_out);
    if (s != FIPS_OK) {
        FIPSD_WARN(HMOD, "link import refused");
        fips_rekey_outputs_clear(&h->link_out);
        if (ids.lease.live) { uint32_t index; if (fips_rekey_index_lease_release(&ids.lease, &index)) (void)fips_index_allocator_free(&h->indices, (fips_session_index){index}); }
        *lease = (fips_rekey_index_lease){0};
        (void)fipsd_active_peer_destroy(peer);
        return NULL;
    }
    *lease = (fips_rekey_index_lease){0};
    peer_rec *rec = hs_rec_alloc(h, peer);
    if (!rec || fipsd_peer_table_insert_reserved(h->peers->table, peer, ticket) != FIPSD_ADMIT_OK) {
        FIPSD_WARN(HMOD, "peer admission refused after handshake");
        if (rec) hs_rec_free(h, rec);
        fips_rekey_carry unused;
        fips_rekey_outputs_clear(&h->link_out);
        fipsd_active_peer_link_close(peer, FIPS_REKEY_CLOSE_Admin, &h->link_out, &unused);
        hs_release_outputs(h, &identity->node_addr, &h->link_out);
        (void)fipsd_active_peer_destroy(peer);
        return NULL;
    }
    link_exec_result r = {0};
    (void)hs_link_execute(h, rec, addr, NULL, 0, &r);
    if (t->ops->authenticated) t->ops->authenticated(t, addr, direction);
    promoted(h, rec);
    return rec;
}
/* Remove an established peer: outputs of the link close run before the table
 * forgets the member; carry (optional) keeps the anchors for a replacement. */
static void remove_peer_carry(handlers *h, peer_rec *rec, fips_rekey_close why, bool notify_peer,
                              fips_fmp_disconnect_reason reason, fips_rekey_carry *carry)
{
    if (!rec->used) return;
    /* Not remembered: with no incumbent left, the peer's identical retransmit
     * must establish through the ordinary new-peer path. */
    restart_clear(h, rec, false, hs_now(h));
    rec->disconnecting = true;
    {
        char hex[33]; (void)fips_node_addr_format(&rec->addr, hex, sizeof hex);
        FIPSD_INFO(HMOD, "closing link with %s (%s, close=%d)", hex, fips_fmp_disconnect_reason_name(reason), (int)why);
    }
    if (notify_peer) {
        fips_fmp_disconnect d = {reason};
        uint8_t msg[8]; fips_writer w; fips_writer_init(&w, msg, sizeof msg);
        if (fips_fmp_disconnect_encode(&d, &w, NULL) == FIPS_OK) (void)hs_link_send(h, rec, msg, fips_writer_len(&w));
    }
    fips_node_addr addr = rec->addr;
    fipsd_active_peer *peer = rec->peer;
    hs_tree_peer_removed(h, rec);
    hs_bloom_peer_removed(h, rec);
    hs_path_peer_removed(h, rec);
    const fipsd_peer_info *info = fipsd_active_peer_info(peer);
    fipsd_transport *transport = hs_transport(h, info->transport);
    if (transport && transport->ops->peer_link) transport->ops->peer_link(transport, &info->addr, &addr, false, true);
    if (transport && transport->ops->close_connection) transport->ops->close_connection(transport, &info->addr);
    hs_session_peer_removed(h, &addr);
    fips_rekey_carry local = {0};
    fips_rekey_outputs_clear(&h->link_out);
    fipsd_active_peer_link_close(peer, why, &h->link_out, carry ? carry : &local);
    hs_release_outputs(h, &addr, &h->link_out);
    hs_rec_free(h, rec);
    /* The table owns admitted members: remove destroys the peer and releases
     * its charge. A member it refuses (still attached) is a logic error here. */
    if (fipsd_peer_table_remove(h->peers->table, &addr) != FIPSD_ADMIT_OK) {
        FIPSD_ERROR(HMOD, "peer table removal refused after link close");
        (void)fipsd_active_peer_destroy(peer);
    }
    configured_peer *c = configured_find(h, &addr);
    if (c && why == FIPS_REKEY_CLOSE_LinkDead) {
        fipsd_peer_retry_policy p = {5000, 300000, h->cfg.timing.handshake_timeout_ms, 5};
        (void)fipsd_peer_retry_failed(&c->retry, p, hs_now(h), true, true, true, false, false);
    }
    hs_notice(h, FIPSD_NOTICE_LINK_DOWN, &addr, NULL, 0);
    hs_mark_dirty(h);
}
void hs_remove_peer(handlers *h, peer_rec *rec, fips_rekey_close why, bool notify_peer, fips_fmp_disconnect_reason reason)
{ remove_peer_carry(h, rec, why, notify_peer, reason, NULL); }
bool hs_disconnect(handlers *h, const fips_node_addr *peer, fips_fmp_disconnect_reason reason, bool notify_peer)
{
    bool any = false;
    peer_rec *rec = hs_rec_find(h, peer);
    if (rec) { hs_remove_peer(h, rec, FIPS_REKEY_CLOSE_Disconnect, notify_peer, reason); any = true; }
    pending_leg *leg = leg_find(h, peer);
    if (leg) { leg_free(h, leg); any = true; }
    configured_peer *c = configured_find(h, peer);
    if (c) { fipsd_peer_retry_init(&c->retry); c->used = false; }
    return any;
}
static bool msg1(handlers *h, fipsd_transport *t, const fips_transport_addr *from, uint8_t *frame, size_t len)
{
    ++h->counters.hs_msg1;
    fips_instant now = hs_now(h);
    fipsd_active_peer *at_endpoint = fipsd_peer_table_endpoint(h->peers->table, t->id, from);
    fips_pending_handshake slot = {0};
    fips_msg1_result admitted = fips_handshake_start(&h->hs_limiter, at_endpoint ? FIPS_MSG1_ESTABLISHED : FIPS_MSG1_STRANGER, now, &slot);
    if (admitted != FIPS_MSG1_ADMIT) { ++h->counters.hs_rate_limited; return false; }
    bool ok = false;
    fips_fmp_ik_msg1 token = {0};
    fips_rekey_completed done = {0};
    fipsd_peer_ticket ticket = {0};
    bool has_ticket = false;
    fips_rekey_index_lease lease = {0};
    if (!t->accept_connections && !at_endpoint) { ++h->counters.hs_rejected; goto out; }
    if (!fips_fmp_rekey_read_msg1(&h->local, frame, len, &token)) { ++h->counters.hs_failed; goto out; }
    fips_peer_identity identity;
    if (!hs_peer_identity(h, &token.proof.remote, &identity) || !memcmp(identity.node_addr.bytes, h->me.bytes, 16)) { ++h->counters.hs_rejected; goto out; }
    /* IK authenticates the normalized identity, not freshness. Admission must
     * precede even same-epoch rekey/rebind and restart classification effects. */
    if (!hs_acl_authorize(h, &identity, "inbound_handshake")) {
        hs_notice(h, FIPSD_NOTICE_HANDSHAKE_FAILED, &identity.node_addr, NULL, 0); goto out;
    }
    peer_rec *rec = hs_rec_find(h, &identity.node_addr);
    fips_fmp_wire_outcome wire = {.peer_identity = identity, .remote_epoch = epoch_of(token.proof.epoch),
        .their_index = {token.theirs}};
    fips_fmp_msg1_digest_of(frame, len, &wire.msg1_digest);
    if (rec && rec->restart.used) {
        restart_candidate *c = &rec->restart;
        if (now.ops->compare(now, c->deadline) >= 0) restart_clear(h, rec, true, now);
        else if (c->msg1.len == len && !memcmp(c->msg1.bytes, frame, len)) {
            /* An exact retransmit reuses msg2/keys/index and never extends the
             * hold; it is answered where it arrived (msg2 is no larger than msg1). */
            ok = fips_transport_id_equal(t->id, c->transport) && restart_send(h, c, from);
            goto out;
        }
    }
    fips_fmp_establish_snapshot snap = {.has_existing_peer = rec != NULL};
    if (rec) snap.existing_peer_epoch = link_epoch(rec);
    else {
        has_ticket = fipsd_peer_table_reserve(h->peers->table, t->id, &ticket) == FIPSD_ADMIT_OK;
        snap.at_max_peers = !has_ticket;
    }
    snap.has_silent_backoff = fips_fmp_silent_sessions_refusing(&h->silent, &identity.node_addr, now, &snap.silent_backoff);
    fips_fmp_inbound_decision d = fips_fmp_establish_inbound(&snap, &wire);
    if (d.kind == FIPS_FMP_IN_Rekey && rec) {
        const fipsd_peer_info *info = fipsd_active_peer_info(rec->peer);
        bool on_link = fips_transport_id_equal(info->transport, t->id) && fips_transport_addr_equal(&info->addr, from);
        link_exec_result r = {0};
        fips_rekey_input in = {.kind = FIPS_REKEY_IN_PeerMsg1, .tok = &token.proof, .on_link = on_link, .reachable = true};
        ok = hs_link_step(h, rec, in, from, frame, len, &r);
        if (r.teardown && rec->used) { hs_remove_peer(h, rec, FIPS_REKEY_CLOSE_Admin, false, FIPS_FMP_ProtocolError); goto out; }
        if (!r.replace) goto out;
        snap.replace = true;
        d = fips_fmp_establish_inbound(&snap, &wire);
        if (d.kind == FIPS_FMP_IN_Rekey || d.kind == FIPS_FMP_IN_Reject) goto out;
    }
    if (d.kind == FIPS_FMP_IN_Reject) {
        ++h->counters.hs_rejected;
        if (d.reason == FIPS_FMP_AtMaxPeers) METRIC(h, handshake, max_peers);
        else METRIC(h, handshake, silent_backoff);
        goto out;
    }
    if (rec && (d.kind == FIPS_FMP_IN_RestartThenPromote || d.kind == FIPS_FMP_IN_ReplaceThenPromote)) {
        ok = restart_hold(h, rec, t, from, &token, frame, len, d.kind == FIPS_FMP_IN_ReplaceThenPromote);
        goto out;
    }
    if (!has_ticket) { ++h->counters.hs_rejected; goto out; }
    fips_rekey_env env; hs_env(h, &env, t->id.value);
    if (!fips_rekey_allocate(env.indices, &lease)) goto out;
    fips_rekey_wire msg2 = {0};
    if (!fips_fmp_rekey_profile.msg1_answer(&token, &h->local, (fips_rekey_index){lease.index, true}, &done, &msg2)) {
        ++h->counters.hs_failed; goto out;
    }
    /* The setup record lets the link answer a retransmitted msg1 from the held
     * msg2 instead of treating it as a fresh dial. */
    fips_rekey_setup_facts fresh = {0};
    (void)fips_fmp_rekey_setup_record(frame, len, &msg2, &fresh.record);
    fips_link_id link = {h->next_link_id++};
    peer_rec *created = establish(h, &identity, t, from, link, FIPS_LINK_INBOUND, &done, &lease,
        (fips_rekey_index){token.theirs, true}, &fresh, &ticket);
    if (!created) goto out;
    has_ticket = false; /* consumed by insert_reserved */
    if (!leg_send_wrap(h, created, msg2.bytes, msg2.len)) FIPSD_WARN(HMOD, "msg2 send failed; the peer's resend is answered by the link machine");
    sodium_memzero(&msg2, sizeof msg2);
    ok = true;
out:
    if (lease.live) { uint32_t index; if (fips_rekey_index_lease_release(&lease, &index)) (void)fips_index_allocator_free(&h->indices, (fips_session_index){index}); }
    if (has_ticket) fipsd_peer_budget_release(&ticket);
    fips_rekey_completed_clear(&done);
    fips_fmp_rekey_msg1_clear(&token);
    fips_pending_handshake_release(&slot);
    return ok;
}
static bool leg_send_wrap(handlers *h, peer_rec *rec, const uint8_t *bytes, size_t len)
{
    const fipsd_peer_info *info = fipsd_active_peer_info(rec->peer);
    fipsd_transport *t = hs_transport(h, info->transport);
    return t && t->ops->send(t, &info->addr, bytes, len) == 0;
}
/* Resolve a candidate index without ever publishing its keys as the peer's
 * live epoch. Workers receive only a key clone; the dispatcher owns admission. */
bool hs_restart_frame(handlers *h, fipsd_transport *t, const fips_transport_addr *from,
    const uint8_t *frame, size_t len, const fips_fmp_encrypted_header *header)
{
    if (!h->restart_pending) return false;
    for (size_t i = 0; i < h->cfg.peer_capacity; ++i) {
        peer_rec *rec = &h->recs[i]; restart_candidate *c = &rec->restart;
        if (!rec->used || !c->used || !c->lease.live || c->lease.index != header->receiver_idx.value) continue;
        fips_instant now = hs_now(h);
        if (now.ops->compare(now, c->deadline) >= 0) { restart_clear(h, rec, true, now); return true; }
        /* Transport-strict like a single-path index lookup; the arrival address
         * is judged by the ordinary authenticated endpoint policy after promotion. */
        if (!fips_transport_id_equal(t->id, c->transport)) return false;
        size_t aad = FIPS_FRAMING_FMP_ESTABLISHED_HEADER_SIZE;
        if (len < aad + FIPS_NOISE_TAG_SIZE || len > FIPSD_DP_WIRE) return true;
        fipsd_dp_packet *p = fipsd_dp_acquire(h);
        if (!p) return true;
        p->restart = true; p->peer = p->carrier = rec->addr; p->tag = c->link.value;
        p->transport = t->id; p->endpoint = *from; p->body = aad; p->wire_size = len;
        p->k = (header->flags & FIPS_FMP_FLAG_KEY_EPOCH) != 0;
        p->ce = (header->flags & FIPS_FMP_FLAG_CE) != 0;
        if (fips_noise_session_recv_cipher_clone(&c->done.session, &p->job.key) != FIPS_NOISE_OK) {
            fipsd_crypto_job_destroy(&p->job); return true;
        }
        memcpy(p->ad, frame, aad); memcpy(p->input, frame + aad, len - aad);
        p->job.operation = FIPSD_CRYPTO_OPEN; p->job.counter = header->counter; p->job.token = c->lease.index;
        p->job.ad = p->ad; p->job.ad_size = aad; p->job.input = p->input; p->job.input_size = len - aad;
        p->job.output = p->output + aad; p->job.capacity = FIPSD_DP_WIRE - aad;
        p->job.worker_counts = h->dataplane->stats.opened;
        (void)fipsd_dp_submit(h, p);
        return true;
    }
    return false;
}
/* The eviction gate. A frame that authenticates under the candidate's keys
 * proves the sender holds the msg1 ephemeral AND processed our fresh msg2; a
 * recording of an earlier epoch can do neither. Only then is the incumbent
 * closed, and the proving frame is the new link's first authenticated input. */
bool hs_restart_result(handlers *h, const fipsd_crypto_job *j)
{
    fipsd_dp_packet *p = j->owner;
    if (!h->running) { ++h->dataplane->stats.stale; return true; }
    peer_rec *rec = hs_rec_find(h, &p->peer);
    restart_candidate *c = rec ? &rec->restart : NULL;
    if (!c || !c->used || c->link.value != p->tag || !c->lease.live || c->lease.index != j->token) {
        /* A sibling frame overtaken by the confirmation now names the promoted
         * link: re-enter the ordinary path under the published keys and replay
         * window. The link id (p->tag) pins it to this candidate's promotion. */
        if (rec && j->result == FIPS_NOISE_OK && fipsd_active_peer_info(rec->peer)->link.value == p->tag &&
            hs_index_lookup(h, p->transport.value, (uint32_t)j->token) == rec &&
            p->body <= p->wire_size && p->wire_size <= sizeof h->in_wire) {
            memcpy(h->in_wire, p->ad, p->body); memcpy(h->in_wire + p->body, p->input, p->wire_size - p->body);
            (void)fipsd_dp_open(h, rec, NULL, p->transport, &p->endpoint, h->in_wire, p->wire_size);
            return true;
        }
        ++h->dataplane->stats.stale; return true;
    }
    fips_instant now = hs_now(h);
    if (now.ops->compare(now, c->deadline) >= 0) {
        restart_clear(h, rec, true, now); ++h->dataplane->stats.stale; return true;
    }
    if (j->result != FIPS_NOISE_OK) {
        ++h->dataplane->stats.authentication_failed; fipsd_dp_reject_frame(h, FIPSD_DP_AUTH); return true;
    }
    if (!fips_fmp_rekey_profile.keys->fresh(&c->done.session, j->counter)) { fipsd_dp_reject_frame(h, FIPSD_DP_REPLAY); return true; }
    /* Admission is rechecked at confirmation (A1): a reload while msg2 was in
     * flight must not let a now-denied candidate evict the incumbent. */
    fips_peer_identity identity = fipsd_active_peer_info(rec->peer)->identity;
    if (!hs_acl_authorize(h, &identity, "inbound_confirmation")) {
        hs_notice(h, FIPSD_NOTICE_HANDSHAKE_FAILED, &identity.node_addr, NULL, 0);
        restart_clear(h, rec, false, now); return true;
    }
    fipsd_transport *t = hs_transport(h, c->transport);
    if (!t) { restart_clear(h, rec, false, now); return true; }

    /* Move the candidate off the incumbent before closing it; retire the epoch
     * the peer has now proven it left behind. */
    restart_candidate confirmed = *c;
    sodium_memzero(c, sizeof *c); --h->restart_pending;
    fips_fmp_epoch old_epoch = link_epoch(rec);
    if (old_epoch.present && memcmp(old_epoch.bytes, confirmed.done.epoch, 8))
        restart_remember(h, &rec->addr, old_epoch.bytes, true, (fips_instant){0});
    fips_rekey_carry carry = {0};
    remove_peer_carry(h, rec, FIPS_REKEY_CLOSE_Restart, false, FIPS_FMP_Restart, &carry);
    rec = NULL;
    fips_rekey_setup_facts facts = confirmed.replace ? carry : (fips_rekey_setup_facts){0};
    /* As in msg1: the setup record answers a retransmitted msg1 from the held msg2. */
    (void)fips_fmp_rekey_setup_record(confirmed.msg1.bytes, confirmed.msg1.len, &confirmed.msg2, &facts.record);
    fipsd_peer_ticket ticket = {0};
    bool reserved = fipsd_peer_table_reserve(h->peers->table, t->id, &ticket) == FIPSD_ADMIT_OK;
    /* Established at the msg1 source, as before this change; the proving frame
     * then carries its own arrival address through the ordinary link policy. */
    peer_rec *created = reserved ? establish(h, &identity, t, &confirmed.addr, confirmed.link,
        FIPS_LINK_INBOUND, &confirmed.done, &confirmed.lease, confirmed.done.theirs, &facts, &ticket) : NULL;
    if (created) {
        ++h->counters.hs_restart;
        memcpy(p->output, p->ad, p->body);
        size_t len = p->body + j->output_size;
        uint64_t tag = 0;
        (void)fips_rekey_tag(fipsd_active_peer_link_machine(created->peer), FIPS_REKEY_Current, &tag);
        fips_rekey_input in = {.kind = FIPS_REKEY_IN_AuthenticatedFrame, .tag = tag,
            .buf = p->output, .bytes = p->output, .len = len,
            .hint = {j->counter, p->k, (uint32_t)j->token, p->body}};
        link_exec_result result = {.counter = j->counter};
        (void)hs_link_step(h, created, in, &p->endpoint, p->output, len, &result);
        if (result.teardown) hs_remove_peer(h, created, FIPS_REKEY_CLOSE_Admin, false, FIPS_FMP_ProtocolError);
        else if (result.delivered && result.begin <= result.end && result.end <= len)
            fipsd_dp_link_deliver(h, created, p, p->output + result.begin, result.end - result.begin);
    } else {
        if (reserved) fipsd_peer_budget_release(&ticket);
        ++h->counters.hs_failed;
    }
    if (confirmed.lease.live) {
        uint32_t index;
        if (fips_rekey_index_lease_release(&confirmed.lease, &index))
            (void)fips_index_allocator_free(&h->indices, (fips_session_index){index});
    }
    fips_rekey_completed_clear(&confirmed.done); sodium_memzero(&confirmed, sizeof confirmed);
    hs_mark_dirty(h);
    return true;
}
static bool msg2(handlers *h, fipsd_transport *t, const fips_transport_addr *from, uint8_t *frame, size_t len)
{
    ++h->counters.hs_msg2;
    fips_fmp_msg2_header header;
    if (!fips_fmp_msg2_header_parse(frame, len, &header)) return false;
    pending_leg *leg = NULL;
    for (size_t i = 0; i < h->cfg.peer_capacity && !leg; ++i) {
        pending_leg *l = &h->legs[i];
        if (l->used && l->lease.live && l->lease.index == header.receiver_idx.value && fips_transport_id_equal(l->transport, t->id)) leg = l;
    }
    if (!leg) {
        /* A rekey reply for an established link resolves through its index. */
        peer_rec *rec = hs_index_lookup(h, t->id.value, header.receiver_idx.value);
        if (!rec) { ++h->counters.frames_dropped; METRIC(h, handshake, unknown_connection); return false; }
        /* The indexed link already pins an authenticated identity. Refuse
         * revoked peers before the rekey machine authenticates/transitions. */
        if (!hs_acl_authorize(h, &fipsd_active_peer_info(rec->peer)->identity, "outbound_rekey")) return false;
        link_exec_result r = {0};
        fips_rekey_input in = {.kind = FIPS_REKEY_IN_Reply, .bytes = frame, .buf = frame, .len = len};
        bool ok = hs_link_step(h, rec, in, from, frame, len, &r);
        if (r.teardown && rec->used) hs_remove_peer(h, rec, FIPS_REKEY_CLOSE_Admin, false, FIPS_FMP_ProtocolError);
        return ok;
    }
    if (leg->machine.state != FIPSD_PEER_HANDSHAKING) return false;
    fips_rekey_read read = {0};
    bool ok = fips_fmp_rekey_profile.reply(&leg->hs, &h->local, frame, len, &read);
    if (!ok) { ++h->counters.hs_failed; sodium_memzero(&read, sizeof read); return false; }
    if (memcmp(read.done.remote.bytes, leg->expected.pubkey.bytes, 32)) {
        /* Authenticated as a different node than the one dialed. */
        ++h->counters.hs_rejected;
        fips_rekey_completed_clear(&read.done); sodium_memzero(&read, sizeof read);
        handshake_failed(h, leg, false); leg_free(h, leg);
        return false;
    }
    /* Upstream OutboundHandshake recheck catches policy changes during IK. */
    if (!hs_acl_authorize(h, &leg->expected, "outbound_handshake")) {
        fips_rekey_completed_clear(&read.done); sodium_memzero(&read, sizeof read);
        handshake_failed(h, leg, false); leg_free(h, leg); return false;
    }
    peer_rec *existing = hs_rec_find(h, &leg->expected.node_addr);
    fipsd_peer_event e = {.kind = FIPSD_PEER_MSG2_AUTHENTICATED};
    e.wire.peer_identity = leg->expected; e.wire.remote_epoch = epoch_of(read.done.epoch);
    e.wire.their_index = (fips_session_index){read.done.theirs.value};
    e.outbound.has_existing_peer = existing != NULL;
    if (existing) {
        e.outbound.existing_peer_epoch = link_epoch(existing);
        e.outbound.own_dial = fipsd_active_peer_info(existing->peer)->direction == FIPS_LINK_OUTBOUND;
    }
    e.outbound.our_outbound_wins = fips_fmp_cross_connection_winner(&h->me, &leg->expected.node_addr, true);
    fipsd_peer_actions actions;
    fipsd_peer_machine_step(&leg->machine, hs_now(h), &e, &actions);
    bool promote = false, swap = false, keep = false, restart = false;
    for (size_t i = 0; i < actions.count; ++i) {
        switch (actions.items[i].kind) {
        case FIPSD_PEER_PROMOTE: promote = true; break;
        case FIPSD_PEER_CROSS_SWAP: swap = true; break;
        case FIPSD_PEER_CROSS_KEEP: keep = true; break;
        case FIPSD_PEER_INVALIDATE: case FIPSD_PEER_LOST_LINK: restart = true; break;
        default: break;
        }
    }
    fips_rekey_carry carry = {0}, fresh = {0}, *facts = &fresh;
    (void)fips_fmp_rekey_setup_record(leg->machine.conn.handshake_msg1, leg->machine.conn.handshake_msg1_len, NULL, &fresh.record);
    if (keep) {
        ++h->counters.hs_cross_keep;
        fips_rekey_completed_clear(&read.done); sodium_memzero(&read, sizeof read);
        leg_free(h, leg);
        return true;
    }
    if (existing && (swap || restart)) {
        if (swap) ++h->counters.hs_cross_swap; else ++h->counters.hs_restart;
        remove_peer_carry(h, existing, swap ? FIPS_REKEY_CLOSE_CrossConnectionMove : FIPS_REKEY_CLOSE_Restart,
                          false, swap ? FIPS_FMP_Other : FIPS_FMP_Restart, &carry);
        if (swap) { carry.record = fresh.record; facts = &carry; }
        promote = true;
    }
    if (!promote) { fips_rekey_completed_clear(&read.done); sodium_memzero(&read, sizeof read); return true; }
    fips_peer_identity identity = leg->expected;
    fips_transport_addr addr = leg->addr;
    fips_link_id link = leg->link;
    fips_rekey_index_lease lease = leg->lease; leg->lease = (fips_rekey_index_lease){0};
    fipsd_peer_ticket ticket = leg->ticket; leg->has_ticket = false;
    peer_rec *rec = establish(h, &identity, t, &addr, link, FIPS_LINK_OUTBOUND, &read.done, &lease, read.done.theirs, facts, &ticket);
    if (!rec) {
        fipsd_peer_budget_release(&ticket);
        fips_rekey_completed_clear(&read.done); sodium_memzero(&read, sizeof read);
        handshake_failed(h, leg, false); leg_free(h, leg);
        return false;
    }
    fips_rekey_completed_clear(&read.done); sodium_memzero(&read, sizeof read);
    leg_free(h, leg);
    return true;
}
bool hs_handshake_frame(handlers *h, fipsd_transport *t, const fips_transport_addr *from, uint8_t *frame, size_t len, int phase)
{ return phase == 1 ? msg1(h, t, from, frame, len) : msg2(h, t, from, frame, len); }

bool hs_peer_effects(handlers *h, fipsd_active_peer *peer, const fipsd_peer_actions *actions)
{
    peer_rec *rec = hs_rec_by_peer(h, peer);
    if (!rec) return true;
    bool remove = false, dead = false, disconnect = false;
    for (size_t i = 0; i < actions->count && rec->used; ++i) {
        switch (actions->items[i].kind) {
        case FIPSD_PEER_SEND_HEARTBEAT: {
            uint8_t hb = FIPS_LINK_Heartbeat;
            if (hs_link_send(h, rec, &hb, 1)) ++h->counters.heartbeats_sent;
            break;
        }
        case FIPSD_PEER_POLL_LINK: {
            FIPSD_INSTR_GATE(rekey_on);
            FIPSD_INSTR_BEGIN(rekey_on, rekey_start);
            link_exec_result r = {0};
            (void)hs_link_step(h, rec, (fips_rekey_input){.kind = FIPS_REKEY_IN_Poll}, NULL, NULL, 0, &r);
            FIPSD_INSTR_END(rekey_on, rekey_start, POLL_LINK_REKEYS);
            if (r.teardown) remove = true;
            break;
        }
        case FIPSD_PEER_SEND_DISCONNECT: disconnect = true; remove = true; break;
        case FIPSD_PEER_LOST_LINK: dead = true; remove = true; break;
        case FIPSD_PEER_TEARDOWN: case FIPSD_PEER_INVALIDATE: remove = true; break;
        default: break;
        }
    }
    if (remove && rec->used)
        hs_remove_peer(h, rec, dead ? FIPS_REKEY_CLOSE_LinkDead : FIPS_REKEY_CLOSE_Disconnect, disconnect,
                       dead ? FIPS_FMP_Timeout : FIPS_FMP_Shutdown);
    return true;
}
uint64_t hs_legs_wait_ms(handlers *h, fips_instant now)
{
    uint64_t wait = UINT64_MAX;
    for (size_t i = 0; i < h->cfg.peer_capacity; ++i)
        if (h->legs[i].used) wait = hs_min_wait(wait, fipsd_peer_machine_next_wake(&h->legs[i].machine), now);
    for (size_t i = 0; i < h->cfg.peer_capacity; ++i) {
        if (h->recs[i].used && h->recs[i].restart.used)
            wait = hs_min_wait(wait, h->recs[i].restart.deadline, now);
        configured_peer *c = &h->configured[i];
        if (c->used && c->retry.scheduled && !c->dialing) wait = hs_min_wait(wait, c->retry.after, now);
    }
    return wait;
}
bool hs_legs_poll(handlers *h, fips_instant now)
{
    FIPSD_INSTR_GATE(timer_on);
    FIPSD_INSTR_BEGIN(timer_on, timer_start);
    for (size_t i = 0; i < h->cfg.peer_capacity; ++i) {
        peer_rec *rec = &h->recs[i];
        if (rec->used && rec->restart.used && now.ops->compare(now, rec->restart.deadline) >= 0)
            restart_clear(h, rec, true, now);
    }
    FIPSD_INSTR_BEGIN(timer_on, pending_start);
    for (size_t i = 0; i < h->cfg.peer_capacity; ++i) {
        pending_leg *leg = &h->legs[i];
        if (!leg->used) continue;
        fips_instant at = fipsd_peer_machine_next_wake(&leg->machine);
        if (!at.ops || at.ops->compare(now, at) < 0) continue;
        fipsd_peer_event e = {.kind = FIPSD_PEER_POLL};
        fipsd_peer_actions actions;
        fipsd_peer_machine_step(&leg->machine, now, &e, &actions);
        if (!hs_leg_effects(h, leg, &actions)) {
            FIPSD_INSTR_END(timer_on, pending_start, POLL_PENDING_CONNECTS);
            FIPSD_INSTR_RETURN(timer_on, timer_start, CHECK_TIMEOUTS, false);
        }
    }
    FIPSD_INSTR_END(timer_on, pending_start, POLL_PENDING_CONNECTS);
    hs_configured_poll(h, now);
    FIPSD_INSTR_RETURN(timer_on, timer_start, CHECK_TIMEOUTS, true);
}
void hs_configured_poll(handlers *h, fips_instant now)
{
    FIPSD_INSTR_GATE(timer_on);
    FIPSD_INSTR_BEGIN(timer_on, timer_start);
    for (size_t i = 0; i < h->cfg.peer_capacity; ++i) {
        configured_peer *c = &h->configured[i];
        if (!c->used || c->dialing || !c->retry.scheduled) continue;
        if (hs_rec_find(h, &c->identity.node_addr) || leg_find(h, &c->identity.node_addr)) { fipsd_peer_retry_init(&c->retry); continue; }
        fipsd_peer_retry_policy p = {5000, 300000, h->cfg.timing.handshake_timeout_ms, 5};
        if (fipsd_peer_retry_take(&c->retry, p, now, false, true, false))
            (void)hs_dial(h, &c->identity, c->transport, &c->addr, true);
    }
    FIPSD_INSTR_END(timer_on, timer_start, PROCESS_PENDING_RETRIES);
}
bool hs_dial_configured(handlers *h)
{
    GoRefPtr cfg = fipsd_node_config(h->ctx);
    const fipsd_config *c = cfg.ptr;
    bool ok = c != NULL;
    size_t count = c ? fipsd_config_count(fipsd_config_get(c, "peers")) : 0;
    for (size_t i = 0; i < count; ++i) {
        char path[64];
        snprintf(path, sizeof path, "peers.%zu.npub", i);
        const char *npub = fipsd_config_string(fipsd_config_get(c, path));
        snprintf(path, sizeof path, "peers.%zu.connect_policy", i);
        const char *policy = fipsd_config_string(fipsd_config_get(c, path));
        if (!npub || (policy && strcmp(policy, "auto_connect"))) continue;
        fips_peer_identity identity;
        if (fips_peer_identity_from_npub(h->secp, npub, strlen(npub), &identity) != FIPS_IDENTITY_OK) continue;
        (void)fipsd_identity_cache_put(&h->identities, identity, hs_wall_ms(h));
        snprintf(path, sizeof path, "peers.%zu.addresses", i);
        const fipsd_config_value *addresses = fipsd_config_get(c, path);
        size_t n = fipsd_config_count(addresses);
        for (size_t j = 0; j < n; ++j) {
            const fipsd_config_value *a = fipsd_config_at(addresses, j);
            const char *tname = fipsd_config_string(fipsd_config_member(a, "transport"));
            const char *text = fipsd_config_string(fipsd_config_member(a, "addr"));
            if (!tname || !text) continue;
            fipsd_transport *t = NULL;
            for (size_t k = 0; k < h->registry->count && !t; ++k)
                if (!strcmp(h->registry->items[k]->name, tname) || !strcmp(h->registry->items[k]->type, tname)) t = h->registry->items[k];
            if (!t) continue;
            configured_peer *slot = configured_find(h, &identity.node_addr);
            for (size_t k = 0; k < h->cfg.peer_capacity && !slot; ++k) if (!h->configured[k].used) slot = &h->configured[k];
            if (!slot) continue;
            slot->used = true; slot->identity = identity; slot->transport = t->id;
            if (fips_transport_addr_from_string(text, strlen(text), &slot->addr) != FIPS_OK) { slot->used = false; continue; }
            fipsd_peer_retry_init(&slot->retry);
            (void)hs_dial(h, &identity, t->id, &slot->addr, true);
            break; /* first usable address; path intake owns the rest (Tier B4) */
        }
    }
    go_refptr_release(&cfg);
    return ok;
}
void hs_close_all(handlers *h)
{
    for (size_t i = 0; i < h->cfg.peer_capacity; ++i)
        if (h->recs[i].used) hs_remove_peer(h, &h->recs[i], FIPS_REKEY_CLOSE_Shutdown, true, FIPS_FMP_Shutdown);
    for (size_t i = 0; i < h->cfg.peer_capacity; ++i) if (h->legs[i].used) leg_free(h, &h->legs[i]);
}
