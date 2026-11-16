#include "control/protocol.h"
#include "control/render.h"
#include "control/counter_schema.h"
#include <fips/identity/peer.h>
#include "host_os.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N(a) (sizeof(a) / sizeof((a)[0]))
static const char zero_addr[] = "00000000000000000000000000000000";
static const char *str(const char *s) { return s ? s : ""; }
static const char *addr(const char *s) { return s ? s : zero_addr; }
static json_t *number(double v) { return isfinite(v) ? json_real(v) : json_null(); }
static json_t *optional(const double *v) { return v ? number(*v) : json_null(); }
static json_t *copy_array(const json_t *a) { return a ? json_deep_copy(a) : json_array(); }
static json_t *copy_object(const json_t *a) { return a ? json_deep_copy(a) : json_object(); }
static uint64_t age(uint64_t now, uint64_t then) { return now > then ? now - then : 0; }
static void set(json_t *o, const char *key, json_t *value) { fipsd_control_put(o, key, value); }
static void integer(json_t *o, const char *key, uint64_t v) { set(o, key, fipsd_control_uint(v)); }
static void string(json_t *o, const char *key, const char *s) { set(o, key, json_string(str(s))); }
static void boolean(json_t *o, const char *key, bool v) { set(o, key, json_boolean(v)); }
static json_t *counters(fipsd_control_counters c, const char *const *fields, size_t count)
{
    json_t *out = json_object();
    for (size_t i = 0; i < count; ++i) integer(out, fields[i], 0);
    for (size_t i = 0; i < c.count; ++i) integer(out, c.items[i].name, c.items[i].value);
    return out;
}
#define COUNTERS(s, member, schema) counters((s)->member, schema##_fields, N(schema##_fields))
static void sample(void *ctx, size_t index, bool is_null, double value)
{
    (void)index;
    fipsd_control_append(ctx, is_null ? json_null() : json_real(value));
}
json_t *fipsd_control_series(const fips_stats_series *series)
{
    fipsd_control_render_begin();
    json_t *values = json_array();
    fips_stats_series_walk(series, sample, values);
    return fipsd_control_render_end(json_pack("{s:s,s:s,s:o,s:o}", "metric", series->metric, "unit", series->unit,
        "granularity_seconds", fipsd_control_uint(series->granularity_seconds), "values", values));
}
static json_t *stats_list(void)
{
    json_t *a = json_array();
    for (size_t i = 0; i < FIPS_METRIC_COUNT + FIPS_PEER_METRIC_COUNT; ++i) {
        bool peer = i >= FIPS_METRIC_COUNT;
        const fips_metric_info *info = peer ? &fips_peer_metrics[i - FIPS_METRIC_COUNT] : &fips_metrics[i];
        fipsd_control_append(a, json_pack("{s:s,s:s,s:s}", "name", info->name, "unit", info->unit,
            "scope", peer ? "peer" : "node"));
    }
    return json_pack("{s:o,s:i,s:i,s:I}", "metrics", a, "fast_ring_seconds", FIPS_FAST_RING_CAPACITY,
        "slow_ring_minutes", FIPS_SLOW_RING_CAPACITY, "peer_retention_seconds", (json_int_t)FIPS_PEER_EVICTION_SECS);
}
static json_t *status(const fipsd_control_snapshot *s)
{
    if (!s->npub || !s->node_addr || !s->ipv6_addr)
        return fipsd_control_error("node not yet initialized");
    json_t *d = json_object();
    string(d, "version", s->version); string(d, "npub", s->npub);
    string(d, "node_addr", s->node_addr); string(d, "ipv6_addr", s->ipv6_addr);
    string(d, "state", s->state ? s->state : "created");
    string(d, "control_socket", s->control_socket); string(d, "exe_path", s->exe_path);
    boolean(d, "is_leaf_only", s->is_leaf_only); boolean(d, "persistent", s->persistent);
    boolean(d, "is_root", s->is_root); string(d, "root", addr(s->tree.root));
    integer(d, "pid", s->pid); integer(d, "uptime_secs", s->uptime_secs);
    integer(d, "peer_count", s->peer_count); integer(d, "session_count", s->session_count);
    integer(d, "link_count", s->link_count); integer(d, "transport_count", s->transport_count);
    integer(d, "connection_count", s->connection_count);
    set(d, "estimated_mesh_size", s->estimated_mesh_size ? fipsd_control_uint(*s->estimated_mesh_size) : json_null());
    set(d, "transport_peer_counts", copy_object(s->transport_peer_counts));
    set(d, "forwarding", COUNTERS(s, forwarding, forwarding));
    json_t *sparks = json_object();
    const fips_metric metrics[] = {FIPS_METRIC_MESH_SIZE, FIPS_METRIC_TREE_DEPTH, FIPS_METRIC_PEER_COUNT,
        FIPS_METRIC_BYTES_IN, FIPS_METRIC_BYTES_OUT, FIPS_METRIC_LOSS_RATE};
    for (size_t i = 0; i < N(metrics); ++i) {
        json_t *a = json_array();
        double buffer[30]; size_t count = 0;
        if (s->history) (void)fips_stats_history_recent(s->history, metrics[i], 30, buffer, N(buffer), &count);
        fips_stats_series series = {.values=buffer, .len=count};
        fips_stats_series_walk(&series, sample, a);
        set(sparks, fips_metrics[metrics[i]].name, a);
    }
    set(d, "sparklines", sparks);
    return fipsd_control_ok(d);
}
static json_t *tree(const fipsd_control_snapshot *s)
{
    json_t *d = json_object();
    string(d, "my_node_addr", addr(s->tree.my_node_addr)); string(d, "root", addr(s->tree.root));
    set(d, "root_npub", s->tree.root_npub ? json_string(s->tree.root_npub) : json_null());
    boolean(d, "is_root", s->tree.is_root); integer(d, "depth", s->tree.depth);
    set(d, "my_coords", copy_array(s->tree.my_coords)); string(d, "parent", addr(s->tree.parent));
    string(d, "parent_display_name", s->tree.parent_display_name);
    integer(d, "declaration_sequence", s->tree.declaration_sequence);
    boolean(d, "declaration_signed", s->tree.declaration_signed);
    integer(d, "peer_tree_count", s->tree.peer_tree_count); set(d, "peers", copy_array(s->tree.peers));
    set(d, "stats", COUNTERS(s, tree_stats, tree));
    return d;
}
static json_t *bloom(const fipsd_control_snapshot *s)
{
    json_t *d = json_object();
    string(d, "own_node_addr", addr(s->bloom.own_node_addr)); boolean(d, "is_leaf_only", s->bloom.is_leaf_only);
    integer(d, "sequence", s->bloom.sequence);
    integer(d, "leaf_dependent_count", json_array_size(s->bloom.leaf_dependents));
    set(d, "leaf_dependents", copy_array(s->bloom.leaf_dependents));
    set(d, "peer_filters", copy_array(s->bloom.peer_filters));
    set(d, "uptree_fill_ratio", optional(s->bloom.uptree_fill_ratio));
    set(d, "uptree_estimated_count", optional(s->bloom.uptree_estimated_count));
    set(d, "stats", COUNTERS(s, bloom_stats, bloom));
    return d;
}
static json_t *cache(const fipsd_control_snapshot *s)
{
    json_t *a = json_array();
    for (size_t i = 0; i < s->cache.entry_count; ++i) {
        const fipsd_control_cache_entry *r = &s->cache.entries[i];
        json_t *o = json_object();
        string(o, "node_addr", r->node_addr); string(o, "display_name", r->display_name);
        integer(o, "depth", r->depth); set(o, "coords", copy_array(r->coord_path));
        integer(o, "age_ms", age(s->now_ms, r->created_at)); integer(o, "last_used_ms", r->last_used_ms);
        if (r->path_mtu) integer(o, "path_mtu", *r->path_mtu);
        fipsd_control_append(a, o);
    }
    json_t *d = json_object();
    integer(d, "count", s->cache.count); integer(d, "max_entries", s->cache.max_entries);
    set(d, "fill_ratio", number(s->cache.fill_ratio)); integer(d, "default_ttl_ms", s->cache.default_ttl_ms);
    integer(d, "expired", s->cache.expired); integer(d, "avg_age_ms", s->cache.avg_age_ms); set(d, "entries", a);
    return d;
}
static json_t *identity_cache(const fipsd_control_snapshot *s)
{
    json_t *a = json_array();
    for (size_t i = 0; i < s->identity_count; ++i) {
        const fipsd_control_identity *r = &s->identities[i];
        json_t *o = json_object();
        string(o, "node_addr", r->node_addr); string(o, "npub", r->npub);
        string(o, "display_name", r->display_name); string(o, "ipv6_addr", r->ipv6_addr);
        integer(o, "last_seen_ms", r->last_seen_ms); integer(o, "age_ms", age(s->now_ms, r->last_seen_ms));
        fipsd_control_append(a, o);
    }
    return json_pack("{s:o,s:I,s:I}", "entries", a, "count", (json_int_t)s->identity_count,
        "max_entries", (json_int_t)s->identity_max_entries);
}
static json_t *routing(const fipsd_control_snapshot *s)
{
    json_t *a = json_array();
    for (size_t i = 0; i < s->lookup_count; ++i) {
        const fipsd_control_lookup *r = &s->lookups[i];
        json_t *o = json_object();
        string(o, "target", r->target); string(o, "display_name", r->display_name);
        integer(o, "age_ms", age(s->now_ms, r->initiated_ms)); integer(o, "last_sent_ms", r->last_sent_ms);
        integer(o, "attempt", r->attempt); fipsd_control_append(a, o);
    }
    json_t *d = json_object();
    integer(d, "coord_cache_entries", s->cache.count); integer(d, "identity_cache_entries", s->identity_count);
    integer(d, "pending_native_destinations", s->pending_native_destinations);
    integer(d, "pending_native_datagrams", s->pending_native_datagrams);
    integer(d, "pending_tun_destinations", s->pending_tun_destinations); integer(d, "pending_tun_packets", s->pending_tun_packets);
    integer(d, "recent_requests", s->recent_requests); set(d, "pending_lookups", a); set(d, "retries", copy_array(s->retries));
    set(d, "forwarding", COUNTERS(s, forwarding, forwarding));
    set(d, "discovery", COUNTERS(s, lookup, lookup)); set(d, "lookup", COUNTERS(s, lookup, lookup));
    set(d, "error_signals", COUNTERS(s, errors, errors)); set(d, "congestion", COUNTERS(s, congestion, congestion));
    return d;
}
static json_t *transports(const fipsd_control_snapshot *s)
{
    json_t *a = json_array();
    for (size_t i = 0; i < s->transports_count; ++i) {
        const fipsd_control_transport *r = &s->transports[i];
        json_t *o = json_object();
        integer(o, "transport_id", r->transport_id); string(o, "type", r->type);
        string(o, "state", r->state); integer(o, "mtu", r->mtu);
        if (r->name) string(o, "name", r->name);
        if (r->local_addr) string(o, "local_addr", r->local_addr);
        if (r->tor_mode) string(o, "tor_mode", r->tor_mode);
        if (r->onion_address) string(o, "onion_address", r->onion_address);
        if (r->tor_monitoring) set(o, "tor_monitoring", json_deep_copy(r->tor_monitoring));
        set(o, "stats", counters(r->stats, transport_fields, N(transport_fields)));
        if (r->interface) {
            const fipsd_control_interface *f = r->interface;
            json_t *in = json_object();
            string(in, "name", f->name); string(in, "presence", f->presence); string(in, "policy", f->policy);
            if (f->medium) string(in, "medium", f->medium);
            boolean(in, "carrier", f->carrier); integer(in, "since_secs", f->since_secs);
            integer(in, "binds", f->binds); integer(in, "failed_attempts", f->failed_attempts); set(o, "interface", in);
        }
        fipsd_control_append(a, o);
    }
    return json_pack("{s:o}", "transports", a);
}
static json_t *connections(const fipsd_control_snapshot *s)
{
    json_t *a = json_array();
    for (size_t i = 0; i < s->connections_count; ++i) {
        const fipsd_control_connection *r = &s->connections[i];
        json_t *o = json_object();
        integer(o, "link_id", r->link_id); string(o, "direction", r->direction);
        string(o, "handshake_state", r->handshake_state); integer(o, "started_at_ms", r->started_at_ms);
        integer(o, "idle_ms", age(s->now_ms, r->last_activity_ms)); integer(o, "resend_count", r->resend_count);
        if (r->expected_peer) string(o, "expected_peer", r->expected_peer);
        fipsd_control_append(a, o);
    }
    return json_pack("{s:o}", "connections", a);
}
static json_t *native_flows(const fipsd_control_snapshot *s)
{
    json_t *a = json_array(), *listeners = json_array();
    for (size_t i = 0; i < s->flow_count; ++i) {
        const fipsd_control_native_flow *r = &s->flows[i];
        json_t *o = json_object();
        integer(o, "flow_id", r->flow_id); string(o, "peer", r->peer); string(o, "peer_addr", r->peer_addr);
        integer(o, "local_port", r->local_port); integer(o, "remote_port", r->remote_port);
        string(o, "state", r->established ? "established" : "pending_accept");
        integer(o, "queued", r->queued); integer(o, "age_ms", age(s->now_ms, r->since_ms));
        fipsd_control_append(a, o);
    }
    for (size_t i = 0; i < s->listener_count; ++i)
        fipsd_control_append(listeners, json_pack("{s:i,s:I}", "local_port", s->listeners[i].local_port,
            "backlog", (json_int_t)s->listeners[i].backlog));
    return json_pack("{s:o,s:o,s:o}", "flows", a, "listeners", listeners, "stats", COUNTERS(s, native, native));
}
static json_t *acl(const fipsd_control_snapshot *s)
{
    json_t *d = json_object();
    string(d, "allow_file", s->acl.allow_file); string(d, "deny_file", s->acl.deny_file);
    string(d, "effective_mode", s->acl.effective_mode); string(d, "default_decision", s->acl.default_decision);
    boolean(d, "enforcement_active", s->acl.enforcement_active); boolean(d, "allow_all", s->acl.allow_all);
    boolean(d, "deny_all", s->acl.deny_all); set(d, "allow_entries", copy_array(s->acl.allow_entries));
    set(d, "deny_entries", copy_array(s->acl.deny_entries));
    set(d, "allow_file_entries", copy_array(s->acl.allow_file_entries)); set(d, "deny_file_entries", copy_array(s->acl.deny_file_entries));
    return d;
}
static json_t *metrics(const fipsd_control_snapshot *s)
{
    json_t *d = json_object();
    set(d, "forwarding", COUNTERS(s, forwarding, forwarding)); set(d, "lookup", COUNTERS(s, lookup, lookup));
    set(d, "discovery", COUNTERS(s, lookup, lookup)); set(d, "tree", COUNTERS(s, tree_stats, tree));
    set(d, "bloom", COUNTERS(s, bloom_stats, bloom)); set(d, "congestion", COUNTERS(s, congestion, congestion));
    set(d, "errors", COUNTERS(s, errors, errors)); set(d, "native", COUNTERS(s, native, native));
    /* These families arrive with the node owner. Absence must not claim a
     * complete registry, so the query refuses until it is supplied. */
    set(d, "dataplane", counters(s->dataplane, NULL, 0));
    set(d, "handshake", counters(s->handshake, NULL, 0)); set(d, "session", counters(s->session, NULL, 0));
    return d;
}
static const char *param(const json_t *p, const char *key, const char *fallback)
{
    const char *v = json_string_value(json_object_get(p, key));
    return v ? v : fallback;
}
static json_t *errorf(const char *prefix, const char *value)
{
    size_t n = strlen(prefix) + strlen(value) + 1;
    char *msg = malloc(n);
    if (!msg) return NULL;
    snprintf(msg, n, "%s%s", prefix, value);
    json_t *e = fipsd_control_error(msg); free(msg); return e;
}
static json_t *window_params(const json_t *p, fips_duration *window, fips_granularity *g)
{
    const char *v = param(p, "window", "10m");
    size_t len = strlen(v);
    if (!len) return fipsd_control_error("empty duration");
    uint64_t value = 0;
    size_t start = v[0] == '+' ? 1 : 0;
    if (start == len - 1) return errorf("invalid duration: ", v);
    for (size_t i = start; i + 1 < len; ++i) {
        if (v[i] < '0' || v[i] > '9' || value > (UINT64_MAX - (unsigned)(v[i] - '0')) / 10)
            return errorf("invalid duration: ", v);
        value = value * 10 + (unsigned)(v[i] - '0');
    }
    unsigned scale = 1;
    switch (v[len - 1]) {
    case 's': break;
    case 'm': scale = 60; break;
    case 'h': scale = 3600; break;
    default: {
        char message[128];
        snprintf(message, sizeof(message), "unknown duration unit: %c (expected s, m, h)", v[len - 1]);
        return fipsd_control_error(message);
    }
    }
    if (value > INT64_MAX / scale) return errorf("invalid duration: ", v);
    *window = (fips_duration){.secs=value * scale};
    v = param(p, "granularity", "1s");
    if (fips_granularity_parse(v, g) != FIPS_OK) return errorf("unknown granularity: ", v);
    return NULL;
}
static void hex_addr(const uint8_t *addr_bytes, char out[33])
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < 16; ++i) {
        out[2 * i] = digits[addr_bytes[i] >> 4]; out[2 * i + 1] = digits[addr_bytes[i] & 15];
    }
    out[32] = 0;
}
static const fipsd_control_peer_meta *meta(const fipsd_control_snapshot *s, const uint8_t *bytes)
{
    for (size_t i = 0; i < s->peer_meta_count; ++i)
        if (!memcmp(s->peer_meta[i].node_addr, bytes, 16)) return &s->peer_meta[i];
    return NULL;
}
static json_t *peer_addr(const char *npub, uint8_t out[16])
{
    fips_secp *secp = NULL;
    if (fips_identity_init_secp(fipsd_host_os_entropy(), &secp) != FIPS_IDENTITY_OK)
        return fipsd_control_error("identity context unavailable");
    fips_peer_identity peer;
    fips_identity_status rc = fips_peer_identity_from_npub(secp, npub, strlen(npub), &peer);
    fips_identity_destroy_secp(secp);
    if (rc != FIPS_IDENTITY_OK) return errorf("invalid peer npub: ", fips_identity_error_name(rc));
    memcpy(out, peer.node_addr.bytes, 16);
    return NULL;
}
static json_t *not_tracked(const fipsd_control_snapshot *s, const uint8_t *bytes)
{
    char hex[33]; hex_addr(bytes, hex);
    const fipsd_control_peer_meta *m = meta(s, bytes);
    if (m && m->display_name) return errorf("peer not tracked in stats history: ", m->display_name);
    hex[8] = '.'; hex[9] = '.'; hex[10] = '.'; hex[11] = 0;
    return errorf("peer not tracked in stats history: ", hex);
}
static json_t *series_query(const fips_stats_history *h, const uint8_t *peer, int metric,
                            fips_duration window, fips_granularity g)
{
    size_t cap = g == FIPS_GRANULARITY_FAST ? FIPS_FAST_RING_CAPACITY : FIPS_SLOW_RING_CAPACITY;
    double *buffer = malloc(cap * sizeof(*buffer));
    if (!buffer) return NULL;
    fips_stats_series series;
    fips_status rc = peer ? fips_stats_history_peer_query(h, peer, (fips_peer_metric)metric, window, g, buffer, cap, &series)
                          : fips_stats_history_query(h, (fips_metric)metric, window, g, buffer, cap, &series);
    json_t *out = rc == FIPS_OK ? fipsd_control_series(&series) : NULL;
    free(buffer); return out;
}
static int peer_order(const void *a, const void *b)
{
    const json_t *aa = *(json_t *const *)a, *bb = *(json_t *const *)b;
    bool active_a = json_is_true(json_object_get(aa, "is_active"));
    bool active_b = json_is_true(json_object_get(bb, "is_active"));
    if (active_a != active_b) return active_a ? -1 : 1;
    return strcmp(param(aa, "display_name", ""), param(bb, "display_name", ""));
}
static void sort_peers(json_t *a)
{
    size_t n = json_array_size(a);
    if (n < 2) return;
    json_t **rows = malloc(n * sizeof(*rows));
    if (!rows) { fipsd_control_render_check(-1); return; }
    for (size_t i = 0; i < n; ++i) rows[i] = json_incref(json_array_get(a, i));
    qsort(rows, n, sizeof(*rows), peer_order);
    for (size_t i = 0; i < n; ++i) fipsd_control_render_check(json_array_set_new(a, i, rows[i]));
    free(rows);
}
static json_t *history_peers(const fipsd_control_snapshot *s, const fips_stats_history *h,
                             bool with_values, fips_peer_metric metric, fips_duration window, fips_granularity g)
{
    json_t *a = json_array();
    for (size_t i = 0; i < h->peer_count; ++i) {
        const fips_peer_stats *r = &h->peers[i];
        const fipsd_control_peer_meta *m = meta(s, r->addr);
        char hex[33], short_hex[12]; hex_addr(r->addr, hex);
        snprintf(short_hex, sizeof(short_hex), "%.8s...", hex);
        json_t *o = json_object();
        string(o, "node_addr", hex); string(o, "display_name", m ? m->display_name : short_hex);
        boolean(o, "is_active", m && m->is_active);
        if (with_values) {
            json_t *series = series_query(h, r->addr, metric, window, g);
            if (!series) { json_decref(o); json_decref(a); return NULL; }
            fipsd_control_render_check(json_object_set(o, "values", json_object_get(series, "values"))); json_decref(series);
        } else {
            string(o, "npub", m ? m->npub : hex);
            uint64_t first = 0, last = 0;
            if (s->now.ops) {
                first = s->now.ops->saturating_duration_since(s->now, r->first_seen).secs;
                last = s->now.ops->saturating_duration_since(s->now, r->last_contact).secs;
            }
            integer(o, "first_seen_secs_ago", first); integer(o, "last_contact_secs_ago", last);
        }
        fipsd_control_append(a, o);
    }
    sort_peers(a);
    return a;
}
static json_t *history(const fipsd_control_snapshot *s, const char *name, const json_t *p)
{
    bool all = !strcmp(name, "show_stats_all_history");
    bool across = !strcmp(name, "show_stats_history_all_peers");
    bool peers = !strcmp(name, "show_stats_peers");
    const char *metric_name = param(p, "metric", NULL);
    if (!all && !peers && !p) return errorf("missing params for ", name);
    if (!all && !peers && !metric_name) return fipsd_control_error("missing 'metric' parameter");
    fips_duration window = {0}; fips_granularity g = FIPS_GRANULARITY_FAST;
    json_t *error = peers ? NULL : window_params(p, &window, &g);
    if (error) return error;
    const char *npub = across || peers ? NULL : param(p, "peer", NULL);
    uint8_t bytes[16];
    if (npub) { error = peer_addr(npub, bytes); if (error) return error; }
    int metric = 0;
    if (!all && !peers) {
        if (npub || across) {
            fips_peer_metric pm;
            if (fips_peer_metric_parse(metric_name, &pm) != FIPS_OK) return errorf("unknown per-peer metric: ", metric_name);
            metric = pm;
        } else {
            fips_metric nm;
            if (fips_metric_parse(metric_name, &nm) != FIPS_OK) return errorf("unknown metric: ", metric_name);
            metric = nm;
        }
    }
    /* The empty constructor is large: allocate off the small fiber stack. */
    fips_stats_history *empty = NULL;
    const fips_stats_history *h = s->history;
    if (!h) {
        empty = malloc(sizeof(*empty));
        if (!empty) return NULL;
        fips_stats_history_init(empty, NULL, 0); h = empty;
    }
    json_t *d = NULL;
    if (npub && !fips_stats_history_peer(h, bytes)) {
        free(empty); return not_tracked(s, bytes);
    }
    if (peers) {
        json_t *a = history_peers(s, h, false, 0, window, g);
        d = json_pack("{s:I,s:o}", "count", (json_int_t)h->peer_count, "peers", a);
    } else if (across) {
        json_t *a = history_peers(s, h, true, (fips_peer_metric)metric, window, g);
        d = json_pack("{s:s,s:s,s:I,s:I,s:o}", "metric", fips_peer_metrics[metric].name,
            "unit", fips_peer_metrics[metric].unit, "granularity_seconds", (json_int_t)(g == FIPS_GRANULARITY_FAST ? 1 : 60),
            "window_seconds", (json_int_t)window.secs, "peers", a);
    } else if (all) {
        json_t *a = json_array();
        int count = npub ? FIPS_PEER_METRIC_COUNT : FIPS_METRIC_COUNT;
        for (int i = 0; i < count; ++i) fipsd_control_append(a, series_query(h, npub ? bytes : NULL, i, window, g));
        d = json_pack("{s:I,s:I,s:o,s:o}", "granularity_seconds", (json_int_t)(g == FIPS_GRANULARITY_FAST ? 1 : 60),
            "window_seconds", (json_int_t)window.secs, "peer", npub ? json_string(npub) : json_null(), "series", a);
    } else d = series_query(h, npub ? bytes : NULL, metric, window, g);
    free(empty); return fipsd_control_ok(d);
}
static json_t *query(const fipsd_control_snapshot *s, const char *name, const json_t *params)
{
    if (!strcmp(name, "show_status")) return status(s);
    if (!strncmp(name, "show_stats_", 11) && strcmp(name, "show_stats_list")) return history(s, name, params);
    json_t *d = NULL;
    if (!strcmp(name, "show_stats_list")) d = stats_list();
    else if (!strcmp(name, "show_tree")) d = tree(s);
    else if (!strcmp(name, "show_bloom")) d = bloom(s);
    else if (!strcmp(name, "show_acl")) d = acl(s);
    else if (!strcmp(name, "show_cache")) d = cache(s);
    else if (!strcmp(name, "show_routing")) d = routing(s);
    else if (!strcmp(name, "show_identity_cache")) d = identity_cache(s);
    else if (!strcmp(name, "show_native_flows")) d = native_flows(s);
    else if (!strcmp(name, "show_transports")) d = transports(s);
    else if (!strcmp(name, "show_connections")) d = connections(s);
    else if (!strcmp(name, "show_metrics")) {
        if (!s->dataplane.count || !s->handshake.count || !s->session.count)
            return fipsd_control_error("subsystem not started");
        d = metrics(s);
    } else if (!strcmp(name, "show_peers")) d = json_pack("{s:o}", "peers", copy_array(s->peers));
    else if (!strcmp(name, "show_sessions")) d = json_pack("{s:o}", "sessions", copy_array(s->sessions));
    else if (!strcmp(name, "show_links")) d = json_pack("{s:o}", "links", copy_array(s->links));
    else if (!strcmp(name, "show_mmp")) d = json_pack("{s:o,s:o}", "peers", copy_array(s->mmp_peers), "sessions", copy_array(s->mmp_sessions));
    return fipsd_control_ok(d);
}

json_t *fipsd_control_query(const fipsd_control_snapshot *s, const char *name, const json_t *params)
{
    fipsd_control_render_begin();
    return fipsd_control_render_end(query(s, name, params));
}
