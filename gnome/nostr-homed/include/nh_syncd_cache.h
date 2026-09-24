/*
 * nh_syncd_cache.h — portable-home Phase 3 (I3) local blob cache,
 *                    generation pin ring, and weekly HEAD sweep.
 *
 * SPDX-License-Identifier: MIT
 *
 * WARNING: EXPERIMENTAL AND UNREVIEWED.
 * Gated behind NOSTR_HOMED_ENABLE_SYNCD_EXPERIMENTAL (requires
 * NOSTR_HOMED_ENABLE_PORTHOME_EXPERIMENTAL). Do not deploy.
 *
 * See docs/designs/home-from-relay.md §6.5 for the spec:
 *   - Local cache is user-scope. We store at
 *     ${XDG_CACHE_HOME:-~/.cache}/nostr-homed/blobs (NOT /var/cache;
 *     the design lists /var for a hypothetical system daemon — the
 *     Phase-3 sync daemon runs under `systemd --user` so a user-scope
 *     cache is what actually matches Session and permissions).
 *   - Content-addressed by SHA-256 with two-char sharding: aa/bb/aabb…
 *   - LRU-evictable against a configurable quota (default:
 *     min(10 GiB, 10 % of the FS reported by statvfs on the cache dir)).
 *   - Blobs referenced by any of the last N=10 generations are pinned
 *     and NEVER evicted.
 *   - Weekly sweep: HEAD every blob in the current snapshot against
 *     every configured Blossom server; if any server 404s, re-upload
 *     via BUD-02. Notification fires per sweep if dropped > 0.
 *   - We do NOT call BUD-02 DELETE in v1.
 *
 * The generation-pin ring is persisted at
 *   ${XDG_STATE_HOME:-~/.local/state}/nostr-homed/pinned.json
 *
 *   {
 *     "schema":     1,
 *     "capacity":   10,
 *     "generations": [
 *       { "gen": <u64>, "hashes": ["<64-hex>", ...] },
 *       ...
 *     ]
 *   }
 *
 * Beads: nostrc-p6qp (Phase 3 tracking), nostrc-h10m (portable-home epic).
 */

#ifndef NH_SYNCD_CACHE_H
#define NH_SYNCD_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* libhanami's signer type is a typedef of an anonymous struct in
 * hanami-types.h and cannot be forward-declared. Match the pattern
 * used by nh_syncd.h / nh_porthome_blossom.h. */
#include <hanami/hanami-types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Disjoint from the nh_syncd_*, nh_porthome_*, nh_porthome_blossom_*
 * ranges so callers can dispatch on rc across the whole stack. */
typedef enum {
    NH_SYNCD_CACHE_OK              =    0,
    NH_SYNCD_CACHE_ERR_ARG         = -300,
    NH_SYNCD_CACHE_ERR_OOM         = -301,
    NH_SYNCD_CACHE_ERR_IO          = -302,
    NH_SYNCD_CACHE_ERR_JSON        = -303,
    NH_SYNCD_CACHE_ERR_NOT_FOUND   = -304,
    NH_SYNCD_CACHE_ERR_HASH        = -305, /* body did not match declared sha */
    NH_SYNCD_CACHE_ERR_QUOTA       = -306, /* single blob larger than quota */
    NH_SYNCD_CACHE_ERR_UPLOAD      = -307,
    NH_SYNCD_CACHE_ERR_HEAD        = -308,
    /* Quota enforcement (Phase 5 I3): a put would push the cache
     * over quota AND every remaining byte on disk is currently
     * pinned by the generation ring. The caller should surface a
     * LIMITED_MODE notification and treat the fetch as dropped. */
    NH_SYNCD_CACHE_ERR_QUOTA_PINNED = -309,
} nh_syncd_cache_status;

/* Retention: pin any blob referenced by the last N generations (§6.5). */
#define NH_SYNCD_CACHE_RETENTION_GENERATIONS 10u

/* Absolute default quota when statvfs is unavailable. 10 GiB (§6.5). */
#define NH_SYNCD_CACHE_DEFAULT_QUOTA_BYTES \
    (10ull * 1024ull * 1024ull * 1024ull)

/* Percent of the FS to use as an alternative cap (§6.5). */
#define NH_SYNCD_CACHE_FS_PERCENT 10u

/* Weekly sweep period (§6.5). */
#define NH_SYNCD_CACHE_SWEEP_PERIOD_SECS (7u * 24u * 3600u)

/* Cache dir mode + file mode (blobs may include manifest ciphertext
 * from other blob references — keep them uid-scoped). */
#define NH_SYNCD_CACHE_DIR_MODE   0700
#define NH_SYNCD_CACHE_FILE_MODE  0600

