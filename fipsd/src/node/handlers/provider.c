/* Control provider (upstream control/snapshot.rs + queries.rs rows): the
 * dispatcher publishes an immutable fipsd_control_snapshot (own arena, deep
 * copies, JSON row projections with upstream field names) through GoSwap after
 * state changes and on every tick; live commands are enqueued to the dispatcher
 * and answered on a capacity-one reply channel selected against cancellation. */
#include "node/handlers/internal.h"
#include "control/counter_schema.h"
#include "cli.h"
#include "transport/transport.h"
#include "control/transport.h"
#include <unistd.h>

typedef struct snap {
    fipsd_control_snapshot s;
    void **blocks; size_t nblocks, cap;
    json_t **jsons; size_t njson, jcap;
    uint64_t mesh; double fill, estimate;
    bool has_mesh, has_fill;
} snap;
/* Arena block the caller overwrites entirely: no zeroing (the stats-history
 * copy is megabytes; zeroing it dominated every snapshot build). */
static void *salloc_raw(snap *x, size_t n)
{
    if (x->nblocks == x->cap) {
        size_t cap = x->cap ? x->cap * 2 : 64;
        void **b = realloc(x->blocks, cap * sizeof *b);
        if (!b) return NULL;
        x->blocks = b; x->cap = cap;
    }
    void *p = malloc(n ? n : 1);
    if (p) x->blocks[x->nblocks++] = p;
    return p;
}
static void *salloc(snap *x, size_t n)
{
    void *p = salloc_raw(x, n);
    if (p) memset(p, 0, n ? n : 1);
    return p;
}
static const char *sdup(snap *x, const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = salloc(x, n);
    if (p) memcpy(p, s, n);
    return p;
}
static json_t *sjson(snap *x, json_t *j)
{
    if (!j) return NULL;
    if (x->njson == x->jcap) {
        size_t cap = x->jcap ? x->jcap * 2 : 32;
        json_t **b = realloc(x->jsons, cap * sizeof *b);
        if (!b) { json_decref(j); return NULL; }
        x->jsons = b; x->jcap = cap;
    }
    x->jsons[x->njson++] = j;
    return j;
}
static void snap_free(void *p)
{
    snap *x = p;
    for (size_t i = 0; i < x->nblocks; ++i) free(x->blocks[i]);
    for (size_t i = 0; i < x->njson; ++i) json_decref(x->jsons[i]);
    free(x->blocks); free(x->jsons); free(x);
}
static const char *hex_of(snap *x, const fips_node_addr *a)
{
    char buf[33]; (void)fips_node_addr_format(a, buf, sizeof buf); return sdup(x, buf);
}
static const char *display_of(snap *x, const fips_node_addr *a)
{
    char buf[12]; (void)fips_node_addr_short_hex(a, buf, sizeof buf); return sdup(x, buf);
}
static const char *npub_of(handlers *h, snap *x, const fips_node_addr *a)
{
    char buf[FIPS_NIP19_CAPACITY];
    if (!memcmp(a->bytes, h->me.bytes, 16)) return sdup(x, h->npub);
    fipsd_active_peer *p = fipsd_peer_table_find(h->peers->table, a);
    const fips_peer_identity *id = p ? &fipsd_active_peer_info(p)->identity : NULL;
    if (!id) {
        for (size_t i = 0; i < h->identities.count; ++i)
            if (!memcmp(h->identities.entries[i].peer.node_addr.bytes, a->bytes, 16)) id = &h->identities.entries[i].peer;
    }
    if (!id || fips_peer_identity_npub(id, buf, sizeof buf) != FIPS_IDENTITY_OK) return NULL;
    return sdup(x, buf);
}
static json_t *coords_json(const fips_tree_coordinate *c)
{
    json_t *a = json_array();
    for (size_t i = 0; c && i < c->len; ++i) {
        char buf[33]; (void)fips_node_addr_format(&c->entries[i].node_addr, buf, sizeof buf);
        json_array_append_new(a, json_string(buf));
    }
    return a;
}
static uint64_t wall_of(handlers *h, fips_instant at) { return at.ops ? h->wall_offset_ms + hs_elapsed_ms(at, h->origin) : 0; }
static fipsd_control_counters counters_of(snap *x, const fipsd_control_counter *items, size_t n)
{
    fipsd_control_counter *copy = salloc(x, n * sizeof *copy);
    if (!copy) return (fipsd_control_counters){NULL, 0};
    memcpy(copy, items, n * sizeof *copy);
    return (fipsd_control_counters){copy, n};
}
static json_t *mmp_json(const fips_mmp_metrics *m, fips_mmp_mode mode, bool session, const uint16_t *path_mtu)
{
    json_t *o = json_object();
    json_object_set_new(o, "mode", json_string(fips_mmp_mode_name(mode)));
    double v;
    if (fips_mmp_metrics_srtt_ms(m, &v)) json_object_set_new(o, "srtt_ms", json_real(v));
    json_object_set_new(o, "loss_rate", json_real(fips_mmp_metrics_loss_rate(m)));
    json_object_set_new(o, "etx", json_real(m->etx));
    json_object_set_new(o, "goodput_bps", json_real(m->goodput_bps));
    json_object_set_new(o, "delivery_ratio_forward", json_real(m->delivery_ratio_forward));
    json_object_set_new(o, "delivery_ratio_reverse", json_real(m->delivery_ratio_reverse));
    if (fips_mmp_metrics_smoothed_loss(m, &v)) json_object_set_new(o, "smoothed_loss", json_real(v));
    if (fips_mmp_metrics_smoothed_etx(m, &v)) json_object_set_new(o, "smoothed_etx", json_real(v));
    if (fips_mmp_metrics_quality_index(m, &v)) json_object_set_new(o, session ? "sqi" : "lqi", json_real(v));
    if (session && path_mtu) json_object_set_new(o, "path_mtu", json_integer(*path_mtu));
    return o;
}
static const char *trend(const fips_mmp_dual_ewma *e)
{
    if (!e->initialized) return NULL;
    return e->short_avg > e->long_avg * 1.05 ? "rising" : e->short_avg < e->long_avg * 0.95 ? "falling" : "stable";
}
static json_t *layer_json(const fips_mmp_metrics *m, bool session, bool spin_initiator, uint32_t ecn, uint16_t path_mtu)
{
    json_t *o = json_object();
    json_object_set_new(o, "loss_rate", json_real(fips_mmp_metrics_loss_rate(m)));
    json_object_set_new(o, "etx", json_real(m->etx));
    double v;
    if (session) json_object_set_new(o, "path_mtu", json_integer(path_mtu));
    else {
        json_object_set_new(o, "goodput_bps", json_real(m->goodput_bps));
        json_object_set_new(o, "spin_bit_role", json_string(spin_initiator ? "initiator" : "responder"));
    }
    if (fips_mmp_metrics_smoothed_loss(m, &v)) json_object_set_new(o, "smoothed_loss", json_real(v));
    if (fips_mmp_metrics_smoothed_etx(m, &v)) json_object_set_new(o, "smoothed_etx", json_real(v));
    if (fips_mmp_metrics_srtt_ms(m, &v)) {
        json_object_set_new(o, "srtt_ms", json_real(v));
        double q; if (fips_mmp_metrics_quality_index(m, &q)) json_object_set_new(o, session ? "sqi" : "lqi", json_real(q));
    }
    const char *t;
    if ((t = trend(&m->rtt_trend))) json_object_set_new(o, "rtt_trend", json_string(t));
    if ((t = trend(&m->loss_trend))) json_object_set_new(o, "loss_trend", json_string(t));
    if (session) { if ((t = trend(&m->etx_trend))) json_object_set_new(o, "etx_trend", json_string(t)); }
    else {
        if ((t = trend(&m->goodput_trend))) json_object_set_new(o, "goodput_trend", json_string(t));
        if ((t = trend(&m->jitter_trend))) json_object_set_new(o, "jitter_trend", json_string(t));
        json_object_set_new(o, "delivery_ratio_forward", json_real(m->delivery_ratio_forward));
        json_object_set_new(o, "delivery_ratio_reverse", json_real(m->delivery_ratio_reverse));
        json_object_set_new(o, "ecn_ce_count", json_integer(ecn));
    }
    return o;
}
static const char *peer_state_name(fipsd_peer_state s)
{
    static const char *const names[] = {"discovered", "connecting", "handshaking", "authorizing", "established",
        "active", "maintaining", "closing", "failed", "closed"};
    return (size_t)s < sizeof names / sizeof names[0] ? names[s] : "unknown";
}
static bool cache_row(void *ctx, const uint8_t addr[16], const fips_cache_entry *e)
{
    struct { handlers *h; snap *x; fipsd_control_cache_entry *rows; size_t n, cap; } *c = ctx;
    if (c->n == c->cap) return false;
    fipsd_control_cache_entry *r = &c->rows[c->n++];
    fips_node_addr a; memcpy(a.bytes, addr, 16);
    r->node_addr = hex_of(c->x, &a); r->display_name = display_of(c->x, &a);
    json_t *path = json_array();
    for (size_t i = 0; i < e->coords.len; ++i) {
        fips_node_addr n; memcpy(n.bytes, e->coords.entries[i].node_addr, 16);
        char buf[33]; (void)fips_node_addr_format(&n, buf, sizeof buf);
        json_array_append_new(path, json_string(buf));
    }
    r->coord_path = sjson(c->x, path);
    r->depth = e->coords.len ? e->coords.len - 1 : 0;
    r->created_at = c->h->wall_offset_ms + e->created_at; r->last_used_ms = c->h->wall_offset_ms + e->last_used;
    if (e->has_path_mtu) { uint16_t *m = salloc(c->x, sizeof *m); if (m) { *m = e->path_mtu; r->path_mtu = m; } }
    return true;
}
static snap *build(handlers *h, fips_instant now)
{
    snap *x = calloc(1, sizeof *x);
    if (!x) return NULL;
    fipsd_control_snapshot *s = &x->s;
    s->npub = sdup(x, h->npub); s->node_addr = hex_of(x, &h->me);
    char ipv6[64]; fips_ipv6_address a6 = fips_identity_address(h->identity);
    (void)fips_ipv6_address_format(&a6, ipv6, sizeof ipv6); s->ipv6_addr = sdup(x, ipv6);
    s->state = h->running ? "running" : "stopped";
    s->version = sdup(x, fipsd_cli_short_version());
    s->pid = (uint64_t)getpid(); s->uptime_secs = hs_elapsed_ms(now, h->started_at) / 1000;
    s->now_ms = wall_of(h, now); s->now = now;
    s->is_leaf_only = h->cfg.leaf_only; s->is_root = fips_tree_state_is_root(h->tree);
    GoRefPtr cfg = fipsd_node_config(h->ctx);
    if (cfg.ptr) s->persistent = fipsd_config_bool(fipsd_config_get(cfg.ptr, "node.identity.persistent"));
    json_t *acl = sjson(x, fipsd_acl_status(fipsd_config_acl(cfg.ptr)));
    s->acl.allow_file = json_string_value(json_object_get(acl, "allow_file"));
    s->acl.deny_file = json_string_value(json_object_get(acl, "deny_file"));
    s->acl.effective_mode = json_string_value(json_object_get(acl, "effective_mode"));
    s->acl.default_decision = json_string_value(json_object_get(acl, "default_decision"));
    s->acl.enforcement_active = json_is_true(json_object_get(acl, "enforcement_active"));
    s->acl.allow_all = json_is_true(json_object_get(acl, "allow_all"));
    s->acl.deny_all = json_is_true(json_object_get(acl, "deny_all"));
    s->acl.allow_entries = json_object_get(acl, "allow_entries");
    s->acl.deny_entries = json_object_get(acl, "deny_entries");
    s->acl.allow_file_entries = json_object_get(acl, "allow_file_entries");
    s->acl.deny_file_entries = json_object_get(acl, "deny_file_entries");
    go_refptr_release(&cfg);
    /* Upstream node mesh estimate and last filter actually sent up-tree. */
    if (hs_stats_mesh(h, &x->mesh)) { x->has_mesh = true; s->estimated_mesh_size = &x->mesh; }
    const fips_parent_declaration *declaration = fips_tree_state_my_declaration(h->tree);
    fips_bloom_peer *parent_filter = fips_tree_state_is_root(h->tree) ? NULL : fips_bloom_state_peer(&h->bloom, &declaration->parent_id);
    if (parent_filter && parent_filter->sent_len) {
        x->fill = fips_bloom_fill_ratio(&parent_filter->sent[0]); s->bloom.uptree_fill_ratio = &x->fill;
        if (fips_bloom_estimated_count(&parent_filter->sent[0], FIPS_BLOOM_ACCEPT_FPR_CAP, &x->estimate))
            s->bloom.uptree_estimated_count = &x->estimate;
    }
    size_t peers = 0, legs = 0, established = 0;
    json_t *counts = json_object(), *peer_rows = json_array(), *link_rows = json_array(), *mmp_peers = json_array();
    json_t *tree_rows = json_array(), *filter_rows = json_array();
    fipsd_control_peer_meta *meta = salloc(x, (h->cfg.peer_capacity + 1) * sizeof *meta);
    size_t nmeta = 0;
    for (size_t i = 0; i < h->cfg.peer_capacity; ++i) {
        peer_rec *rec = &h->recs[i];
        if (!rec->used) continue;
        ++peers;
        const fipsd_peer_info *info = fipsd_active_peer_info(rec->peer);
        char key[16]; snprintf(key, sizeof key, "%u", (unsigned)info->transport.value);
        json_object_set_new(counts, key, json_integer(json_integer_value(json_object_get(counts, key)) + 1));
        const char *hex = hex_of(x, &rec->addr), *display = display_of(x, &rec->addr), *npub = npub_of(h, x, &rec->addr);
        if (meta) { memcpy(meta[nmeta].node_addr, rec->addr.bytes, 16); meta[nmeta].npub = npub; meta[nmeta].display_name = display; meta[nmeta].is_active = true; ++nmeta; }
        char ip[64]; (void)fips_ipv6_address_format(&info->identity.address, ip, sizeof ip);
        char addr[300]; size_t n = 0; if (fips_transport_addr_format(&info->addr, addr, sizeof addr, &n) != FIPS_OK) addr[0] = 0;
        fipsd_transport *t = hs_transport(h, info->transport);
        fips_rekey_view v = fipsd_active_peer_link_view(rec->peer);
        const fips_tree_coordinate *coords = hs_tree_peer_coords(h, &rec->addr);
        bool is_parent = hs_tree_is_parent(h, &rec->addr), is_child = hs_tree_is_child(h, &rec->addr);
        json_t *row = json_object();
        json_object_set_new(row, "node_addr", json_string(hex)); json_object_set_new(row, "npub", json_string(npub ? npub : ""));
        json_object_set_new(row, "display_name", json_string(display)); json_object_set_new(row, "ipv6_addr", json_string(ip));
        json_object_set_new(row, "connectivity", json_string(rec->heard && hs_elapsed_ms(now, rec->last_heard) < h->cfg.link_dead_timeout_ms ? "connected" : "stale"));
        json_object_set_new(row, "link_id", json_integer((json_int_t)info->link.value));
        json_object_set_new(row, "authenticated_at_ms", json_integer((json_int_t)info->data.authenticated_at));
        json_object_set_new(row, "last_seen_ms", json_integer((json_int_t)info->data.last_seen));
        json_object_set_new(row, "has_tree_position", json_boolean(coords != NULL));
        json_object_set_new(row, "has_bloom_filter", json_boolean(info->data.filter_sequence != 0));
        json_object_set_new(row, "filter_sequence", json_integer((json_int_t)info->data.filter_sequence));
        json_object_set_new(row, "is_parent", json_boolean(is_parent)); json_object_set_new(row, "is_child", json_boolean(is_child));
        json_object_set_new(row, "transport_addr", json_string(addr));
        json_object_set_new(row, "direction", json_string(info->direction == FIPS_LINK_OUTBOUND ? "outbound" : "inbound"));
        if (t) json_object_set_new(row, "transport_type", json_string(t->type));
        if (coords) json_object_set_new(row, "tree_depth", json_integer((json_int_t)fips_tree_coordinate_depth(coords)));
        double srtt;
        if (coords && hs_mmp_srtt(h, rec, &srtt)) json_object_set_new(row, "effective_depth", json_real((double)fips_tree_coordinate_depth(coords) + hs_mmp_link_cost(h, rec)));
        else json_object_set_new(row, "effective_depth", json_null());
        json_object_set_new(row, "stats", json_pack("{s:I,s:I,s:I,s:I}", "packets_sent", (json_int_t)info->data.link_stats.packets_sent,
            "packets_recv", (json_int_t)info->data.link_stats.packets_recv, "bytes_sent", (json_int_t)info->data.link_stats.bytes_sent,
            "bytes_recv", (json_int_t)info->data.link_stats.bytes_recv));
        json_object_set_new(row, "replay_suppressed", json_integer(0));
        json_object_set_new(row, "replay_suppressed_total", json_integer((json_int_t)info->data.replay_total));
        json_object_set_new(row, "consecutive_decrypt_failures", json_integer(0));
        json_object_set_new(row, "nostr_traversal", json_pack("{s:i,s:b,s:n,s:n}", "consecutive_failures", 0, "in_cooldown", 0, "cooldown_until_ms", "last_observed_skew_ms"));
        fips_rekey *m = fipsd_active_peer_link_machine(rec->peer);
        if (m) {
            const fips_noise_session *ns = fips_rekey_keys(m, FIPS_REKEY_Current);
            if (ns) json_object_set_new(row, "noise", json_pack("{s:I,s:I}", "send_counter", (json_int_t)fips_noise_session_current_send_counter(ns),
                "highest_recv_counter", (json_int_t)fips_noise_session_highest_received_counter(ns)));
            fips_rekey_index ours = fips_rekey_ours(m, FIPS_REKEY_Current);
            if (ours.some) { char ix[9]; (void)fips_session_index_format((fips_session_index){ours.value}, ix, sizeof ix); json_object_set_new(row, "our_session_index", json_string(ix)); }
        }
        if (v.cycle != FIPS_REKEY_Idle) json_object_set_new(row, "rekey_in_progress", json_true());
        if (v.previous.present) json_object_set_new(row, "rekey_draining", json_true());
        json_object_set_new(row, "current_k_bit", json_boolean(v.current.present && v.current.k));
        fips_mmp_peer_state *mmp = fipsd_active_peer_mmp(rec->peer);
        if (mmp) json_object_set_new(row, "mmp", mmp_json(&mmp->metrics, mmp->mode, false, NULL));
        json_t *paths = json_array();
        if (rec->path_live) {
            fips_path_bundle_view pv; fips_path_bundle_view_get(rec->path, now, &pv);
            for (size_t k = 0; k < pv.len; ++k) {
                fips_path_view *p = &pv.paths[k];
                char paddr[300]; size_t pn = 0; if (fips_transport_addr_format(&p->to.addr, paddr, sizeof paddr, &pn) != FIPS_OK) paddr[0] = 0;
                fipsd_transport *pt = hs_transport(h, p->to.transport);
                json_array_append_new(paths, json_pack("{s:i,s:o,s:o,s:s,s:s,s:b,s:b,s:s,s:b,s:o,s:o,s:I,s:f,s:o}",
                    "transport_id", (int)p->to.transport.value, "transport", pt ? json_string(pt->name) : json_null(),
                    "transport_type", pt ? json_string(pt->type) : json_null(), "addr", paddr,
                    "state", p->state == FIPS_PATH_Probing ? "probing" : p->state == FIPS_PATH_Live ? "live" : p->state == FIPS_PATH_Suspect ? "suspect" : "dead",
                    "active", p->active, "remote_active", p->remote, "role", "normal", "pinned", p->pinned,
                    "last_rtt_ms", p->has_rtt ? json_integer((json_int_t)(p->rtt.secs * 1000 + p->rtt.nanos / 1000000)) : json_null(),
                    "min_rtt_ms", p->has_least ? json_integer((json_int_t)(p->least.secs * 1000 + p->least.nanos / 1000000)) : json_null(),
                    "rtt_samples", (json_int_t)p->samples, "etx", p->etx, "score", p->has_score ? json_real(p->score) : json_null()));
            }
        }
        json_object_set_new(row, "paths", paths);
        json_array_append_new(peer_rows, row);
        json_array_append_new(link_rows, json_pack("{s:I,s:i,s:s,s:s,s:s,s:I,s:{s:I,s:I,s:I,s:I,s:I}}", "link_id", (json_int_t)info->link.value,
            "transport_id", (int)info->transport.value, "remote_addr", addr, "direction", info->direction == FIPS_LINK_OUTBOUND ? "outbound" : "inbound",
            "state", peer_state_name(info->state), "created_at_ms", (json_int_t)info->data.authenticated_at,
            "stats", "packets_sent", (json_int_t)info->data.link_stats.packets_sent, "packets_recv", (json_int_t)info->data.link_stats.packets_recv,
            "bytes_sent", (json_int_t)info->data.link_stats.bytes_sent, "bytes_recv", (json_int_t)info->data.link_stats.bytes_recv,
            "last_recv_ms", (json_int_t)info->data.link_stats.last_recv_ms));
        if (mmp) json_array_append_new(mmp_peers, json_pack("{s:s,s:s,s:s,s:o}", "peer", hex, "display_name", display,
            "mode", fips_mmp_mode_name(mmp->mode), "link_layer", layer_json(&mmp->metrics, false, mmp->spin_bit.is_initiator, mmp->receiver.ecn_ce_count, 0)));
        json_t *trow = json_pack("{s:s,s:s}", "node_addr", hex, "display_name", display);
        if (coords) {
            size_t distance = 0; (void)fips_tree_state_distance_to_peer(h->tree, rec->addr, &distance);
            char root[33]; (void)fips_node_addr_format(fips_tree_coordinate_root_id(coords), root, sizeof root);
            json_object_set_new(trow, "depth", json_integer((json_int_t)fips_tree_coordinate_depth(coords)));
            json_object_set_new(trow, "root", json_string(root)); json_object_set_new(trow, "coords", coords_json(coords));
            json_object_set_new(trow, "distance_to_us", json_integer((json_int_t)distance));
        }
        json_array_append_new(tree_rows, trow);
        fips_bloom_peer *bp = fips_bloom_state_peer(&h->bloom, &rec->addr);
        json_t *frow = json_pack("{s:s,s:s,s:b,s:I,s:s}", "peer", hex, "display_name", display, "has_filter", bp && bp->has_inbound,
            "filter_sequence", (json_int_t)(bp ? bp->inbound_sequence : 0), "tree_role", is_parent ? "parent" : is_child ? "child" : "none");
        if (bp && bp->has_inbound) {
            double est;
            json_object_set_new(frow, "estimated_count", fips_bloom_estimated_count(&bp->inbound, FIPS_BLOOM_ACCEPT_FPR_CAP, &est) ? json_real(est) : json_null());
            json_object_set_new(frow, "set_bits", json_integer((json_int_t)fips_bloom_count_ones(&bp->inbound)));
            json_object_set_new(frow, "fill_ratio", json_real(fips_bloom_fill_ratio(&bp->inbound)));
        }
        json_array_append_new(filter_rows, frow);
    }
    s->peer_count = s->link_count = peers; s->transport_count = h->registry ? h->registry->count : 0;
    s->transport_peer_counts = sjson(x, counts); s->peers = sjson(x, peer_rows); s->links = sjson(x, link_rows); s->mmp_peers = sjson(x, mmp_peers);
    s->peer_meta = meta; s->peer_meta_count = nmeta;
    /* Sessions */
    json_t *session_rows = json_array(), *mmp_sessions = json_array();
    for (size_t i = 0; i < h->cfg.session_capacity; ++i) {
        fips_session_entry *e = &h->session_entries[i]; session_ext *xt = &h->session_ext[i];
        if (!e->occupied || !xt->used) continue;
        fips_rekey_view v = fips_rekey_get_view(&e->machine);
        if (xt->established) ++established;
        const char *hex = hex_of(x, &xt->peer), *display = display_of(x, &xt->peer), *npub = npub_of(h, x, &xt->peer);
        json_t *row = json_pack("{s:s,s:s,s:s,s:b,s:I,s:s,s:{s:I,s:I,s:I,s:I}}", "remote_addr", hex, "display_name", display,
            /* queries.rs show_sessions vocabulary: a draining session is still
             * `established` (`is_draining` carries the drain); before that the
             * initiator is `initiating` and the responder `awaiting_msg3`. */
            "state", xt->established ? "established" : xt->is_initiator ? "initiating" : "awaiting_msg3", "is_initiator", xt->is_initiator,
            "last_activity_ms", (json_int_t)wall_of(h, e->last_activity), "identity", v.proven ? "proven" : "unproven",
            "stats", "packets_sent", (json_int_t)xt->sent, "packets_recv", (json_int_t)xt->received, "bytes_sent", (json_int_t)0, "bytes_recv", (json_int_t)0);
        if (npub) json_object_set_new(row, "npub", json_string(npub));
        if (!xt->established) json_object_set_new(row, "resend_count", json_integer((json_int_t)v.candidates));
        else {
            json_object_set_new(row, "session_start_ms", json_integer((json_int_t)wall_of(h, v.current.started)));
            json_object_set_new(row, "current_k_bit", json_boolean(v.current.k));
            json_object_set_new(row, "coords_warmup_remaining", json_integer(0));
            json_object_set_new(row, "is_draining", json_boolean(v.previous.present));
        }
        if (xt->mmp_live) {
            uint16_t mtu = xt->mmp.path_mtu.current_mtu;
            json_object_set_new(row, "mmp", mmp_json(&xt->mmp.roles.metrics, xt->mmp.roles.mode, true, &mtu));
            json_array_append_new(mmp_sessions, json_pack("{s:s,s:s,s:s,s:o}", "remote", hex, "display_name", display,
                "mode", fips_mmp_mode_name(xt->mmp.roles.mode), "session_layer", layer_json(&xt->mmp.roles.metrics, true, false, 0, mtu)));
        }
        json_array_append_new(session_rows, row);
    }
    s->session_count = established; s->sessions = sjson(x, session_rows); s->mmp_sessions = sjson(x, mmp_sessions);
    /* Pending handshakes */
    fipsd_control_connection *conns = salloc(x, (h->cfg.peer_capacity + 1) * sizeof *conns);
    for (size_t i = 0; conns && i < h->cfg.peer_capacity; ++i) {
        pending_leg *leg = &h->legs[i];
        if (!leg->used) continue;
        conns[legs++] = (fipsd_control_connection){.link_id = leg->link.value, .started_at_ms = wall_of(h, leg->started),
            .last_activity_ms = wall_of(h, leg->started), .direction = "outbound", .handshake_state = peer_state_name(leg->machine.state),
            .expected_peer = hex_of(x, &leg->expected.node_addr), .resend_count = leg->machine.conn.resend_count};
    }
    s->connections = conns; s->connections_count = legs; s->connection_count = legs;
    /* Transports */
    size_t tn = h->registry ? h->registry->count : 0;
    fipsd_control_transport *ts = salloc(x, (tn + 1) * sizeof *ts);
    fipsd_control_transport_view *views = salloc(x, (tn + 1) * sizeof *views);
    for (size_t i = 0; ts && views && i < tn; ++i) {
        fipsd_transport *t = h->registry->items[i];
        fipsd_control_transport_read(t, fipsd_transport_now_ms(), &views[i]);
        ts[i] = views[i].row;
        ts[i].type = sdup(x, ts[i].type); ts[i].name = sdup(x, ts[i].name);
        if (ts[i].local_addr) ts[i].local_addr = sdup(x, ts[i].local_addr);
        if (ts[i].onion_address) ts[i].onion_address = sdup(x, ts[i].onion_address);
        if (views[i].tor_monitoring) ts[i].tor_monitoring = sjson(x, views[i].tor_monitoring);
        if (ts[i].interface) views[i].interface.name = sdup(x, views[i].interface.name);
    }
    s->transports = ts; s->transports_count = ts && views ? tn : 0;
    /* Tree */
    const fips_parent_declaration *d = fips_tree_state_my_declaration(h->tree);
    s->tree.my_node_addr = s->node_addr; s->tree.root = hex_of(x, fips_tree_state_root(h->tree));
    s->tree.root_npub = npub_of(h, x, fips_tree_state_root(h->tree));
    s->tree.parent = hex_of(x, &d->parent_id); s->tree.parent_display_name = display_of(x, &d->parent_id);
    s->tree.is_root = fips_tree_state_is_root(h->tree); s->tree.declaration_signed = fips_tree_state_is_declaration_signed(h->tree);
    s->tree.depth = fips_tree_coordinate_depth(fips_tree_state_my_coords(h->tree)); s->tree.declaration_sequence = d->sequence;
    s->tree.peer_tree_count = fips_tree_state_peer_count(h->tree);
    s->tree.my_coords = sjson(x, coords_json(fips_tree_state_my_coords(h->tree))); s->tree.peers = sjson(x, tree_rows);
    /* Bloom */
    s->bloom.own_node_addr = s->node_addr; s->bloom.is_leaf_only = h->cfg.leaf_only; s->bloom.sequence = h->bloom.sequence;
    json_t *leaves = json_array();
    for (size_t i = 0; i < h->bloom.leaf_count; ++i) { char buf[33]; (void)fips_node_addr_format(&h->bloom.leaves[i], buf, sizeof buf); json_array_append_new(leaves, json_string(buf)); }
    s->bloom.leaf_dependents = sjson(x, leaves); s->bloom.peer_filters = sjson(x, filter_rows);
    /* Coordinate cache */
    fips_cache_stats cs = fips_coord_cache_stats(&h->cache, now);
    s->cache.count = cs.entries; s->cache.max_entries = cs.max_entries; s->cache.expired = cs.expired;
    s->cache.fill_ratio = fips_cache_stats_fill_ratio(&cs); s->cache.default_ttl_ms = fips_coord_cache_default_ttl_ms(&h->cache);
    s->cache.avg_age_ms = cs.avg_age_ms;
    size_t rows_cap = cs.entries < 256 ? cs.entries : 256;
    struct { handlers *h; snap *x; fipsd_control_cache_entry *rows; size_t n, cap; } cc = {h, x, salloc(x, (rows_cap + 1) * sizeof(fipsd_control_cache_entry)), 0, rows_cap};
    if (cc.rows) fips_coord_cache_iter(&h->cache, now, cache_row, &cc);
    s->cache.entries = cc.rows; s->cache.entry_count = cc.n;
    /* Identity cache */
    fipsd_control_identity *ids = salloc(x, (h->identities.count + 1) * sizeof *ids);
    for (size_t i = 0; ids && i < h->identities.count; ++i) {
        const fipsd_identity_entry *e = &h->identities.entries[i];
        char npub[FIPS_NIP19_CAPACITY] = "", ip[64];
        (void)fips_peer_identity_npub(&e->peer, npub, sizeof npub); (void)fips_ipv6_address_format(&e->peer.address, ip, sizeof ip);
        ids[i] = (fipsd_control_identity){.node_addr = hex_of(x, &e->peer.node_addr), .npub = sdup(x, npub),
            .display_name = display_of(x, &e->peer.node_addr), .ipv6_addr = sdup(x, ip), .last_seen_ms = e->used_ms};
    }
    s->identities = ids; s->identity_count = ids ? h->identities.count : 0; s->identity_max_entries = h->identities.capacity;
    /* Lookups */
    fipsd_control_lookup *lk = salloc(x, (h->lookup.pending_len + 1) * sizeof *lk);
    for (size_t i = 0; lk && i < h->lookup.pending_len; ++i) {
        const fips_pending_lookup *p = &h->lookup.pending_lookups[i];
        lk[i] = (fipsd_control_lookup){.target = hex_of(x, &p->target), .display_name = display_of(x, &p->target),
            .initiated_ms = h->wall_offset_ms + p->initiated_ms, .last_sent_ms = h->wall_offset_ms + p->last_sent_ms, .attempt = p->attempt};
    }
    s->lookups = lk; s->lookup_count = lk ? h->lookup.pending_len : 0;
    for (size_t i = 0; i < h->cfg.session_capacity; ++i) if (h->held[i].used) { ++s->pending_native_destinations; s->pending_native_datagrams += h->held[i].len; }
    s->recent_requests = h->lookup.recent_len;
    s->retries = sjson(x, json_array());

    /* Complete upstream counter families from the node registry. */
    hs_metrics_refresh(h);
    fipsd_control_counter buf[32]; size_t n;
    n = fipsd_metrics_snapshot(&h->metrics, FIPSD_METRICS_forwarding, buf);
    s->forwarding = counters_of(x, buf, n);
    n = fipsd_metrics_snapshot(&h->metrics, FIPSD_METRICS_lookup, buf);
    s->lookup = counters_of(x, buf, n);
    n = fipsd_metrics_snapshot(&h->metrics, FIPSD_METRICS_tree, buf);
    s->tree_stats = counters_of(x, buf, n);
    n = fipsd_metrics_snapshot(&h->metrics, FIPSD_METRICS_bloom, buf);
    s->bloom_stats = counters_of(x, buf, n);
    n = fipsd_metrics_snapshot(&h->metrics, FIPSD_METRICS_congestion, buf);
    s->congestion = counters_of(x, buf, n);
    n = fipsd_metrics_snapshot(&h->metrics, FIPSD_METRICS_errors, buf);
    s->errors = counters_of(x, buf, n);
    n = fipsd_metrics_snapshot(&h->metrics, FIPSD_METRICS_native, buf);
    s->native = counters_of(x, buf, n);
    n = fipsd_metrics_snapshot(&h->metrics, FIPSD_METRICS_dataplane, buf);
    s->dataplane = counters_of(x, buf, n);
    n = fipsd_metrics_snapshot(&h->metrics, FIPSD_METRICS_handshake, buf);
    s->handshake = counters_of(x, buf, n);
    n = fipsd_metrics_snapshot(&h->metrics, FIPSD_METRICS_session, buf);
    s->session = counters_of(x, buf, n);
    /* Stats history: copy the rings and re-point the peer storage. */
    fips_stats_history *hist = salloc_raw(x, sizeof *hist);
    fips_peer_stats *hp = h->history->peer_capacity ? salloc(x, h->history->peer_capacity * sizeof *hp) : NULL;
    if (hist && (hp || !h->history->peer_capacity)) {
        *hist = *h->history;
        if (hp) memcpy(hp, h->history->peers, h->history->peer_count * sizeof *hp);
        hist->peers = hp; s->history = hist;
    }
    return x;
}
#ifdef FIPSD_TEST_HOOKS
/* Test binaries alone link this provider copy. Set before runtime start and
 * clear after join; capacity-one pulses coalesce without blocking dispatch. */
