/* Multi-path liveness (upstream handlers/path.rs, path_intake.rs,
 * path_control.rs): one fips_path_bundle per established peer, fed by probe/ack/
 * close link messages, candidate reports, carrier/presence facts and its own
 * next_wake; outputs are encoded path messages on the named path, endpoint
 * switches through the peer table and connection requests on the transport. */
#include "node/handlers/internal.h"
#include "transport/transport.h"

/* What each intake feed lists for one linked peer (upstream Listings; see
 * hs_path_intake below). */
#define HS_PATH_FEEDS 1u
#define HS_PATH_LISTED_MAX 16u
typedef struct hs_path_listed { fips_transport_id transport; fips_transport_addr addr; uint64_t at; } hs_path_listed;
struct hs_path_listing {
    uint64_t clock; /* report order: the least recently reported pair has the lowest `at` */
    size_t count[HS_PATH_FEEDS];
    hs_path_listed items[HS_PATH_FEEDS][HS_PATH_LISTED_MAX];
};

static size_t facts_mtu(void *ctx, fips_transport_id t, const fips_transport_addr *a)
{ fipsd_transport *x = hs_transport(ctx, t); return x ? x->ops->mtu(x, a) : 0; }
static fips_path_medium facts_medium(void *ctx, fips_transport_id t, const fips_transport_addr *a)
{
    (void)a; fipsd_transport *x = hs_transport(ctx, t);
    if (!x) return FIPS_PATH_Unknown;
    fips_path_medium medium = fipsd_transport_medium(x);
    if (medium != FIPS_PATH_Unknown) return medium;
    /* Legacy/custom transports and undetected media retain kind defaults. */
    if (!strcmp(x->type, "ethernet")) return FIPS_PATH_Wired;
    if (!strcmp(x->type, "ble")) return FIPS_PATH_Ble;
    return FIPS_PATH_Unknown;
}
static bool facts_oriented(void *ctx, fips_transport_id t)
{ fipsd_transport *x = hs_transport(ctx, t); return x && x->ops->connect_async != NULL; }
static fips_path_class facts_class(void *ctx, fips_transport_id t)
{
    fipsd_transport *x = hs_transport(ctx, t);
    if (x && !strcmp(x->type, "tor")) return FIPS_PATH_Tor;
    if (x && !strcmp(x->type, "nym")) return FIPS_PATH_Nym;
    return FIPS_PATH_Clear;
}
static fips_path_kind facts_kind(void *ctx, fips_transport_id t)
{ fipsd_transport *x = hs_transport(ctx, t); return x && !strcmp(x->type, "ethernet") ? FIPS_PATH_Local : FIPS_PATH_Unfixed; }
static fips_path_role facts_role(void *ctx, fips_transport_id t) { (void)ctx; (void)t; return FIPS_PATH_Normal; }
static bool facts_accepts(void *ctx, fips_transport_id t) { fipsd_transport *x = hs_transport(ctx, t); return x && x->accept_connections; }
static bool facts_dials(void *ctx, fips_transport_id t) { fipsd_transport *x = hs_transport(ctx, t); return x && x->auto_connect; }
static fips_path_facts facts_of(handlers *h)
{
    return (fips_path_facts){h, facts_mtu, facts_medium, facts_oriented, facts_class, facts_kind, facts_role, facts_accepts, facts_dials};
}
static fips_path_params params_of(handlers *h)
{
    fips_path_config cfg = fips_path_config_draft();
    cfg.enabled = h->cfg.path_enabled;
    if (cfg.heartbeat.secs * 1000 + cfg.heartbeat.nanos / 1000000 < h->cfg.tick_ms) cfg.heartbeat = fips_duration_from_millis(h->cfg.tick_ms);
    fips_path_params params; fips_path_params_error e;
    if (fips_path_params_new(&params, cfg, &e) != FIPS_OK) {
        FIPSD_WARN(HMOD, "path parameters refused (%s); multipath disabled", e.field ? e.field : "?");
        params = fips_path_params_draft(); params.cfg.enabled = false;
    }
    return params;
}
void hs_path_peer_added(handlers *h, peer_rec *rec)
{
    if (!rec->path) rec->path = calloc(1, sizeof *rec->path);
    if (!rec->path) return;
    const fipsd_peer_info *info = fipsd_active_peer_info(rec->peer);
    fips_path_facts facts = facts_of(h);
    fips_path_env env = {&facts, hs_mmp_link_cost(h, rec)};
    fips_path_bundle_new(rec->path, hs_now(h), info->transport, info->addr, params_of(h), h->entropy, &env);
    rec->path_live = true;
    hs_timers_invalidate(h); /* a new Bundle */
    /* A new machine starts with an empty candidate table: forget what the
     * feeds listed for the old one (upstream paths_renewed), so each feed's
     * next report is a candidate for this one. */
    if (rec->listing) memset(rec->listing, 0, sizeof *rec->listing);
}
static void run_outputs(handlers *h, peer_rec *rec, fips_path_outputs *out)
{
    for (size_t i = 0; i < out->len && rec->used; ++i) {
        fips_path_output *o = &out->items[i];
        switch (o->kind) {
        case FIPS_PATH_OUTPUT_Send: {
            fips_writer w; fips_writer_init(&w, h->tx_msg, sizeof h->tx_msg);
            if (fips_path_output_encode(o, &w, NULL) == FIPS_OK &&
                hs_link_send_to(h, rec, o->to.transport, &o->to.addr, h->tx_msg, fips_writer_len(&w))) ++h->counters.path_sent;
            else {
                fips_path_input failed = {.kind = FIPS_PATH_INPUT_SendFailed, .to = o->to};
                fips_path_outputs more = {h->path_outputs, FIPS_PATH_OUTPUTS_MAX, 0};
                fips_path_facts facts = facts_of(h); fips_path_env env = {&facts, hs_mmp_link_cost(h, rec)};
                /* Re-entrancy on the same output storage is avoided: the failure
                 * is reported on the next poll instead. */
                (void)failed; (void)more; (void)env;
            }
            break;
        }
        case FIPS_PATH_OUTPUT_Switched: {
            /* Before the old pair is closed: no worker sends on it after this. */
            fipsd_dp_link_moved(h, rec);
            const fipsd_peer_info *old = fipsd_active_peer_info(rec->peer);
            fipsd_transport *previous = hs_transport(h, old->transport);
            fips_transport_id old_transport = old->transport;
            fips_transport_addr old_addr = old->addr;
            if (previous && previous->ops->close_connection) previous->ops->close_connection(previous, &old->addr);
            fipsd_peer_admission a = fipsd_peer_table_rebind(h->peers->table, &rec->addr, o->to.transport, &o->to.addr);
            if (a != FIPSD_ADMIT_OK) {
                /* The Bundle already switched. Do not keep a live link whose
                 * send pair disagrees with it (or steal another link's pair). */
                FIPSD_WARN(HMOD, "path switch rebind refused (%d)", (int)a);
                hs_remove_peer(h, rec, FIPS_REKEY_CLOSE_Admin, false, FIPS_FMP_ResourceExhaustion);
                return;
            }
            if (!fipsd_dp_peer_rebind(h, rec)) {
                hs_remove_peer(h, rec, FIPS_REKEY_CLOSE_Admin, false, FIPS_FMP_ResourceExhaustion);
                return;
            }
            /* Upstream relink: the rebind marked the new link before the old
             * one is cleared, so a newcomer never finds the peer unlinked. */
            if (previous && previous->ops->peer_link && !(fips_transport_id_equal(old_transport, o->to.transport) &&
                    fips_transport_addr_equal(&old_addr, &o->to.addr)))
                previous->ops->peer_link(previous, &old_addr, &rec->addr, false, false);
            ++h->counters.path_switched;
            fipsd_node_notice n = {.kind = FIPSD_NOTICE_PATH_EVENT, .peer = rec->addr, .value = 1};
            hs_notice_full(h, &n, NULL, 0);
            hs_mark_dirty(h);
            break;
        }
        case FIPS_PATH_OUTPUT_Connect: {
            fipsd_transport *t = hs_transport(h, o->to.transport);
            if (t && t->ops->connect_async) (void)t->ops->connect_async(t, &o->to.addr);
            break;
        }
        case FIPS_PATH_OUTPUT_Orphaned:
            hs_remove_peer(h, rec, FIPS_REKEY_CLOSE_LinkDead, false, FIPS_FMP_TransportFailure);
            return;
        case FIPS_PATH_OUTPUT_Proven: {
            const fipsd_peer_info *info = fipsd_active_peer_info(rec->peer);
            fipsd_transport *t = hs_transport(h, o->to.transport);
            /* As upstream reseed: standby proofs wait until that path becomes
             * active; connection-oriented paths use their transport MTU. */
            if (t && !t->ops->connect_async && fips_transport_id_equal(info->transport, o->to.transport) &&
                fips_transport_addr_equal(&info->addr, &o->to.addr) && !fipsd_dp_peer_rebind(h, rec)) {
                hs_remove_peer(h, rec, FIPS_REKEY_CLOSE_Admin, false, FIPS_FMP_ResourceExhaustion);
                return;
            }
            break;
        }
        default: break; /* Moved/Released driver slots are unused; state/capability notices are diagnostic */
        }
    }
}
static void step(handlers *h, peer_rec *rec, const fips_path_input *input)
{
    if (!rec->path_live) return;
    fips_path_facts facts = facts_of(h);
    fips_path_env env = {&facts, hs_mmp_link_cost(h, rec)};
    fips_path_outputs out = {h->path_outputs, FIPS_PATH_OUTPUTS_MAX, 0};
    if (fips_path_bundle_step(rec->path, hs_now(h), input, &env, &out) != FIPS_OK) return;
    hs_timers_fold_path(h, rec);
    run_outputs(h, rec, &out);
    hs_timers_fold_path(h, rec);
}
void hs_path_peer_removed(handlers *h, peer_rec *rec)
{
    if (!rec->path_live) return;
    fips_path_outputs out = {h->path_outputs, FIPS_PATH_OUTPUTS_MAX, 0};
    (void)fips_path_bundle_close(rec->path, &out);
    rec->path_live = false;
    /* The listing goes with the link (upstream Listings::expire drops peers
     * no longer held). */
    if (rec->listing) memset(rec->listing, 0, sizeof *rec->listing);
}
void hs_path_message(handlers *h, peer_rec *rec, fipsd_transport *t, const fips_transport_addr *from, fips_link_message_type type, const uint8_t *body, size_t len)
{
    fips_reader r; fips_reader_init(&r, body, len);
    fips_path_input in = {.to = {t->id, *from}};
    if (type == FIPS_LINK_PathProbe) {
        if (fips_path_probe_decode(&r, &in.as.probe, NULL) != FIPS_OK) return;
        in.kind = FIPS_PATH_INPUT_Probe; ++h->counters.path_probes_in;
    } else if (type == FIPS_LINK_PathAck) {
        if (fips_path_ack_decode(&r, &in.as.ack, NULL) != FIPS_OK) return;
        in.kind = FIPS_PATH_INPUT_Ack; ++h->counters.path_acks_in;
    } else {
        if (fips_path_close_decode(&r, &in.as.close, NULL) != FIPS_OK) return;
        in.kind = FIPS_PATH_INPUT_Close; ++h->counters.path_closes_in;
    }
    step(h, rec, &in);
}
void hs_path_account(handlers *h, peer_rec *rec, fipsd_transport *t, const fips_transport_addr *from, size_t len, uint8_t msg_type)
{
    /* Hearing a path (or a roam) can arm its probe or capability wake. */
    if (rec->path_live) {
        (void)fips_path_bundle_account(rec->path, hs_now(h), t->id, from, len, msg_type);
        hs_timers_fold_path(h, rec);
    }
}
fips_instant hs_path_next(handlers *h)
{
    fips_instant at = {0};
    for (size_t i = 0; i < h->cfg.peer_capacity; ++i)
        if (h->recs[i].used && h->recs[i].path_live) at = hs_instant_min(at, fips_path_bundle_next_wake(h->recs[i].path));
    return at;
}
void hs_path_poll(handlers *h, fips_instant now)
{
    for (size_t i = 0; i < h->cfg.peer_capacity; ++i) {
        peer_rec *rec = &h->recs[i];
        if (!rec->used || !rec->path_live) continue;
        fips_instant wake = fips_path_bundle_next_wake(rec->path);
        if (!wake.ops || wake.ops->compare(now, wake) < 0) continue;
        fips_path_input in = {.kind = FIPS_PATH_INPUT_Poll};
        step(h, rec, &in);
    }
}
void hs_path_network_event(handlers *h, const fipsd_netmon_event *e)
{
    if (e->kind == FIPSD_BINDING_UP || e->kind == FIPSD_BINDING_DOWN ||
        e->kind == FIPSD_BINDING_MEDIUM_CHANGED) {
        fips_path_input in = {.kind = e->kind == FIPSD_BINDING_MEDIUM_CHANGED ?
            FIPS_PATH_INPUT_Poll : FIPS_PATH_INPUT_Present,
            .to = {.transport = e->transport}, .as.present = e->kind == FIPSD_BINDING_UP};
        /* Facts were stored before the binder queued this event. Poll here is
         * a reducer input, not a timer: it reranks without releasing held candidates.
         * Any resulting dwell is serviced by the Bundle's existing next_wake. */
        for (size_t i = 0; i < h->cfg.peer_capacity; ++i)
            if (h->recs[i].used) step(h, &h->recs[i], &in);
        return;
    }
    if (e->kind != FIPSD_INTERFACE_UP && e->kind != FIPSD_INTERFACE_DOWN) return;
    /* Present is transport-wide, not one address/path: the Bundle shelves
     * candidates on departure and reoffers them on return. Like upstream
     * path_presence, feed every linked peer, including candidate-only users. */
    for (size_t t = 0; t < h->registry->count; ++t) {
        fipsd_transport *transport = h->registry->items[t];
        if (!transport->interface_name ||
            strcmp(transport->interface_name, e->interface_name)) continue;
        fips_path_input in = {.kind = FIPS_PATH_INPUT_Present,
            .to = {.transport = transport->id}, .as.present = e->kind == FIPSD_INTERFACE_UP};
        for (size_t i = 0; i < h->cfg.peer_capacity; ++i)
            if (h->recs[i].used) step(h, &h->recs[i], &in);
    }
}
/* ---- candidate intake (upstream path_intake.rs) -------------------------
 * For each linked peer and each feed, the pairs the feed lists. Upstream's
 * Feed::Mdns reports one pair at a time and never goes silent (mdns-sd
 * reports a service again only when its records change), so a pair it did
 * not list is a Candidate from Source::Discovery, one it lists already only
 * refreshes its recency, and past HS_PATH_LISTED_MAX pairs the least
 * recently reported is Unlisted. The listing lasts until the link goes or
 * its machine starts again. Upstream's other feeds (Beacon, Config, Nostr,
 * Heard) are not ported; Beacon shares Source::Discovery with Mdns, and its
 * port must add upstream's `elsewhere` check across same-source feeds. */
