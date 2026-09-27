/*
 * test_syncd_watcher_budget.c — nostrc-lqm2: the syncd inotify watcher
 * must never hold more watches than its budget, must skip noisy trees,
 * and must cover whatever it does not watch with the fallback rescan.
 *
 * SPDX-License-Identifier: MIT
 *
 * Watch counts are read from the kernel (/proc/self/fdinfo/<fd>, one
 * "inotify wd:" line per watch), not just from the watcher's own
 * bookkeeping, so a leaked or double-counted watch fails the test.
 *
 * Synthetic tree (112 watchable directories):
 *   top00..top09/sub0..sub9   110
 *   proj/                       1   (+ proj/node_modules/pkg*: noisy)
 *   (root)                      1
 *   .git/objects/aa..         noisy (VCS)
 *   build/ + CACHEDIR.TAG     noisy (tagged cache)
 *   .cache/x                  out of sync scope entirely
 */

#include "nh_syncd.h"
#include "nh_syncd_watcher.h"

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../nh_test_fs.h"

static char g_home[64];
static const char *FIXTURE_PK =
    "0000000000000000000000000000000000000000000000000000000000000001";

#define WATCHABLE 112u

static void mkdirf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void mkdirf(const char *fmt, ...) {
    char rel[256], abs[512];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(rel, sizeof rel, fmt, ap);
    va_end(ap);
    assert(n > 0 && (size_t)n < sizeof rel);
    n = snprintf(abs, sizeof abs, "%s/%s", g_home, rel);
    assert(n > 0 && (size_t)n < sizeof abs);
    assert(mkdir(abs, 0700) == 0 || errno == EEXIST);
}

static void write_rel(const char *rel, const char *body) {
    char abs[512];
    int n = snprintf(abs, sizeof abs, "%s/%s", g_home, rel);
    assert(n > 0 && (size_t)n < sizeof abs);
    FILE *f = fopen(abs, "w");
    assert(f);
    fputs(body, f);
    fclose(f);
}

static void build_tree(void) {
    snprintf(g_home, sizeof g_home, "/tmp/nh_watch_budget_XXXXXX");
    assert(mkdtemp(g_home));
    for (int t = 0; t < 10; t++) {
        mkdirf("top%02d", t);
        for (int s = 0; s < 10; s++) mkdirf("top%02d/sub%d", t, s);
    }
    mkdirf("proj");
    mkdirf("proj/node_modules");
    for (int i = 0; i < 10; i++) mkdirf("proj/node_modules/pkg%d", i);
    mkdirf(".git");
    mkdirf(".git/objects");
    for (int i = 0; i < 20; i++) mkdirf(".git/objects/%02x", i);
    mkdirf("build");
    write_rel("build/CACHEDIR.TAG", "Signature: 8a477f597d28d172789f06886806bc55\n");
    for (int i = 0; i < 10; i++) mkdirf("build/o%d", i);
    mkdirf(".cache");
    mkdirf(".cache/x");
    write_rel("top00/sub0/seed.txt", "seed\n");
    write_rel(".git/HEAD", "ref: refs/heads/master\n");
    write_rel("proj/node_modules/pkg0/index.js", "module.exports = 1;\n");
}

static unsigned fdinfo_watches(int fd) {
    char p[64];
    snprintf(p, sizeof p, "/proc/self/fdinfo/%d", fd);
    FILE *f = fopen(p, "r");
    assert(f);
    char line[512];
    unsigned n = 0;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, "inotify wd:", 11)) n++;
    fclose(f);
    return n;
}

static uint64_t vnow(void *ud) { (void)ud; static uint64_t t = 0; t += 10ull * 1000000000ull; return t; }

typedef struct {
    nh_syncd_ignore  *ig;
    nh_syncd_batcher *ba;
    nh_syncd_watcher *w;
} rig;

static void rig_up(rig *r, uint32_t budget) {
    memset(r, 0, sizeof *r);
    assert(nh_syncd_ignore_new(g_home, &r->ig) == NH_SYNCD_OK);
    assert(nh_syncd_batcher_new(vnow, NULL, 1, 1, &r->ba) == NH_SYNCD_OK);
    assert(nh_syncd_watcher_new_with_budget(g_home, r->ig, r->ba, budget, &r->w)
           == NH_SYNCD_OK);
}

