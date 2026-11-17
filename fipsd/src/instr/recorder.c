#include "instr/recorder.h"
#include <time.h>
#include <sched.h>
/* The dispatcher is the only tick producer; the writer exchanges each slot.
 * Like upstream, fields are independent relaxed atomics: a span straddling a
 * drain may contribute count and duration to adjacent intervals. No locks,
 * allocations, stack of spans, or run-length-dependent buffer. Unsigned totals
 * deliberately wrap modulo 2^64, exactly like upstream fetch_add. */
typedef struct counters { _Atomic unsigned long long count, max, total; } counters;
static counters steps[FIPSD_INSTR_STEPS], gauges[FIPSD_INSTR_GAUGES];
static _Atomic unsigned long long previous;
/* Odd epochs are armed. A token belongs to one capture even if a span
 * crosses off/on. Only the tiny synchronous recording operation is counted
 * here, never the measured body (which may park a dispatcher fiber). */
static _Atomic unsigned long long epoch;
static _Atomic unsigned writing;
static const char *const names[] = {
    "check_timeouts",
    "reload_peer_acl",
    "reload_host_map",
    "poll_pending_connects",
    "poll_nostr_rendezvous",
    "poll_lan_rendezvous",
    "drive_peer_timers",
    "poll_session_rekeys",
    "purge_idle_sessions",
    "purge_expired_path_mtu",
    "process_pending_retries",
    "check_tree_state",
    "check_bloom_state",
    "compute_mesh_size",
    "record_stats_history",
    "check_mmp_reports",
    "check_session_mmp_reports",
    "check_link_heartbeats",
    "poll_link_rekeys",
    "check_pending_lookups",
    "poll_probes",
    "poll_transport_discovery",
    "sample_transport_congestion",
    "activate_connected_udp_sessions",
    "poll_worker_liveness",
    "debug_assert_peer_maps_coherent",
    "whole_tick",
};
uint64_t fipsd_instr_now(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}
uint64_t fipsd_instr_gate(void)
{
    uint64_t current = atomic_load_explicit(&epoch, memory_order_seq_cst);
    return current & 1 ? current : 0;
}
void fipsd_instr_arm(bool on)
{
    unsigned long long current = atomic_load(&epoch);
    while ((current & 1) != (unsigned)on && !atomic_compare_exchange_weak(&epoch, &current, current + 1)) {}
    if (!on) {
        /* Retire accepted atomic updates, not in-flight spans. An old span
         * trying to enter after this check observes the retired epoch and
         * cannot mutate any slot. No dispatcher/fiber handoff is required. */
        while (atomic_load_explicit(&writing, memory_order_seq_cst)) sched_yield();
    }
}
static bool enter(uint64_t capture)
{
    atomic_fetch_add_explicit(&writing, 1, memory_order_seq_cst);
    if ((capture & 1) && capture == atomic_load_explicit(&epoch, memory_order_seq_cst)) return true;
    atomic_fetch_sub_explicit(&writing, 1, memory_order_seq_cst);
    return false;
}
static void leave(void) { atomic_fetch_sub_explicit(&writing, 1, memory_order_seq_cst); }
static void record(counters *c, uint64_t value)
{
    atomic_fetch_add_explicit(&c->count, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&c->total, value, memory_order_relaxed);
    unsigned long long max = atomic_load_explicit(&c->max, memory_order_relaxed);
    while (max < value && !atomic_compare_exchange_weak_explicit(&c->max, &max, value,
            memory_order_relaxed, memory_order_relaxed)) {}
}
void fipsd_instr_record(fipsd_instr_step step, uint64_t ns) { record(&steps[step], ns); }
void fipsd_instr_gauge_record(fipsd_instr_gauge gauge, uint64_t value) { record(&gauges[gauge], value); }
void fipsd_instr_record_at(uint64_t capture, fipsd_instr_step step, uint64_t ns)
{
    if (!enter(capture)) return;
    fipsd_instr_record(step, ns); leave();
}
void fipsd_instr_gauge_at(uint64_t capture, fipsd_instr_gauge gauge, uint64_t value)
{
    if (!enter(capture)) return;
    fipsd_instr_gauge_record(gauge, value); leave();
}
void fipsd_instr_entry_at(uint64_t capture, uint64_t stamp, uint64_t late_ns)
{
    if (!enter(capture)) return;
    fipsd_instr_tick_entry(stamp, late_ns); leave();
}
void fipsd_instr_tick_entry(uint64_t stamp, uint64_t late_ns)
{
    uint64_t prev = atomic_exchange_explicit(&previous, stamp, memory_order_relaxed);
    record(&gauges[FIPSD_INSTR_TICKS], 1);
    record(&gauges[FIPSD_INSTR_ARM_STARVATION], late_ns);
    if (prev) record(&gauges[FIPSD_INSTR_TICK_GAP], stamp > prev ? stamp - prev : 0);
}
static fipsd_instr_sample take(counters *c)
{
    fipsd_instr_sample s;
    s.count = atomic_exchange_explicit(&c->count, 0, memory_order_relaxed);
    s.max = atomic_exchange_explicit(&c->max, 0, memory_order_relaxed);
    s.total = atomic_exchange_explicit(&c->total, 0, memory_order_relaxed);
    return s;
}
fipsd_instr_sample fipsd_instr_take_step(fipsd_instr_step step) { return take(&steps[step]); }
fipsd_instr_sample fipsd_instr_take_gauge(fipsd_instr_gauge gauge) { return take(&gauges[gauge]); }
void fipsd_instr_reset(void)
{
    fipsd_instr_arm(false);
    for (int i = 0; i < FIPSD_INSTR_STEPS; ++i) (void)take(&steps[i]);
    for (int i = 0; i < FIPSD_INSTR_GAUGES; ++i) (void)take(&gauges[i]);
    atomic_store_explicit(&previous, 0, memory_order_relaxed);
}
const char *fipsd_instr_step_name(fipsd_instr_step step) { return names[step]; }
bool fipsd_instr_step_emitted(fipsd_instr_step step)
{
#if !defined(__linux__) && !defined(__APPLE__)
    if (step == FIPSD_INSTR_ACTIVATE_CONNECTED_UDP_SESSIONS) return false;
#endif
#if !defined(__unix__) && !defined(__APPLE__)
    if (step == FIPSD_INSTR_POLL_WORKER_LIVENESS) return false;
#endif
#ifdef NDEBUG
    if (step == FIPSD_INSTR_DEBUG_ASSERT_PEER_MAPS_COHERENT) return false;
#endif
    (void)step;
    return true;
}
