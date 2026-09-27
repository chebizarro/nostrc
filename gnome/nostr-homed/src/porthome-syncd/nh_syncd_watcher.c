/*
 * nh_syncd_watcher.c — budgeted inotify watcher over $HOME (§6.2).
 *
 * SPDX-License-Identifier: MIT
 *
 * This is production glue: the daemon boots it, feeds each inotify
 * event into an nh_syncd_batcher, and runs the resulting batches
 * through nh_syncd_push_batch.
 *
 * WATCH BUDGET (nostrc-lqm2). inotify costs one watch per directory and
 * fs.inotify.max_user_watches is shared by EVERY process of the user. An
 * uncapped recursive watch over a home full of build trees took 65,145 of
 * 65,536 watches on the lab; every other inotify_add_watch in the session
 * then failed ENOSPC and GLib's GFileMonitor attached silently and never
 * fired (Nautilus, tracker, file choosers, the shell extension...). So:
 *   - the watcher never holds more than `budget` watches (default
 *     min(8192, max_user_watches / 4); NOSTR_HOMED_SYNCD_MAX_WATCHES
 *     overrides, clamped to max_user_watches / 2);
 *   - directories are watched breadth-first, so when the budget runs out
 *     it is the deepest levels that go unwatched, not whole siblings;
 *   - well-known noisy trees (VCS metadata, node_modules, language and
 *     package-manager caches, anything tagged CACHEDIR.TAG) are never
 *     watched. They are still synced — this is a watch-skip list, not an
 *     ignore list;
 *   - every directory that is in sync scope but unwatched (noisy, over
 *     budget, or refused with ENOSPC) is recorded as an "unwatched root";
 *     nh_syncd_watcher_rescan_unwatched() diffs just those subtrees
 *     against state on the daemon's fallback tick, and re-promotes them
 *     to real watches when budget has freed up;
 *   - hitting the budget (or ENOSPC) logs one warning per process.
 *
 * KNOWN INOTIFY LIMITATIONS:
 *   - inotify is per-mount; xdev directories are excluded upstream by
 *     the ignore matcher, so we don't waste watches on them anyway.
 *   - hardlink edits: inotify fires on the file, not each link. The
 *     other link's snapshot entry lags until it is touched (§6.3
 *     known-bad in v1).
 *   - IN_MOVED_TO from outside the watched tree looks like a CREATE
 *     with no cookie match; we handle both halves as separate
 *     CREATE/DELETE events.
 *   - A rapid rmdir + mkdir of a watched directory may miss the first
 *     event inside the new dir; the rescan paths close that gap.
 *
 * The watcher publishes an fd (from inotify_init1) so the daemon's
 * poll loop can select on it.
 */

#include "nh_syncd.h"
#include "nh_syncd_watcher.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

/* Hard ceiling on the default budget, and the fraction of the kernel's
 * per-user limit the default may take. */
#define NH_WATCH_DEFAULT_CAP      8192u
#define NH_WATCH_DEFAULT_DIVISOR  4u
/* An explicit override may not take more than half the user's budget. */
#define NH_WATCH_OVERRIDE_DIVISOR 2u
/* Used when /proc/sys/fs/inotify/max_user_watches can't be read; the
 * historical kernel default. */
#define NH_WATCH_KERNEL_FALLBACK  8192u

enum { UNWATCHED_NOISY = 1, UNWATCHED_BUDGET = 2 };

/* wd -> rel_path, 256-bucket chained hash keyed on wd. rel -> wd lives
 * in a jansson object alongside so re-walks can skip directories they
 * already own without a syscall. */
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
    json_t  *by_rel;            /* rel -> wd */
    json_t  *unwatched;         /* rel -> UNWATCHED_* */
    uint32_t n_watches;
    uint32_t budget;
    uint32_t kernel_limit;
    bool     kernel_full;       /* last add_watch said ENOSPC */
    bool     warned_budget;
    bool     warned_enospc;
    bool     overflow_pending;  /* IN_Q_OVERFLOW seen; rescan after drain */
    /* xnxd part 2: state pointer (borrowed) used by the rescan paths.
     * May be NULL when the daemon hasn't loaded a state yet; rescans
     * then only rebuild watches. */
    nh_syncd_state *state;
    uint64_t overflow_count;
    uint64_t rescan_count;
    uint64_t fallback_count;
};