static fips_path_source feed_source(fipsd_path_feed feed) { (void)feed; return FIPS_PATH_Discovery; }
void hs_path_intake(handlers *h, const fips_node_addr *peer, fipsd_path_feed feed,
                    fips_transport_id transport, const fips_transport_addr *addr)
{
    if (!h->cfg.path_enabled || (unsigned)feed >= HS_PATH_FEEDS) return;
    peer_rec *rec = hs_rec_find(h, peer);
    if (!rec || !rec->path_live) return;
    if (!rec->listing && !(rec->listing = calloc(1, sizeof *rec->listing))) return;
    struct hs_path_listing *l = rec->listing;
    hs_path_listed *list = l->items[feed];
    size_t *n = &l->count[feed];
    for (size_t i = 0; i < *n; ++i)
        if (fips_transport_id_equal(list[i].transport, transport) && fips_transport_addr_equal(&list[i].addr, addr)) {
            list[i].at = ++l->clock;
            return;
        }
    hs_path_listed gone = {0};
    bool evicted = false;
    if (*n == HS_PATH_LISTED_MAX) {
        size_t oldest = 0;
        for (size_t i = 1; i < *n; ++i) if (list[i].at < list[oldest].at) oldest = i;
        gone = list[oldest];
        memmove(&list[oldest], &list[oldest + 1], (*n - oldest - 1) * sizeof *list);
        --*n; evicted = true;
    }
    list[(*n)++] = (hs_path_listed){transport, *addr, ++l->clock};
    fips_path_input in = {.kind = FIPS_PATH_INPUT_Candidate, .to = {transport, *addr}, .as.source = feed_source(feed)};
    step(h, rec, &in);
    if (evicted && rec->used && rec->path_live) {
        fips_path_input out = {.kind = FIPS_PATH_INPUT_Unlisted, .to = {gone.transport, gone.addr}, .as.source = feed_source(feed)};
        step(h, rec, &out);
    }
}
static const char *state_name(fips_path_state s)
{ return s == FIPS_PATH_Probing ? "probing" : s == FIPS_PATH_Live ? "live" : s == FIPS_PATH_Suspect ? "suspect" : "dead"; }
json_t *hs_path_show(handlers *h, const fips_node_addr *peer)
{
    peer_rec *rec = hs_rec_find(h, peer);
    if (!rec || !rec->path_live) return fipsd_control_error("peer not found");
    fips_path_bundle_view v; fips_path_bundle_view_get(rec->path, hs_now(h), &v);
    json_t *paths = json_array();
    for (size_t i = 0; i < v.len; ++i) {
        fips_path_view *p = &v.paths[i];
        char addr[300]; size_t n = 0;
        if (fips_transport_addr_format(&p->to.addr, addr, sizeof addr, &n) != FIPS_OK) addr[0] = 0;
        fipsd_transport *t = hs_transport(h, p->to.transport);
        json_t *row = json_pack("{s:i,s:o,s:o,s:s,s:s,s:b,s:b,s:s,s:b,s:o,s:o,s:I,s:f,s:o}",
            "transport_id", (int)p->to.transport.value, "transport", t ? json_string(t->name) : json_null(),
            "transport_type", t ? json_string(t->type) : json_null(), "addr", addr, "state", state_name(p->state),
            "active", p->active, "remote_active", p->remote, "role", "normal", "pinned", p->pinned,
            "last_rtt_ms", p->has_rtt ? json_integer((json_int_t)(p->rtt.secs * 1000 + p->rtt.nanos / 1000000)) : json_null(),
            "min_rtt_ms", p->has_least ? json_integer((json_int_t)(p->least.secs * 1000 + p->least.nanos / 1000000)) : json_null(),
            "rtt_samples", (json_int_t)p->samples, "etx", p->etx, "score", p->has_score ? json_real(p->score) : json_null());
        if (row) json_array_append_new(paths, row);
    }
    json_t *pin = json_null();
    if (v.has_pin) {
        char addr[300]; size_t n = 0;
        if (v.pin.has_addr && fips_transport_addr_format(&v.pin.addr, addr, sizeof addr, &n) != FIPS_OK) addr[0] = 0;
        pin = json_pack("{s:i,s:o,s:b}", "transport_id", (int)v.pin.transport.value,
            "addr", v.pin.has_addr ? json_string(addr) : json_null(), "enforced", v.pin_enforced);
    }
    json_t *d = json_pack("{s:b,s:b,s:b,s:f,s:o,s:o}", "enabled", v.enabled, "capable", v.capable, "held", v.held,
        "link_cost", hs_mmp_link_cost(h, rec), "pin", pin, "paths", paths);
    return fipsd_control_ok(d);
}
json_t *hs_path_pin(handlers *h, const fips_node_addr *peer, fips_transport_id transport, const fips_transport_addr *addr, bool has_addr, bool pin)
{
    peer_rec *rec = hs_rec_find(h, peer);
    if (!rec || !rec->path_live) return fipsd_control_error("peer not found");
    if (!h->cfg.path_enabled) return fipsd_control_error("multipath is disabled");
    fips_path_input in = {.kind = pin ? FIPS_PATH_INPUT_Pin : FIPS_PATH_INPUT_Unpin, .to = {.transport = transport}};
    if (has_addr) in.to.addr = *addr;
    in.as.has_addr = has_addr;
    step(h, rec, &in);
    return fipsd_control_ok(json_pack("{s:b}", "pinned", pin));
}
