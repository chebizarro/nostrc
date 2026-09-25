/*
 * nh_syncd_watcher.c — recursive inotify watcher over $HOME (§6.2).
 *
 * SPDX-License-Identifier: MIT
 *
 * This is production glue: the daemon boots it, feeds each inotify
 * event into an nh_syncd_batcher, and runs the resulting batches
 * through nh_syncd_push_batch.
 *
 * KNOWN INOTIFY LIMITATIONS (documented, deliberately not fixed in I1):
 *   - inotify is per-mount; xdev directories are excluded upstream by
 *     the ignore matcher, so we don't waste watches on them anyway.
 *   - hardlink edits: inotify fires on the file, not each link. If
 *     two links exist to the same inode, only the changed name gets
 *     an IN_MODIFY. The other link's snapshot entry lags until it is
 *     touched. This is a known-limitation punt; the design §6.3
 *     already flags "concurrent edits to the same file on two
 *     machines" as a known-bad in v1.
 *   - IN_MOVED_TO from outside the watched tree looks like a CREATE
 *     with no cookie match; we handle both halves as separate
 *     CREATE/DELETE events. That is correct for our snapshot; it
 *     just doubles the batch size briefly.
 *   - A rapid rmdir + mkdir of a watched directory drops watches.
 *     We rebuild watches on IN_CREATE of a directory and on
 *     IN_MOVED_TO of a directory; a race where the mkdir happens
 *     while we're processing the delete may miss the first inotify
 *     inside the new dir. On next 15-minute force flush the state
 *     rebuilder will pick it up.
 *
 * I1 intentionally does NOT include a full-tree rescan on missed
 * events. I2 owns rescan-and-reconcile.
 *
 * The watcher publishes an fd (from inotify_init1) so the daemon's
 * epoll loop can select on it alongside the batcher's timerfd and
 * signal fd.
 */

#include "nh_syncd.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

/* One watch descriptor per directory. We keep a small growable map
 * wd -> rel_path so inotify_event lookups don't linear-scan through
 * the whole tree. 256-bucket chained hash keyed on wd. */
typedef struct wd_ent {
    int   wd;
    char *rel;                  /* $HOME-relative directory path, "" for root */
    struct wd_ent *next;
} wd_ent;

struct nh_syncd_watcher {
    int      fd;
    char    *home;
    nh_syncd_ignore *ignore;
    nh_syncd_batcher *batcher;
    wd_ent  *bucket[256];
    /* xnxd part 2: state pointer (borrowed) used by the rescan-diff
     * trigger. May be NULL when the daemon hasn't loaded a state yet;
     * in that case IN_Q_OVERFLOW is logged but the diff is skipped
     * (there is no baseline to diff against — the next batch push will
     * seed one). */
    nh_syncd_state *state;
    uint64_t overflow_count;
    uint64_t rescan_count;
};

static unsigned wd_hash(int wd) { return (unsigned)wd & 0xff; }

static void wd_insert(struct nh_syncd_watcher *w, int wd, const char *rel) {
    wd_ent *e = calloc(1, sizeof *e);
    if (!e) return;
    e->wd = wd;
    e->rel = strdup(rel);
    if (!e->rel) { free(e); return; }
    unsigned h = wd_hash(wd);
    e->next = w->bucket[h];
    w->bucket[h] = e;
}

static const char *wd_lookup(struct nh_syncd_watcher *w, int wd) {
    for (wd_ent *e = w->bucket[wd_hash(wd)]; e; e = e->next)
        if (e->wd == wd) return e->rel;
    return NULL;
}

static void wd_remove(struct nh_syncd_watcher *w, int wd) {
    wd_ent **p = &w->bucket[wd_hash(wd)];
    while (*p) {
        if ((*p)->wd == wd) {
            wd_ent *dead = *p;
            *p = dead->next;
            free(dead->rel); free(dead);
            return;
        }
        p = &(*p)->next;
    }
}

static const uint32_t WATCH_MASK =
    IN_CLOSE_WRITE | IN_MOVED_FROM | IN_MOVED_TO | IN_CREATE |
    IN_DELETE | IN_DELETE_SELF | IN_MOVE_SELF | IN_ATTRIB |
    IN_ONLYDIR | IN_EXCL_UNLINK;

