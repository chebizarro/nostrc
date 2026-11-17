#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1 /* O_NOFOLLOW, _SC_NPROCESSORS_ONLN on macOS */
#include "runtime.h"

#include <errno.h>
#include <fcntl.h>
#include <libgo/fiber.h>
#include <nsync.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "blocking_executor.h"
#include "fiber_hooks.h"
#include "go.h"
#include "log.h"
#include "transport/sockos.h"

/* The self-pipe's ends. Windows has neither pipe() descriptors the netpoller
 * can wait on nor POSIX signals: console control events stand in for signals
 * (Ctrl+C and Ctrl+Break are SIGINT; close, logoff and shutdown SIGTERM; no
 * SIGHUP) and a connected loopback socket pair stands in for the pipe. */
#if defined(_WIN32)
typedef gof_socket_t signal_pipe_end;
#define SIGNAL_PIPE_NONE GOF_INVALID_SOCKET
#else
typedef int signal_pipe_end;
#define SIGNAL_PIPE_NONE (-1)
#endif

#define MODULE "runtime"
#define SIGNAL_FIBER_STOP 0 /* byte the shutdown path writes to end the signal fiber */

enum { STATE_CREATED, STATE_STARTED, STATE_STOPPED };

struct fipsd_runtime {
    fipsd_runtime_config cfg;
    GoContext *background;
    CancelContextResult root;     /* root.context is handed to subsystems */
    GoChannel *reload;
    GoChannel *ready;             /* signal fiber -> start(): one token once running */
    _Atomic int fiber_hooks_live; /* GoChannel parks fibers instead of worker threads */
    GoWaitGroup wg;               /* subsystem fibers */
    GoWaitGroup signal_wg;        /* the signal fiber */
    fipsd_subsystem *subsystems[FIPSD_RUNTIME_MAX_SUBSYSTEMS];
    size_t nsubsystems;
    size_t nstarted;
    signal_pipe_end sigpipe[2];
#if !defined(_WIN32)
    struct sigaction saved[3];
#endif
    bool signals_installed;
    int workers;
    _Atomic int state;
};

/* Only one runtime may own the process signal dispositions. The handler
 * reads the write end atomically; -1 means "no runtime listening". */
static _Atomic signal_pipe_end signal_pipe_fd = SIGNAL_PIPE_NONE;

#if defined(_WIN32)
/* Runs on a thread the console creates for the event. */
static BOOL WINAPI on_console_event(DWORD event)
{
    unsigned char byte;
    switch (event) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
        byte = SIGINT;
        break;
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        byte = SIGTERM;
        break;
    default:
        return FALSE;
    }
    gof_socket_t s = atomic_load_explicit(&signal_pipe_fd, memory_order_acquire);
    if (s == GOF_INVALID_SOCKET)
        return FALSE; /* no runtime listening: default handling */
    (void)send(s, (const char *)&byte, 1, 0);
    return TRUE;
}
#else
static const int handled_signals[3] = {SIGINT, SIGTERM, SIGHUP};

static void on_signal(int sig)
{
    int fd = atomic_load_explicit(&signal_pipe_fd, memory_order_acquire);
    if (fd < 0)
        return;
    unsigned char byte = (unsigned char)sig;
    ssize_t r = write(fd, &byte, 1); /* non-blocking; a full pipe coalesces */
    (void)r;
}
#endif

void fipsd_runtime_config_default(fipsd_runtime_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->grace_ms = FIPSD_RUNTIME_DEFAULT_GRACE_MS;
    cfg->handle_signals = true;
}

