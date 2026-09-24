/*
 * auth_porthome_fetch.c — fork+exec of the nostr-home-fetch helper.
 * See auth_porthome_fetch.h. Bead nostrc-9k4g.
 *
 * SPDX-License-Identifier: MIT
 */

#define _GNU_SOURCE

#include "auth_porthome_fetch.h"

#ifdef NH_AUTH_BROKER_ENABLE_PORTHOME

#include "../porthome-fetch/porthome_fetch_ctl.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <openssl/crypto.h>

/* Compile-time default. Can be overridden via CMake:
 *   -DNH_PORTHOME_FETCH_HELPER_PATH="/opt/whatever/nostr-home-fetch" */
#ifndef NH_PORTHOME_FETCH_HELPER_PATH
#define NH_PORTHOME_FETCH_HELPER_PATH \
    "/usr/libexec/nostr-homed/nostr-home-fetch"
#endif

static const char *g_helper_override = NULL;

void nh_auth_porthome_fetch_set_helper_path(const char *path) {
    /* Not thread-safe; broker calls this only at startup / in tests. */
    g_helper_override = path;
}

const char *nh_auth_porthome_fetch_helper_path(void) {
    if (g_helper_override && g_helper_override[0]) return g_helper_override;
    const char *env = getenv("NH_PORTHOME_FETCH_HELPER");
    if (env && env[0]) return env;
    return NH_PORTHOME_FETCH_HELPER_PATH;
}

/* Emit the control payload as JSON into `buf`. Returns length or -1 on
 * bound overflow. Deliberately hand-rolled — jansson is a link-time
 * dep the broker doesn't need and we already write JSON for the greeter
 * progress artifact. */
static int build_control_json(char *buf, size_t cap,
                              const nh_auth_porthome_fetch_args *a)
{
    /* Assemble URL arrays into JSON-array text. Bounded. */
    char relays_buf[4096];
    char servers_buf[4096];
    size_t rp = 0, sp = 0;
    #define APPENDC(dst, cap, dp, ch) do { \
        if ((dp) + 1 >= (cap)) return -1; \
        (dst)[(dp)++] = (ch); \
    } while (0)
    #define APPENDS(dst, cap, dp, s) do { \
        for (const char *__p = (s); *__p; __p++) { \
            APPENDC(dst, cap, dp, *__p); \
        } \
    } while (0)

    APPENDC(relays_buf, sizeof relays_buf, rp, '[');
    for (size_t i = 0; i < a->relays_count; i++) {
        if (i) APPENDC(relays_buf, sizeof relays_buf, rp, ',');
        APPENDC(relays_buf, sizeof relays_buf, rp, '"');
        /* No escapes: relays are wss:// URLs sanitised upstream. */
        for (const char *p = a->relays[i]; *p; p++) {
            unsigned char c = (unsigned char)*p;
            if (c < 0x20 || c > 0x7E || c == '"' || c == '\\') return -1;
            APPENDC(relays_buf, sizeof relays_buf, rp, (char)c);
        }
        APPENDC(relays_buf, sizeof relays_buf, rp, '"');
    }
    APPENDC(relays_buf, sizeof relays_buf, rp, ']');
    if (rp >= sizeof relays_buf) return -1;
    relays_buf[rp] = '\0';

    APPENDC(servers_buf, sizeof servers_buf, sp, '[');
    for (size_t i = 0; i < a->blossom_servers_count; i++) {
        if (i) APPENDC(servers_buf, sizeof servers_buf, sp, ',');
        APPENDC(servers_buf, sizeof servers_buf, sp, '"');
        for (const char *p = a->blossom_servers[i]; *p; p++) {
            unsigned char c = (unsigned char)*p;
            if (c < 0x20 || c > 0x7E || c == '"' || c == '\\') return -1;
            APPENDC(servers_buf, sizeof servers_buf, sp, (char)c);
        }
        APPENDC(servers_buf, sizeof servers_buf, sp, '"');
    }
    APPENDC(servers_buf, sizeof servers_buf, sp, ']');
    if (sp >= sizeof servers_buf) return -1;
    servers_buf[sp] = '\0';

    #undef APPENDC
    #undef APPENDS

    int n = snprintf(buf, cap,
        "{"
        "\"account_pubkey_hex\":\"%s\","
        "\"home_root_id_hex\":\"%s\","
        "\"home_key_hex\":\"%s\","
        "\"d_tag\":\"%s\","
        "\"relays\":%s,"
        "\"blossom_servers\":%s,"
        "\"bandwidth_cap_bytes\":%llu,"
        "\"per_file_timeout_sec\":%u,"
        "\"max_total_bytes\":%llu,"
        "\"relay_timeout_ms\":%u,"
        "\"allow_insecure\":%s"
        "}",
        a->account_pubkey_hex, a->home_root_id_hex, a->home_key_hex,
        a->d_tag, relays_buf, servers_buf,
        (unsigned long long)a->bandwidth_cap_bytes,
        a->per_file_timeout_sec,
        (unsigned long long)a->max_total_bytes,
        a->relay_timeout_ms,
        a->allow_insecure ? "true" : "false");
    if (n < 0 || (size_t)n >= cap) return -1;
    return n;
}