GoChannel *fipsd_test_published;
#endif
void hs_publish(handlers *h, fips_instant now)
{
    if (!h->attached) return;
    FIPSD_PERF_BEGIN(snapshot_start);
    snap *x = build(h, now);
    FIPSD_PERF_END(snapshot_start, PUBLISH_SNAPSHOT);
    if (!x) return;
    if (!go_swap_store(h->snapshot, x)) { snap_free(x); return; }
    ++h->snapshot_generation; h->dirty = false;
#ifdef FIPSD_TEST_HOOKS
    if (fipsd_test_published) (void)go_channel_try_send(fipsd_test_published, h);
#endif
}
static GoRefPtr acquire(void *user) { handlers *h = user; return go_swap_load(h->snapshot); }
json_t *hs_control_enqueue(handlers *h, const char *name, const json_t *params, const fipsd_control_request_context *rc);
static json_t *command(void *user, const char *name, const json_t *params, const fipsd_control_request_context *rc)
{
    handlers *h = user;
    if (!h->attached) return NULL;
    return hs_control_enqueue(h, name, params, rc);
}
fipsd_control_provider fipsd_node_handlers_provider(fipsd_node_handlers *h)
{ return (fipsd_control_provider){.user = h, .acquire = acquire, .command = command}; }
bool hs_provider_init(handlers *h)
{
    h->snapshot = go_swap_create(NULL, snap_free);
    return h->snapshot != NULL;
}
static bool parse_peer(handlers *h, const json_t *params, const char *key, fips_peer_identity *out)
{
    const char *npub = json_string_value(json_object_get(params, key));
    return npub && fips_peer_identity_from_npub(h->secp, npub, strlen(npub), out) == FIPS_IDENTITY_OK;
}
/* `transport` as fipsctl sends it: an instance name, a type, or the numeric
 * id `path show` prints (as a string on the wire; an integer is taken too). */