fipsd_runtime *fipsd_runtime_create(const fipsd_runtime_config *cfg)
{
    fipsd_runtime *rt = calloc(1, sizeof(*rt));
    if (!rt)
        return NULL;
    if (cfg)
        rt->cfg = *cfg;
    else
        fipsd_runtime_config_default(&rt->cfg);
    if (rt->cfg.grace_ms == 0)
        rt->cfg.grace_ms = FIPSD_RUNTIME_DEFAULT_GRACE_MS;
    rt->sigpipe[0] = rt->sigpipe[1] = SIGNAL_PIPE_NONE;
    rt->background = go_context_background();
    rt->reload = go_channel_create(1);
    rt->ready = go_channel_create(1);
    if (!rt->background || !rt->reload || !rt->ready) {
        fipsd_runtime_destroy(rt);
        return NULL;
    }
    rt->root = go_context_with_cancel(rt->background);
    go_wait_group_init(&rt->wg);
    go_wait_group_init(&rt->signal_wg);
    atomic_store(&rt->state, STATE_CREATED);
    return rt;
}

void fipsd_runtime_destroy(fipsd_runtime *rt)
{
    if (!rt)
        return;
    if (atomic_load(&rt->state) == STATE_STARTED)
        FIPSD_WARN(MODULE, "destroying a started runtime without fipsd_runtime_run()");
    /* After every thread has been joined; the signal handler is already
     * restored, so nothing can write to the pipe any more. */
#if defined(_WIN32)
    fipsd_socket_close(rt->sigpipe[0]);
    fipsd_socket_close(rt->sigpipe[1]);
#else
    if (rt->sigpipe[0] >= 0)
        close(rt->sigpipe[0]);
    if (rt->sigpipe[1] >= 0)
        close(rt->sigpipe[1]);
#endif
    if (rt->root.context)
        go_context_unref(rt->root.context);
    if (rt->background)
        go_context_unref(rt->background);
    if (rt->reload)
        go_channel_unref(rt->reload);
    if (rt->ready)
        go_channel_unref(rt->ready);
    go_wait_group_destroy(&rt->wg);
    go_wait_group_destroy(&rt->signal_wg);
    free(rt);
}

int fipsd_runtime_register(fipsd_runtime *rt, fipsd_subsystem *subsystem)
{
    if (!rt || !subsystem || !subsystem->name || atomic_load(&rt->state) != STATE_CREATED)
        return -1;
    if (rt->nsubsystems >= FIPSD_RUNTIME_MAX_SUBSYSTEMS) {
        FIPSD_ERROR(MODULE, "cannot register %s: subsystem table full", subsystem->name);
        return -1;
    }
    rt->subsystems[rt->nsubsystems++] = subsystem;
    return 0;
}

GoContext *fipsd_runtime_context(fipsd_runtime *rt)
{
    return rt->root.context;
}

GoWaitGroup *fipsd_runtime_wait_group(fipsd_runtime *rt)
{
    return &rt->wg;
}

GoChannel *fipsd_runtime_reload_channel(fipsd_runtime *rt)
{
    return rt->reload;
}

int fipsd_runtime_workers(const fipsd_runtime *rt)
{
    return rt->workers;
}

void fipsd_runtime_shutdown(fipsd_runtime *rt)
{
    if (rt && !go_context_is_canceled(rt->root.context))
        rt->root.cancel(rt->root.context);
}

/* ── tracked fibers ────────────────────────────────────────────────────── */

typedef struct tracked_fiber {
    fipsd_runtime *rt;
    void (*fn)(void *arg);
    void *arg;
} tracked_fiber;

static void tracked_fiber_main(void *p)
{
    tracked_fiber t = *(tracked_fiber *)p;
    free(p);
    t.fn(t.arg);
    go_wait_group_done(&t.rt->wg);
}

int fipsd_runtime_spawn(fipsd_runtime *rt, void (*fn)(void *arg), void *arg)
{
    tracked_fiber *t = malloc(sizeof(*t));
    if (!t)
        return -1;
    t->rt = rt;
    t->fn = fn;
    t->arg = arg;
    go_wait_group_add(&rt->wg, 1);
    if (go_fiber(tracked_fiber_main, t, 0) != 0) {
        go_wait_group_done(&rt->wg);
        free(t);
        return -1;
    }
    return 0;
}