/* Basenames never worth a watch at any depth. */
static const char *const NOISY_NAMES[] = {
    ".git", ".hg", ".svn", ".bzr", "_darcs", "CVS",
    "node_modules", "bower_components", ".pnpm-store", ".yarn", ".npm",
    "__pycache__", ".tox", ".nox", ".venv", ".mypy_cache", ".pytest_cache",
    ".ruff_cache", ".gradle", ".m2", ".ccache", ".cargo", ".rustup",
    ".cache", ".Trash",
    NULL,
};

/* $HOME-anchored subtrees that are caches or package stores. (~/.cache
 * and ~/.local/share/Trash are already outside sync scope.) */
static const char *const NOISY_PREFIX[] = {
    ".local/share/containers",
    ".local/share/flatpak",
    ".local/share/Trash",
    ".var/app",
    "snap",
    NULL,
};

static unsigned wd_hash(int wd) { return (unsigned)wd & 0xff; }

static char *join_rel(const char *dir, const char *name) {
    char *out = NULL;
    int n = dir[0] ? asprintf(&out, "%s/%s", dir, name)
                   : asprintf(&out, "%s", name);
    return n < 0 ? NULL : out;
}

static char *abs_of(const struct nh_syncd_watcher *w, const char *rel) {
    char *out = NULL;
    int n = rel[0] ? asprintf(&out, "%s/%s", w->home, rel)
                   : asprintf(&out, "%s", w->home);
    return n < 0 ? NULL : out;
}

static int has_prefix_component(const char *rel, const char *prefix) {
    size_t pn = strlen(prefix);
    if (strncmp(rel, prefix, pn) != 0) return 0;
    return rel[pn] == '\0' || rel[pn] == '/';
}

static wd_ent *wd_find(struct nh_syncd_watcher *w, int wd) {
    for (wd_ent *e = w->bucket[wd_hash(wd)]; e; e = e->next)
        if (e->wd == wd) return e;
    return NULL;
}

static const char *wd_lookup(struct nh_syncd_watcher *w, int wd) {
    wd_ent *e = wd_find(w, wd);
    return e ? e->rel : NULL;
}

/* Record a watch the kernel handed us. inotify_add_watch returns the
 * existing wd for an inode we already watch (a directory that moved);
 * then only the path changes and nothing is counted twice. */
static void wd_record(struct nh_syncd_watcher *w, int wd, const char *rel) {
    wd_ent *e = wd_find(w, wd);
    if (e) {
        if (strcmp(e->rel, rel) != 0) {
            char *nr = strdup(rel);
            if (!nr) return;
            json_object_del(w->by_rel, e->rel);
            free(e->rel);
            e->rel = nr;
            json_object_set_new(w->by_rel, rel, json_integer(wd));
        }
        return;
    }
    e = calloc(1, sizeof *e);
    if (!e) return;
    e->wd = wd;
    e->rel = strdup(rel);
    if (!e->rel) { free(e); return; }
    unsigned h = wd_hash(wd);
    e->next = w->bucket[h];
    w->bucket[h] = e;
    json_object_set_new(w->by_rel, rel, json_integer(wd));
    w->n_watches++;
}

/* Forget a watch (the kernel dropped it, or we removed it). */
static void wd_forget(struct nh_syncd_watcher *w, int wd) {
    wd_ent **p = &w->bucket[wd_hash(wd)];
    while (*p) {
        if ((*p)->wd == wd) {
            wd_ent *dead = *p;
            *p = dead->next;
            json_t *cur = json_object_get(w->by_rel, dead->rel);
            if (cur && json_integer_value(cur) == wd)
                json_object_del(w->by_rel, dead->rel);
            free(dead->rel); free(dead);
            if (w->n_watches) w->n_watches--;
            return;
        }
        p = &(*p)->next;
    }
}

