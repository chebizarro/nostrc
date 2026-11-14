#ifndef FIPS_NODE_STATS_HISTORY_H
#define FIPS_NODE_STATS_HISTORY_H
#include <fips/host.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
#define FIPS_FAST_RING_CAPACITY 3600
#define FIPS_SLOW_RING_CAPACITY 1440
#define FIPS_DOWNSAMPLE_FACTOR 60
#define FIPS_PEER_EVICTION_SECS UINT64_C(86400)

typedef enum fips_metric {
    FIPS_METRIC_MESH_SIZE, FIPS_METRIC_TREE_DEPTH, FIPS_METRIC_PEER_COUNT,
    FIPS_METRIC_PARENT_SWITCHES, FIPS_METRIC_BYTES_IN, FIPS_METRIC_BYTES_OUT,
    FIPS_METRIC_PACKETS_IN, FIPS_METRIC_PACKETS_OUT, FIPS_METRIC_LOSS_RATE,
    FIPS_METRIC_ACTIVE_SESSIONS, FIPS_METRIC_COUNT
} fips_metric;
typedef enum fips_peer_metric {
    FIPS_PEER_SRTT_MS, FIPS_PEER_LOSS_RATE, FIPS_PEER_BYTES_IN, FIPS_PEER_BYTES_OUT,
    FIPS_PEER_PACKETS_IN, FIPS_PEER_PACKETS_OUT, FIPS_PEER_ECN_CE, FIPS_PEER_METRIC_COUNT
} fips_peer_metric;
typedef enum fips_aggregation { FIPS_AGG_LAST, FIPS_AGG_SUM, FIPS_AGG_MEAN } fips_aggregation;
typedef enum fips_granularity { FIPS_GRANULARITY_FAST, FIPS_GRANULARITY_SLOW } fips_granularity;
typedef struct fips_metric_info {
    const char *name, *unit;
    fips_aggregation aggregation;
    bool is_counter;
} fips_metric_info;
/** These arrays also define the stable enumeration / wire order. */
FIPS_CORE_API extern const fips_metric_info fips_metrics[FIPS_METRIC_COUNT];
FIPS_CORE_API extern const fips_metric_info fips_peer_metrics[FIPS_PEER_METRIC_COUNT];
FIPS_CORE_API fips_status fips_metric_parse(const char *name, fips_metric *out);
FIPS_CORE_API fips_status fips_peer_metric_parse(const char *name, fips_peer_metric *out);
FIPS_CORE_API fips_status fips_granularity_parse(const char *name, fips_granularity *out);

typedef struct fips_stats_snapshot {
    bool has_mesh_size;
    uint64_t mesh_size;
    uint32_t tree_depth;
    uint64_t peer_count, parent_switches_total, bytes_in_total, bytes_out_total;
    uint64_t packets_in_total, packets_out_total;
    double loss_rate;
    uint64_t active_sessions;
} fips_stats_snapshot;
typedef struct fips_peer_snapshot {
    uint8_t node_addr[16];
    fips_instant last_seen; /* Retained input parity; contact uses tick's now. */
    bool has_srtt_ms, has_loss_rate;
    double srtt_ms, loss_rate;
    uint64_t bytes_in_total, bytes_out_total, packets_in_total, packets_out_total, ecn_ce_total;
} fips_peer_snapshot;

/* Storage layout is public for caller allocation, not direct mutation. Large
 * objects: prefer host heap/static storage, not a small event-loop stack. */