/* ── signal fiber ──────────────────────────────────────────────────────── */

static const char *signal_name(int sig)
{
    switch (sig) {
    case SIGINT:
        return "SIGINT";
    case SIGTERM:
        return "SIGTERM";
#ifdef SIGHUP
    case SIGHUP:
        return "SIGHUP";
#endif
    default:
        return "signal";
    }
}

static void signal_fiber(void *arg)
{
    fipsd_runtime *rt = arg;
    gof_set_name("fipsd-signals");
    /* Running on a worker: libgo's channel waits park this fiber only when
     * go_fiber's hook implementations won the link over libgo's weak stubs.
     * Report that, and that the scheduler is live, before start() proceeds. */
    atomic_store(&rt->fiber_hooks_live, gof_hook_current() != NULL);
    (void)go_channel_try_send(rt->ready, rt);
    for (;;) {
        unsigned char byte;
        ssize_t r = gof_read(rt->sigpipe[0], &byte, 1); /* parks on the netpoller */
        if (r == 0)
            break;
        if (r < 0) {
            if (errno == EINTR)
                continue;
            FIPSD_ERROR(MODULE, "signal pipe read failed: %s", strerror(errno));
            break;
        }
        if (byte == SIGNAL_FIBER_STOP)
            break;
        switch (byte) {
        case SIGINT:
        case SIGTERM:
            FIPSD_INFO(MODULE, "%s received; shutting down", signal_name(byte));
            fipsd_runtime_shutdown(rt);
            break;
#ifdef SIGHUP
        case SIGHUP:
            if (go_channel_try_send(rt->reload, rt) == 0)
                FIPSD_INFO(MODULE, "SIGHUP received; configuration reload requested");
            else
                FIPSD_DEBUG(MODULE, "SIGHUP received; a reload is already pending");
            break;
#endif
        default:
            FIPSD_WARN(MODULE, "unexpected signal %d on the signal pipe", (int)byte);
            break;
        }
    }
    go_wait_group_done(&rt->signal_wg);
}

#if defined(_WIN32)
/* A connected loopback TCP pair; [0] is read by the signal fiber. */
static int signal_pair(gof_socket_t pair[2])
{
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    int len = sizeof a;
    gof_socket_t l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (l == GOF_INVALID_SOCKET) { errno = gof_socket_errno(); return -1; }
    int ok = !bind(l, (struct sockaddr *)&a, len) && !getsockname(l, (struct sockaddr *)&a, &len) &&
             !listen(l, 1) && (pair[1] = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) != GOF_INVALID_SOCKET &&
             !connect(pair[1], (struct sockaddr *)&a, len) &&
             (pair[0] = accept(l, NULL, NULL)) != GOF_INVALID_SOCKET;
    if (!ok) errno = gof_socket_errno();
    fipsd_socket_close(l);
    if (ok && (fipsd_socket_prepare(pair[0]) || fipsd_socket_prepare(pair[1]))) ok = 0;
    return ok ? 0 : -1;
}

static int start_signal_fiber(fipsd_runtime *rt)
{
    gof_socket_t expected = GOF_INVALID_SOCKET;
    if (signal_pair(rt->sigpipe) != 0) {
        FIPSD_ERROR(MODULE, "cannot create the signal socket pair: %s", strerror(errno));
        return -1;
    }
    if (!atomic_compare_exchange_strong(&signal_pipe_fd, &expected, rt->sigpipe[1])) {
        FIPSD_ERROR(MODULE, "another runtime already owns the process signals");
        return -1;
    }
    go_wait_group_add(&rt->signal_wg, 1);
    if (!gof_spawn(signal_fiber, rt, 0)) {
        go_wait_group_done(&rt->signal_wg);
        atomic_store(&signal_pipe_fd, GOF_INVALID_SOCKET);
        FIPSD_ERROR(MODULE, "cannot spawn the signal fiber");
        return -1;
    }
    return 0;
}

