/*
 * nh_syncd_spawn.h — run the real nostr-home-syncd binary in a
 * throwaway sandbox (HOME, XDG dirs, state dir, seed path all under one
 * tmp root) with stderr captured to a log file. Header-only; shared by
 * the daemon-level integration tests.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef NH_SYNCD_SPAWN_H
#define NH_SYNCD_SPAWN_H

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    char root[128];
    char home[192];
    char state[192];
    char seed_path[256];   /* NOSTR_HOMED_SYNCD_SEED_FILE */
    char log[192];         /* daemon stderr */
} nh_syncd_sandbox;

static inline void nh_syncd_sandbox_init(nh_syncd_sandbox *sb, const char *tag,
                                         const char *seed_rel) {
    snprintf(sb->root, sizeof sb->root, "/tmp/nh_%s_%d", tag, (int)getpid());
    snprintf(sb->home, sizeof sb->home, "%s/home", sb->root);
    snprintf(sb->state, sizeof sb->state, "%s/state", sb->root);
    snprintf(sb->seed_path, sizeof sb->seed_path, "%s/%s", sb->root, seed_rel);
    snprintf(sb->log, sizeof sb->log, "%s/daemon.log", sb->root);
}

/* Fork+exec the daemon (`check` adds --check). extra_env: NULL-terminated
 * "K=V" list applied last. Returns the child pid, or -1. */
static inline pid_t nh_syncd_spawn(const char *bin, const nh_syncd_sandbox *sb,
                                   bool check, const char *const *extra_env) {
    pid_t pid = fork();
    if (pid != 0) return pid;
    int lfd = open(sb->log, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (lfd >= 0) { dup2(lfd, 2); close(lfd); }
    char buf[256];
    setenv("HOME", sb->home, 1);
    setenv("NOSTR_HOMED_SYNCD_HOME", sb->home, 1);
    setenv("NOSTR_HOMED_SYNCD_STATE_DIR", sb->state, 1);
    snprintf(buf, sizeof buf, "%s/xdg-state", sb->root);  setenv("XDG_STATE_HOME", buf, 1);
    snprintf(buf, sizeof buf, "%s/xdg-cache", sb->root);  setenv("XDG_CACHE_HOME", buf, 1);
    snprintf(buf, sizeof buf, "%s/xdg-config", sb->root); setenv("XDG_CONFIG_HOME", buf, 1);
    setenv("NOSTR_HOMED_SYNCD_SEED_FILE", sb->seed_path, 1);
    unsetenv("NOSTR_HOMED_SYNCD_SEED_HEX");
    unsetenv("NOSTR_HOMED_SYNCD_TEST_MODE");
    unsetenv("NOSTR_HOMED_SYNCD_SNAPSHOT_MAX_BYTES");
    unsetenv("NOSTR_HOME_STATE");
    /* Never reaches the network: nothing in $HOME changes, so no batch. */
    setenv("NOSTR_HOMED_SYNCD_NSEC_HEX",
           "1122334455667788112233445566778811223344556677881122334455667788", 1);
    setenv("NOSTR_HOMED_SYNCD_BLOSSOM", "https://ignored.invalid", 1);
    setenv("NOSTR_HOMED_SYNCD_RELAYS", "wss://ignored.invalid", 1);
    for (size_t i = 0; extra_env && extra_env[i]; i++) putenv((char *)extra_env[i]);
    if (check) execl(bin, bin, "--check", (char *)NULL);
    else       execl(bin, bin, (char *)NULL);
    _exit(127);
}

static inline double nh_syncd_now(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Wait for exit up to `secs`. Returns exit status, or -1 on timeout /
 * abnormal termination (the child is then killed). */
static inline int nh_syncd_wait(pid_t pid, double secs) {
    double end = nh_syncd_now() + secs;
    for (;;) {
        int st = 0;
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        if (r < 0 || nh_syncd_now() > end) {
            kill(pid, SIGKILL); waitpid(pid, NULL, 0); return -1;
        }
        usleep(20000);
    }
}

/* SIGTERM and collect the exit status. */
static inline int nh_syncd_stop(pid_t pid) {
    kill(pid, SIGTERM);
    return nh_syncd_wait(pid, 20.0);
}

/* Does the daemon log contain `needle`? */
static inline bool nh_syncd_log_has(const nh_syncd_sandbox *sb, const char *needle) {
    FILE *f = fopen(sb->log, "r");
    if (!f) return false;
    char line[1024];
    bool hit = false;
    while (!hit && fgets(line, sizeof line, f)) hit = strstr(line, needle) != NULL;
    fclose(f);
    return hit;
}

/* Poll `pred` (bounded) — the daemon is a separate process with no
 * readiness channel other than its log and the files it writes. */
static inline bool nh_syncd_until(bool (*pred)(void *), void *ud, double secs) {
    double end = nh_syncd_now() + secs;
    while (nh_syncd_now() < end) {
        if (pred(ud)) return true;
        usleep(20000);
    }
    return pred(ud);
}

static inline void nh_syncd_dump_log(const nh_syncd_sandbox *sb) {
    FILE *f = fopen(sb->log, "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof line, f)) fprintf(stderr, "  | %s", line);
    fclose(f);
}

#endif
