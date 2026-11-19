#ifndef FIPSD_NODE_STATS_VIEW_H
#define FIPSD_NODE_STATS_VIEW_H
#include <fips/node/stats_history.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Control snapshots read the stats history through immutable, ref-counted
 * views rather than a copy per publish: one ring is ~40 KB and every retained
 * peer carries seven. The dispatcher logs each history update's inputs and
 * brings a free view current by replaying them (the update is deterministic),
 * so an unchanged history costs a retain and a changed one a tick's pushes.
 * Only the dispatcher writes a view, and only while no snapshot holds it;
 * fipsd_stats_view_release is the one call any thread may make. */
#define FIPSD_STATS_VIEWS 2
#define FIPSD_STATS_LOG 8

typedef struct fipsd_stats_view fipsd_stats_view;
typedef struct fipsd_stats_update {
    uint64_t seq;
    bool replayable;
    fips_instant now;
    fips_stats_snapshot snapshot;
    fips_peer_snapshot *peers;
    size_t count;
} fipsd_stats_update;
/* Zero-initialized is valid: no log, so every refresh is a full copy. */
typedef struct fipsd_stats_views {
    fipsd_stats_view *pool[FIPSD_STATS_VIEWS], *current;
    fipsd_stats_update log[FIPSD_STATS_LOG];
    size_t log_capacity; /* peers per logged update */
    uint64_t seq;        /* updates applied to the live history */
    uint64_t copies, replays; /* view refreshes, for diagnostics */
} fipsd_stats_views;

typedef enum fipsd_stats_outcome {
    FIPSD_STATS_APPLIED, FIPSD_STATS_NO_MEMORY, FIPSD_STATS_EXHAUSTED
} fipsd_stats_outcome;
/* One history update: cleanup, grow *storage (h's peer storage) to fit the new
 * peers, tick, and on refusal clean up and tick once more. */
fipsd_stats_outcome fipsd_stats_history_update(fips_stats_history *h, fips_peer_stats **storage,
    fips_instant now, const fips_stats_snapshot *snapshot, const fips_peer_snapshot *peers, size_t count);

bool fipsd_stats_views_init(fipsd_stats_views *v, size_t peer_capacity);
void fipsd_stats_views_free(fipsd_stats_views *v);
/* After an update was applied to the live history (applied: its outcome). */
void fipsd_stats_views_record(fipsd_stats_views *v, bool applied, fips_instant now,
    const fips_stats_snapshot *snapshot, const fips_peer_snapshot *peers, size_t count);
/* After the live history changed some other way (re-initialized). */
void fipsd_stats_views_invalidate(fipsd_stats_views *v);
/* A retained view equal to live, or NULL if memory ran out. */
fipsd_stats_view *fipsd_stats_views_acquire(fipsd_stats_views *v, const fips_stats_history *live);
const fips_stats_history *fipsd_stats_view_history(const fipsd_stats_view *view);
void fipsd_stats_view_release(fipsd_stats_view *view);
#endif
