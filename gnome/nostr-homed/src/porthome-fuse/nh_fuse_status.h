/*
 * nh_fuse_status.h — fuse-status.json writer.
 *
 * SPDX-License-Identifier: MIT
 *
 * Extracted from nostr-home-fuse.c so the JSON writer is exercisable
 * from a unit test (bead nostrc-h4tv) without wiring a full FUSE
 * mount. Design §6.2 defines this file as the health seam that
 * `systemctl --user status` and other monitors read.
 *
 * Schema (stable within v1):
 *   { "schema": 1,
 *     "mounted": <bool>,
 *     "generation": <u64>,
 *     "tier0": true,
 *     "hits_local": <u64>, "hits_cache": <u64>,
 *     "fetches":    <u64>, "misses":     <u64>,
 *     "last_miss_epoch": <u64> }
 *
 * Beads: nostrc-h4tv, parent nostrc-1u55.
 */

#ifndef NH_FUSE_STATUS_H
#define NH_FUSE_STATUS_H

#include "nh_fuse_source.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Atomic write of the fuse-status.json body to `path`.
 *   path       — absolute path; parent dir must already exist.
 *   mounted    — true iff the FUSE session is live.
 *   generation — active snapshot generation (0 when no snapshot loaded).
 *   stats      — chunk-source counters; may be NULL (treated as zeroed).
 *
 * Uses O_CREAT|O_TRUNC to `<path>.tmp` then rename(2). Mode 0600.
 * Returns 0 on success or a negative errno on failure. */
int nh_fuse_write_status(const char *path,
                         bool mounted,
                         uint64_t generation,
                         const nh_fuse_source_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif /* NH_FUSE_STATUS_H */
