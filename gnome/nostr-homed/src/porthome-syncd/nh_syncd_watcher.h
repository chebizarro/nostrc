/*
 * nh_syncd_watcher.h — internal header for the budgeted inotify
 * watcher. Not installed; consumed by the daemon main and its tests.
 */
#ifndef NH_SYNCD_WATCHER_H
#define NH_SYNCD_WATCHER_H

#include "nh_syncd.h"

typedef struct nh_syncd_watcher nh_syncd_watcher;

/* Watches $HOME with nh_syncd_watcher_default_budget() watches. */
int  nh_syncd_watcher_new(const char *home,
                          nh_syncd_ignore *ignore,
                          nh_syncd_batcher *batcher,
                          nh_syncd_watcher **out);
/* nostrc-lqm2: same, with an explicit hard cap on inotify watches.
 * 0 watches nothing and leaves the whole tree to the fallback rescan. */
int  nh_syncd_watcher_new_with_budget(const char *home,
                                      nh_syncd_ignore *ignore,
                                      nh_syncd_batcher *batcher,
                                      uint32_t budget,
                                      nh_syncd_watcher **out);
void nh_syncd_watcher_free(nh_syncd_watcher *w);
int  nh_syncd_watcher_fd(const nh_syncd_watcher *w);
/* Read and dispatch every pending inotify event. Returns -1 on IO
 * error, otherwise number of events processed. */
int  nh_syncd_watcher_drain(nh_syncd_watcher *w);

/* xnxd part 2 — attach state baseline for rescans. */
void     nh_syncd_watcher_set_state       (nh_syncd_watcher *w,
                                           nh_syncd_state *state);
/* Rebuild watches (within budget) and diff the whole tree. */
int      nh_syncd_watcher_force_rescan    (nh_syncd_watcher *w);
uint64_t nh_syncd_watcher_overflow_count  (const nh_syncd_watcher *w);
uint64_t nh_syncd_watcher_rescan_count    (const nh_syncd_watcher *w);

/* nostrc-lqm2 — watch budget.
 * kernel_limit: fs.inotify.max_user_watches (8192 if unreadable).
 * default_budget: NOSTR_HOMED_SYNCD_MAX_WATCHES if set (clamped to half
 * the kernel limit), else min(8192, kernel_limit / 4). */
uint32_t nh_syncd_watcher_kernel_limit    (void);
uint32_t nh_syncd_watcher_default_budget  (void);
uint32_t nh_syncd_watcher_watch_count     (const nh_syncd_watcher *w);
uint32_t nh_syncd_watcher_budget          (const nh_syncd_watcher *w);
/* In-scope directories left unwatched (noisy, over budget, ENOSPC). */
size_t   nh_syncd_watcher_unwatched_count (const nh_syncd_watcher *w);
int      nh_syncd_watcher_is_watched      (const nh_syncd_watcher *w,
                                           const char *rel);
/* 0 = not an unwatched root, 1 = noisy (watch-skip list), 2 = budget. */
int      nh_syncd_watcher_unwatched_reason(const nh_syncd_watcher *w,
                                           const char *rel);
/* Fallback tick: diff the unwatched subtrees against state, then
 * re-promote over-budget roots if budget has freed up. Returns the
 * number of roots rescanned (0 when fully watched), or < 0 on error. */
int      nh_syncd_watcher_rescan_unwatched(nh_syncd_watcher *w);

#endif
