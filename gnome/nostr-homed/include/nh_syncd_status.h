/*
 * nh_syncd_status.h — syncd's writer for the unified
 * $XDG_STATE_HOME/nostr-homed/porthome-status.json (Phase 5 I3 +
 * h10m.1.1).
 *
 * SPDX-License-Identifier: MIT
 *
 * See docs/designs/home-from-relay.md §6.5 for the retention/quota
 * context and gnome/nostr-homed/include/nh_porthome_status.h for the
 * merge contract. Every state transition inside the syncd daemon
 * (bootstrap, pull start/end, push done, sweep tick, reconcile
 * conflict) rebuilds the "syncd" key body and hands it to
 * nh_porthome_status_write_key_default().
 *
 * The bundle intentionally lives inside src/porthome-syncd (not the
 * common library) so a rewrite of the field set never destabilises
 * the fuse-side "fuse" key writer: each daemon owns its slice of the
 * merged file and shares only the merge helper.
 *
 * Beads: nostrc-8hw8, nostrc-h10m.1.1.
 */
#ifndef NH_SYNCD_STATUS_H
#define NH_SYNCD_STATUS_H

#include "nh_porthome_notify.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bounded number of most-recent notification records kept in-memory
 * for the "syncd" key's recent[] array (design §D9: no unbounded
 * queues). New entries evict the oldest slot; render order is
 * most-recent-first. */
#define NH_SYNCD_STATUS_RECENT_CAP 20u

/* State slugs surfaced through status.syncd.state. */
typedef enum {
    NH_SYNCD_STATE_IDLE = 0,
    NH_SYNCD_STATE_PULLING,
    NH_SYNCD_STATE_PUSHING,
    NH_SYNCD_STATE_RECONCILING,
    NH_SYNCD_STATE_LIMITED,
    NH_SYNCD_STATE_OFFLINE,
    NH_SYNCD_STATE_ERROR,
} nh_syncd_state_slug;

/* Opaque writer. Owns the rolling recent[] ring + the last known
 * field values. Thread-safety: single-writer (the syncd main loop);
 * the notifier record callback is invoked from the same thread. */
typedef struct nh_syncd_status_writer nh_syncd_status_writer;

/* Construct. Never returns NULL — allocation failure aborts. */
nh_syncd_status_writer *nh_syncd_status_writer_new(void);

/* Free. Safe on NULL. */
void nh_syncd_status_writer_free(nh_syncd_status_writer *w);

/* Field mutators — copy-on-set, cheap. All safe on NULL writer. */
void nh_syncd_status_set_state         (nh_syncd_status_writer *w,
                                        nh_syncd_state_slug s);
void nh_syncd_status_set_last_push_gen (nh_syncd_status_writer *w,
                                        uint64_t gen);
void nh_syncd_status_set_last_pull_gen (nh_syncd_status_writer *w,
                                        uint64_t gen);
void nh_syncd_status_set_last_error    (nh_syncd_status_writer *w,
                                        const char *class_slug);
void nh_syncd_status_set_pinned_count  (nh_syncd_status_writer *w,
                                        uint32_t n);
void nh_syncd_status_set_cache_bytes   (nh_syncd_status_writer *w,
                                        uint64_t bytes);
void nh_syncd_status_set_cache_quota   (nh_syncd_status_writer *w,
                                        uint64_t bytes,
                                        const char *source_slug);
void nh_syncd_status_set_evict_rate    (nh_syncd_status_writer *w,
                                        uint32_t per_hour);

/* Emit — build the syncd key body and merge it into porthome-status.
 * `path` may be NULL to use nh_porthome_status_default_path(). Returns
 * 0 on success (whatever nh_porthome_status_write_key returned). */
int nh_syncd_status_emit(nh_syncd_status_writer *w, const char *path);

/* Adapter for nh_notify_record_fn — appends to the recent[] ring and
 * re-emits the status body. Wire once at startup:
 *
 *   nh_porthome_notifier_set_record(notif, nh_syncd_status_notify_record, w);
 */
void nh_syncd_status_notify_record(void *ud,
                                   int64_t epoch_secs,
                                   nh_notify_category cat,
                                   const char *key,
                                   const char *summary,
                                   const char *body,
                                   bool delivered);

/* String slug for a state — exported so tests can assert without
 * duplicating the mapping. */
const char *nh_syncd_status_state_slug(nh_syncd_state_slug s);

#ifdef __cplusplus
}
#endif
#endif /* NH_SYNCD_STATUS_H */