static void rig_down(rig *r) {
    nh_syncd_watcher_free(r->w);
    nh_syncd_batcher_free(r->ba);
    nh_syncd_ignore_free(r->ig);
}

static void assert_kernel_count(const rig *r, unsigned want) {
    unsigned got = fdinfo_watches(nh_syncd_watcher_fd(r->w));
    if (got != want || nh_syncd_watcher_watch_count(r->w) != want) {
        fprintf(stderr, "watch count: kernel=%u watcher=%u want=%u\n",
                got, nh_syncd_watcher_watch_count(r->w), want);
        assert(0);
    }
}

/* Take everything pending; report whether `rel` was in it. */
static int drain_batch_has(nh_syncd_batcher *ba, const char *rel) {
    nh_syncd_batch *b = nh_syncd_batcher_take(ba, true);
    int hit = 0;
    for (size_t i = 0; b && i < nh_syncd_batch_len(b); i++) {
        const char *r = NULL; nh_syncd_change_kind k = 0;
        if (nh_syncd_batch_at(b, i, &r, &k) == 0 && r && !strcmp(r, rel)) hit = 1;
    }
    if (b) nh_syncd_batch_free(b);
    return hit;
}

static void t_unbounded_skips_noisy(void) {
    rig r; rig_up(&r, 1000);
    assert_kernel_count(&r, WATCHABLE);
    assert(nh_syncd_watcher_is_watched(r.w, ""));
    assert(nh_syncd_watcher_is_watched(r.w, "top09/sub9"));
    assert(nh_syncd_watcher_is_watched(r.w, "proj"));
    assert(!nh_syncd_watcher_is_watched(r.w, ".git"));
    assert(!nh_syncd_watcher_is_watched(r.w, ".git/objects/00"));
    assert(!nh_syncd_watcher_is_watched(r.w, "proj/node_modules"));
    assert(!nh_syncd_watcher_is_watched(r.w, "build"));
    assert(!nh_syncd_watcher_is_watched(r.w, ".cache"));
    assert(nh_syncd_watcher_unwatched_reason(r.w, ".git") == 1);
    assert(nh_syncd_watcher_unwatched_reason(r.w, "proj/node_modules") == 1);
    assert(nh_syncd_watcher_unwatched_reason(r.w, "build") == 1);
    /* Out of sync scope: neither watched nor queued for rescan. */
    assert(nh_syncd_watcher_unwatched_reason(r.w, ".cache") == 0);
    assert(nh_syncd_watcher_unwatched_count(r.w) == 3);
    rig_down(&r);
    printf("t_unbounded_skips_noisy OK\n");
}

static void t_budget_is_hard_and_breadth_first(void) {
    rig r; rig_up(&r, 20);
    assert_kernel_count(&r, 20);
    /* Breadth-first: every depth-1 directory beats every depth-2 one. */
    char rel[32];
    for (int t = 0; t < 10; t++) {
        snprintf(rel, sizeof rel, "top%02d", t);
        assert(nh_syncd_watcher_is_watched(r.w, rel));
    }
    assert(nh_syncd_watcher_is_watched(r.w, "proj"));
    /* root + 11 depth-1 + 8 depth-2 = 20; the other 92 subs are over
     * budget, plus the 3 noisy roots. */
    assert(nh_syncd_watcher_unwatched_count(r.w) == 92 + 3);
    rig_down(&r);
    printf("t_budget_is_hard_and_breadth_first OK\n");
}

static const char *first_unwatched_sub(const rig *r, char *buf, size_t n, int want_watched) {
    for (int t = 0; t < 10; t++)
        for (int s = 0; s < 10; s++) {
            if (t == 0 && s == 0) continue;   /* holds seed.txt */
            snprintf(buf, n, "top%02d/sub%d", t, s);
            if (nh_syncd_watcher_is_watched(r->w, buf) == want_watched) return buf;
        }
    return NULL;
}

