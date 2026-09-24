/*
 * test_fuse_evict_notify.c — bead nostrc-k4j4.
 *
 * The FUSE process opens its own nh_syncd_cache with auto-evict ON.
 * Before this fix, evictions there produced no user-visible signal
 * because only the syncd process wired nh_syncd_cache_set_evict_notify
 * to an nh_porthome_notifier. This test exercises the fuse-side wiring
 * WITHOUT spawning a real mount:
 *
 *   1. Open a tiny 100-byte quota cache with auto-evict ON.
 *   2. Register a fuse-side notifier (fixed-clock + stub backend) + a
 *      fuse-side status writer.
 *   3. Register an evict-thrash callback that calls
 *      nh_porthome_notify(..., NH_NOTIFY_CAT_LIMITED_MODE,
 *                         "fuse-cache-thrashing", ...).
 *   4. Drive N > threshold puts that force evictions.
 *   5. Assert:
 *      - the notifier fired the desktop backend exactly once per
 *        throttle window (matches syncd behaviour);
 *      - the record adapter appended into the writer's recent[] ring;
 *      - the emitted "fuse" key body contains the recent[] entry with
 *        category slug "limited-mode" and summary "portable home cache
 *        thrashing (fuse)".
 *
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include "nh_fuse_status_writer.h"
#include "nh_porthome_crypto.h"
#include "nh_porthome_notify.h"
#include "nh_porthome_status.h"
#include "nh_syncd_cache.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static void rm_rf(const char *p) {
    char c[512]; snprintf(c, sizeof c, "rm -rf '%s'", p); (void)system(c);
}

static void hex_of(const uint8_t *buf, size_t len, char out[65]) {
    uint8_t h[32];
    assert(nh_porthome_sha256(buf, len, h) == 0);
    nh_porthome_hex64(h, out);
}

/* Fixed clock for the notifier. */
static int64_t g_now = 1000000;
static int64_t clock_fn(void *ud) { (void)ud; return g_now; }

/* Backend hook — count how many times a desktop-visible notify would
 * have fired. */
static int g_backend_calls = 0;
static char g_last_summary[128] = {0};
static char g_last_body[192] = {0};
static void backend_fn(void *ud, const char *app, const char *icon,
                       const char *summary, const char *body) {
    (void)ud; (void)app; (void)icon;
    g_backend_calls++;
    if (summary) snprintf(g_last_summary, sizeof g_last_summary, "%s", summary);
    if (body)    snprintf(g_last_body,    sizeof g_last_body,    "%s", body);
}

/* Bridge: the cache invokes on_thrash → our on_thrash calls
 * nh_porthome_notify → notifier invokes record_fn → writer's
 * recent[] ring appends. */
static nh_porthome_notifier  *g_notifier = NULL;
static nh_fuse_status_writer *g_writer   = NULL;
static unsigned g_thrash_hits = 0;

static void on_thrash(void *ud, unsigned evict_count, unsigned threshold,
                      int64_t first_evict_epoch) {
    (void)ud; (void)first_evict_epoch;
    g_thrash_hits++;
    char body[192];
    snprintf(body, sizeof body,
             "%u cache evictions in the last hour (threshold %u).",
             evict_count, threshold);
    (void)nh_porthome_notify(g_notifier,
                             "nostr-home-fuse",
                             "folder-remote",
                             "portable home cache thrashing (fuse)",
                             body,
                             NH_NOTIFY_CAT_LIMITED_MODE,
                             "fuse-cache-thrashing");
}