typedef struct fips_stats_ring {
    double fast[FIPS_FAST_RING_CAPACITY], slow[FIPS_SLOW_RING_CAPACITY];
    size_t fast_start, fast_len, slow_start, slow_len;
    double sum, last;
    uint32_t count, total;
    uint64_t prev_total;
    bool has_prev;
    fips_aggregation aggregation;
} fips_stats_ring;
typedef struct fips_peer_stats {
    uint8_t addr[16];
    fips_stats_ring rings[FIPS_PEER_METRIC_COUNT];
    fips_instant first_seen, last_contact;
    bool seen_tick;
} fips_peer_stats;
typedef struct fips_stats_history {
    fips_stats_ring rings[FIPS_METRIC_COUNT];
    fips_peer_stats *peers; /* Sorted by 16-byte address, like Rust BTreeMap. */
    size_t peer_count, peer_capacity;
    uint64_t fast_pushes;
    fips_instant last_tick;
    bool has_data;
} fips_stats_history;
/** Caller owns history and peer_storage for the entire lifetime. No allocations.
 * Upstream has no fixed peer-map ceiling; choose a host retention budget here.
 * Storage may be NULL iff capacity is zero. Do not shallow-copy live history:
 * it borrows peer_storage. Instants must share a domain. */
FIPS_CORE_API void fips_stats_history_init(fips_stats_history *h,
    fips_peer_stats *peer_storage, size_t peer_capacity);
/** One push per call, NOT per elapsed second. Downsample every 60 calls.
 * Capacity preflight is transactional: FIPS_ERR_RANGE leaves ALL state unchanged.
 * It counts distinct new peers before end-of-tick eviction; explicit cleanup
 * can reclaim expired peers before retry. Missing peers append NaN and reset
 * counter baselines. Present peers update contact to now, not last_seen.
 * Duplicate snapshots follow upstream: each is pushed in slice order.
 * peers may be NULL iff count == 0. */
FIPS_CORE_API fips_status fips_stats_history_tick(fips_stats_history *h, fips_instant now,
    const fips_stats_snapshot *snapshot, const fips_peer_snapshot *peers, size_t count);
FIPS_CORE_API void fips_stats_history_cleanup(fips_stats_history *h, fips_instant now);
/** Borrowed metadata, invalidated by the next mutating history call. NULL if absent. */
FIPS_CORE_API const fips_peer_stats *fips_stats_history_peer(const fips_stats_history *h,
                                                          const uint8_t addr[16]);
/** Serialization model: metric/unit/granularity_seconds/values have upstream
 * Series JSON names; len is only buffer extent, NOT an extra JSON field.
 * values are oldest-first, front-padded with NaN, clipped to ring capacity.
 * Emit EVERY non-finite value (NaN AND +/-infinity) as JSON null, never a
 * string or an invalid JSON number. The core has no JSON dependency. The
 * walk hook supplies this decision; hosts serialize metadata and delimiters.
 * values borrows the caller's query buffer; metadata strings are static. */
typedef struct fips_stats_series {
    const char *metric, *unit;
    uint64_t granularity_seconds;
    const double *values;
    size_t len;
} fips_stats_series;
typedef void (*fips_series_visitor)(void *ctx, size_t index, bool is_null, double value);
FIPS_CORE_API void fips_stats_series_walk(const fips_stats_series *series,
                                         fips_series_visitor visit, void *ctx);
/** Insufficient buffer/unknown peer: FIPS_ERR_RANGE; buffer and out unchanged.
 * Enums must be valid. Buffer may be NULL for zero-length output. */
FIPS_CORE_API fips_status fips_stats_history_query(const fips_stats_history *h,
    fips_metric metric, fips_duration window, fips_granularity granularity,
    double *buffer, size_t capacity, fips_stats_series *out);
FIPS_CORE_API fips_status fips_stats_history_peer_query(const fips_stats_history *h,
    const uint8_t addr[16], fips_peer_metric metric, fips_duration window,
    fips_granularity granularity, double *buffer, size_t capacity, fips_stats_series *out);
/** false if empty, out unchanged. */
FIPS_CORE_API bool fips_stats_history_latest(const fips_stats_history *h, fips_metric metric,
                                            double *out);
/** Last min(n, available) fast samples, without query's NaN front padding. */
FIPS_CORE_API fips_status fips_stats_history_recent(const fips_stats_history *h,
    fips_metric metric, size_t n, double *buffer, size_t capacity, size_t *out_len);
#ifdef __cplusplus
}
#endif
#endif
