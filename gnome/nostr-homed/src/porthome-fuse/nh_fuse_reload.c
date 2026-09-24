/*
 * nh_fuse_reload.c — see nh_fuse_reload.h.
 *
 * SPDX-License-Identifier: MIT
 *
 * Bead: nostrc-plo4.
 */
#define _GNU_SOURCE
#include "nh_fuse_reload.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <time.h>
#include <unistd.h>

#ifndef NH_FUSE_RELOAD_DEFAULT_DEBOUNCE_MS
#define NH_FUSE_RELOAD_DEFAULT_DEBOUNCE_MS 250L
#endif

#ifndef NH_FUSE_RELOAD_DEFAULT_BASENAME
#define NH_FUSE_RELOAD_DEFAULT_BASENAME "snapshot.json"
#endif

struct nh_fuse_reload {
    /* Config snapshot. */
    char                       *state_dir;
    char                       *watch_basename;
    long                        debounce_ms;
    bool                        disabled;

    nh_fuse_reload_load_fn      load_fn;
    void                       *load_ud;
    nh_fuse_reload_swap_fn      swap_fn;
    void                       *swap_ud;
    nh_fuse_reload_fail_fn      fail_fn;
    void                       *fail_ud;
    nh_fuse_reload_clock_fn     clock_fn;
    void                       *clock_ud;

    /* Watcher state. */
    int                         inotify_fd;
    int                         wd;
    int                         wake_pipe[2];   /* [0]=read, [1]=write */
    pthread_t                   thread;
    bool                        thread_started;

    /* Debounce ledger. */
    pthread_mutex_t             mu;
    bool                        pending;
    int64_t                     last_event_ms;

    /* Counters. */
    _Atomic unsigned            swap_count;
    _Atomic unsigned            fail_count;

    /* Shutdown flag. */
    _Atomic bool                stopping;
};

static int64_t default_clock_ms(void *ud) {
    (void)ud;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000ll + (int64_t)ts.tv_nsec / 1000000ll;
}

static int64_t reload_now_ms(const nh_fuse_reload_t *r) {
    if (r->clock_fn) return r->clock_fn(r->clock_ud);
    return default_clock_ms(NULL);
}

static void set_cloexec_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) (void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int fd_flags = fcntl(fd, F_GETFD, 0);
    if (fd_flags >= 0) (void)fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC);
}

/* Drain the inotify fd into pending/last_event_ms. Called from both
 * the watcher thread and the test-seam tick. Non-blocking read. */
static void drain_inotify_locked(nh_fuse_reload_t *r) {
    if (r->inotify_fd < 0) return;
    uint8_t buf[4096];
    for (;;) {
        ssize_t n = read(r->inotify_fd, buf, sizeof buf);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return;
        }
        size_t i = 0;
        while (i + sizeof(struct inotify_event) <= (size_t)n) {
            struct inotify_event *ev = (struct inotify_event *)(buf + i);
            i += sizeof(struct inotify_event) + ev->len;
            if (ev->len == 0) continue;
            /* Match exact basename. */
            if (strcmp(ev->name, r->watch_basename) == 0) {
                r->pending = true;
                r->last_event_ms = reload_now_ms(r);
            }
        }
    }
}

/* Try one reload. Returns 1 on success (swap performed), 0 on
 * no-op, <0 on failure. Called with mu HELD; releases it around
 * user callbacks to avoid deadlocks. */
static int try_reload_locked(nh_fuse_reload_t *r) {
    if (!r->pending) return 0;
    int64_t now = reload_now_ms(r);
    int64_t elapsed = now - r->last_event_ms;
    if (elapsed < r->debounce_ms) return 0;
    r->pending = false;
    pthread_mutex_unlock(&r->mu);

    nh_fuse_table *nt = NULL;
    int rc = r->load_fn ? r->load_fn(r->load_ud, r->state_dir, &nt) : -EINVAL;
    if (rc != 0 || !nt) {
        atomic_fetch_add(&r->fail_count, 1u);
        if (r->fail_fn) r->fail_fn(r->fail_ud, rc);
        pthread_mutex_lock(&r->mu);
        return rc < 0 ? rc : -EIO;
    }
    if (r->swap_fn) r->swap_fn(r->swap_ud, nt);
    else            nh_fuse_table_free(nt); /* defensive; misuse only */
    atomic_fetch_add(&r->swap_count, 1u);
    pthread_mutex_lock(&r->mu);
    return 1;
}