static int install_signal_handlers(fipsd_runtime *rt)
{
    if (!SetConsoleCtrlHandler(on_console_event, TRUE)) {
        FIPSD_ERROR(MODULE, "cannot install the console control handler (error %lu)", GetLastError());
        return -1;
    }
    rt->signals_installed = true;
    return 0;
}

static void stop_signal_fiber(fipsd_runtime *rt)
{
    if (rt->signals_installed) {
        (void)SetConsoleCtrlHandler(on_console_event, FALSE);
        rt->signals_installed = false;
    }
    if (rt->sigpipe[1] != GOF_INVALID_SOCKET && atomic_load(&signal_pipe_fd) == rt->sigpipe[1]) {
        atomic_store(&signal_pipe_fd, GOF_INVALID_SOCKET);
        unsigned char stop = SIGNAL_FIBER_STOP;
        (void)send(rt->sigpipe[1], (const char *)&stop, 1, 0);
        go_wait_group_wait(&rt->signal_wg);
    }
}
#else
static int set_fd_flags(int fd)
{
    int fl = fcntl(fd, F_GETFL);
    int fd_fl = fcntl(fd, F_GETFD);
    if (fl < 0 || fd_fl < 0)
        return -1;
    if (fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0 || fcntl(fd, F_SETFD, fd_fl | FD_CLOEXEC) < 0)
        return -1;
    return 0;
}

/* The signal fiber is spawned before the background workers start: libgo's
 * workers exit as soon as no fiber is live and no I/O waiter is registered,
 * so a daemon with nothing spawned yet would lose its scheduler between
 * gof_start_background() and the first subsystem start(). Parked in
 * gof_read() on the pipe, this fiber anchors the scheduler for the whole run
 * and leaves when the stop byte arrives. */
static int start_signal_fiber(fipsd_runtime *rt)
{
    int expected = -1;
    if (pipe(rt->sigpipe) != 0) {
        FIPSD_ERROR(MODULE, "cannot create signal pipe: %s", strerror(errno));
        return -1;
    }
    if (set_fd_flags(rt->sigpipe[0]) != 0 || set_fd_flags(rt->sigpipe[1]) != 0) {
        FIPSD_ERROR(MODULE, "cannot configure signal pipe: %s", strerror(errno));
        return -1;
    }
    if (!atomic_compare_exchange_strong(&signal_pipe_fd, &expected, rt->sigpipe[1])) {
        FIPSD_ERROR(MODULE, "another runtime already owns the process signals");
        return -1;
    }
    go_wait_group_add(&rt->signal_wg, 1);
    if (!gof_spawn(signal_fiber, rt, 0)) {
        go_wait_group_done(&rt->signal_wg);
        atomic_store(&signal_pipe_fd, -1);
        FIPSD_ERROR(MODULE, "cannot spawn the signal fiber");
        return -1;
    }
    return 0;
}

static int install_signal_handlers(fipsd_runtime *rt)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    for (size_t i = 0; i < 3; i++) {
        if (sigaction(handled_signals[i], &sa, &rt->saved[i]) != 0) {
            FIPSD_ERROR(MODULE, "cannot install %s handler: %s",
                        signal_name(handled_signals[i]), strerror(errno));
            while (i-- > 0)
                sigaction(handled_signals[i], &rt->saved[i], NULL);
            return -1;
        }
    }
    rt->signals_installed = true;
    /* A parent may have started us with these blocked (a spawner's mask is
     * inherited across exec); a stop held pending until now must reach the
     * handler, not the default action, so unblock them on this thread. */
    sigset_t unblock;
    sigemptyset(&unblock);
    for (size_t i = 0; i < 3; i++)
        sigaddset(&unblock, handled_signals[i]);
    if (pthread_sigmask(SIG_UNBLOCK, &unblock, NULL) != 0)
        FIPSD_WARN(MODULE, "cannot unblock handled signals: %s", strerror(errno));
    return 0;
}

