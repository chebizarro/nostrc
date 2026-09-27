/*
 * test_syncd_seed_late.c — nostrc-p8y6, daemon level.
 *
 * SPDX-License-Identifier: MIT
 *
 * The user unit can start before the broker drops the home seed. The
 * daemon must then wait for the drop and turn managed when it lands,
 * without a restart. The backstop poll is disabled
 * (NOSTR_HOMED_SYNCD_SEED_POLL_SEC=0) so both scenarios prove the
 * inotify path:
 *   1. none of the drop path exists at start; it is created one level
 *      at a time and the seed is renamed in (as the broker does);
 *   2. the per-uid dir exists but is not yet accessible (the broker
 *      mkdirs it root-owned 0700 and chowns it afterwards): it is
 *      chmodded, and the seed lands only later — pickup relies on the
 *      IN_ATTRIB re-arm.
 * In both, the daemon logs the transition, consumes (unlinks) the drop,
 * builds its baseline (additive rescan -> snapshot.json with the home's
 * file), and exits 0 on SIGTERM.
 */
#define _GNU_SOURCE
#include "nh_syncd.h"
#include "nh_syncd_spawn.h"
#include "../nh_test_fs.h"

#include <assert.h>

static const char SEED_HEX[] =
    "1122334455667788112233445566778811223344556677881122334455667788";

static nh_syncd_sandbox g_sb;

static void write_file(const char *path, const char *data) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0);
    assert(write(fd, data, strlen(data)) == (ssize_t)strlen(data));
    close(fd);
}

/* Broker-style drop: tmp sibling + rename. */
static void drop_seed(void) {
    char tmp[300]; snprintf(tmp, sizeof tmp, "%s.tmp.%d", g_sb.seed_path, (int)getpid());
    write_file(tmp, SEED_HEX);
    assert(rename(tmp, g_sb.seed_path) == 0);
}

static bool log_waiting(void *ud) { (void)ud; return nh_syncd_log_has(&g_sb, "Waiting for"); }
static bool log_managed(void *ud) {
    (void)ud;
    return nh_syncd_log_has(&g_sb, "now porthome-managed") &&
           nh_syncd_log_has(&g_sb, "syncd: watching ");
}
static bool baseline_built(void *ud) {
    (void)ud;
    bool unknown = true;
    nh_syncd_state *s = NULL;
    if (nh_syncd_state_load(g_sb.state, &s, &unknown) != NH_SYNCD_OK) return false;
    bool ok = !unknown && nh_syncd_state_find(s, "notes.txt") != NULL;
    nh_syncd_state_free(s);
    return ok;
}

static void fresh_sandbox(const char *tag) {
    nh_syncd_sandbox_init(&g_sb, tag, "run/session/1000/home_seed");
    nh_test_rm_rf(g_sb.root);
    assert(mkdir(g_sb.root, 0700) == 0);
    assert(mkdir(g_sb.home, 0700) == 0);
    assert(mkdir(g_sb.state, 0700) == 0);
    char note[256]; snprintf(note, sizeof note, "%s/notes.txt", g_sb.home);
    write_file(note, "hello\n");
}

static pid_t spawn_waiting(const char *bin) {
    static const char *const env[] = { "NOSTR_HOMED_SYNCD_SEED_POLL_SEC=0", NULL };
    pid_t pid = nh_syncd_spawn(bin, &g_sb, false, env);
    assert(pid > 0);
    if (!nh_syncd_until(log_waiting, NULL, 20.0)) { nh_syncd_dump_log(&g_sb); assert(0); }
    /* Not managed: no watcher, no baseline yet. */
    assert(!nh_syncd_log_has(&g_sb, "syncd: watching "));
    assert(!baseline_built(NULL));
    return pid;
}

static void expect_managed(pid_t pid, const char *what) {
    bool managed = nh_syncd_until(log_managed, NULL, 20.0);
    bool built = managed && nh_syncd_until(baseline_built, NULL, 20.0);
    struct stat st;
    bool consumed = stat(g_sb.seed_path, &st) != 0;
    int rc = nh_syncd_stop(pid);
    if (!managed || !built || !consumed || rc != 0) {
        fprintf(stderr, "%s: managed=%d baseline=%d consumed=%d exit=%d\n",
                what, managed, built, consumed, rc);
        nh_syncd_dump_log(&g_sb);
    }
    assert(managed && built && consumed && rc == 0);
    assert(nh_syncd_partial_state_is_set(g_sb.state));
    fprintf(stderr, "%s ok\n", what);
}

static void mkdir_under_root(const char *rel, mode_t mode) {
    char p[256]; snprintf(p, sizeof p, "%s/%s", g_sb.root, rel);
    assert(mkdir(p, mode) == 0);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <nostr-home-syncd>\n", argv[0]); return 64; }
    const char *bin = argv[1];

    /* 1 — the whole drop path appears after start. */
    fresh_sandbox("syncd_seedlate1");
    pid_t pid = spawn_waiting(bin);
    mkdir_under_root("run", 0755);
    mkdir_under_root("run/session", 0755);
    mkdir_under_root("run/session/1000", 0700);
    drop_seed();
    expect_managed(pid, "scenario 1 (path created after start)");
    nh_test_rm_rf(g_sb.root);

    /* 2 — per-uid dir present but inaccessible until "chown". Root can
     * traverse a 0000 dir, so this degenerates to scenario 1 there. */
    fresh_sandbox("syncd_seedlate2");
    mkdir_under_root("run", 0755);
    mkdir_under_root("run/session", 0755);
    mkdir_under_root("run/session/1000", 0000);
    pid = spawn_waiting(bin);
    char udir[256]; snprintf(udir, sizeof udir, "%s/run/session/1000", g_sb.root);
    assert(chmod(udir, 0700) == 0);
    /* Let the daemon re-arm on the IN_ATTRIB before the seed lands, so
     * the rename is only visible through the new watch. */
    usleep(300000);
    drop_seed();
    expect_managed(pid, "scenario 2 (dir accessible only after chmod)");
    nh_test_rm_rf(g_sb.root);

    printf("ok\n");
    return 0;
}