static void mark_unwatched(struct nh_syncd_watcher *w, const char *rel, int why) {
    json_object_set_new(w->unwatched, rel, json_integer(why));
}

/* Drop every watch and unwatched root at or below `rel` — the directory
 * moved or vanished, so the paths recorded for it are stale. Returns 1 if
 * any unwatched root was at or below `rel` (its contents produced no
 * per-file events on the way out). */
static int drop_subtree(struct nh_syncd_watcher *w, const char *rel) {
    int had_unwatched = 0;
    const char *key; json_t *val; void *tmp;
    json_object_foreach_safe(w->by_rel, tmp, key, val) {
        if (!has_prefix_component(key, rel)) continue;
        int wd = (int)json_integer_value(val);
        (void)inotify_rm_watch(w->fd, wd);
        wd_forget(w, wd);
    }
    json_object_foreach_safe(w->unwatched, tmp, key, val) {
        (void)val;
        if (has_prefix_component(key, rel)) {
            json_object_del(w->unwatched, key);
            had_unwatched = 1;
        }
    }
    return had_unwatched;
}

/* A directory left `rel` without per-file events for (some of) its
 * contents — a move, or a delete of a tree we were not (fully) watching.
 * Queue a DELETE for every snapshot entry below it so the snapshot does
 * not keep ghosts. The directory's own entry is reported by its parent. */
static void delete_state_below(struct nh_syncd_watcher *w, const char *rel) {
    if (!w->state) return;
    size_t rn = strlen(rel);
    size_t n = nh_syncd_state_file_count(w->state);
    for (size_t i = 0; i < n; i++) {
        const char *er = NULL;
        if (!nh_syncd_state_at(w->state, i, &er) || !er) continue;
        if (strncmp(er, rel, rn) == 0 && er[rn] == '/')
            nh_syncd_batcher_push(w->batcher, er, NH_SYNCD_CHANGE_DELETE);
    }
}

static int is_noisy(const char *rel, const char *abs) {
    const char *base = strrchr(rel, '/');
    base = base ? base + 1 : rel;
    for (int i = 0; NOISY_NAMES[i]; i++)
        if (!strcmp(base, NOISY_NAMES[i])) return 1;
    if (!strncmp(base, ".Trash-", 7)) return 1;
    for (int i = 0; NOISY_PREFIX[i]; i++)
        if (has_prefix_component(rel, NOISY_PREFIX[i])) return 1;
    /* https://bford.info/cachedir/ — build tools (cargo, ccache, ...)
     * tag their scratch trees with this file. */
    char *tag = NULL;
    if (asprintf(&tag, "%s/CACHEDIR.TAG", abs) < 0) return 0;
    int tagged = access(tag, F_OK) == 0;
    free(tag);
    return tagged;
}

static void note_budget_hit(struct nh_syncd_watcher *w) {
    if (w->warned_budget) return;
    w->warned_budget = true;
    fprintf(stderr,
            "syncd/watcher: WARNING inotify watch budget reached (%u watches, "
            "budget %u of fs.inotify.max_user_watches=%u); deeper directories "
            "fall back to periodic rescan. Raise NOSTR_HOMED_SYNCD_MAX_WATCHES "
            "or list bulky trees in ~/.config/nostr-homed/ignore.\n",
            w->n_watches, w->budget, w->kernel_limit);
}

static void note_enospc(struct nh_syncd_watcher *w) {
    if (w->warned_enospc) return;
    w->warned_enospc = true;
    fprintf(stderr,
            "syncd/watcher: WARNING inotify_add_watch: ENOSPC at %u of our "
            "watches — another process has exhausted this user's "
            "fs.inotify.max_user_watches=%u (find it with: grep -c "
            "'^inotify wd' /proc/*/fdinfo/*); falling back to periodic "
            "rescan for the rest.\n",
            w->n_watches, w->kernel_limit);
}

