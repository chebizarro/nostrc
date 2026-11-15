#include "control/probe.h"
#include "control/render.h"
#include <math.h>
static void put(json_t *o, const char *k, json_t *v) { fipsd_control_put(o, k, v); }
static json_t *text(const char *v) { return v ? json_string(v) : json_null(); }
static json_t *num(uint64_t v) { return fipsd_control_uint(v); }
static json_t *stage(const fips_probe_stage_record *r)
{
    return json_pack("{s:s,s:o,s:o,s:o}", "verdict", fips_probe_stage_verdict_name(r->verdict),
        "reason", r->has_reason ? text(fips_probe_fail_kind_name(r->reason)) : json_null(),
        "detail", r->has_detail ? text(r->detail) : json_null(), "elapsed_ms", r->has_elapsed_ms ? num(r->elapsed_ms) : json_null());
}
json_t *fipsd_control_probe_render(const fipsd_control_probe_report *r)
{
    fipsd_control_render_begin();
    const fips_probe_snapshot *s = r->snapshot;
    json_t *out = json_object();
    put(out, "probe_id", num(r->probe_id)); put(out, "overall", text(fips_probe_overall_name(s->overall)));
    put(out, "elapsed_ms", num(r->elapsed_ms)); put(out, "tick_ms", num(s->tick_ms));
    put(out, "target", json_pack("{s:s,s:s,s:s,s:s}", "npub", r->npub, "node_addr", r->node_addr,
        "display_name", r->display_name, "ipv6_addr", r->ipv6_addr));
    json_t *b = stage(&s->bloom);
    put(b, "fanout", s->has_lookup_fanout ? num(s->lookup_fanout) : json_null()); put(out, "bloom", b);
    json_t *d = stage(&s->discovery), *timeouts = json_array();
    put(d, "source", s->has_resolve_source ? text(fips_probe_resolve_source_name(s->resolve_source)) : json_null());
    put(d, "attempts", r->attempts ? num(*r->attempts) : json_null());
    for (size_t i = 0; i < r->timeout_count; ++i) fipsd_control_append(timeouts, num(r->attempt_timeouts_secs[i]));
    put(d, "attempt_timeouts_secs", timeouts); put(out, "discovery", d);
    json_t *p = stage(&s->path);
    put(p, "computed_locally", json_true()); put(p, "observed", json_false());
    put(p, "coords_known", json_boolean(r->path != NULL));
    const fipsd_control_probe_path *path = r->path;
    put(p, "our_coords", path && path->our_coords ? json_deep_copy(path->our_coords) : json_array());
    put(p, "their_coords", path && path->their_coords ? json_deep_copy(path->their_coords) : json_array());
    put(p, "our_depth", path ? num(path->our_depth) : json_null());
    put(p, "their_depth", path ? num(path->their_depth) : json_null());
    put(p, "same_root", path ? json_boolean(path->same_root) : json_null());
    put(p, "lca", path ? text(path->lca) : json_null());
    put(p, "lca_depth", path && path->lca ? num(path->lca_depth) : json_null());
    put(p, "tree_hops_up", path && path->lca ? num(path->hops_up) : json_null());
    put(p, "tree_hops_down", path && path->lca ? num(path->hops_down) : json_null());
    put(p, "tree_distance", path && path->lca ? num(path->distance) : json_null());
    json_t *hop = json_null();
    if (path && path->next_hop_addr) hop = json_pack("{s:s,s:s,s:s,s:b,s:b}", "node_addr", path->next_hop_addr,
        "display_name", path->next_hop_display_name, "class", path->next_hop_class,
        "direct_peer", path->direct_peer, "leaves_tree_walk", path->leaves_tree_walk);
    put(p, "next_hop", hop);
    put(p, "no_hop_reason", s->has_no_hop_reason ? text(fips_probe_no_hop_reason_name(s->no_hop_reason)) : json_null());
    put(out, "path", p);
    json_t *session = stage(&s->session);
    put(session, "preexisting", json_boolean(s->session_preexisting)); put(session, "established", json_boolean(s->session_established));
    if (s->has_path_mtu) put(session, "path_mtu", num(s->path_mtu));
    put(out, "session", session);
    json_t *rtt = stage(&s->rtt);
    if (s->has_rtt_ms) put(rtt, "rtt_ms", num(s->rtt_ms));
    if (s->has_srtt_ms) put(rtt, "srtt_ms", isfinite(s->srtt_ms) ? json_real(s->srtt_ms) : json_null());
    put(rtt, "reports_seen", num(s->counters.reports_seen)); put(rtt, "samples", num(s->counters.samples));
    put(rtt, "zero_samples", num(s->counters.zero)); put(rtt, "arith_failures", num(s->counters.arith_fail));
    put(out, "rtt", rtt);
    put(out, "cleanup", json_pack("{s:b,s:b,s:o,s:b,s:b,s:b,s:i}", "session_created_and_torn_down", s->torn_down,
        "session_left_intact", s->has_left_intact, "left_intact_reason", s->has_left_intact ? text(fips_probe_left_intact_name(s->left_intact)) : json_null(),
        "lookup_issued", s->lookup_issued, "coords_were_cached", s->coords_were_cached,
        "identity_was_cached", s->identity_was_cached, "warmups_sent", s->warmups_sent));
    return fipsd_control_render_end(out);
}
