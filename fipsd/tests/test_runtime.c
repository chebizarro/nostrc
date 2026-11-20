#define _POSIX_C_SOURCE 200809L
#include "runtime.h"

#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "go.h"
#include "log.h"

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); return 1; \
} } while (0)

/* Start/stop order shared by every subsystem; main thread only. */
static int events[16];
static int nevents;
static void record(int id)
{
    if (nevents < 16)
        events[nevents++] = id;
}

typedef struct subsystem_state {
    int id;
    fipsd_runtime *rt;
    int fail_start;
    _Atomic int started, stopped, fibers_done;
    GoChannel *release; /* fiber waits on this, not on the context */
} subsystem_state;

/* Returns when the root context is cancelled: the done channel yields a
 * token to the first receiver and -1 (closed) to every later one. */
static void context_fiber(void *arg)
{
    subsystem_state *s = arg;
    void *token = NULL;
    (void)go_channel_receive(go_context_done(fipsd_runtime_context(s->rt)), &token);
    atomic_fetch_add(&s->fibers_done, 1);
}

/* Ignores the context and waits for stop() to release it, which proves the
 * runtime calls stop() and waits for fibers to drain. */
static void release_fiber(void *arg)
{
    subsystem_state *s = arg;
    void *token = NULL;
    (void)go_channel_receive(s->release, &token);
    atomic_fetch_add(&s->fibers_done, 1);
}

static int sub_start(fipsd_subsystem *self, fipsd_runtime *rt)
{
    subsystem_state *s = self->user;
    s->rt = rt;
    record(s->id * 10 + 1);
    atomic_store(&s->started, 1);
    if (s->fail_start)
        return -1;
    if (fipsd_runtime_spawn(rt, context_fiber, s) != 0)
        return -1;
    if (s->release && fipsd_runtime_spawn(rt, release_fiber, s) != 0)
        return -1;
    return 0;
}

static void sub_stop(fipsd_subsystem *self, fipsd_runtime *rt)
{
    subsystem_state *s = self->user;
    (void)rt;
    record(s->id * 10 + 2);
    atomic_store(&s->stopped, 1);
    if (s->release)
        go_channel_close(s->release);
}

static int handler_is_default(int sig)
{
    struct sigaction sa;
    if (sigaction(sig, NULL, &sa) != 0)
        return 0;
    return sa.sa_handler == SIG_DFL;
}

static int scenario_signals(void)
{
    fipsd_runtime_config cfg;
    fipsd_runtime_config_default(&cfg);
    cfg.grace_ms = 10000; /* bounded, but the drain is expected to be immediate */
    fipsd_runtime *rt = fipsd_runtime_create(&cfg);
    CHECK(rt != NULL);

    subsystem_state a = {.id = 1}, b = {.id = 2, .release = go_channel_create(1)};
    fipsd_subsystem sa = {"alpha", sub_start, sub_stop, &a};
    fipsd_subsystem sb = {"beta", sub_start, sub_stop, &b};
    CHECK(fipsd_runtime_register(rt, &sa) == 0);
    CHECK(fipsd_runtime_register(rt, &sb) == 0);
    CHECK(fipsd_runtime_run(rt) == -1); /* not started yet */

    CHECK(fipsd_runtime_start(rt) == 0);
    CHECK(fipsd_runtime_register(rt, &sa) == -1); /* too late */
    CHECK(fipsd_runtime_workers(rt) >= 1);
    CHECK(atomic_load(&a.started) == 1 && atomic_load(&b.started) == 1);
    CHECK(nevents == 2 && events[0] == 11 && events[1] == 21);
    CHECK(!handler_is_default(SIGTERM) && !handler_is_default(SIGINT) && !handler_is_default(SIGHUP));
    CHECK(!go_context_is_canceled(fipsd_runtime_context(rt)));

    /* SIGHUP -> one token on the reload channel; the receive blocks until
     * the signal fiber has dispatched it. */
    GoChannel *reload = fipsd_runtime_reload_channel(rt);
    CHECK(kill(getpid(), SIGHUP) == 0);
    void *token = NULL;
    CHECK(go_channel_receive(reload, &token) == 0 && token != NULL);
    CHECK(go_channel_try_receive(reload, &token) == -1); /* exactly one */
    CHECK(!go_context_is_canceled(fipsd_runtime_context(rt)));
    CHECK(atomic_load(&a.fibers_done) == 0);

    /* SIGTERM -> context cancelled -> run() returns after an orderly stop. */
    CHECK(kill(getpid(), SIGTERM) == 0);
    CHECK(fipsd_runtime_run(rt) == FIPSD_RUN_OK);
    CHECK(go_context_is_canceled(fipsd_runtime_context(rt)));
    CHECK(atomic_load(&a.stopped) == 1 && atomic_load(&b.stopped) == 1);
    CHECK(nevents == 4 && events[2] == 22 && events[3] == 12); /* reverse order */
    CHECK(atomic_load(&a.fibers_done) == 1 && atomic_load(&b.fibers_done) == 2);
    CHECK(atomic_load(&fipsd_runtime_wait_group(rt)->counter) == 0);
    CHECK(handler_is_default(SIGTERM) && handler_is_default(SIGINT) && handler_is_default(SIGHUP));
    CHECK(fipsd_runtime_run(rt) == -1); /* stopped */
    fipsd_runtime_destroy(rt);
    go_channel_unref(b.release);
    return 0;
}