static const uint32_t WATCH_MASK =
    IN_CLOSE_WRITE | IN_MOVED_FROM | IN_MOVED_TO | IN_CREATE |
    IN_DELETE | IN_DELETE_SELF | IN_MOVE_SELF | IN_ATTRIB |
    IN_ONLYDIR | IN_EXCL_UNLINK;

/* Watch one directory. Returns 1 if it is (now) watched and its
 * children should be visited, 0 otherwise. */
static int watch_dir(struct nh_syncd_watcher *w, const char *rel, const char *abs) {
    if (json_object_get(w->by_rel, rel)) return 1;
    if (w->n_watches >= w->budget || w->kernel_full) {
        if (w->n_watches >= w->budget) note_budget_hit(w);
        mark_unwatched(w, rel, UNWATCHED_BUDGET);
        return 0;
    }
    int wd = inotify_add_watch(w->fd, abs, WATCH_MASK);
    if (wd < 0) {
        if (errno == ENOSPC) {
            w->kernel_full = true;
            note_enospc(w);
        }
        /* ENOENT/ENOTDIR: raced with a delete, nothing to cover. Anything
         * else (ENOSPC, EACCES, ENOMEM...) leaves an in-scope directory
         * unwatched: hand it to the fallback rescan, which also retries
         * the watch. */
        if (errno != ENOENT && errno != ENOTDIR)
            mark_unwatched(w, rel, UNWATCHED_BUDGET);
        return 0;
    }
    wd_record(w, wd, rel);
    json_object_del(w->unwatched, rel);
    return 1;
}

/* Breadth-first watch of the subtree at `start` (itself included).
 * `announce`: `start` just appeared (created or moved in) — queue a CREATE
 * for everything already inside it, which inotify never reported (files
 * written before the watch existed, or the whole content of a moved-in
 * tree). Unwatched roots inside are covered by the fallback rescan.
 * Returns -1 only when `start` is the root and could not be watched
 * for a reason other than the budget. */