static fipsd_transport *parse_transport(handlers *h, const json_t *params)
{
    json_t *v = json_object_get(params, "transport");
    if (!v) v = json_object_get(params, "transport_id");
    const char *text = json_is_string(v) ? json_string_value(v) : NULL;
    bool numeric = text && text[0] && strspn(text, "0123456789") == strlen(text) && strlen(text) <= 9;
    unsigned long id = numeric ? strtoul(text, NULL, 10) : 0;
    for (size_t i = 0; h->registry && i < h->registry->count; ++i) {
        fipsd_transport *t = h->registry->items[i];
        if (json_is_integer(v) && (json_int_t)t->id.value == json_integer_value(v)) return t;
        if (numeric && (unsigned long)t->id.value == id) return t;
        if (text && (!strcmp(t->name, text) || !strcmp(t->type, text))) return t;
    }
    return NULL;
}
/* Runs on the dispatcher: the mailbox command handler calls this. */
json_t *hs_control_command(handlers *h, const char *name, const json_t *params)
{
    fips_peer_identity peer;
    if (!strcmp(name, "connect")) {
        if (!parse_peer(h, params, "npub", &peer)) return fipsd_control_error("invalid peer npub");
        fipsd_transport *t = parse_transport(h, params);
        const char *addr = json_string_value(json_object_get(params, "addr"));
        if (!addr) addr = json_string_value(json_object_get(params, "address"));
        fips_transport_addr a;
        if (!t || !addr || fipsd_transport_parse_address(t, addr, &a) != FIPS_OK) return fipsd_control_error("unknown transport or address");
        (void)fipsd_identity_cache_put(&h->identities, peer, hs_wall_ms(h));
        bool ok = hs_dial(h, &peer, t->id, &a, false);
        return ok ? fipsd_control_ok(json_pack("{s:s}", "result", hs_rec_find(h, &peer.node_addr) ? "connected" : "connecting"))
                  : fipsd_control_error("connection refused by admission");
    }
    if (!strcmp(name, "disconnect")) {
        if (!parse_peer(h, params, "npub", &peer)) return fipsd_control_error("invalid peer npub");
        bool any = hs_disconnect(h, &peer.node_addr, FIPS_FMP_Shutdown, true);
        return any ? fipsd_control_ok(json_pack("{s:s}", "result", "disconnected")) : fipsd_control_error("peer not found");
    }
    if (!strcmp(name, "path_show")) {
        if (!parse_peer(h, params, "npub", &peer)) return fipsd_control_error("invalid peer npub");
        return hs_path_show(h, &peer.node_addr);
    }
    if (!strcmp(name, "path_pin") || !strcmp(name, "path_unpin")) {
        if (!parse_peer(h, params, "npub", &peer)) return fipsd_control_error("invalid peer npub");
        bool pin = !strcmp(name, "path_pin");
        fips_transport_id transport = {0};
        fips_transport_addr a = {0}; bool has = false;
        /* path_unpin carries only the peer (control-socket.md); the pin names
         * a transport and, to pin one path, that path's `address`. */
        if (pin) {
            fipsd_transport *t = parse_transport(h, params);
            if (!t) return fipsd_control_error("unknown transport");
            transport = t->id;
            const char *addr = json_string_value(json_object_get(params, "address"));
            if (!addr) addr = json_string_value(json_object_get(params, "addr"));
            has = addr && fipsd_transport_parse_address(t, addr, &a) == FIPS_OK;
            if (addr && !has) return fipsd_control_error("invalid address");
        }
        return hs_path_pin(h, &peer.node_addr, transport, &a, has, pin);
    }
    if (!strcmp(name, "probe_start")) {
        if (!parse_peer(h, params, "npub", &peer)) return fipsd_control_error("invalid peer npub");
        return hs_probe_start(h, &peer);
    }
    if (!strcmp(name, "probe_poll") || !strcmp(name, "probe_cancel")) {
        json_t *id = json_object_get(params, "probe_id");
        if (!id) id = json_object_get(params, "id");
        if (!json_is_integer(id)) return fipsd_control_error("missing probe id");
        return !strcmp(name, "probe_poll") ? hs_probe_poll(h, (uint64_t)json_integer_value(id)) : hs_probe_cancel(h, (uint64_t)json_integer_value(id));
    }
    return fipsd_control_error("unknown command");
}