static void t_fallback_rescan_covers_unwatched(void) {
    rig r; rig_up(&r, 20);
    uint8_t root[32] = {0};
    nh_syncd_state *state = NULL;
    assert(nh_syncd_state_new(g_home, "d", FIXTURE_PK, root, &state) == 0);
    assert(nh_syncd_rescan_home_additive(state, g_home, r.ig, NULL) == NH_SYNCD_OK);
    nh_syncd_watcher_set_state(r.w, state);
    (void)nh_syncd_batcher_take(r.ba, true);

    char cold[32], f_cold[64];
    assert(first_unwatched_sub(&r, cold, sizeof cold, 0));
    assert(nh_syncd_watcher_unwatched_reason(r.w, cold) == 2);
    snprintf(f_cold, sizeof f_cold, "%s/late.txt", cold);
    write_rel(f_cold, "written while unwatched\n");
    write_rel(".git/objects/00/obj", "blob\n");
    /* Nothing is watching those directories. */
    assert(nh_syncd_watcher_drain(r.w) == 0);

    int roots = nh_syncd_watcher_rescan_unwatched(r.w);
    assert(roots == 95);
    nh_syncd_batch *b = nh_syncd_batcher_take(r.ba, true);
    int saw_cold = 0, saw_git = 0;
    for (size_t i = 0; b && i < nh_syncd_batch_len(b); i++) {
        const char *rel = NULL; nh_syncd_change_kind k = 0;
        assert(nh_syncd_batch_at(b, i, &rel, &k) == 0);
        if (!strcmp(rel, f_cold)) { saw_cold = 1; assert(k == NH_SYNCD_CHANGE_CREATE); }
        if (!strcmp(rel, ".git/objects/00/obj")) { saw_git = 1; assert(k == NH_SYNCD_CHANGE_CREATE); }
        /* A watched, untouched directory must not be walked. */
        assert(strcmp(rel, "top00/sub0/seed.txt") != 0);
    }
    if (b) nh_syncd_batch_free(b);
    assert(saw_cold && saw_git);
    /* Budget still full: nothing promoted, nothing leaked. */
    assert_kernel_count(&r, 20);

    /* A directory created while the budget is full becomes a root. */
    mkdirf("top00/newdir");
    assert(nh_syncd_watcher_drain(r.w) >= 1);
    assert(nh_syncd_watcher_unwatched_reason(r.w, "top00/newdir") == 2);
    assert_kernel_count(&r, 20);

    /* Deleting a watched directory returns its watch (IN_IGNORED)... */
    char hot[32], abs[512];
    assert(first_unwatched_sub(&r, hot, sizeof hot, 1));
    snprintf(abs, sizeof abs, "%s/%s", g_home, hot);
    assert(rmdir(abs) == 0);
    (void)nh_syncd_watcher_drain(r.w);
    assert_kernel_count(&r, 19);
    /* ...and the next fallback tick spends it on an unwatched root. */
    assert(nh_syncd_watcher_rescan_unwatched(r.w) > 0);
    assert_kernel_count(&r, 20);

    rig_down(&r);
    nh_syncd_state_free(state);
    printf("t_fallback_rescan_covers_unwatched OK\n");
}

static void t_dir_move_does_not_leak(void) {
    rig r; rig_up(&r, 1000);
    unsigned base = nh_syncd_watcher_watch_count(r.w);
    char from[512], to[512];
    snprintf(from, sizeof from, "%s/top01", g_home);
    snprintf(to, sizeof to, "%s/top01-moved", g_home);
    assert(rename(from, to) == 0);
    (void)nh_syncd_watcher_drain(r.w);
    (void)drain_batch_has(r.ba, "");
    assert(!nh_syncd_watcher_is_watched(r.w, "top01"));
    assert(!nh_syncd_watcher_is_watched(r.w, "top01/sub3"));
    assert(nh_syncd_watcher_is_watched(r.w, "top01-moved"));
    assert(nh_syncd_watcher_is_watched(r.w, "top01-moved/sub3"));
    assert_kernel_count(&r, base);
    /* Events under the new name carry the new path. */
    write_rel("top01-moved/sub3/after-move.txt", "x\n");
    assert(nh_syncd_watcher_drain(r.w) >= 1);
    assert(drain_batch_has(r.ba, "top01-moved/sub3/after-move.txt"));
    assert(rename(to, from) == 0);
    (void)nh_syncd_watcher_drain(r.w);
    assert_kernel_count(&r, base);
    rig_down(&r);
    printf("t_dir_move_does_not_leak OK\n");
}