static int watch_subtree(struct nh_syncd_watcher *w, const char *start,
                         bool announce) {
    size_t cap = 64, head = 0, tail = 0;
    char **q = malloc(cap * sizeof *q);
    if (!q) return -1;
    q[tail] = strdup(start);
    if (!q[tail]) { free(q); return -1; }
    tail++;
    int rc = 0;
    while (head < tail) {
        char *rel = q[head++];
        char *abs = abs_of(w, rel);
        if (!abs) { free(rel); continue; }
        if (rel[0]) {
            struct stat st;
            if (lstat(abs, &st) != 0 || !S_ISDIR(st.st_mode) ||
                nh_syncd_ignore_check(w->ignore, rel, (uint64_t)st.st_dev,
                                      (uint32_t)st.st_mode) != NH_SYNCD_IGNORE_PASS) {
                free(abs); free(rel); continue;
            }
            if (is_noisy(rel, abs)) {
                mark_unwatched(w, rel, UNWATCHED_NOISY);
                free(abs); free(rel); continue;
            }
        }
        if (!watch_dir(w, rel, abs)) {
            if (!rel[0] && !json_object_get(w->unwatched, rel)) rc = -1;
            free(abs); free(rel); continue;
        }
        DIR *d = opendir(abs);
        free(abs);
        if (!d) { free(rel); continue; } /* raced with a delete */
        struct dirent *de;
        while ((de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            bool maybe_dir = de->d_type == DT_DIR || de->d_type == DT_UNKNOWN;
            if (!maybe_dir && !announce) continue;
            char *child = join_rel(rel, de->d_name);
            if (!child) continue;
            if (announce &&
                nh_syncd_ignore_check_path(w->ignore, child) == NH_SYNCD_IGNORE_PASS)
                nh_syncd_batcher_push(w->batcher, child, NH_SYNCD_CHANGE_CREATE);
            if (!maybe_dir) { free(child); continue; }
            if (tail == cap) {
                size_t ncap = cap * 2;
                char **nq = realloc(q, ncap * sizeof *nq);
                if (!nq) { free(child); continue; }
                q = nq; cap = ncap;
            }
            q[tail++] = child;
        }
        closedir(d);
        free(rel);
    }
    free(q);
    return rc;
}

uint32_t nh_syncd_watcher_kernel_limit(void) {
    FILE *f = fopen("/proc/sys/fs/inotify/max_user_watches", "r");
    unsigned long v = 0;
    if (f) {
        if (fscanf(f, "%lu", &v) != 1) v = 0;
        fclose(f);
    }
    if (v == 0 || v > UINT32_MAX) return NH_WATCH_KERNEL_FALLBACK;
    return (uint32_t)v;
}

uint32_t nh_syncd_watcher_default_budget(void) {
    uint32_t lim = nh_syncd_watcher_kernel_limit();
    uint32_t def = lim / NH_WATCH_DEFAULT_DIVISOR;
    if (def > NH_WATCH_DEFAULT_CAP) def = NH_WATCH_DEFAULT_CAP;
    const char *ov = getenv("NOSTR_HOMED_SYNCD_MAX_WATCHES");
    if (!ov || !*ov) return def;
    char *end = NULL;
    errno = 0;
    unsigned long v = strtoul(ov, &end, 10);
    if (errno || !end || *end || ov[0] == '-') {
        fprintf(stderr, "syncd/watcher: ignoring malformed "
                        "NOSTR_HOMED_SYNCD_MAX_WATCHES='%s' (using %u)\n", ov, def);
        return def;
    }
    uint32_t ceiling = lim / NH_WATCH_OVERRIDE_DIVISOR;
    if (v > ceiling) {
        fprintf(stderr, "syncd/watcher: NOSTR_HOMED_SYNCD_MAX_WATCHES=%lu "
                        "clamped to %u (half of fs.inotify.max_user_watches)\n",
                v, ceiling);
        return ceiling;
    }
    return (uint32_t)v;
}

int nh_syncd_watcher_new_with_budget(const char *home,
                                     nh_syncd_ignore *ignore,
                                     nh_syncd_batcher *batcher,
                                     uint32_t budget,
                                     nh_syncd_watcher **out)
{
    if (!home || !ignore || !batcher || !out) return NH_SYNCD_ERR_ARG;
    struct nh_syncd_watcher *w = calloc(1, sizeof *w);
    if (!w) return NH_SYNCD_ERR_OOM;
    w->fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (w->fd < 0) { free(w); return NH_SYNCD_ERR_IO; }
    w->home = strdup(home);
    w->ignore = ignore;
    w->batcher = batcher;
    w->budget = budget;
    w->kernel_limit = nh_syncd_watcher_kernel_limit();
    w->by_rel = json_object();
    w->unwatched = json_object();
    if (!w->home || !w->by_rel || !w->unwatched) {
        nh_syncd_watcher_free(w);
        return NH_SYNCD_ERR_OOM;
    }
    if (watch_subtree(w, "", false) < 0) {
        nh_syncd_watcher_free(w);
        return NH_SYNCD_ERR_IO;
    }
    *out = w;
    return NH_SYNCD_OK;
}

int nh_syncd_watcher_new(const char *home,
                         nh_syncd_ignore *ignore,
                         nh_syncd_batcher *batcher,
                         nh_syncd_watcher **out)
{
    return nh_syncd_watcher_new_with_budget(home, ignore, batcher,
                                            nh_syncd_watcher_default_budget(),
                                            out);
}

/* xnxd part 2: attach the state pointer so rescans have a baseline to
 * diff against. Optional; safe on NULL watcher. */
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
uint32_t nh_syncd_watcher_watch_count(const struct nh_syncd_watcher *w) {
    return w ? w->n_watches : 0;
}
uint32_t nh_syncd_watcher_budget(const struct nh_syncd_watcher *w) {
    return w ? w->budget : 0;
}
size_t nh_syncd_watcher_unwatched_count(const struct nh_syncd_watcher *w) {
    return w ? json_object_size(w->unwatched) : 0;
}
int nh_syncd_watcher_is_watched(const struct nh_syncd_watcher *w, const char *rel) {
    return w && rel && json_object_get(w->by_rel, rel) != NULL;
}
int nh_syncd_watcher_unwatched_reason(const struct nh_syncd_watcher *w,
                                      const char *rel) {
    if (!w || !rel) return 0;
    json_t *v = json_object_get(w->unwatched, rel);
    return v ? (int)json_integer_value(v) : 0;
}

/* Public trigger — rebuild watches under $HOME then diff the whole tree
 * vs `state` into the batcher. Called on IN_Q_OVERFLOW and by the
 * operator-configured periodic rescan. Returns 0 on success (even if no
 * work fell out); < 0 on error. Never crashes on a NULL state — logs and
 * skips the diff. */
int nh_syncd_watcher_force_rescan(struct nh_syncd_watcher *w) {
    if (!w) return NH_SYNCD_ERR_ARG;
    w->rescan_count++;
    /* A create/delete race during an overflow may have dropped subtrees.
     * Directories we already own are skipped without a syscall; the
     * budget still applies. */
    w->kernel_full = false;
    (void)watch_subtree(w, "", false);
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

/* nostrc-lqm2 fallback tick: diff every unwatched root against state,
 * then try to turn over-budget roots back into real watches (budget may
 * have freed up since). Returns the number of roots rescanned, or < 0. */
int nh_syncd_watcher_rescan_unwatched(struct nh_syncd_watcher *w) {
    if (!w) return NH_SYNCD_ERR_ARG;
    size_t n = json_object_size(w->unwatched);
    if (n == 0) return 0;
    w->fallback_count++;
    char **roots = calloc(n, sizeof *roots);
    int  *why    = calloc(n, sizeof *why);
    if (!roots || !why) { free(roots); free(why); return NH_SYNCD_ERR_OOM; }
    size_t k = 0;
    const char *key; json_t *val;
    json_object_foreach(w->unwatched, key, val) {
        if (k == n) break;
        roots[k] = strdup(key);
        if (!roots[k]) continue;
        why[k++] = (int)json_integer_value(val);
    }

    nh_syncd_rescan_stats st = {0};
    int rc = NH_SYNCD_OK;
    if (w->state)
        rc = nh_syncd_rescan_diff_roots(w->state, w->home,
                                        (const char *const *)roots, k,
                                        w->ignore, w->batcher, &st);

    /* Re-promote after the diff so changes made while unwatched were
     * already captured; prune roots that no longer exist. */
    w->kernel_full = false;
    for (size_t i = 0; i < k; i++) {
        if (!json_object_get(w->unwatched, roots[i])) continue;
        char *abs = abs_of(w, roots[i]);
        struct stat sb;
        if (abs && lstat(abs, &sb) != 0 && errno == ENOENT) {
            json_object_del(w->unwatched, roots[i]);
        } else if (why[i] == UNWATCHED_BUDGET &&
                   w->n_watches < w->budget && !w->kernel_full) {
            json_object_del(w->unwatched, roots[i]);
            (void)watch_subtree(w, roots[i], false);
        }
        free(abs);
    }
    fprintf(stderr,
            "syncd/watcher: fallback rescan roots=%zu added=%llu modified=%llu"
            " deleted=%llu watches=%u/%u unwatched=%zu rc=%d\n",
            k, (unsigned long long)st.added, (unsigned long long)st.modified,
            (unsigned long long)st.deleted, w->n_watches, w->budget,
            json_object_size(w->unwatched), rc);
    for (size_t i = 0; i < k; i++) free(roots[i]);
    free(roots); free(why);
    return rc < 0 ? rc : (int)k;
}

void nh_syncd_watcher_free(struct nh_syncd_watcher *w) {
    if (!w) return;
    if (w->fd >= 0) close(w->fd);
    for (int i = 0; i < 256; i++) {
        wd_ent *e = w->bucket[i];
        while (e) { wd_ent *n = e->next; free(e->rel); free(e); e = n; }
    }
    json_decref(w->by_rel);
    json_decref(w->unwatched);
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
            if (len < 0 && errno == EINTR) continue;
            if (len < 0 && errno != EAGAIN) return -1;
            /* Queue drained. Rescan only now, so a long walk does not
             * sit between us and a still-filling kernel queue. */
            if (w->overflow_pending) {
                w->overflow_pending = false;
                (void)nh_syncd_watcher_force_rescan(w);
            }
            return n_events;
        }
        for (char *p = buf; p < buf + len; ) {
            struct inotify_event *ev = (struct inotify_event *)p;
            p += sizeof(struct inotify_event) + ev->len;
            n_events++;
            /* xnxd part 2: IN_Q_OVERFLOW arrives as a synthetic event
             * with wd == -1. The kernel has dropped an unknown number
             * of events; the only correct response is to walk $HOME
             * and diff against state (once the queue is drained). */
            if (ev->mask & IN_Q_OVERFLOW) {
                w->overflow_count++;
                fprintf(stderr,
                        "syncd/watcher: WARNING IN_Q_OVERFLOW #%llu — "
                        "full-tree rescan queued\n",
                        (unsigned long long)w->overflow_count);
                w->overflow_pending = true;
                continue;
            }
            /* wds are allocated cyclically by the kernel, so a stale
             * IN_IGNORED for a wd we already forgot cannot name a newer
             * watch; wd_forget ignores unknown wds. */
            /* The kernel dropped the watch (dir deleted, unmounted, or
             * we removed it): the only event that returns budget. */
            if (ev->mask & IN_IGNORED) {
                wd_forget(w, ev->wd);
                continue;
            }
            const char *dir_rel = wd_lookup(w, ev->wd);
            if (!dir_rel || ev->len == 0) continue;
            char *child = join_rel(dir_rel, ev->name);
            if (!child) continue;
            bool is_dir = (ev->mask & IN_ISDIR) != 0;
            /* A directory left this path: its recorded watches and
             * unwatched roots are stale. (Its IN_MOVE_SELF needs no
             * handling of its own.) A move out reports nothing for the
             * contents, and neither does a delete of content we were not
             * watching: drop those snapshot entries explicitly. */
            if (is_dir && (ev->mask & (IN_MOVED_FROM | IN_DELETE))) {
                int blind = drop_subtree(w, child);
                if ((ev->mask & IN_MOVED_FROM) || blind)
                    delete_state_below(w, child);
            }
            if (nh_syncd_ignore_check_path(w->ignore, child) == NH_SYNCD_IGNORE_PASS) {
                nh_syncd_change_kind k = 0;
                if (ev->mask & (IN_CREATE | IN_MOVED_TO))       k = NH_SYNCD_CHANGE_CREATE;
                else if (ev->mask & (IN_DELETE | IN_MOVED_FROM)) k = NH_SYNCD_CHANGE_DELETE;
                else if (ev->mask & (IN_CLOSE_WRITE | IN_ATTRIB)) k = NH_SYNCD_CHANGE_MODIFY;
                if (k) nh_syncd_batcher_push(w->batcher, child, k);
                /* Watch newly-created / moved-in directories and report
                 * what is already inside them. */
                if (is_dir && (ev->mask & (IN_CREATE | IN_MOVED_TO)))
                    (void)watch_subtree(w, child, true);
            }
            free(child);
        }
    }
}
