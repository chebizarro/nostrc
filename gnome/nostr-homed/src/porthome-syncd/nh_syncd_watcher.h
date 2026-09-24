/*
 * nh_syncd_watcher.h — internal header for the recursive inotify
 * watcher. Not installed; consumed only by the daemon main.
 */
#ifndef NH_SYNCD_WATCHER_H
#define NH_SYNCD_WATCHER_H

#include "nh_syncd.h"

typedef struct nh_syncd_watcher nh_syncd_watcher;

int  nh_syncd_watcher_new(const char *home,
                          nh_syncd_ignore *ignore,
                          nh_syncd_batcher *batcher,
                          nh_syncd_watcher **out);
void nh_syncd_watcher_free(nh_syncd_watcher *w);
int  nh_syncd_watcher_fd(const nh_syncd_watcher *w);
int  nh_syncd_watcher_drain(nh_syncd_watcher *w);

#endif
