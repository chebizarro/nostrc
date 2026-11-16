#include "gateway/shutdown.h"
#include "log.h"

#define MODULE "gateway"

void fipsd_gw_task_init(fipsd_gw_task *t, const char *name)
{
    nsync_mu_init(&t->mu); nsync_cv_init(&t->cv);
    t->name = name; t->running = t->ended = t->failed = false;
}
void fipsd_gw_task_begin(fipsd_gw_task *t)
{
    nsync_mu_lock(&t->mu);
    t->running = true; t->ended = t->failed = false;
    nsync_mu_unlock(&t->mu);
}
void fipsd_gw_task_end(fipsd_gw_task *t, bool failed)
{
    nsync_mu_lock(&t->mu);
    t->running = false; t->ended = true; t->failed = failed;
    nsync_cv_broadcast(&t->cv);
    nsync_mu_unlock(&t->mu);
}
bool fipsd_gw_task_ended(fipsd_gw_task *t)
{
    nsync_mu_lock(&t->mu);
    bool ended = t->ended;
    nsync_mu_unlock(&t->mu);
    return ended;
}
/* Wait for task until deadline; past it, abort the task. */
static fipsd_gw_task_outcome wait_until(const fipsd_gw_stop_hooks *h, fipsd_gw_task *t, nsync_time deadline, unsigned timeout_ms)
{
    nsync_mu_lock(&t->mu);
    while (!t->ended)
        if (nsync_cv_wait_with_deadline(&t->cv, &t->mu, deadline, NULL) != 0) break;
    bool ended = t->ended, failed = t->failed;
    nsync_mu_unlock(&t->mu);
    if (ended) return failed ? FIPSD_GW_TASK_FAILED : FIPSD_GW_TASK_FINISHED;
    FIPSD_WARN(MODULE, "Gateway task did not stop in time; abandoning it: task=%s timeout_ms=%u", t->name, timeout_ms);
    if (h->abort) h->abort(h->user, t);
    /* It may have ended between the deadline and the abort. */
    return fipsd_gw_task_ended(t) ? FIPSD_GW_TASK_FINISHED : FIPSD_GW_TASK_ABANDONED;
}
fipsd_gw_stopped fipsd_gw_stop_tasks(const fipsd_gw_stop_hooks *h, fipsd_gw_task *resolver, fipsd_gw_task *tick,
                                     unsigned timeout_ms)
{
    /* Close the event channel first: nothing receives from it any more, and
     * a resolver or tick blocked sending into a full channel cannot see the
     * shutdown signal; closing it makes that send fail at once. */
    if (h->events) go_channel_close(h->events);
    if (h->signal) h->signal(h->user);
    nsync_time deadline = nsync_time_add(nsync_time_now(), nsync_time_ms(timeout_ms));
    if (h->abort_control) h->abort_control(h->user);
    fipsd_gw_stopped out;
    out.resolver = resolver ? wait_until(h, resolver, deadline, timeout_ms) : FIPSD_GW_TASK_ABSENT;
    out.tick = tick ? wait_until(h, tick, deadline, timeout_ms) : FIPSD_GW_TASK_ABSENT;
    return out;
}