int main(void) {
    char rootdir[128];
    snprintf(rootdir, sizeof rootdir, "/tmp/nh_fuse_ev_%d", (int)getpid());
    rm_rf(rootdir);
    (void)mkdir(rootdir, 0700);

    /* Route porthome-status.json into this dir so we don't touch the
     * user's real state directory. */
    char state_home[200];
    snprintf(state_home, sizeof state_home, "%s/state", rootdir);
    (void)mkdir(state_home, 0700);
    setenv("XDG_STATE_HOME", state_home, 1);

    /* Route the notifier's quiet-hours config into an empty tmp dir. */
    char cfg_home[200];
    snprintf(cfg_home, sizeof cfg_home, "%s/cfg", rootdir);
    (void)mkdir(cfg_home, 0700);
    setenv("XDG_CONFIG_HOME", cfg_home, 1);

    char cache_dir[200];
    snprintf(cache_dir, sizeof cache_dir, "%s/cache", rootdir);

    /* Threshold 3 via env, quota 100 bytes → every 100-byte put forces
     * an eviction. */
    setenv("NOSTR_HOMED_PORTHOME_QUOTA_THRASH_THRESHOLD", "3", 1);

    nh_syncd_cache *c = NULL;
    assert(nh_syncd_cache_open(cache_dir, 100, &c) == NH_SYNCD_CACHE_OK);
    nh_syncd_cache_set_auto_evict(c, true);
    assert(nh_syncd_cache_thrash_threshold(c) == 3u);

    g_notifier = nh_porthome_notifier_new();
    assert(g_notifier);
    nh_porthome_notifier_set_backend(g_notifier, backend_fn, NULL);
    nh_porthome_notifier_set_clock  (g_notifier, clock_fn,   NULL);
    nh_porthome_notifier_set_window (g_notifier, 600);

    g_writer = nh_fuse_status_writer_new();
    assert(g_writer);
    nh_fuse_status_writer_set_mountpoint(g_writer, "/tmp/fake-portable");
    nh_porthome_notifier_set_record(g_notifier,
        nh_fuse_status_writer_notify_record, g_writer);

    nh_syncd_cache_set_evict_notify(c, on_thrash, NULL);

    /* Drive 6 distinct 100-byte blobs. */
    uint8_t buf[100];
    for (int i = 0; i < 6; ++i) {
        memset(buf, 0, sizeof buf);
        buf[0] = (uint8_t)('a' + i);
        char hex[65]; hex_of(buf, sizeof buf, hex);
        (void)nh_syncd_cache_put(c, hex, buf, sizeof buf);
    }

    assert(g_thrash_hits >= 1);
    /* Notifier throttled to one desktop delivery within the 600s
     * window. */
    assert(g_backend_calls == 1);
    assert(strstr(g_last_summary, "portable home cache thrashing (fuse)") != NULL);

    /* Recent ring should hold the one delivered event. */
    unsigned rc = nh_fuse_status_writer_recent_count(g_writer);
    assert(rc >= 1u);

    /* Emit and inspect the JSON — must land in the "fuse" key with
     * category limited-mode. */
    assert(nh_fuse_status_writer_emit(g_writer, NULL) == 0);
    char *status_path = nh_porthome_status_default_path();
    assert(status_path);
    int fd = open(status_path, O_RDONLY | O_CLOEXEC);
    assert(fd >= 0);
    struct stat st; assert(fstat(fd, &st) == 0);
    char *body = malloc((size_t)st.st_size + 1);
    assert(body);
    ssize_t n = read(fd, body, (size_t)st.st_size);
    assert(n > 0);
    body[n] = '\0';
    close(fd);

    assert(strstr(body, "\"fuse\"") != NULL);
    assert(strstr(body, "\"recent\"") != NULL);
    assert(strstr(body, "\"limited_mode\"") != NULL);
    assert(strstr(body, "portable home cache thrashing (fuse)") != NULL);
    /* Mountpoint recorded. */
    assert(strstr(body, "/tmp/fake-portable") != NULL);

    /* A second thrash-fire within the throttle window should NOT
     * deliver another desktop pop but SHOULD still be recorded (design
     * §D9: recent[] mirrors every attempt, delivered flag flips). */
    unsigned before = nh_fuse_status_writer_recent_count(g_writer);
    (void)nh_porthome_notify(g_notifier,
                             "nostr-home-fuse", "folder-remote",
                             "portable home cache thrashing (fuse)",
                             "6 more evictions.",
                             NH_NOTIFY_CAT_LIMITED_MODE,
                             "fuse-cache-thrashing");
    assert(g_backend_calls == 1);
    unsigned after = nh_fuse_status_writer_recent_count(g_writer);
    assert(after == before + 1u);

    /* Advance clock past the throttle window; deliver again. */
    g_now += 601;
    (void)nh_porthome_notify(g_notifier,
                             "nostr-home-fuse", "folder-remote",
                             "portable home cache thrashing (fuse) again",
                             "6 more evictions later.",
                             NH_NOTIFY_CAT_LIMITED_MODE,
                             "fuse-cache-thrashing");
    assert(g_backend_calls == 2);

    free(body);
    free(status_path);
    nh_fuse_status_writer_free(g_writer);
    nh_porthome_notifier_free(g_notifier);
    nh_syncd_cache_close(c);
    rm_rf(rootdir);
    unsetenv("NOSTR_HOMED_PORTHOME_QUOTA_THRASH_THRESHOLD");
    unsetenv("XDG_STATE_HOME");
    unsetenv("XDG_CONFIG_HOME");
    printf("test_fuse_evict_notify OK (thrash_hits=%u backend_calls=%d)\n",
           g_thrash_hits, g_backend_calls);
    return 0;
}