/* Does batch `b` hold (rel, want)? */
static int batch_has(nh_syncd_batch *b, const char *rel, nh_syncd_change_kind want) {
    for (size_t i = 0; b && i < nh_syncd_batch_len(b); i++) {
        const char *r = NULL; nh_syncd_change_kind k = 0;
        if (nh_syncd_batch_at(b, i, &r, &k) == 0 && r && !strcmp(r, rel) && k == want)
            return 1;
    }
    return 0;
}

/* Directories that leave or arrive in one event carry their whole
 * contents: a move out / delete of unwatched content must not leave
 * ghost snapshot entries, and a moved-in tree must be reported. */
static void t_dir_leave_and_arrive(void) {
    rig r; rig_up(&r, 1000);
    uint8_t root[32] = {0};
    nh_syncd_state *state = NULL;
    assert(nh_syncd_state_new(g_home, "d", FIXTURE_PK, root, &state) == 0);
    assert(nh_syncd_rescan_home_additive(state, g_home, r.ig, NULL) == NH_SYNCD_OK);
    nh_syncd_watcher_set_state(r.w, state);
    (void)nh_syncd_batcher_take(r.ba, true);

    /* 1. Move a tree (with an unwatched node_modules) out of $HOME. */
    char from[512], out[512];
    snprintf(from, sizeof from, "%s/proj", g_home);
    snprintf(out, sizeof out, "%s-outside", g_home);
    assert(rename(from, out) == 0);
    assert(nh_syncd_watcher_drain(r.w) >= 1);
    nh_syncd_batch *b = nh_syncd_batcher_take(r.ba, true);
    assert(batch_has(b, "proj", NH_SYNCD_CHANGE_DELETE));
    assert(batch_has(b, "proj/node_modules/pkg0/index.js", NH_SYNCD_CHANGE_DELETE));
    nh_syncd_batch_free(b);
    assert(!nh_syncd_watcher_is_watched(r.w, "proj"));
    assert(nh_syncd_watcher_unwatched_reason(r.w, "proj/node_modules") == 0);

    /* 2. Move it back under a new name: contents are announced; the
     * noisy part arrives through the fallback rescan. */
    char back[512];
    snprintf(back, sizeof back, "%s/proj2", g_home);
    assert(rename(out, back) == 0);
    assert(nh_syncd_watcher_drain(r.w) >= 1);
    b = nh_syncd_batcher_take(r.ba, true);
    assert(batch_has(b, "proj2", NH_SYNCD_CHANGE_CREATE));
    assert(batch_has(b, "proj2/node_modules", NH_SYNCD_CHANGE_CREATE));
    nh_syncd_batch_free(b);
    assert(nh_syncd_watcher_is_watched(r.w, "proj2"));
    assert(nh_syncd_watcher_unwatched_reason(r.w, "proj2/node_modules") == 1);
    assert(nh_syncd_watcher_rescan_unwatched(r.w) > 0);
    b = nh_syncd_batcher_take(r.ba, true);
    assert(batch_has(b, "proj2/node_modules/pkg0/index.js", NH_SYNCD_CHANGE_CREATE));
    nh_syncd_batch_free(b);

    /* 3. Delete an unwatched tree: no per-file events exist for it. */
    char git[512];
    snprintf(git, sizeof git, "%s/.git", g_home);
    nh_test_rm_rf(git);
    assert(nh_syncd_watcher_drain(r.w) >= 1);
    b = nh_syncd_batcher_take(r.ba, true);
    assert(batch_has(b, ".git", NH_SYNCD_CHANGE_DELETE));
    assert(batch_has(b, ".git/HEAD", NH_SYNCD_CHANGE_DELETE));
    nh_syncd_batch_free(b);
    assert(nh_syncd_watcher_unwatched_reason(r.w, ".git") == 0);

    /* 4. A directory created with content already in it (mkdir -p, cp -r
     * racing the watch) reports that content. */
    char tmpd[512], dst[512];
    snprintf(tmpd, sizeof tmpd, "%s-staged", g_home);
    assert(mkdir(tmpd, 0700) == 0);
    char f[600]; snprintf(f, sizeof f, "%s/early.txt", tmpd);
    FILE *fp = fopen(f, "w"); assert(fp); fputs("x\n", fp); fclose(fp);
    snprintf(dst, sizeof dst, "%s/top02/staged", g_home);
    assert(rename(tmpd, dst) == 0);
    assert(nh_syncd_watcher_drain(r.w) >= 1);
    b = nh_syncd_batcher_take(r.ba, true);
    assert(batch_has(b, "top02/staged/early.txt", NH_SYNCD_CHANGE_CREATE));
    nh_syncd_batch_free(b);

    assert_kernel_count(&r, nh_syncd_watcher_watch_count(r.w));
    rig_down(&r);
    nh_syncd_state_free(state);
    printf("t_dir_leave_and_arrive OK\n");
}