/* Recursively add watches under `rel` (starting empty for the root
 * itself). Skips ignored paths. */
static int add_watches(struct nh_syncd_watcher *w, const char *rel) {
    char abs[PATH_MAX];
    if (rel[0]) snprintf(abs, sizeof abs, "%s/%s", w->home, rel);
    else        snprintf(abs, sizeof abs, "%s", w->home);
    int wd = inotify_add_watch(w->fd, abs, WATCH_MASK);
    if (wd < 0) return -1;
    wd_insert(w, wd, rel);
    DIR *d = opendir(abs);
    if (!d) return 0; /* not fatal — a race means we caught it emptied */
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char child[PATH_MAX];
        if (rel[0]) snprintf(child, sizeof child, "%s/%s", rel, de->d_name);
        else        snprintf(child, sizeof child, "%s", de->d_name);
        char cab[PATH_MAX];
        snprintf(cab, sizeof cab, "%s/%s", w->home, child);
        struct stat st;
        if (lstat(cab, &st) != 0) continue;
        if (!S_ISDIR(st.st_mode)) continue;
        if (nh_syncd_ignore_check(w->ignore, child,
                                  (uint64_t)st.st_dev,
                                  (uint32_t)st.st_mode) != NH_SYNCD_IGNORE_PASS)
            continue;
        add_watches(w, child);
    }
    closedir(d);
    return 0;
}

int nh_syncd_watcher_new(const char *home,
                         nh_syncd_ignore *ignore,
                         nh_syncd_batcher *batcher,
                         struct nh_syncd_watcher **out);

void nh_syncd_watcher_free(struct nh_syncd_watcher *w);
int  nh_syncd_watcher_fd(const struct nh_syncd_watcher *w);
/* Read and dispatch every pending inotify event. Returns -1 on IO
 * error, otherwise number of events processed. */
int  nh_syncd_watcher_drain(struct nh_syncd_watcher *w);

int nh_syncd_watcher_new(const char *home,
                         nh_syncd_ignore *ignore,
                         nh_syncd_batcher *batcher,
                         struct nh_syncd_watcher **out)
{
    if (!home || !ignore || !batcher || !out) return NH_SYNCD_ERR_ARG;
    struct nh_syncd_watcher *w = calloc(1, sizeof *w);
    if (!w) return NH_SYNCD_ERR_OOM;
    w->fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (w->fd < 0) { free(w); return NH_SYNCD_ERR_IO; }
    w->home = strdup(home);
    w->ignore = ignore;
    w->batcher = batcher;
    if (!w->home) { close(w->fd); free(w); return NH_SYNCD_ERR_OOM; }
    if (add_watches(w, "") < 0) {
        nh_syncd_watcher_free(w);
        return NH_SYNCD_ERR_IO;
    }
    *out = w;
    return NH_SYNCD_OK;
}

/* xnxd part 2: attach the state pointer so overflow-driven rescans
 * have a baseline to diff against. Optional; safe on NULL watcher. */
void nh_syncd_watcher_set_state(struct nh_syncd_watcher *w,
                                nh_syncd_state *state) {
    if (w) w->state = state;
}
uint64_t nh_syncd_watcher_overflow_count(const struct nh_syncd_watcher *w) {
    return w ? w->overflow_count : 0;
}
uint64_t nh_syncd_watcher_rescan_count(const struct nh_syncd_watcher *w) {
    return w ? w->rescan_count : 0;
}

/* Public trigger — rebuild watches under $HOME then diff the tree vs
 * `state` into the batcher. Called on IN_Q_OVERFLOW and by the periodic
 * rescan tick. Returns 0 on success (even if no work fell out); < 0 on
 * error. Never crashes on a NULL state — logs and skips the diff. */