/* Path helpers — malloc'd, caller free()s. Return NULL on OOM. Do NOT
 * mkdir; they are pure strcat helpers. */

/* $XDG_CACHE_HOME/nostr-homed/blobs (or ~/.cache/nostr-homed/blobs). */
char *nh_syncd_cache_default_dir(void);

/* $XDG_STATE_HOME/nostr-homed/pinned.json (or the ~/.local/state/…
 * fallback), matching nh_syncd_state's state_dir convention. */
char *nh_syncd_cache_default_pin_path(void);

/* Compute the effective quota for a cache dir. Returns the smaller of
 * NH_SYNCD_CACHE_DEFAULT_QUOTA_BYTES and NH_SYNCD_CACHE_FS_PERCENT % of
 * the filesystem hosting the cache dir (statvfs). If the dir does not
 * yet exist, walks up parents until one does. Falls back to the
 * absolute default on statvfs error. */
uint64_t nh_syncd_cache_default_quota(const char *cache_dir);

/* ─────────────────────────────────────────────────────────────────
 * Content-addressed cache.
 * ───────────────────────────────────────────────────────────────── */

typedef struct nh_syncd_cache nh_syncd_cache;

/* Open (or create) a cache rooted at `dir`. mkdir -p; dir mode 0700;
 * per-shard mode 0700; file mode 0600. `quota_bytes == 0` means use
 * nh_syncd_cache_default_quota(dir). */
int  nh_syncd_cache_open(const char *dir,
                         uint64_t quota_bytes,
                         nh_syncd_cache **out);
void nh_syncd_cache_close(nh_syncd_cache *c);

const char *nh_syncd_cache_dir(const nh_syncd_cache *c);
uint64_t    nh_syncd_cache_quota_bytes(const nh_syncd_cache *c);

/* Live disk usage (walks shard dirs, sums file sizes). O(n) — the
 * cache is bounded by quota anyway. */
uint64_t    nh_syncd_cache_used_bytes(const nh_syncd_cache *c);

bool nh_syncd_cache_has(const nh_syncd_cache *c, const char *sha256_hex);

/* Return the absolute path to the cached blob (caller free()s) or
 * NULL if missing. Touches atime for LRU. */
char *nh_syncd_cache_get_path(nh_syncd_cache *c, const char *sha256_hex);

/* Insert bytes. Verifies sha256(data) == sha256_hex. Atomic:
 * writes to dir/aa/bb/aabb….tmp.<pid>.<rand>, fsync, rename(2).
 * If a concurrent writer beat us to the target, the tmp is unlinked
 * and success is returned. Files land with mode 0600. */
int  nh_syncd_cache_put(nh_syncd_cache *c,
                        const char *sha256_hex,
                        const uint8_t *data,
                        size_t len);

/* Best-effort atomic-tmp cleanup: scans the cache for stale `.tmp.*`
 * files older than 1 hour and unlinks them. Called by open(); can be
 * called ad hoc. */
int  nh_syncd_cache_cleanup_stale_tmps(nh_syncd_cache *c);

/* Refcount pin/unpin. A pinned blob (refcount > 0) is never evicted
 * by nh_syncd_cache_sweep(). Pins do NOT persist across daemon
 * restart on their own — callers should also record them in the
 * generation pin ring (which does persist). */
int  nh_syncd_cache_pin(nh_syncd_cache *c, const char *sha256_hex);
int  nh_syncd_cache_unpin(nh_syncd_cache *c, const char *sha256_hex);
bool nh_syncd_cache_is_pinned(const nh_syncd_cache *c, const char *sha256_hex);

/* LRU eviction: while used_bytes > quota, evict the oldest unpinned
 * blob (by atime, tie-broken by mtime, then name). Returns 0 on
 * success; sets *out_evicted / *out_reclaimed if non-NULL. Pinned
 * blobs are never touched. */
int  nh_syncd_cache_sweep(nh_syncd_cache *c,
                          size_t   *out_evicted,
                          uint64_t *out_reclaimed);

/* v1 forbids BUD-02 DELETE (§6.5). Kept as an explicit guard so a
 * later caller trying to wire deletion can't do so silently. Always
 * returns NH_SYNCD_CACHE_ERR_ARG and logs to stderr with `reason`. */
int  nh_syncd_cache_forbid_remote_delete(const char *reason);

/* ─────────────────────────────────────────────────────────────────
 * Phase 5 I3 — quota policy, override discovery, thrash notifier.
 * ───────────────────────────────────────────────────────────────── */

