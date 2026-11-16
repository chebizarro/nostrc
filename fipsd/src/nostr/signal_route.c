#include "nostr/signal_route.h"
#include "nostr/internal.h"
#include <nostr/nip40/nip40.h>
#include <stdlib.h>
#include <string.h>
bool fipsd_signal_router_init(fipsd_signal_router *r, const char *recipient, uint64_t now, uint64_t replay)
{
    if (!r || !recipient || strlen(recipient) != 64) return false;
    memset(r, 0, sizeof(*r));
    if (pthread_mutex_init(&r->mutex, NULL)) return false;
    r->signals = go_channel_create(FIPSD_SIGNAL_BACKLOG);
    r->seen.ids = calloc(FIPSD_SIGNAL_BACKLOG, sizeof(*r->seen.ids));
    r->expires = calloc(FIPSD_SIGNAL_BACKLOG, sizeof(*r->expires));
    r->seen.capacity = FIPSD_SIGNAL_BACKLOG;
    r->window_ms = replay;
    memcpy(r->recipient, recipient, 65); fipsd_signal_gate_init(&r->gate, now);
    if (!r->signals || !r->seen.ids || !r->expires) { fipsd_signal_router_clear(r); return false; }
    return true;
}
void fipsd_signal_router_clear(fipsd_signal_router *r)
{
    if (r->signals) {
        void *p;
        while (go_channel_try_receive(r->signals, &p) == 0) nostr_event_free(p);
        go_channel_unref(r->signals);
    }
    free(r->seen.ids); free(r->expires); pthread_mutex_destroy(&r->mutex);
}
void fipsd_signal_router_awaiting(fipsd_signal_router *r, bool value)
{ pthread_mutex_lock(&r->mutex); r->awaiting = value; pthread_mutex_unlock(&r->mutex); }
bool fipsd_signal_router_route(fipsd_signal_router *r, NostrEvent *e, uint64_t mono, uint64_t wall)
{
    bool accepted = false; uint8_t id[32];
    const char *target = fn_tag(e, "p");
    if (!target || strcmp(target, r->recipient) || e->kind != NOSTR_KIND_FIPS_TRAVERSAL ||
        !e->content || strlen(e->content) > FIPSD_NOSTR_EVENT_MAX ||
        !fn_tags_bounded(e) || !fipsd_nostr_event_id(e, id) ||
        nostr_nip40_is_expired_at(e, (int64_t)(wall / 1000))) goto done;
    pthread_mutex_lock(&r->mutex);
    for (size_t i = 0; i < r->seen.count;) {
        if (r->expires[i] <= mono) {
            --r->seen.count;
            memcpy(r->seen.ids[i], r->seen.ids[r->seen.count], 32);
            r->expires[i] = r->expires[r->seen.count];
        } else ++i;
    }
    if (fipsd_nostr_seen_contains(&r->seen, id)) ++r->duplicates;
    else if (fipsd_signal_gate_admit(&r->gate, r->awaiting, mono) != FIPSD_SIGNAL_ADMIT) ++r->shed;
    else if (r->seen.count == r->seen.capacity) ++r->full;
    else if (nostr_event_validate(e, NULL) == NOSTR_EVENT_VALIDATION_OK) {
        if (go_channel_try_send(r->signals, e) == 0) {
            r->expires[r->seen.count] = fn_add(mono, r->window_ms);
            accepted = fipsd_nostr_seen_insert(&r->seen, id);
        } else ++r->full;
    }
    pthread_mutex_unlock(&r->mutex);
done:
    if (!accepted) nostr_event_free(e);
    return accepted;
}
