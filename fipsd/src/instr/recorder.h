#ifndef FIPSD_INSTR_RECORDER_H
#define FIPSD_INSTR_RECORDER_H
/* Default-off macros do not evaluate even their gate/clock arguments. */
#ifdef FIPSD_ENABLE_PROFILING
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#if ATOMIC_LLONG_LOCK_FREE != 2
#error "profiling requires lock-free 64-bit atomics"
#endif
typedef enum fipsd_instr_step {
    FIPSD_INSTR_CHECK_TIMEOUTS,
    FIPSD_INSTR_RELOAD_PEER_ACL,
    FIPSD_INSTR_RELOAD_HOST_MAP,
    FIPSD_INSTR_POLL_PENDING_CONNECTS,
    FIPSD_INSTR_POLL_NOSTR_RENDEZVOUS,
    FIPSD_INSTR_POLL_LAN_RENDEZVOUS,
    FIPSD_INSTR_DRIVE_PEER_TIMERS,
    FIPSD_INSTR_POLL_SESSION_REKEYS,
    FIPSD_INSTR_PURGE_IDLE_SESSIONS,
    FIPSD_INSTR_PURGE_EXPIRED_PATH_MTU,
    FIPSD_INSTR_PROCESS_PENDING_RETRIES,
    FIPSD_INSTR_CHECK_TREE_STATE,
    FIPSD_INSTR_CHECK_BLOOM_STATE,
    FIPSD_INSTR_COMPUTE_MESH_SIZE,
    FIPSD_INSTR_RECORD_STATS_HISTORY,
    FIPSD_INSTR_CHECK_MMP_REPORTS,
    FIPSD_INSTR_CHECK_SESSION_MMP_REPORTS,
    FIPSD_INSTR_CHECK_LINK_HEARTBEATS,
    FIPSD_INSTR_POLL_LINK_REKEYS,
    FIPSD_INSTR_CHECK_PENDING_LOOKUPS,
    FIPSD_INSTR_POLL_PROBES,
    FIPSD_INSTR_POLL_TRANSPORT_DISCOVERY,
    FIPSD_INSTR_SAMPLE_TRANSPORT_CONGESTION,
    FIPSD_INSTR_ACTIVATE_CONNECTED_UDP_SESSIONS,
    FIPSD_INSTR_POLL_WORKER_LIVENESS,
    FIPSD_INSTR_DEBUG_ASSERT_PEER_MAPS_COHERENT,
    FIPSD_INSTR_WHOLE_TICK,
    FIPSD_INSTR_STEPS
} fipsd_instr_step;
typedef enum fipsd_instr_gauge {
    FIPSD_INSTR_TICKS, FIPSD_INSTR_PEERS, FIPSD_INSTR_TICK_GAP,
    FIPSD_INSTR_ARM_STARVATION, FIPSD_INSTR_GAUGES
} fipsd_instr_gauge;
typedef struct fipsd_instr_sample { uint64_t count, max, total; } fipsd_instr_sample;
uint64_t fipsd_instr_now(void);
uint64_t fipsd_instr_gate(void);
void fipsd_instr_arm(bool on);
void fipsd_instr_record(fipsd_instr_step step, uint64_t ns);
void fipsd_instr_record_at(uint64_t capture, fipsd_instr_step step, uint64_t ns);
void fipsd_instr_gauge_at(uint64_t capture, fipsd_instr_gauge gauge, uint64_t value);
void fipsd_instr_entry_at(uint64_t capture, uint64_t stamp, uint64_t late_ns);
void fipsd_instr_gauge_record(fipsd_instr_gauge gauge, uint64_t value);
void fipsd_instr_tick_entry(uint64_t stamp, uint64_t late_ns);
void fipsd_instr_reset(void);
fipsd_instr_sample fipsd_instr_take_step(fipsd_instr_step step);
fipsd_instr_sample fipsd_instr_take_gauge(fipsd_instr_gauge gauge);
const char *fipsd_instr_step_name(fipsd_instr_step step);
bool fipsd_instr_step_emitted(fipsd_instr_step step);
#define FIPSD_INSTR_RETURN(on, id, step, ...) do { bool instr_result_ = (__VA_ARGS__); FIPSD_INSTR_END(on, id, step); return instr_result_; } while (0)
#define FIPSD_INSTR_GATE(on) const uint64_t on = fipsd_instr_gate()
#define FIPSD_INSTR_BEGIN(on, id) uint64_t id = (on) ? fipsd_instr_now() : 0
#define FIPSD_INSTR_END(on, id, step) do { if (on) fipsd_instr_record_at(on, FIPSD_INSTR_##step, fipsd_instr_now() - (id)); } while (0)
#define FIPSD_INSTR_STEP(on, step, ...) do { FIPSD_INSTR_BEGIN(on, instr_start_); __VA_ARGS__; FIPSD_INSTR_END(on, instr_start_, step); } while (0)
#define FIPSD_INSTR_ENTRY(on, now, deadline) do { if (on) { \
    fips_instant instr_entry_ = (now), instr_deadline_ = (deadline); \
    fips_duration instr_late_ = instr_entry_.ops->saturating_duration_since(instr_entry_, instr_deadline_); \
    fipsd_instr_entry_at(on, fipsd_instr_now(), instr_late_.secs * UINT64_C(1000000000) + instr_late_.nanos); \
} } while (0)
#define FIPSD_INSTR_PEER_COUNT(on, n) do { if (on) fipsd_instr_gauge_at(on, FIPSD_INSTR_PEERS, (n)); } while (0)
#else
#define FIPSD_INSTR_RETURN(on, id, step, ...) return __VA_ARGS__
#define FIPSD_INSTR_GATE(on)
#define FIPSD_INSTR_BEGIN(on, id)
#define FIPSD_INSTR_END(on, id, step)
#define FIPSD_INSTR_STEP(on, step, ...) __VA_ARGS__
#define FIPSD_INSTR_ENTRY(on, now, deadline)
#define FIPSD_INSTR_PEER_COUNT(on, n)
#endif
#endif
