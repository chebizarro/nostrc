/*
 * test_syncd_broker_seed_drop.c — W(3) integration coverage.
 *
 * SPDX-License-Identifier: MIT
 *
 * Boots the nostr-home-syncd binary against a fake broker-drop file:
 *   1. Writes 64 hex chars to $TMPDIR/nh_seed_drop_<pid>/home_seed.
 *   2. Points NOSTR_HOMED_SYNCD_SEED_FILE at that path.
 *   3. Runs `nostr-home-syncd --check` with NO NOSTR_HOMED_SYNCD_SEED_HEX
 *      env set (proving the drop-file path is what supplied the seed).
 *   4. Asserts the daemon exited 0 (dry-run success) AND that the
 *      seed file was unlink(2)ed (broker-owned; consumed by the
 *      daemon on start).
 *
 * Also asserts the env fallback still works: a second run with the
 * drop file absent and NOSTR_HOMED_SYNCD_SEED_HEX set still exits 0.
 *
 * Bead: nostrc-p6qp (W(3)).
 */

#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static const char SEED_HEX[65] =
    "1122334455667788112233445566778811223344556677881122334455667788";

static void rm_rf(const char *p) {
    char c[512]; snprintf(c, sizeof c, "rm -rf '%s'", p); (void)system(c);
}

static int write_file(const char *path, const char *data, size_t n) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    ssize_t w = write(fd, data, n);
    close(fd);
    return (w == (ssize_t)n) ? 0 : -1;
}

static int file_exists(const char *path) {
    struct stat st; return stat(path, &st) == 0 ? 1 : 0;
}

/* Spawn the daemon binary at `bin` with --check and env overrides.
 * Returns child exit status (>= 0) or -1 on spawn failure. */
static int run_daemon_check(const char *bin,
                            const char *state_dir,
                            const char *home,
                            const char *seed_file /* nullable */,
                            const char *seed_hex_env /* nullable */) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        /* Child. */
        setenv("NOSTR_HOMED_SYNCD_HOME", home, 1);
        setenv("NOSTR_HOMED_SYNCD_STATE_DIR", state_dir, 1);
        unsetenv("NOSTR_HOMED_SYNCD_SEED_FILE");
        unsetenv("NOSTR_HOMED_SYNCD_SEED_HEX");
        if (seed_file) setenv("NOSTR_HOMED_SYNCD_SEED_FILE", seed_file, 1);
        if (seed_hex_env) setenv("NOSTR_HOMED_SYNCD_SEED_HEX", seed_hex_env, 1);
        /* Provide dummies for other required-in-prod env; --check exits
         * before opening sockets so URL contents don't matter. */
        setenv("NOSTR_HOMED_SYNCD_NSEC_HEX", SEED_HEX, 1);
        setenv("NOSTR_HOMED_SYNCD_BLOSSOM", "https://ignored.example", 1);
        setenv("NOSTR_HOMED_SYNCD_RELAYS", "wss://ignored.example", 1);
        execl(bin, bin, "--check", (char *)NULL);
        perror("execl");
        _exit(127);
    }
    int st = 0;
    if (waitpid(pid, &st, 0) < 0) return -1;
    if (!WIFEXITED(st)) return -1;
    return WEXITSTATUS(st);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <path-to-nostr-home-syncd>\n", argv[0]);
        return 64;
    }
    const char *bin = argv[1];
    if (access(bin, X_OK) != 0) {
        fprintf(stderr, "daemon binary not executable: %s\n", bin);
        return 64;
    }

    char root[128];
    snprintf(root, sizeof root, "/tmp/nh_syncd_bs_%d", (int)getpid());
    rm_rf(root);
    char home[192], state[192], seed_dir[192], seed_path[224];
    snprintf(home,      sizeof home,      "%s/home",  root);
    snprintf(state,     sizeof state,     "%s/state", root);
    snprintf(seed_dir,  sizeof seed_dir,  "%s/drop",  root);
    snprintf(seed_path, sizeof seed_path, "%s/home_seed", seed_dir);
    assert(mkdir(root, 0700) == 0);
    assert(mkdir(home, 0700) == 0);
    assert(mkdir(state, 0700) == 0);
    assert(mkdir(seed_dir, 0700) == 0);

    /* --- Scenario A: broker-drop file present. --- */
    assert(write_file(seed_path, SEED_HEX, 64) == 0);
    assert(file_exists(seed_path));

    int rc = run_daemon_check(bin, state, home, seed_path, NULL);
    fprintf(stderr, "scenario A: exit=%d, drop-file-still-present=%d\n",
            rc, file_exists(seed_path));
    assert(rc == 0);
    /* The daemon MUST have consumed + unlinked the drop file. */
    assert(!file_exists(seed_path));

    /* --- Scenario B: no drop file; env fallback. --- */
    /* Clear state — the additive-rescan marker from A shouldn't affect
     * this scenario, but a clean rerun is easier to reason about. */
    rm_rf(state); assert(mkdir(state, 0700) == 0);
    /* No drop file. */
    assert(!file_exists(seed_path));
    rc = run_daemon_check(bin, state, home, seed_path /* still points at absent file */,
                          SEED_HEX);
    fprintf(stderr, "scenario B: exit=%d\n", rc);
    assert(rc == 0);
    /* Drop file was absent; the daemon MUST NOT have created it. */
    assert(!file_exists(seed_path));

    /* --- Scenario C: drop file present AND env set — drop wins.
     * We can't easily inspect *which* seed the daemon used from --check
     * (it doesn't push), but we assert that the daemon unlinked the
     * drop file (proves it took the broker path first). --- */
    assert(write_file(seed_path, SEED_HEX, 64) == 0);
    rm_rf(state); assert(mkdir(state, 0700) == 0);
    rc = run_daemon_check(bin, state, home, seed_path, SEED_HEX);
    fprintf(stderr, "scenario C: exit=%d, drop-file-still-present=%d\n",
            rc, file_exists(seed_path));
    assert(rc == 0);
    assert(!file_exists(seed_path));

    rm_rf(root);
    printf("ok\n");
    return 0;
}
