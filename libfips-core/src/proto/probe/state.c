#include <fips/proto/probe/state.h>

const char *fips_probe_stage_verdict_name(fips_probe_stage_verdict v)
{
    switch (v) {
    case FIPS_PROBE_VERDICT_Pending: return "pending";
    case FIPS_PROBE_VERDICT_Running: return "running";
    case FIPS_PROBE_VERDICT_Ok: return "ok";
    case FIPS_PROBE_VERDICT_Skipped: return "skipped";
    case FIPS_PROBE_VERDICT_Failed: return "failed";
    }
    return "unknown";
}

const char *fips_probe_fail_kind_name(fips_probe_fail_kind v)
{
    switch (v) {
    case FIPS_PROBE_Cached: return "cached";
    case FIPS_PROBE_DirectPeer: return "direct_peer";
    case FIPS_PROBE_BloomMiss: return "bloom_miss";
    case FIPS_PROBE_BackoffSuppressed: return "backoff_suppressed";
    case FIPS_PROBE_AlreadyPending: return "already_pending";
    case FIPS_PROBE_NoTreePeers: return "no_tree_peers";
    case FIPS_PROBE_NoResponse: return "no_response";
    case FIPS_PROBE_BloomUnconfirmed: return "bloom_unconfirmed";
    case FIPS_PROBE_DisjointTrees: return "disjoint_trees";
    case FIPS_PROBE_NoNextHop: return "no_next_hop";
    case FIPS_PROBE_Preexisting: return "preexisting";
    case FIPS_PROBE_SendError: return "send_error";
    case FIPS_PROBE_HandshakeTimeout: return "handshake_timeout";
    case FIPS_PROBE_NoReport: return "no_report";
    case FIPS_PROBE_NoEcho: return "no_echo";
    case FIPS_PROBE_SubMillisecond: return "sub_millisecond";
    case FIPS_PROBE_BadTimestampEcho: return "bad_timestamp_echo";
    case FIPS_PROBE_NotReached: return "not_reached";
    }
    return "unknown";
}

const char *fips_probe_resolve_source_name(fips_probe_resolve_source v)
{
    switch (v) {
    case FIPS_PROBE_RESOLVE_Cache: return "cache";
    case FIPS_PROBE_RESOLVE_Lookup: return "lookup";
    case FIPS_PROBE_RESOLVE_DirectPeer: return "direct_peer";
    }
    return "unknown";
}

const char *fips_probe_overall_name(fips_probe_overall v)
{
    switch (v) {
    case FIPS_PROBE_OVERALL_Running: return "running";
    case FIPS_PROBE_OVERALL_Ok: return "ok";
    case FIPS_PROBE_OVERALL_Partial: return "partial";
    case FIPS_PROBE_OVERALL_Failed: return "failed";
    case FIPS_PROBE_OVERALL_Cancelled: return "cancelled";
    }
    return "unknown";
}

const char *fips_probe_no_hop_reason_name(fips_probe_no_hop_reason v)
{
    switch (v) {
    case FIPS_PROBE_HOP_Local: return "local";
    case FIPS_PROBE_HOP_NoCoords: return "no_coords";
    case FIPS_PROBE_HOP_DisjointTrees: return "disjoint_trees";
    case FIPS_PROBE_HOP_NoCloserPeer: return "no_closer_peer";
    case FIPS_PROBE_HOP_HopNotSendReady: return "hop_not_send_ready";
    }
    return "unknown";
}

const char *fips_probe_left_intact_name(fips_probe_left_intact v)
{
    switch (v) {
    case FIPS_PROBE_LEFT_Preexisting: return "preexisting";
    case FIPS_PROBE_LEFT_AdoptedByTraffic: return "adopted_by_traffic";
    case FIPS_PROBE_LEFT_Replaced: return "replaced";
    }
    return "unknown";
}

fips_probe_stage_record fips_probe_stage_record_pending(void)
{
    return (fips_probe_stage_record){.verdict = FIPS_PROBE_VERDICT_Pending};
}
bool fips_probe_stage_record_is_failed(const fips_probe_stage_record *r)
{
    return r->verdict == FIPS_PROBE_VERDICT_Failed;
}
static uint64_t sub(uint64_t a, uint64_t b) { return a > b ? a - b : 0; }
fips_probe_rtt_counters fips_probe_rtt_counters_delta(
    fips_probe_rtt_counters v, fips_probe_rtt_counters b)
{
    return (fips_probe_rtt_counters){sub(v.reports_seen, b.reports_seen),
        sub(v.samples, b.samples), sub(v.zero, b.zero), sub(v.arith_fail, b.arith_fail)};
}