static void *watcher_main(void *ud) {
    nh_fuse_reload_t *r = ud;
    while (!atomic_load(&r->stopping)) {
        /* Compute next timeout. If pending, wait up to (debounce -
         * elapsed) ms. Otherwise block until an fd fires. */
        pthread_mutex_lock(&r->mu);
        bool pending = r->pending;
        int64_t last  = r->last_event_ms;
        pthread_mutex_unlock(&r->mu);
        int timeout_ms = -1;
        if (pending) {
            int64_t now = reload_now_ms(r);
            int64_t left = r->debounce_ms - (now - last);
            if (left < 0) left = 0;
            if (left > INT_MAX) left = INT_MAX;
            timeout_ms = (int)left;
        }

        struct pollfd pfds[2];
        pfds[0].fd = r->inotify_fd;
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;
        pfds[1].fd = r->wake_pipe[0];
        pfds[1].events = POLLIN;
        pfds[1].revents = 0;

        int pr = poll(pfds, 2, timeout_ms);
        if (pr < 0) {
            if (errno == EINTR) continue;
            /* Poll error — bail so we don't spin. */
            break;
        }
        if (pfds[1].revents & POLLIN) {
            /* Wake pipe — drain and re-check stopping. */
            uint8_t drain[64];
            (void)read(r->wake_pipe[0], drain, sizeof drain);
            if (atomic_load(&r->stopping)) break;
        }
        pthread_mutex_lock(&r->mu);
        if (pfds[0].revents & POLLIN) drain_inotify_locked(r);
        /* Debounce evaluation. */
        (void)try_reload_locked(r);
        pthread_mutex_unlock(&r->mu);
    }
    return NULL;
}

int nh_fuse_reload_new(const nh_fuse_reload_cfg *cfg, nh_fuse_reload_t **out) {
    if (!cfg || !cfg->state_dir || !out) return -EINVAL;
    *out = NULL;

    /* Honour env override. */
    bool disabled = cfg->disable_inotify;
    const char *env = getenv("NOSTR_HOMED_PORTHOME_FUSE_NO_INOTIFY");
    if (env && *env && strcmp(env, "0") != 0) disabled = true;

    nh_fuse_reload_t *r = calloc(1, sizeof *r);
    if (!r) return -ENOMEM;
    pthread_mutex_init(&r->mu, NULL);
    r->state_dir      = strdup(cfg->state_dir);
    r->watch_basename = strdup(cfg->watch_basename && *cfg->watch_basename
                               ? cfg->watch_basename
                               : NH_FUSE_RELOAD_DEFAULT_BASENAME);
    r->debounce_ms    = cfg->debounce_ms > 0 ? cfg->debounce_ms
                                             : NH_FUSE_RELOAD_DEFAULT_DEBOUNCE_MS;
    r->disabled       = disabled;
    r->load_fn        = cfg->load_fn;
    r->load_ud        = cfg->load_ud;
    r->swap_fn        = cfg->swap_fn;
    r->swap_ud        = cfg->swap_ud;
    r->fail_fn        = cfg->fail_fn;
    r->fail_ud        = cfg->fail_ud;
    r->clock_fn       = cfg->clock_fn;
    r->clock_ud       = cfg->clock_ud;
    r->inotify_fd     = -1;
    r->wd             = -1;
    r->wake_pipe[0]   = -1;
    r->wake_pipe[1]   = -1;
    atomic_init(&r->swap_count, 0);
    atomic_init(&r->fail_count, 0);
    atomic_init(&r->stopping, false);

    if (!r->state_dir || !r->watch_basename) {
        nh_fuse_reload_free(r);
        return -ENOMEM;
    }

    if (disabled) {
        *out = r;
        return 0;
    }

    /* Open inotify. Failure here degrades to disabled mode (log NOTICE);
     * the mount stays serviceable. */
    r->inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (r->inotify_fd < 0) {
        fprintf(stderr,
                "porthome-fuse: NOTICE inotify_init1 failed errno=%d — "
                "snapshot reload disabled\n", errno);
        r->disabled = true;
        *out = r;
        return 0;
    }
    r->wd = inotify_add_watch(r->inotify_fd, r->state_dir,
                              IN_CLOSE_WRITE | IN_MOVED_TO);
    if (r->wd < 0) {
        fprintf(stderr,
                "porthome-fuse: NOTICE inotify_add_watch(%s) failed errno=%d — "
                "snapshot reload disabled\n", r->state_dir, errno);
        close(r->inotify_fd); r->inotify_fd = -1;
        r->disabled = true;
        *out = r;
        return 0;
    }
    if (pipe(r->wake_pipe) != 0) {
        int e = errno;
        (void)inotify_rm_watch(r->inotify_fd, r->wd);
        close(r->inotify_fd); r->inotify_fd = -1;
        r->disabled = true;
        fprintf(stderr,
                "porthome-fuse: NOTICE wake pipe failed errno=%d — "
                "snapshot reload disabled\n", e);
        *out = r;
        return 0;
    }
    set_cloexec_nonblock(r->wake_pipe[0]);
    set_cloexec_nonblock(r->wake_pipe[1]);

    /* Spawn watcher thread. */
    if (pthread_create(&r->thread, NULL, watcher_main, r) != 0) {
        int e = errno;
        close(r->wake_pipe[0]); close(r->wake_pipe[1]);
        r->wake_pipe[0] = r->wake_pipe[1] = -1;
        (void)inotify_rm_watch(r->inotify_fd, r->wd);
        close(r->inotify_fd); r->inotify_fd = -1;
        r->disabled = true;
        fprintf(stderr,
                "porthome-fuse: NOTICE pthread_create failed errno=%d — "
                "snapshot reload disabled\n", e);
        *out = r;
        return 0;
    }
    r->thread_started = true;
    *out = r;
    return 0;
}

