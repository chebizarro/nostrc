#ifndef FIPSD_PERF_PROFILE_H
#define FIPSD_PERF_PROFILE_H
#ifdef FIPSD_ENABLE_PROFILING
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "instr/recorder.h"
#define FIPSD_PERF_HIST_BUCKETS 48u
typedef enum fipsd_perf_stage {
    FIPSD_PERF_UDP_RECV,
    FIPSD_PERF_FMP_DECRYPT,
    FIPSD_PERF_LINK_DISPATCH,
    FIPSD_PERF_FSP_DECRYPT,
    FIPSD_PERF_TUN_WRITE,
    FIPSD_PERF_FSP_ENCRYPT,
    FIPSD_PERF_FMP_ENCRYPT,
    FIPSD_PERF_UDP_SEND,
    FIPSD_PERF_PROCESS_PACKET,
    FIPSD_PERF_ENDPOINT_DELIVER,
    FIPSD_PERF_FSP_HANDLE,
    FIPSD_PERF_ENDPOINT_SEND,
    FIPSD_PERF_ENDPOINT_COMMAND_WAIT,
    FIPSD_PERF_FMP_WORKER_QUEUE_WAIT,
    FIPSD_PERF_TRANSPORT_QUEUE_WAIT,
    FIPSD_PERF_ENDPOINT_EVENT_WAIT,
    FIPSD_PERF_PUBLISH_SNAPSHOT, /* C-only dispatcher projection cost */
    FIPSD_PERF_SNAPSHOT_HEADER,
    FIPSD_PERF_SNAPSHOT_PEERS,
    FIPSD_PERF_SNAPSHOT_SESSIONS,
    FIPSD_PERF_SNAPSHOT_TRANSPORTS,
    FIPSD_PERF_SNAPSHOT_TREE,
    FIPSD_PERF_SNAPSHOT_CACHE,
    FIPSD_PERF_SNAPSHOT_COUNTERS,
    FIPSD_PERF_SNAPSHOT_HISTORY,
    FIPSD_PERF_STAGES
} fipsd_perf_stage;
typedef enum fipsd_perf_event {
    FIPSD_PERF_UDP_SEND_CONNECTED,
    FIPSD_PERF_UDP_SEND_WILDCARD,
    FIPSD_PERF_UDP_SEND_BACKPRESSURE,
    FIPSD_PERF_CONNECTED_UDP_INSTALLED,
    FIPSD_PERF_CONNECTED_UDP_ACTIVATION_FAILED,
    FIPSD_PERF_UDP_SEND_BACKPRESSURE_SLEEP,
    FIPSD_PERF_EVENTS
} fipsd_perf_event;
typedef struct fipsd_perf_snapshot {
    uint64_t total[FIPSD_PERF_STAGES], count[FIPSD_PERF_STAGES];
    uint64_t hist[FIPSD_PERF_STAGES][FIPSD_PERF_HIST_BUCKETS];
    uint64_t events[FIPSD_PERF_EVENTS];
} fipsd_perf_snapshot;
bool fipsd_perf_enabled(void);
void fipsd_perf_record(fipsd_perf_stage stage, uint64_t ns);
void fipsd_perf_event_count(fipsd_perf_event event, uint64_t count);
unsigned fipsd_perf_bucket(uint64_t ns);
uint64_t fipsd_perf_percentile(const uint64_t *hist, uint64_t total, uint64_t pct);
int fipsd_perf_report(FILE *file, uint64_t interval, fipsd_perf_snapshot *previous);
void fipsd_perf_start(void);
void fipsd_perf_shutdown(void);
#define FIPSD_PERF_START() fipsd_perf_start()
#define FIPSD_PERF_SHUTDOWN() fipsd_perf_shutdown()
#define FIPSD_PERF_BEGIN(id) uint64_t id = fipsd_perf_enabled() ? fipsd_instr_now() : 0
#define FIPSD_PERF_END(id, stage) do { if (id) fipsd_perf_record(FIPSD_PERF_##stage, fipsd_instr_now() - (id)); } while (0)
#define FIPSD_PERF_RETURN(id, stage, ...) do { bool perf_result_ = (__VA_ARGS__); FIPSD_PERF_END(id, stage); return perf_result_; } while (0)
#define FIPSD_PERF_QUEUE_FIELD uint64_t profile_queued;
#define FIPSD_PERF_STAMP(p) do { (p)->profile_queued = fipsd_perf_enabled() ? fipsd_instr_now() : 0; } while (0)
#define FIPSD_PERF_WAIT(p, stage) do { if ((p)->profile_queued) fipsd_perf_record(FIPSD_PERF_##stage, fipsd_instr_now() - (p)->profile_queued); } while (0)
#define FIPSD_PERF_BLOCK(stage, ...) do { FIPSD_PERF_BEGIN(block_start_); __VA_ARGS__; FIPSD_PERF_END(block_start_, stage); } while (0)
#define FIPSD_PERF_JOB_FIELDS uint64_t profile_queued; bool profile_session;
#define FIPSD_PERF_JOB_TAG(j, session) do { (j)->profile_session = (session); } while (0)
#define FIPSD_PERF_JOB_QUEUE(j) do { (j)->profile_queued = fipsd_perf_enabled() ? fipsd_instr_now() : 0; } while (0)
#define FIPSD_PERF_JOB_WAIT(j) do { if ((j)->profile_queued && (j)->operation == FIPSD_CRYPTO_SEAL && !(j)->profile_session) fipsd_perf_record(FIPSD_PERF_FMP_WORKER_QUEUE_WAIT, fipsd_instr_now() - (j)->profile_queued); } while (0)
#define FIPSD_PERF_CRYPTO_END(id, j, fsp, fmp) do { if (id) fipsd_perf_record((j)->profile_session ? FIPSD_PERF_##fsp : FIPSD_PERF_##fmp, fipsd_instr_now() - (id)); } while (0)
#define FIPSD_PERF_EVENT(event, n) fipsd_perf_event_count(FIPSD_PERF_##event, n)
#else
#define FIPSD_PERF_START()
#define FIPSD_PERF_SHUTDOWN()
#define FIPSD_PERF_BEGIN(id)
#define FIPSD_PERF_END(id, stage)
#define FIPSD_PERF_RETURN(id, stage, ...) return __VA_ARGS__
#define FIPSD_PERF_QUEUE_FIELD
#define FIPSD_PERF_STAMP(p)
#define FIPSD_PERF_WAIT(p, stage)
#define FIPSD_PERF_BLOCK(stage, ...) __VA_ARGS__
#define FIPSD_PERF_JOB_FIELDS
#define FIPSD_PERF_JOB_TAG(j, session)
#define FIPSD_PERF_JOB_QUEUE(j)
#define FIPSD_PERF_JOB_WAIT(j)
#define FIPSD_PERF_CRYPTO_END(id, j, fsp, fmp)
#define FIPSD_PERF_EVENT(event, n)
#endif
#endif
