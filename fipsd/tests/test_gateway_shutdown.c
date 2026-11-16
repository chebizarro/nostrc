/* Ported from upstream src/gateway/tests/shutdown.rs (nostrc-iq1n9.3): the
 * stop closes the event channel first, so a task blocked sending into it
 * ends on its own; a task that ignores the signal is abandoned at the bound.
 * The tasks are threads here, blocked on the same libgo channel the fibers
 * use. */
#include "test_gateway.h"
#include "gateway/shutdown.h"
#include <nsync.h>
#include <pthread.h>

typedef struct world {
    GoChannel *events;
    fipsd_gw_task resolver, tick;
    nsync_mu mu; nsync_cv cv;
    bool signalled, released, control_aborted;
    int aborted;
} world;
static void world_init(world *w, size_t capacity)
{
    memset(w, 0, sizeof *w);
    w->events = go_channel_create(capacity); CHECK(w->events);
    fipsd_gw_task_init(&w->resolver, "resolver"); fipsd_gw_task_init(&w->tick, "tick");
    nsync_mu_init(&w->mu); nsync_cv_init(&w->cv);
}
static void hook_signal(void *user) { world *w = user; nsync_mu_lock(&w->mu); w->signalled = true; nsync_cv_broadcast(&w->cv); nsync_mu_unlock(&w->mu); }
static void hook_control(void *user) { world *w = user; nsync_mu_lock(&w->mu); w->control_aborted = true; nsync_mu_unlock(&w->mu); }
static void hook_abort(void *user, fipsd_gw_task *t) { (void)t; world *w = user; nsync_mu_lock(&w->mu); w->aborted++; nsync_mu_unlock(&w->mu); }
static fipsd_gw_stop_hooks hooks(world *w) { return (fipsd_gw_stop_hooks){w->events, hook_signal, hook_control, hook_abort, w}; }
/* A backlog event: any pointer the channel can carry. */
static int backlog_event;
/* Send into the channel until a send fails, as a task with a backlog does. */
static void *sender(void *arg)
{
    fipsd_gw_task *t = arg;
    world *w = (world *)((char *)t - (t->name[0] == 'r' ? offsetof(world, resolver) : offsetof(world, tick)));
    while (go_channel_send(w->events, &backlog_event) == 0) {}
    fipsd_gw_task_end(t, false);
    return NULL;
}
/* Wait for the shutdown signal, then end. */
static void *waiter(void *arg)
{
    fipsd_gw_task *t = arg;
    world *w = (world *)((char *)t - (t->name[0] == 'r' ? offsetof(world, resolver) : offsetof(world, tick)));
    nsync_mu_lock(&w->mu);
    while (!w->signalled) nsync_cv_wait(&w->cv, &w->mu);
    nsync_mu_unlock(&w->mu);
    fipsd_gw_task_end(t, false);
    return NULL;
}
/* Ignore the signal: end only once the test releases it (a resolver waiting
 * on a silent upstream, a tick that never ends). */
static void *stuck(void *arg)
{
    fipsd_gw_task *t = arg;
    world *w = (world *)((char *)t - (t->name[0] == 'r' ? offsetof(world, resolver) : offsetof(world, tick)));
    nsync_mu_lock(&w->mu);
    while (!w->released) nsync_cv_wait(&w->cv, &w->mu);
    nsync_mu_unlock(&w->mu);
    fipsd_gw_task_end(t, false);
    return NULL;
}
static void release(world *w) { nsync_mu_lock(&w->mu); w->released = true; nsync_cv_broadcast(&w->cv); nsync_mu_unlock(&w->mu); }
static pthread_t start(fipsd_gw_task *t, void *(*fn)(void *)) { fipsd_gw_task_begin(t); pthread_t th; CHECK(pthread_create(&th, NULL, fn, t) == 0); return th; }
static void fill(world *w) { while (go_channel_try_send(w->events, &backlog_event) == 0) {} }