int nh_syncd_watcher_force_rescan(struct nh_syncd_watcher *w) {
    if (!w) return NH_SYNCD_ERR_ARG;
    w->rescan_count++;
    /* Rebuild watches: the kernel keeps the descriptors we already have,
     * but a create/delete race during the overflow may have dropped
     * subtrees.  add_watches skips watches it already owns via
     * inotify_add_watch's idempotence (same wd returned; wd_insert
     * would double-insert — cheap to tolerate, small ring). */
    (void)add_watches(w, "");
    if (!w->state) {
        fprintf(stderr, "syncd/watcher: rescan skipped — no state baseline\n");
        return 0;
    }
    nh_syncd_rescan_stats st = {0};
    int rc = nh_syncd_rescan_diff(w->state, w->home, w->ignore,
                                  w->batcher, &st);
    fprintf(stderr,
            "syncd/watcher: rescan done added=%llu modified=%llu deleted=%llu"
            " unchanged=%llu rc=%d\n",
            (unsigned long long)st.added,
            (unsigned long long)st.modified,
            (unsigned long long)st.deleted,
            (unsigned long long)st.unchanged, rc);
    return rc;
}

void nh_syncd_watcher_free(struct nh_syncd_watcher *w) {
    if (!w) return;
    if (w->fd >= 0) close(w->fd);
    for (int i = 0; i < 256; i++) {
        wd_ent *e = w->bucket[i];
        while (e) { wd_ent *n = e->next; free(e->rel); free(e); e = n; }
    }
    free(w->home);
    free(w);
}

int nh_syncd_watcher_fd(const struct nh_syncd_watcher *w) {
    return w ? w->fd : -1;
}

int nh_syncd_watcher_drain(struct nh_syncd_watcher *w) {
    if (!w) return -1;
    /* Buffer sized per inotify(7). */
    char buf[8192] __attribute__((aligned(__alignof__(struct inotify_event))));
    int n_events = 0;
    for (;;) {
        ssize_t len = read(w->fd, buf, sizeof buf);
        if (len <= 0) {
            if (len < 0 && errno == EAGAIN) return n_events;
            if (len < 0 && errno == EINTR) continue;
            return -1;
        }
        for (char *p = buf; p < buf + len; ) {
            struct inotify_event *ev = (struct inotify_event *)p;
            /* xnxd part 2: IN_Q_OVERFLOW arrives as a synthetic event
             * with wd == -1. The kernel has dropped an unknown number
             * of events; the only correct response is to walk $HOME
             * and diff against state so the reconcile pipeline sees
             * every add/change/remove that inotify would have driven. */
            if (ev->mask & IN_Q_OVERFLOW) {
                w->overflow_count++;
                fprintf(stderr,
                        "syncd/watcher: WARNING IN_Q_OVERFLOW #%llu — "
                        "triggering full-tree rescan\n",
                        (unsigned long long)w->overflow_count);
                (void)nh_syncd_watcher_force_rescan(w);
                p += sizeof(struct inotify_event) + ev->len;
                n_events++;
                continue;
            }
            const char *dir_rel = wd_lookup(w, ev->wd);
            if (dir_rel && ev->len > 0) {
                char child[PATH_MAX];
                if (dir_rel[0])
                    snprintf(child, sizeof child, "%s/%s", dir_rel, ev->name);
                else
                    snprintf(child, sizeof child, "%s", ev->name);
                if (nh_syncd_ignore_check_path(w->ignore, child) == NH_SYNCD_IGNORE_PASS) {
                    nh_syncd_change_kind k = 0;
                    if (ev->mask & (IN_CREATE | IN_MOVED_TO))       k = NH_SYNCD_CHANGE_CREATE;
                    else if (ev->mask & (IN_DELETE | IN_MOVED_FROM)) k = NH_SYNCD_CHANGE_DELETE;
                    else if (ev->mask & (IN_CLOSE_WRITE | IN_ATTRIB)) k = NH_SYNCD_CHANGE_MODIFY;
                    if (k) nh_syncd_batcher_push(w->batcher, child, k);
                    /* Recurse into newly-created directories. */
                    if ((ev->mask & (IN_CREATE | IN_MOVED_TO)) && (ev->mask & IN_ISDIR)) {
                        add_watches(w, child);
                    }
                }
            }
            /* Watch-descriptor removed by kernel (self-delete). */
            if (ev->mask & (IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF)) {
                wd_remove(w, ev->wd);
            }
            p += sizeof(struct inotify_event) + ev->len;
            n_events++;
        }
    }
}