static int scenario_start_failure(void)
{
    fipsd_runtime_config cfg;
    fipsd_runtime_config_default(&cfg);
    cfg.grace_ms = 10000;
    fipsd_runtime *rt = fipsd_runtime_create(&cfg);
    CHECK(rt != NULL);

    subsystem_state a = {.id = 1}, f = {.id = 2, .fail_start = 1}, c = {.id = 3};
    fipsd_subsystem sa = {"alpha", sub_start, sub_stop, &a};
    fipsd_subsystem sf = {"failing", sub_start, sub_stop, &f};
    fipsd_subsystem sc = {"never", sub_start, sub_stop, &c};
    CHECK(fipsd_runtime_register(rt, &sa) == 0);
    CHECK(fipsd_runtime_register(rt, &sf) == 0);
    CHECK(fipsd_runtime_register(rt, &sc) == 0);
    CHECK(fipsd_runtime_start(rt) == -1);
    /* alpha started and was stopped; the failing one is not stopped (it never
     * started); the third never ran. */
    CHECK(nevents == 3 && events[0] == 11 && events[1] == 21 && events[2] == 12);
    CHECK(atomic_load(&a.stopped) == 1 && atomic_load(&f.stopped) == 0 && atomic_load(&c.started) == 0);
    CHECK(atomic_load(&a.fibers_done) == 1);
    CHECK(go_context_is_canceled(fipsd_runtime_context(rt)));
    CHECK(handler_is_default(SIGTERM));
    CHECK(fipsd_runtime_run(rt) == -1);
    fipsd_runtime_destroy(rt);
    nevents = 0;

    /* The process can bring a second runtime up (the scheduler is reused)
     * and shut it down programmatically without signal handling. */
    fipsd_runtime_config_default(&cfg);
    cfg.handle_signals = false;
    cfg.grace_ms = 10000;
    rt = fipsd_runtime_create(&cfg);
    CHECK(rt != NULL);
    subsystem_state d = {.id = 4};
    fipsd_subsystem sd = {"delta", sub_start, sub_stop, &d};
    CHECK(fipsd_runtime_register(rt, &sd) == 0);
    CHECK(fipsd_runtime_start(rt) == 0);
    CHECK(handler_is_default(SIGTERM)); /* not installed */
    fipsd_runtime_shutdown(rt);
    fipsd_runtime_shutdown(rt); /* idempotent */
    CHECK(fipsd_runtime_run(rt) == FIPSD_RUN_OK);
    CHECK(nevents == 2 && events[0] == 41 && events[1] == 42);
    CHECK(atomic_load(&d.fibers_done) == 1);
    fipsd_runtime_destroy(rt);
    return 0;
}

int main(int argc, char **argv)
{
    const char *scenario = argc > 1 ? argv[1] : "signals";
    fipsd_log *log = fipsd_log_create(FIPSD_LOG_DEBUG, NULL);
    if (!log)
        return 1;
    fipsd_log_set_default(log);
    int rc;
    if (strcmp(scenario, "signals") == 0)
        rc = scenario_signals();
    else if (strcmp(scenario, "start-failure") == 0)
        rc = scenario_start_failure();
    else {
        fprintf(stderr, "unknown scenario %s\n", scenario);
        rc = 1;
    }
    fipsd_log_set_default(NULL);
    fipsd_log_destroy(log);
    return rc;
}