static void stop_signal_fiber(fipsd_runtime *rt)
{
    if (rt->signals_installed) {
        /* Default dispositions again: a second SIGTERM during teardown ends
         * the process at once, as operators expect. */
        for (size_t i = 0; i < 3; i++)
            sigaction(handled_signals[i], &rt->saved[i], NULL);
        rt->signals_installed = false;
    }
    if (rt->sigpipe[1] >= 0 && atomic_load(&signal_pipe_fd) == rt->sigpipe[1]) {
        atomic_store(&signal_pipe_fd, -1);
        unsigned char stop = SIGNAL_FIBER_STOP;
        ssize_t r;
        do {
            r = write(rt->sigpipe[1], &stop, 1);
        } while (r < 0 && errno == EINTR);
        /* The pipe holds at least PIPE_BUF bytes and signals coalesce long
         * before that, so the stop byte is never refused with EAGAIN. */
        go_wait_group_wait(&rt->signal_wg);
    }
}
#endif

/* ── scheduler sizing ──────────────────────────────────────────────────── */

static int env_int(const char *name)
{
    const char *v = getenv(name);
    if (!v || !*v)
        return 0;
    char *end = NULL;
    long n = strtol(v, &end, 10);
    if (end == v || *end != '\0' || n < 1 || n > 64) {
        FIPSD_WARN(MODULE, "ignoring %s=%s: expected an integer in 1..64", name, v);
        return 0;
    }
    return (int)n;
}

static void size_scheduler(fipsd_runtime *rt)
{
    int workers = env_int("FIPSD_WORKERS");
    if (workers <= 0)
        workers = rt->cfg.workers;
    if (workers <= 0)
        workers = env_int("GOF_NWORKERS");
    if (workers <= 0) {
#if defined(_WIN32)
        long cpus = (long)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
#else
        long cpus = sysconf(_SC_NPROCESSORS_ONLN);
#endif
        workers = cpus > 0 ? (int)cpus : 1;
    }
    if (workers > 64)
        workers = 64;
    /* The scheduler reads its worker count from this variable in gof_init();
     * we are single-threaded here, so setenv() is safe. */
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", workers);
#if defined(_WIN32)
    _putenv_s("GOF_NWORKERS", buf);
#else
    setenv("GOF_NWORKERS", buf, 1);
#endif

    int pollers = rt->cfg.pollers;
    if (pollers <= 0) {
        pollers = workers / 4;
        if (pollers < 1)
            pollers = 1;
        if (pollers > 4)
            pollers = 4;
    }
    (void)gof_set_npollers(pollers); /* -1 once the scheduler is initialized: keep its value */
    rt->workers = workers;
}

/* ── start / run ───────────────────────────────────────────────────────── */

static void stop_subsystems(fipsd_runtime *rt)
{
    while (rt->nstarted > 0) {
        fipsd_subsystem *s = rt->subsystems[--rt->nstarted];
        FIPSD_DEBUG(MODULE, "stopping %s", s->name);
        if (s->stop)
            s->stop(s, rt);
    }
}

/* Bounded wait on the subsystem wait group, using its own nsync primitives. */
static bool wait_group_wait_ms(GoWaitGroup *wg, unsigned ms)
{
    nsync_time deadline = nsync_time_add(nsync_time_now(), nsync_time_ms(ms));
    nsync_mu_lock(&wg->mutex);
    while (atomic_load(&wg->counter) > 0) {
        if (nsync_cv_wait_with_deadline(&wg->cond, &wg->mutex, deadline, NULL) != 0)
            break;
    }
    bool drained = atomic_load(&wg->counter) <= 0;
    nsync_mu_unlock(&wg->mutex);
    return drained;
}