/* Env override honoured by nh_syncd_cache_effective_quota(). */
#define NH_SYNCD_CACHE_QUOTA_ENV "NOSTR_HOMED_PORTHOME_CACHE_QUOTA_BYTES"

/* On-disk override, checked when the env var is unset. Contents: a
 * single decimal integer (bytes) with optional trailing newline. */
#define NH_SYNCD_CACHE_QUOTA_CONFIG_REL "nostr-homed/cache-quota"

/* Guards on the operator override (§6.5 quota knobs). */
#define NH_SYNCD_CACHE_QUOTA_OVERRIDE_MIN (1ull   * 1024ull * 1024ull * 1024ull)  /*   1 GiB */
#define NH_SYNCD_CACHE_QUOTA_OVERRIDE_MAX (200ull * 1024ull * 1024ull * 1024ull)  /* 200 GiB */

/* Thrash-detection defaults (§6.5: a user-visible notification when
 * eviction is thrashing > N/hr). */
#define NH_SYNCD_CACHE_THRASH_WINDOW_SECS   3600u
#define NH_SYNCD_CACHE_THRASH_DEFAULT_LIMIT 100u
#define NH_SYNCD_CACHE_THRASH_ENV           "NOSTR_HOMED_PORTHOME_QUOTA_THRASH_THRESHOLD"

/* Compute the effective quota including overrides. Layering:
 *   1. $NOSTR_HOMED_PORTHOME_CACHE_QUOTA_BYTES if valid.
 *   2. Else contents of $XDG_CONFIG_HOME/nostr-homed/cache-quota.
 *   3. Else nh_syncd_cache_default_quota(cache_dir).
 * Override values are clamped to [MIN_OVERRIDE, MAX_OVERRIDE]; values
 * outside that band are ignored (fall through to the next layer).
 * If out_source is non-NULL, it is set to one of:
 *   "env", "config", "default"
 * — a stable slug the CLI + status writer surface to operators. */
uint64_t nh_syncd_cache_effective_quota(const char *cache_dir,
                                        const char **out_source);

/* Recompute the effective quota and update the cache in-place.
 * Callable at any time (syncd wires this to SIGHUP). Returns the
 * new quota value. */
uint64_t nh_syncd_cache_reload_quota(nh_syncd_cache *c);

/* Evict-thrash notification callback. Fires the FIRST time the
 * eviction count within a rolling NH_SYNCD_CACHE_THRASH_WINDOW_SECS
 * window exceeds `threshold`, and again only after the window rolls
 * over and the threshold is re-breached. `evict_count` is the count
 * that tripped the limiter; `first_evict_epoch` is the head of the
 * rolling window in Unix seconds. */
typedef void (*nh_syncd_cache_evict_notify_fn)(void *ud,
                                               unsigned evict_count,
                                               unsigned threshold,
                                               int64_t  first_evict_epoch);

/* Register the evict-thrash callback. NULL fn clears the callback.
 * The threshold defaults to NH_SYNCD_CACHE_THRASH_DEFAULT_LIMIT and
 * can be overridden via $NOSTR_HOMED_PORTHOME_QUOTA_THRASH_THRESHOLD
 * at cache open time. */
void nh_syncd_cache_set_evict_notify(nh_syncd_cache *c,
                                     nh_syncd_cache_evict_notify_fn fn,
                                     void *ud);

/* Toggle put-time auto-eviction (Phase 5 I3). OFF at cache_open;
 * fuse + syncd flip it ON immediately after open so every
 * pull-write path enforces the quota. Tests that pre-date the
 * behaviour keep the OFF default and drive sweep manually. */
void nh_syncd_cache_set_auto_evict(nh_syncd_cache *c, bool on);

/* Introspection for status writers + tests. */
unsigned nh_syncd_cache_evict_count_1h(const nh_syncd_cache *c);
unsigned nh_syncd_cache_thrash_threshold(const nh_syncd_cache *c);

/* ─────────────────────────────────────────────────────────────────
 * Generation pin ring (persisted, ring of size N=10).
 * ───────────────────────────────────────────────────────────────── */

typedef struct nh_syncd_pin_ring nh_syncd_pin_ring;

/* Open or initialise. `json_path` may not exist — an empty ring is
 * returned in that case. Corrupt JSON returns NH_SYNCD_CACHE_ERR_JSON;
 * callers should treat this as recoverable (delete the file, reopen). */
int  nh_syncd_pin_ring_open(const char *json_path,
                            nh_syncd_pin_ring **out);
void nh_syncd_pin_ring_close(nh_syncd_pin_ring *r);

size_t nh_syncd_pin_ring_size(const nh_syncd_pin_ring *r);
size_t nh_syncd_pin_ring_capacity(const nh_syncd_pin_ring *r);