void nh_fuse_reload_free(nh_fuse_reload_t *r) {
    if (!r) return;
    atomic_store(&r->stopping, true);
    if (r->thread_started) {
        /* Wake the watcher via the pipe. */
        if (r->wake_pipe[1] >= 0) {
            uint8_t b = 1;
            (void)write(r->wake_pipe[1], &b, 1);
        }
        (void)pthread_join(r->thread, NULL);
    }
    if (r->wake_pipe[0] >= 0) close(r->wake_pipe[0]);
    if (r->wake_pipe[1] >= 0) close(r->wake_pipe[1]);
    if (r->inotify_fd >= 0) {
        if (r->wd >= 0) (void)inotify_rm_watch(r->inotify_fd, r->wd);
        close(r->inotify_fd);
    }
    free(r->state_dir);
    free(r->watch_basename);
    pthread_mutex_destroy(&r->mu);
    free(r);
}

int nh_fuse_reload_fd(const nh_fuse_reload_t *r) {
    if (!r || r->disabled) return -1;
    return r->inotify_fd;
}

void nh_fuse_reload_mark_event(nh_fuse_reload_t *r) {
    if (!r) return;
    pthread_mutex_lock(&r->mu);
    r->pending = true;
    r->last_event_ms = reload_now_ms(r);
    pthread_mutex_unlock(&r->mu);
}

int nh_fuse_reload_tick_now(nh_fuse_reload_t *r) {
    if (!r) return -EINVAL;
    pthread_mutex_lock(&r->mu);
    if (r->inotify_fd >= 0) drain_inotify_locked(r);
    int rc = try_reload_locked(r);
    pthread_mutex_unlock(&r->mu);
    return rc;
}

unsigned nh_fuse_reload_swap_count(const nh_fuse_reload_t *r) {
    if (!r) return 0;
    return atomic_load(&((nh_fuse_reload_t *)r)->swap_count);
}
unsigned nh_fuse_reload_fail_count(const nh_fuse_reload_t *r) {
    if (!r) return 0;
    return atomic_load(&((nh_fuse_reload_t *)r)->fail_count);
}
