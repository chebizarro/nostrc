#ifndef FIPS_PROBE_CORE_H
#define FIPS_PROBE_CORE_H
#include <fips/proto/probe/state.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct fips_probe_budgets {
    uint64_t bloom_ms, discovery_ms, session_ms, rtt_ms, tick_ms;
} fips_probe_budgets;
typedef struct fips_probe_observation {
    uint64_t now_ms;
    bool coords_cached, lookup_pending, has_lookup_outcome;
    fips_probe_lookup_outcome_kind lookup_outcome;
    bool has_lookup_fanout;
    size_t lookup_fanout;
    const fips_probe_path_facts *path; /* NULL = None; copied on step */
    const fips_probe_next_hop_facts *next_hop;
    bool has_no_hop_reason;
    fips_probe_no_hop_reason no_hop_reason;
    bool session_present, session_established, session_is_ours;
    const char *session_error; /* NULL = None; counted bytes, copied on failure */
    size_t session_error_len; /* <= DETAIL_CAPACITY - 1, no embedded NUL */
    bool target_is_direct_peer;
    fips_probe_rtt_counters counters;
    bool has_last_rtt_ms, has_srtt_ms, has_path_mtu;
    uint32_t last_rtt_ms, path_mtu;
    double srtt_ms;
} fips_probe_observation;
typedef enum fips_probe_action {
    FIPS_PROBE_InitiateLookup, FIPS_PROBE_OpenSession, FIPS_PROBE_SendWarmup,
    FIPS_PROBE_TeardownSession, FIPS_PROBE_Finish
} fips_probe_action;
typedef struct fips_probe_actions {
    fips_probe_action items[FIPS_PROBE_ACTION_CAPACITY];
    size_t len;
} fips_probe_actions;
/* Caller-owned value, no allocation/IO/clock reads. All times are monotonic
 * milliseconds in one host domain, matching Rust Observation rather than wall
 * time. Initialize before use; do not mutate implementation fields. */
typedef struct fips_probe {
    uint64_t started_ms, ended_ms, deadline_ms, stage_started_ms;
    bool has_ended_ms;
    fips_probe_budgets budgets;
    fips_probe_preflight preflight;
    fips_probe_stage stage;
    fips_probe_snapshot report;
    bool owns_session, open_requested, teardown_emitted, has_teardown_at_ms;
    uint64_t teardown_at_ms;
    bool lookup_was_pending, has_lookup_outcome;
    fips_probe_lookup_outcome_kind lookup_outcome;
    fips_probe_rtt_counters baseline;
    bool has_last_warmup_ms;
    uint64_t last_warmup_ms;
    bool finished;
} fips_probe;

/* Saturating at u64 MAX, including C's explicit extension for huge tick
 * products / teardown deadlines that overflow the upstream Rust arithmetic. */
FIPS_CORE_API fips_probe_budgets fips_probe_budgets_derive(
    uint64_t tick_ms, const uint64_t *attempt_timeouts_secs, size_t count);
FIPS_CORE_API uint64_t fips_probe_budgets_total_ms(fips_probe_budgets b);
FIPS_CORE_API void fips_probe_new(fips_probe *p, uint64_t now_ms,
    fips_probe_budgets budgets, fips_probe_preflight preflight);
FIPS_CORE_API bool fips_probe_owns_session(const fips_probe *p);
FIPS_CORE_API bool fips_probe_is_finished(const fips_probe *p);
/* Replaces actions, never appends; fixed maximum two (teardown then finish).
 * Overlong/error text containing NUL returns RANGE before modifying p or out.
 * Other inputs are typed, trusted host snapshots, not network bytes. All input,
 * output and p storage must be disjoint. Finish/cancel is idempotent.
 * Host MUST recheck identity/adoption before executing TeardownSession, including
 * on cancel (which has no Observation). OpenSession also rechecks entry absence.
 * Host owns the <=4-job registry, target claims, IDs and 30s retention/reaping. */
FIPS_CORE_API fips_status fips_probe_step(fips_probe *p,
    const fips_probe_observation *obs, fips_probe_actions *out);
FIPS_CORE_API void fips_probe_cancel(fips_probe *p, uint64_t now_ms, fips_probe_actions *out);
FIPS_CORE_API void fips_probe_snapshot_get(const fips_probe *p, fips_probe_snapshot *out);
FIPS_CORE_API uint64_t fips_probe_elapsed_ms(const fips_probe *p, uint64_t now_ms);
/* Valid, constructed coordinates; output never aliases either input. */
FIPS_CORE_API void fips_probe_describe_path(const fips_tree_coordinate *my,
    const fips_tree_coordinate *their, fips_probe_path_facts *out);
#ifdef __cplusplus
}
#endif
#endif