/* Record a new generation. If it already exists as the most-recent
 * slot, replace it (idempotent for double-fires); otherwise push at
 * the tail, evicting slot 0 if size == capacity. Persists atomically
 * to disk (tmp + rename, mode 0600). */
int  nh_syncd_pin_ring_promote(nh_syncd_pin_ring *r,
                               uint64_t generation,
                               const char *const *hashes,
                               size_t n_hashes);

/* Convenience: derive `hashes` by walking snapshot.json under
 * `state_dir`. Collects every chunk_addrs_hex entry (deduped). Uses
 * the generation embedded in that snapshot. */
int  nh_syncd_pin_ring_promote_from_snapshot(nh_syncd_pin_ring *r,
                                             const char *state_dir);

/* Enumerate the effective (unioned) pin set. Caller owns:
 *   free((*out_hashes)[i]); free(*out_hashes);
 */
int  nh_syncd_pin_ring_effective_pins(const nh_syncd_pin_ring *r,
                                      char ***out_hashes,
                                      size_t  *out_n);

/* Apply ring's effective pin set to a cache (pin every hash the ring
 * knows about; missing cache entries are fine — pin still records
 * intent so a later put() lands as pinned). */
int  nh_syncd_pin_ring_apply(const nh_syncd_pin_ring *r,
                             nh_syncd_cache *cache);

/* ─────────────────────────────────────────────────────────────────
 * Weekly HEAD sweep.
 *
 * For each blob in the current snapshot, HEAD against every configured
 * Blossom server; if any server returns 404, re-upload via
 * nh_porthome_blossom (BUD-02 PUT). Emits per-sweep counters and fires
 * an optional notification once per sweep when dropped > 0.
 * ───────────────────────────────────────────────────────────────── */

typedef void (*nh_syncd_sweep_notify_fn)(void *ud,
                                         size_t dropped,
                                         size_t reuploaded,
                                         const char *summary);

/* Per-server HEAD test seam. `server` is the base URL string; return
 *   1 => exists, 0 => 404 / missing, <0 => network/other error. */
typedef int (*nh_syncd_head_fn)(void *ud,
                                const char *server,
                                const char *sha256_hex);

/* Uploader test seam. Called instead of nh_porthome_blossom_upload
 * when non-NULL. Return 0 on success, <0 on failure. The sweep
 * reads plaintext bytes from the local cache (get_path); callers
 * that don't populate the cache must provide this seam. */
typedef int (*nh_syncd_upload_fn)(void *ud,
                                  const char *sha256_hex,
                                  const uint8_t *data,
                                  size_t len);

typedef struct {
    /* Blossom transport — same shape as nh_syncd_push_cfg. */
    const char * const   *servers;
    size_t                n_servers;

    /* Signer for re-upload PUTs. Required when upload_fn is NULL. */
    const hanami_signer_t *bud02_signer;

    /* Timeout for the individual HTTP calls. 0 => default (30 s). */
    long   timeout_seconds;

    /* Notification hook (optional). */
    nh_syncd_sweep_notify_fn    notify_fn;
    void                 *notify_ud;

    /* Test seams. */
    nh_syncd_head_fn      head_fn;
    void                 *head_ud;
    nh_syncd_upload_fn    upload_fn;
    void                 *upload_ud;
} nh_syncd_sweep_cfg;

/* Enumerate every chunk_addrs_hex from `${state_dir}/snapshot.json`;
 * HEAD each server for each blob; re-upload on 404. Uses `cache` to
 * source blob plaintext for re-upload. `cache` may be NULL — in that
 * case a re-upload with no upload_fn returns NH_SYNCD_CACHE_ERR_UPLOAD
 * (nothing to source from) and the blob is counted as still-dropped. */
int nh_syncd_sweep_run_once(const nh_syncd_sweep_cfg *cfg,
                            const char *state_dir,
                            nh_syncd_cache *cache,
                            size_t *out_dropped,
                            size_t *out_reuploaded);

/* Fixed-clock periodic driver. `now_secs` is the current monotonic
 * time; `state_ptr` is a caller-owned scalar (initialise to 0)
 * remembering the last sweep timestamp. Returns 1 iff a sweep ran
 * this tick, 0 otherwise, <0 on error. */
int nh_syncd_sweep_tick(const nh_syncd_sweep_cfg *cfg,
                        const char *state_dir,
                        nh_syncd_cache *cache,
                        uint64_t now_secs,
                        uint64_t *state_ptr,
                        size_t *out_dropped,
                        size_t *out_reuploaded);

#ifdef __cplusplus
}
#endif

#endif /* NH_SYNCD_CACHE_H */
