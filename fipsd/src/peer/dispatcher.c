#include "peer/dispatcher.h"
#include "peer/link.h"
#include "select.h"
#include <stdlib.h>

void fipsd_peer_command_discard(fipsd_peer_command *c)
{
    /* An INSERT must be a not-yet-attached peer. Its key owner is local. */
    if (c->kind == FIPSD_PEER_COMMAND_INSERT && c->insert)
        (void)fipsd_active_peer_destroy(c->insert);
    free(c);
}
static fips_instant next_wake(fipsd_active_peer *p)
{
    fips_instant at = fipsd_active_peer_next_wake(p);
    fips_instant link = fipsd_active_peer_link_next_wake(p);
    if (link.ops && (!at.ops || at.ops->compare(link, at) < 0)) at = link;
    return at;
}
uint64_t fipsd_peer_dispatcher_wait_ms(fipsd_peer_dispatcher *d, fips_instant now)
{
    uint64_t timeout = UINT64_MAX;
    for (size_t i = 0; i < fipsd_peer_table_count(d->table); ++i) {
        fips_instant at = next_wake(fipsd_peer_table_at(d->table, i));
        if (!at.ops) continue;
        if (at.ops->compare(at, now) <= 0) return 0;
        fips_duration diff = at.ops->saturating_duration_since(at, now);
        uint64_t fraction = (diff.nanos + 999999u) / 1000000u;
        uint64_t ms = diff.secs > (UINT64_MAX - fraction) / 1000 ? UINT64_MAX : diff.secs * 1000 + fraction;
        uint64_t ceiling = FIPS_DEADLINE_CEILING_SECS * 1000;
        if (ms > ceiling) ms = ceiling;
        if (ms < timeout) timeout = ms;
    }
    return timeout;
}
static bool drive(fipsd_peer_dispatcher *d, fipsd_active_peer *p, const fipsd_peer_event *e)
{
    fipsd_peer_actions a;
    fipsd_active_peer_step(p, d->clock.now(d->clock.ctx), e, &a);
    fips_instant at = fipsd_active_peer_link_next_wake(p);
    fips_instant now = d->clock.now(d->clock.ctx);
    if (e->kind == FIPSD_PEER_POLL && at.ops && at.ops->compare(now, at) >= 0)
        a.items[a.count++] = (fipsd_peer_action){.kind = FIPSD_PEER_POLL_LINK};
    if (a.count && !d->effects(d->user, p, &a)) return false;
    return fipsd_active_peer_publish(p);
}
bool fipsd_peer_dispatcher_command(fipsd_peer_dispatcher *d, fipsd_peer_command *c)
{
    bool ok = true;
    fipsd_peer_admission result = FIPSD_ADMIT_OK;
    fipsd_active_peer *p = fipsd_peer_table_find(d->table, &c->node);
    switch (c->kind) {
    case FIPSD_PEER_COMMAND_INSERT:
        result = fipsd_peer_table_insert(d->table, c->insert);
        if (result == FIPSD_ADMIT_OK) c->insert = NULL;
        break;
    case FIPSD_PEER_COMMAND_REMOVE:
        result = fipsd_peer_table_remove(d->table, &c->node); break;
    case FIPSD_PEER_COMMAND_REBIND:
        result = fipsd_peer_table_rebind(d->table, &c->node, c->transport, &c->addr); break;
    case FIPSD_PEER_COMMAND_EVENT:
        if (!p) result = FIPSD_ADMIT_DUPLICATE;
        else if (!drive(d, p, &c->event)) ok = false;
        break;
    }
    if (d->result) d->result(d->user, c, result);
    fipsd_peer_command_discard(c);
    return ok;
}
bool fipsd_peer_dispatcher_service(fipsd_peer_dispatcher *d)
{
    /* Service due deadlines even under a continuously nonempty mailbox. */
    fips_instant now = d->clock.now(d->clock.ctx);
    for (size_t i = 0; i < fipsd_peer_table_count(d->table); ++i) {
        fipsd_active_peer *p = fipsd_peer_table_at(d->table, i);
        fips_instant at = next_wake(p);
        if (at.ops && at.ops->compare(now, at) >= 0) {
            fipsd_peer_event e = {.kind = FIPSD_PEER_POLL};
            if (!drive(d, p, &e)) return false;
            /* Effects may remove members; re-read the count and stay in range. */
            if (i >= fipsd_peer_table_count(d->table)) break;
        }
    }
    return fipsd_peer_table_publish(d->table);
}
bool fipsd_peer_dispatcher_run(fipsd_peer_dispatcher *d)
{
    bool ok = true;
    for (;;) {
        if (go_context_is_canceled(d->context)) break;
        void *msg = NULL, *cancel = NULL;
        GoSelectCase cases[] = {
            {GO_SELECT_RECEIVE, go_context_done(d->context), NULL, &cancel},
            {GO_SELECT_RECEIVE, d->inbox, NULL, &msg}
        };
        uint64_t timeout = fipsd_peer_dispatcher_wait_ms(d, d->clock.now(d->clock.ctx));
        GoSelectResult r;
        if (timeout == UINT64_MAX) {
            int selected = go_select(cases, 2);
            if (selected < 0) { ok = false; break; }
            /* A closed inbox may still yield a buffered owned command. Commands
             * are non-NULL, so distinguish that successful dequeue from EOF. */
            r = (GoSelectResult){selected, selected != 1 || msg != NULL};
        } else r = go_select_timeout(cases, 2, timeout);
        if (r.selected_case == 0 || (r.selected_case == 1 && !r.ok)) break;
        if (r.selected_case == 1 && !fipsd_peer_dispatcher_command(d, msg)) { ok = false; break; }
        if (!fipsd_peer_dispatcher_service(d)) { ok = false; break; }
    }
    /* Close before drain so a racing producer either succeeds into the drain
     * or observes closed and keeps ownership. */
    go_channel_close(d->inbox);
    void *left = NULL;
    while (go_channel_try_receive(d->inbox, &left) == 0) fipsd_peer_command_discard(left);
    return ok;
}