static void t_zero_budget_watches_nothing(void) {
    rig r; rig_up(&r, 0);
    assert_kernel_count(&r, 0);
    assert(nh_syncd_watcher_unwatched_reason(r.w, "") == 2);
    rig_down(&r);
    printf("t_zero_budget_watches_nothing OK\n");
}

static void t_default_budget(void) {
    uint32_t lim = nh_syncd_watcher_kernel_limit();
    uint32_t def = lim / 4 < 8192 ? lim / 4 : 8192;
    unsetenv("NOSTR_HOMED_SYNCD_MAX_WATCHES");
    assert(nh_syncd_watcher_default_budget() == def);
    assert(def < lim);
    setenv("NOSTR_HOMED_SYNCD_MAX_WATCHES", "37", 1);
    assert(nh_syncd_watcher_default_budget() == (lim / 2 < 37 ? lim / 2 : 37));
    setenv("NOSTR_HOMED_SYNCD_MAX_WATCHES", "4000000000", 1);
    assert(nh_syncd_watcher_default_budget() == lim / 2);
    setenv("NOSTR_HOMED_SYNCD_MAX_WATCHES", "lots", 1);
    assert(nh_syncd_watcher_default_budget() == def);
    unsetenv("NOSTR_HOMED_SYNCD_MAX_WATCHES");
    printf("t_default_budget OK (kernel limit %u, default %u)\n", lim, def);
}

/* The inotify watch limit is per user: if another process of this user
 * has exhausted it (the very bug this guards against — a pre-fix
 * nostr-home-syncd over a big $HOME), no kernel count can be asserted.
 * Check the degradation path instead and report SKIP. */
static int user_watch_budget_exhausted(void) {
    int fd = inotify_init1(IN_CLOEXEC);
    if (fd < 0) return 0;
    int wd = inotify_add_watch(fd, g_home, IN_CREATE);
    int full = wd < 0 && errno == ENOSPC;
    close(fd);
    return full;
}

int main(void) {
    build_tree();
    t_default_budget();
    if (user_watch_budget_exhausted()) {
        rig r; rig_up(&r, 1000);
        assert_kernel_count(&r, 0);
        assert(nh_syncd_watcher_unwatched_reason(r.w, "") == 2);
        rig_down(&r);
        nh_syncd_watcher_free(NULL);
        nh_test_rm_rf(g_home);
        printf("SKIP: this user's fs.inotify.max_user_watches is exhausted by "
               "another process (grep -c '^inotify wd' /proc/*/fdinfo/*); "
               "only the ENOSPC fallback was checked\n");
        return 77;
    }
    t_unbounded_skips_noisy();
    t_budget_is_hard_and_breadth_first();
    t_zero_budget_watches_nothing();
    t_dir_move_does_not_leak();
    t_fallback_rescan_covers_unwatched();
    t_dir_leave_and_arrive();   /* mutates the tree: keep last */
    nh_test_rm_rf(g_home);
    printf("test_syncd_watcher_budget: ALL OK\n");
    return 0;
}
