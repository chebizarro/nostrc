/*
 * nh_fuse_source.h — 4-tier read ladder for the portable-home FUSE mount.
 *
 * SPDX-License-Identifier: MIT
 *
 * WARNING: EXPERIMENTAL AND UNREVIEWED — gated by
 * NOSTR_HOMED_ENABLE_PORTHOME_FUSE_EXPERIMENTAL. Do not deploy.
 *
 * Beads: nostrc-1u55 (P4-I), parent nostrc-h10m.
 *
 * The FUSE handlers must not talk to four libraries directly. This
 * module (design §3.2) owns:
 *
 *   Tier 0 — $HOME/<rel> pread  (if state == ready and stat matches snapshot)
 *   Tier 1 — plaintext chunk LRU (in-memory only, MADV_DONTDUMP)
 *   Tier 2 — nh_syncd_cache_get_path — decrypt into tier 1
 *   Tier 3 — nh_porthome_blossom_fetch (content-sha verified),
 *              nh_syncd_cache_put, decrypt into tier 1
 *
 * All plaintext handled here is transient — the mount is the one
 * process in the stack that has no business persisting plaintext.
 * home_key lives on an mlock'd page.
 */

#ifndef NH_FUSE_SOURCE_H
#define NH_FUSE_SOURCE_H

#include "nh_porthome_blossom.h"
#include "nh_porthome_crypto.h"
#include "nh_syncd_cache.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nh_fuse_source nh_fuse_source;

typedef struct {
    /* Owning references handed in by the mount main. Borrowed for
     * the lifetime of the source. */
    const char           *home_dir;         /* $HOME, for tier 0 pread */
    bool                  tier0_enabled;    /* false unless state == ready */
    nh_syncd_cache       *cache;            /* borrowed, tier 2 */
    nh_porthome_blossom_t *blossom;         /* borrowed, tier 3 */
    /* home_key already derived from the seed on the caller's mlock'd
     * page. The source copies the 32 bytes into its own mlock'd page
     * and cleanses on close. */
    uint8_t               home_key[NH_PORTHOME_KEY_LEN];
    size_t                chunk_cache_bytes; /* 0 → 64 MiB */
    long                  offline_budget_ms; /* 0 → 15000 */
    /* Called on any tier-3 miss (EIO). `rel_path` may be NULL if the
     * miss is chunk-only. `errcode` is a positive errno (EIO, ENOENT). */
    void (*on_miss)(void *ud, const char *rel_path, int errcode);
    void                 *on_miss_ud;
} nh_fuse_source_cfg;

int  nh_fuse_source_open (const nh_fuse_source_cfg *cfg, nh_fuse_source **out);
void nh_fuse_source_close(nh_fuse_source *s);

/* Tier-1/2/3 chunk retrieval by 64-hex address (from snapshot.json).
 * On success *out_pt is a borrowed pointer into the tier-1 LRU valid
 * until the next call to this function or to _close from the same
 * thread (single-threaded loop in v1). Returns 0 on success or one
 * of:
 *   -ENOENT  no server has it
 *   -EIO     sha mismatch, AEAD failure, or offline-budget exhausted
 *   -ENOMEM
 */
int  nh_fuse_source_chunk(nh_fuse_source *s,
                          const char sha256_hex[65],
                          const uint8_t **out_pt, size_t *out_len);

/* Full read service. Resolves tier 0 first if enabled; otherwise
 * walks a chunk-address list. `chunks_hex` is an array of 64-hex+NUL
 * strings; `chunk_size` is the plaintext chunk size (4 MiB in v1).
 * `rel` is used for logging / tier-0 lookup / miss notification.
 *
 * Return value semantics match pread(2): number of bytes read
 * (possibly less than `len` when reaching EOF), 0 on EOF, or a
 * negative errno on error.
 */
ssize_t nh_fuse_source_pread(nh_fuse_source *s,
                             const char *rel,
                             const char *content_hash_hex,  /* may be "" */
                             uint64_t declared_size,
                             const char * const *chunks_hex,
                             size_t n_chunks,
                             size_t chunk_size,
                             void *buf, size_t len, off_t off);

/* Force the source to skip tier 0 for this open (e.g. handle keyed
 * to the old generation). */
void nh_fuse_source_disable_tier0(nh_fuse_source *s);

/* Counters for the status file + notification throttle. */
typedef struct {
    uint64_t hits_local;      /* tier 0 */
    uint64_t hits_chunk_lru;  /* tier 1 */
    uint64_t hits_cache;      /* tier 2 */
    uint64_t fetches;         /* tier 3 */
    uint64_t misses;          /* EIO */
    uint64_t last_miss_epoch; /* wall time of most recent miss */
} nh_fuse_source_stats_t;

void nh_fuse_source_stats(const nh_fuse_source *s,
                          nh_fuse_source_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* NH_FUSE_SOURCE_H */