static int teardown(fipsd_runtime *rt)
{
    int status = FIPSD_RUN_OK;
    fipsd_runtime_shutdown(rt); /* fibers observe the cancelled context */
    stop_subsystems(rt);
    if (!wait_group_wait_ms(&rt->wg, rt->cfg.grace_ms)) {
        FIPSD_ERROR(MODULE, "%d subsystem fiber(s) still running after the %u ms grace period",
                    atomic_load(&rt->wg.counter), rt->cfg.grace_ms);
        status = FIPSD_RUN_GRACE_ELAPSED;
    }
    stop_signal_fiber(rt);
    gof_request_stop();
    if (gof_join_background() != 0)
        FIPSD_WARN(MODULE, "fiber runtime was not running at shutdown");
    go_blocking_executor_shutdown();
    atomic_store(&rt->state, STATE_STOPPED);
    FIPSD_INFO(MODULE, "runtime stopped");
    return status;
}

int fipsd_runtime_start(fipsd_runtime *rt)
{
    if (!rt || atomic_load(&rt->state) != STATE_CREATED)
        return -1;
    size_scheduler(rt);
    gof_init(0); /* reads GOF_NWORKERS set above; a no-op if already initialized */
    if (start_signal_fiber(rt) != 0)
        return -1;
    if (gof_start_background(0) != 0) {
        FIPSD_ERROR(MODULE, "cannot start the fiber runtime");
        return -1;
    }
    /* Barrier: the signal fiber is running (so the scheduler is live and a
     * stop can already be observed) before any subsystem starts. */
    void *ready_token = NULL;
    (void)go_channel_receive(rt->ready, &ready_token);
    if (!atomic_load(&rt->fiber_hooks_live)) {
        FIPSD_ERROR(MODULE, "fiber hooks are not linked: GoChannel waits would block worker "
                            "threads (link go_fiber before libgo)");
        atomic_store(&rt->state, STATE_STARTED);
        teardown(rt);
        return -1;
    }
    gof_sched_stats stats;
    gof_get_stats(&stats);
    if (stats.nworkers > 0 && stats.nworkers != rt->workers) {
        FIPSD_WARN(MODULE, "scheduler already initialized with %d workers; %d requested",
                   stats.nworkers, rt->workers);
        rt->workers = stats.nworkers;
    }
    if (go_blocking_executor_init((size_t)(rt->cfg.blocking_threads > 0 ? rt->cfg.blocking_threads : 0)) != 0) {
        FIPSD_ERROR(MODULE, "cannot start the blocking executor");
        gof_request_stop();
        gof_join_background();
        return -1;
    }
    atomic_store(&rt->state, STATE_STARTED);
    if (rt->cfg.handle_signals && install_signal_handlers(rt) != 0) {
        teardown(rt);
        return -1;
    }
    FIPSD_INFO(MODULE, "fiber runtime started: %d worker(s), %d poller(s), signals %s",
               rt->workers, gof_get_npollers(), rt->cfg.handle_signals ? "handled" : "default");
    for (size_t i = 0; i < rt->nsubsystems; i++) {
        fipsd_subsystem *s = rt->subsystems[i];
        FIPSD_DEBUG(MODULE, "starting %s", s->name);
        if (s->start && s->start(s, rt) != 0) {
            FIPSD_ERROR(MODULE, "subsystem %s failed to start", s->name);
            teardown(rt);
            return -1;
        }
        rt->nstarted = i + 1;
    }
    return 0;
}

int fipsd_runtime_run(fipsd_runtime *rt)
{
    if (!rt || atomic_load(&rt->state) != STATE_STARTED)
        return -1;
    /* The main thread is not a fiber: this parks it on nsync until the root
     * context is cancelled (its done channel receives a token, then closes). */
    void *token = NULL;
    (void)go_channel_receive(go_context_done(rt->root.context), &token);
    FIPSD_INFO(MODULE, "shutting down: stopping %zu subsystem(s), grace %u ms",
               rt->nstarted, rt->cfg.grace_ms);
    return teardown(rt);
}
