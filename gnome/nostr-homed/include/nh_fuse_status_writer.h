/*
 * nh_fuse_status_writer.h — stateful writer for the FUSE contribution
 * to $XDG_STATE_HOME/nostr-homed/porthome-status.json.
 *
 * SPDX-License-Identifier: MIT
 *
 * Companion of nh_syncd_status.h on the syncd side. The stateless
 * nh_fuse_write_status_unified (see nh_fuse_status.h) covers the
 * scalar mounted/generation/counters set. Phase 5 follow-up
 * nostrc-k4j4 adds a rolling `recent[]` mirror of every notifier
 * event the FUSE process fires (design §D9: bounded, most-recent
 * first, no unbounded queues) so the desktop and CLI see a fuse-side
 * event trace that matches the syncd side.
 *
 * Threading: WITH_LOCK-style mutex around every setter and the
 * emitter. The notifier record adapter is invoked from the same
 * thread that posts notifications in v1 (the mount main or the
 * reload watcher), but the lock keeps us safe if that changes.
 *
 * Beads: nostrc-k4j4, parent nostrc-h10m.
 */
#ifndef NH_FUSE_STATUS_WRITER_H
#define NH_FUSE_STATUS_WRITER_H

#include "nh_fuse_source.h"
#include "nh_porthome_notify.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bounded — matches NH_SYNCD_STATUS_RECENT_CAP so operators see the
 * same shape on both keys. */
#define NH_FUSE_STATUS_RECENT_CAP 20u

typedef struct nh_fuse_status_writer nh_fuse_status_writer;

/* Construct. Aborts on OOM. */
nh_fuse_status_writer *nh_fuse_status_writer_new(void);

/* Free. Safe on NULL. */
void nh_fuse_status_writer_free(nh_fuse_status_writer *w);

/* Field setters — all safe on NULL writer. */
void nh_fuse_status_writer_set_mounted     (nh_fuse_status_writer *w, bool mounted);
void nh_fuse_status_writer_set_mountpoint  (nh_fuse_status_writer *w, const char *mp);
void nh_fuse_status_writer_set_generation  (nh_fuse_status_writer *w, uint64_t gen);
void nh_fuse_status_writer_set_cache_bytes (nh_fuse_status_writer *w, uint64_t bytes);
void nh_fuse_status_writer_set_stats       (nh_fuse_status_writer *w,
                                            const nh_fuse_source_stats_t *st);
void nh_fuse_status_writer_set_last_error  (nh_fuse_status_writer *w,
                                            const char *class_slug, int64_t ts);
void nh_fuse_status_writer_set_evict_rate  (nh_fuse_status_writer *w,
                                            uint32_t per_hour);

/* Emit — merge the "fuse" key into porthome-status.json. `path` may be
 * NULL to use the default resolver. Returns 0 on success, -errno on
 * write failure. */
int nh_fuse_status_writer_emit(nh_fuse_status_writer *w, const char *path);

/* Adapter for nh_notify_record_fn. Wire once at startup:
 *   nh_porthome_notifier_set_record(notif,
 *                                   nh_fuse_status_writer_notify_record, w);
 * Appends to the rolling recent[] and re-emits the "fuse" key body. */
void nh_fuse_status_writer_notify_record(void *ud,
                                         int64_t epoch_secs,
                                         nh_notify_category cat,
                                         const char *key,
                                         const char *summary,
                                         const char *body,
                                         bool delivered);

/* Test-only introspection: count of recent[] entries currently held. */
unsigned nh_fuse_status_writer_recent_count(const nh_fuse_status_writer *w);

#ifdef __cplusplus
}
#endif
#endif /* NH_FUSE_STATUS_WRITER_H */