/* Monotonic ms. */
static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000);
}

static int write_all_nb(int fd, const uint8_t *buf, size_t len,
                        uint64_t deadline_ms)
{
    size_t off = 0;
    while (off < len) {
        uint64_t now = now_ms();
        if (deadline_ms && now >= deadline_ms) return -1;
        int timeout = deadline_ms ? (int)(deadline_ms - now) : -1;
        struct pollfd pfd = { .fd = fd, .events = POLLOUT };
        int pr = poll(&pfd, 1, timeout);
        if (pr < 0) { if (errno == EINTR) continue; return -1; }
        if (pr == 0) return -1;
        ssize_t n = write(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) continue;
            return -1;
        }
        if (n == 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

/* Consume newline-delimited progress lines from `stdout_fd` up to
 * `deadline_ms` or until EOF/child-exit. Invokes cb on each valid
 * line. Excess data or lines beyond MAX_PROGRESS_LINE are dropped
 * (bounded misbehaviour of a helper cannot exhaust the broker). */
static void drain_progress(int stdout_fd, uint64_t deadline_ms,
                           const nh_auth_porthome_fetch_progress_cb *cb,
                           int *cancel_requested)
{
    char line_buf[NH_PORTHOME_FETCH_MAX_PROGRESS_LINE];
    size_t used = 0;
    uint8_t chunk[512];

    for (;;) {
        uint64_t now = now_ms();
        if (deadline_ms && now >= deadline_ms) return;
        int wait = deadline_ms ? (int)(deadline_ms - now) : -1;
        struct pollfd pfd = { .fd = stdout_fd, .events = POLLIN };
        int pr = poll(&pfd, 1, wait);
        if (pr < 0) { if (errno == EINTR) continue; return; }
        if (pr == 0) return;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            /* Try a final read then return. */
            ssize_t n = read(stdout_fd, chunk, sizeof chunk);
            if (n <= 0) return;
            /* Fall through to lex the chunk. */
            goto have_chunk;
        }
        {
            ssize_t n = read(stdout_fd, chunk, sizeof chunk);
            if (n < 0) { if (errno == EINTR) continue; return; }
            if (n == 0) return; /* EOF */
        have_chunk:;
            /* Split at '\n', accumulate up to MAX_PROGRESS_LINE. */
            for (ssize_t i = 0; i < n; i++) {
                unsigned char c = chunk[i];
                if (c == '\n') {
                    if (used < sizeof line_buf) {
                        nh_porthome_fetch_progress p = {0};
                        if (nh_porthome_fetch_progress_parse(line_buf, used, &p)
                            == NH_PORTHOME_FETCH_PROG_OK && cb && cb->fn) {
                            int r = cb->fn(cb->ctx, p.bytes, p.files, (int)p.phase);
                            if (r != 0 && cancel_requested) *cancel_requested = 1;
                        }
                    }
                    used = 0;
                    continue;
                }
                if (used < sizeof line_buf) line_buf[used++] = (char)c;
                /* else: line too long; drop until next newline. */
            }
        }
    }
}

/* Reap child, best effort. */
static void reap(pid_t pid, int *status) {
    for (;;) {
        int st = 0;
        pid_t r = waitpid(pid, &st, 0);
        if (r == pid) { if (status) *status = st; return; }
        if (r < 0) {
            if (errno == EINTR) continue;
            return;
        }
    }
}

/* Map helper exit code → fetch result. */
static nh_auth_porthome_fetch_result map_exit(int status,
                                              int *out_exit_code) {
    if (!WIFEXITED(status)) {
        if (out_exit_code) *out_exit_code = -1;
        return NH_PORTHOME_FETCH_RES_FAILED;
    }
    int ec = WEXITSTATUS(status);
    if (out_exit_code) *out_exit_code = ec;
    switch (ec) {
    case NH_PORTHOME_FETCH_EXIT_OK:            return NH_PORTHOME_FETCH_RES_OK;
    case NH_PORTHOME_FETCH_EXIT_NETWORK_FAIL:
    case NH_PORTHOME_FETCH_EXIT_DECODE_FAIL:
    case NH_PORTHOME_FETCH_EXIT_SIZE_CAP:
    case NH_PORTHOME_FETCH_EXIT_TIMEOUT:
    case NH_PORTHOME_FETCH_EXIT_SSRF:
        return NH_PORTHOME_FETCH_RES_LIMITED;
    case NH_PORTHOME_FETCH_EXIT_DECRYPT_FAIL:
    case NH_PORTHOME_FETCH_EXIT_INTERNAL:
    case NH_PORTHOME_FETCH_EXIT_ARG:
    default:
        return NH_PORTHOME_FETCH_RES_FAILED;
    }
}

nh_auth_porthome_fetch_result
nh_auth_porthome_fetch_spawn(const nh_auth_porthome_fetch_args *a,
                             const nh_auth_porthome_fetch_progress_cb *cb,
                             int *exit_code_out)
{
    if (!a) return NH_PORTHOME_FETCH_RES_FAILED;
    if ((a->staging_fd < 0 && !a->staging_dir) ||
        (a->staging_fd >= 0 && a->staging_dir))
        return NH_PORTHOME_FETCH_RES_FAILED;
    if (!a->relays || a->relays_count == 0) return NH_PORTHOME_FETCH_RES_LIMITED;
    if (!a->blossom_servers || a->blossom_servers_count == 0)
        return NH_PORTHOME_FETCH_RES_LIMITED;

    const char *helper = nh_auth_porthome_fetch_helper_path();
    if (!helper || access(helper, X_OK) != 0) {
        if (exit_code_out) *exit_code_out = -1;
        return NH_PORTHOME_FETCH_RES_UNAVAILABLE;
    }
    /* Kill switch: NH_PORTHOME_FETCH_HELPER=off disables real fetch. */
    const char *e = getenv("NH_PORTHOME_FETCH_HELPER");
    if (e && strcmp(e, "off") == 0) {
        if (exit_code_out) *exit_code_out = -1;
        return NH_PORTHOME_FETCH_RES_UNAVAILABLE;
    }

    /* Build the control JSON. */
    char ctl[NH_PORTHOME_FETCH_MAX_CTL_BYTES];
    int ctl_len = build_control_json(ctl, sizeof ctl, a);
    if (ctl_len < 0) {
        if (exit_code_out) *exit_code_out = -1;
        return NH_PORTHOME_FETCH_RES_FAILED;
    }

    /* Set up pipes. stdin (write from parent), stdout (read into parent).
     * stderr is left connected so syslog / journald can capture helper
     * diagnostics. */
    int in_pipe[2] = {-1, -1};
    int out_pipe[2] = {-1, -1};
    if (pipe(in_pipe) < 0) {
        OPENSSL_cleanse(ctl, sizeof ctl);
        return NH_PORTHOME_FETCH_RES_FAILED;
    }
    if (pipe(out_pipe) < 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        OPENSSL_cleanse(ctl, sizeof ctl);
        return NH_PORTHOME_FETCH_RES_FAILED;
    }
    /* Parent ends must be CLOEXEC; child ends must NOT be. */
    (void)fcntl(in_pipe[1], F_SETFD, FD_CLOEXEC);
    (void)fcntl(out_pipe[0], F_SETFD, FD_CLOEXEC);
    /* If the caller gave us a staging fd, we must make sure it is
     * inheritable across exec — clear FD_CLOEXEC on a duplicate we
     * control. */
    int child_staging_fd = -1;
    if (a->staging_fd >= 0) {
        child_staging_fd = dup(a->staging_fd);
        if (child_staging_fd < 0) goto io_fail;
        int fl = fcntl(child_staging_fd, F_GETFD);
        if (fl >= 0) (void)fcntl(child_staging_fd, F_SETFD, fl & ~FD_CLOEXEC);
    }

    /* Argv. Two forms: fd-based or dir-based. */
    char fd_arg[32];
    char *argv[8];
    int argi = 0;
    argv[argi++] = (char *)helper;
    if (child_staging_fd >= 0) {
        snprintf(fd_arg, sizeof fd_arg, "%d", child_staging_fd);
        argv[argi++] = "--staging-fd";
        argv[argi++] = fd_arg;
    } else {
        argv[argi++] = "--staging-dir";
        argv[argi++] = (char *)a->staging_dir;
    }
    if (a->allow_insecure) argv[argi++] = "--allow-insecure";
    argv[argi] = NULL;

    pid_t pid = fork();
    if (pid < 0) {
    io_fail:
        if (in_pipe[0]  != -1) close(in_pipe[0]);
        if (in_pipe[1]  != -1) close(in_pipe[1]);
        if (out_pipe[0] != -1) close(out_pipe[0]);
        if (out_pipe[1] != -1) close(out_pipe[1]);
        if (child_staging_fd >= 0) close(child_staging_fd);
        OPENSSL_cleanse(ctl, sizeof ctl);
        return NH_PORTHOME_FETCH_RES_FAILED;
    }
    if (pid == 0) {
        /* Child: rewire fds and exec. */
        if (dup2(in_pipe[0], STDIN_FILENO) < 0) _exit(NH_PORTHOME_FETCH_EXIT_INTERNAL);
        if (dup2(out_pipe[1], STDOUT_FILENO) < 0) _exit(NH_PORTHOME_FETCH_EXIT_INTERNAL);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        /* Disable core dumps: control payload contains home_key. */
        struct rlimit no_core = {0, 0};
        (void)setrlimit(RLIMIT_CORE, &no_core);
        execv(helper, argv);
        _exit(NH_PORTHOME_FETCH_EXIT_INTERNAL);
    }
    /* Parent. */
    close(in_pipe[0]);
    close(out_pipe[1]);
    if (child_staging_fd >= 0) close(child_staging_fd);

    /* Ignore SIGPIPE briefly. */
    void (*old_pipe)(int) = signal(SIGPIPE, SIG_IGN);

    uint32_t tmo = a->total_timeout_ms ? a->total_timeout_ms : 300000u;
    uint64_t deadline = now_ms() + tmo;

    /* Feed the control payload. */
    int wrc = write_all_nb(in_pipe[1], (const uint8_t *)ctl,
                           (size_t)ctl_len, deadline);
    /* Wipe the local buffer — it held home_key. */
    OPENSSL_cleanse(ctl, sizeof ctl);
    close(in_pipe[1]);
    if (wrc != 0) {
        kill(pid, SIGKILL);
        int st = 0; reap(pid, &st);
        close(out_pipe[0]);
        signal(SIGPIPE, old_pipe);
        if (exit_code_out) *exit_code_out = -1;
        return NH_PORTHOME_FETCH_RES_LIMITED;
    }

    int cancel = 0;
    drain_progress(out_pipe[0], deadline, cb, &cancel);
    if (cancel) kill(pid, SIGTERM);
    close(out_pipe[0]);

    /* Reap the child. Enforce the deadline by SIGKILL if still alive. */
    int status = 0;
    for (uint64_t rem;;) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (r < 0 && errno != EINTR) break;
        rem = now_ms();
        if (rem >= deadline) {
            kill(pid, SIGKILL);
            reap(pid, &status);
            signal(SIGPIPE, old_pipe);
            if (exit_code_out) *exit_code_out = -1;
            return NH_PORTHOME_FETCH_RES_LIMITED; /* timeout → LIMITED */
        }
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 20 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    signal(SIGPIPE, old_pipe);
    return map_exit(status, exit_code_out);
}

#else  /* !NH_AUTH_BROKER_ENABLE_PORTHOME */

const char *nh_auth_porthome_fetch_helper_path(void) { return NULL; }
void nh_auth_porthome_fetch_set_helper_path(const char *p) { (void)p; }

nh_auth_porthome_fetch_result
nh_auth_porthome_fetch_spawn(const nh_auth_porthome_fetch_args *a,
                             const nh_auth_porthome_fetch_progress_cb *cb,
                             int *exit_code_out) {
    (void)a; (void)cb;
    if (exit_code_out) *exit_code_out = -1;
    return NH_PORTHOME_FETCH_RES_UNAVAILABLE;
}

#endif /* NH_AUTH_BROKER_ENABLE_PORTHOME */