#define BOUND_MS 300u
static void stopping_with_the_resolver_blocked_on_a_full_event_channel_returns_and_the_resolver_ends_on_its_own(void)
{
    world w; world_init(&w, 1); fill(&w);
    pthread_t r = start(&w.resolver, sender), t = start(&w.tick, waiter);
    fipsd_gw_stop_hooks h = hooks(&w);
    fipsd_gw_instant started = fipsd_gw_now();
    fipsd_gw_stopped s = fipsd_gw_stop_tasks(&h, &w.resolver, &w.tick, BOUND_MS);
    CHECKM(fipsd_gw_since(fipsd_gw_now(), started) < MILLIS(BOUND_MS), "the stop waited out its bound");
    CHECK(s.resolver == FIPSD_GW_TASK_FINISHED && s.tick == FIPSD_GW_TASK_FINISHED);
    CHECK(w.control_aborted && w.aborted == 0);
    pthread_join(r, NULL); pthread_join(t, NULL);
    go_channel_free(w.events);
}
static void stopping_with_the_tick_blocked_sending_a_backlog_returns_and_the_tick_ends_on_its_own(void)
{
    world w; world_init(&w, 1); fill(&w);
    pthread_t t = start(&w.tick, sender);
    fipsd_gw_stop_hooks h = hooks(&w);
    fipsd_gw_stopped s = fipsd_gw_stop_tasks(&h, NULL, &w.tick, BOUND_MS);
    CHECKM(s.resolver == FIPSD_GW_TASK_ABSENT, "no resolver was running");
    CHECK(s.tick == FIPSD_GW_TASK_FINISHED);
    pthread_join(t, NULL);
    go_channel_free(w.events);
}
static void stopping_with_the_resolver_waiting_on_a_silent_upstream_abandons_it_at_the_bound(void)
{
    world w; world_init(&w, 64);
    pthread_t r = start(&w.resolver, stuck), t = start(&w.tick, waiter);
    fipsd_gw_stop_hooks h = hooks(&w);
    fipsd_gw_instant started = fipsd_gw_now();
    fipsd_gw_stopped s = fipsd_gw_stop_tasks(&h, &w.resolver, &w.tick, BOUND_MS);
    fipsd_gw_instant took = fipsd_gw_since(fipsd_gw_now(), started);
    CHECKM(took >= MILLIS(BOUND_MS) && took < SECS(5), "the stop returned after %llu ms", (unsigned long long)(took / 1000000u));
    CHECK(s.resolver == FIPSD_GW_TASK_ABANDONED && s.tick == FIPSD_GW_TASK_FINISHED && w.aborted == 1);
    release(&w);
    pthread_join(r, NULL); pthread_join(t, NULL);
    go_channel_free(w.events);
}
static void stopping_with_a_tick_that_never_ends_abandons_it_at_the_bound(void)
{
    world w; world_init(&w, 64);
    pthread_t r = start(&w.resolver, waiter), t = start(&w.tick, stuck);
    fipsd_gw_stop_hooks h = hooks(&w);
    fipsd_gw_stopped s = fipsd_gw_stop_tasks(&h, &w.resolver, &w.tick, BOUND_MS);
    CHECK(s.resolver == FIPSD_GW_TASK_FINISHED && s.tick == FIPSD_GW_TASK_ABANDONED);
    release(&w);
    pthread_join(r, NULL); pthread_join(t, NULL);
    go_channel_free(w.events);
}
static void one_deadline_covers_both_tasks(void)
{
    world w; world_init(&w, 64);
    pthread_t r = start(&w.resolver, stuck), t = start(&w.tick, stuck);
    fipsd_gw_stop_hooks h = hooks(&w);
    fipsd_gw_instant started = fipsd_gw_now();
    fipsd_gw_stopped s = fipsd_gw_stop_tasks(&h, &w.resolver, &w.tick, BOUND_MS);
    fipsd_gw_instant took = fipsd_gw_since(fipsd_gw_now(), started);
    CHECK(s.resolver == FIPSD_GW_TASK_ABANDONED && s.tick == FIPSD_GW_TASK_ABANDONED);
    CHECKM(took < MILLIS(2 * BOUND_MS), "two stuck tasks took %llu ms, one bound each", (unsigned long long)(took / 1000000u));
    release(&w);
    pthread_join(r, NULL); pthread_join(t, NULL);
    go_channel_free(w.events);
}

int gw_suite_shutdown(void)
{
    RUN(stopping_with_the_resolver_blocked_on_a_full_event_channel_returns_and_the_resolver_ends_on_its_own);
    RUN(stopping_with_the_tick_blocked_sending_a_backlog_returns_and_the_tick_ends_on_its_own);
    RUN(stopping_with_the_resolver_waiting_on_a_silent_upstream_abandons_it_at_the_bound);
    RUN(stopping_with_a_tick_that_never_ends_abandons_it_at_the_bound);
    RUN(one_deadline_covers_both_tasks);
    return 0;
}
