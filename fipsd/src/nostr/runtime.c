#define _POSIX_C_SOURCE 200809L
#include "nostr/runtime.h"
#include "nostr/internal.h"
#include "nostr/inbox.h"
#include "nostr/signal_route.h"
#include "log.h"
#include "host_os.h"
#include <limits.h>
#include <stdio.h>
#include <nostr-simple-pool.h>
#include <nostr-subscription.h>
#include <select.h>
#include <ticker.h>
#include <sodium.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

#define PEER_MAX 4096u
#define RELAY_MAX (FIPSD_NOSTR_RELAYS_MAX * 2u)
#define RETRY_MAX_MS UINT64_C(300000)
typedef enum { EVENT, EOSE, CLOSED, AUTH, DISCONNECTED, TRACK, REFETCH, SEND, FINISH, RESULT } message_kind;
typedef struct {
    message_kind kind;
    size_t relay;
    unsigned query;
    uintptr_t connection;
    bool live, success, mismatch;
    NostrEvent *event;
    char peer[65], reason[FIPSD_NOSTR_REASON_MAX + 1];
    fipsd_nostr_traversal *signal;
} message;
typedef struct {
    char url[FIPSD_NOSTR_URL_MAX + 1];
    NostrRelay *relay;
    bool adverts, signals, usable, live;
    uint64_t retry_at, backoff, auth_generation;
} relay_slot;
typedef struct {
    bool used, configured, delivered;
    char key[65], id[65];
    uint64_t created, expires;
    fipsd_nostr_advert *advert;
} peer_slot;
typedef struct {
    char sender[64], session[FIPSD_NOSTR_SESSION_MAX + 1];
    uint64_t expires;
    bool active, outbound;
    fipsd_nostr_traversal *offer;
} session_slot;
typedef struct {
    struct fipsd_nostr_runtime *owner;
    NostrSimplePool *pool;
    unsigned id;
} callback_source;
struct fipsd_nostr_runtime {
    fipsd_subsystem subsystem;
    const fips_identity *identity;
    const fipsd_node_context *node;
    fipsd_rendezvous_driver driver;
    fips_secp *secp;
    uint64_t mismatch_ms;
    char *app, key[65];
    bool open, disabled, advertise, started, subscribed;
    uint64_t advert_ttl, refresh, signal_ttl, replay;
    size_t peer_cap, session_cap, outgoing_cap, incoming_cap, sender_cap;
    peer_slot *peers;
    session_slot *sessions, *outgoing;
    relay_slot relays[RELAY_MAX];
    size_t relay_count;
    fipsd_nostr_inbox inbox;
    fipsd_nostr_advert local;
    fipsd_nostr_failure_state failures;
    fipsd_signal_router router;
    bool router_ready;
    GoChannel *incoming, *commands;
    CancelContextResult cancel;
    pthread_t thread;
    callback_source main_source, query_source;
    unsigned query_serial;
    char query_peer[65];
    uint64_t query_eose, query_expected, query_deadline;
    bool query_any, query_invalid;
    NostrEvent *query_best;
    Ticker *ticker;
};
static uint64_t clock_ms(clockid_t clock)
{
    struct timespec t; clock_gettime(clock, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
static uint64_t wall(void) { return clock_ms(CLOCK_REALTIME); }
static uint64_t mono(void) { return clock_ms(CLOCK_MONOTONIC); }
static bool canceled(void *user)
{ return go_context_is_canceled(((fipsd_nostr_runtime *)user)->cancel.context); }
static bool key_valid(const char *key)
{
    uint8_t b[32]; return key && strlen(key) == 64 && sodium_hex2bin(b, 32, key, 64, NULL, NULL, NULL) == 0;
}
static void discard(message *m)
{ if (m) { nostr_event_free(m->event); free(m->signal); free(m); } }
static message *new_message(message_kind kind)
{ message *m = calloc(1, sizeof(*m)); if (m) m->kind = kind; return m; }
static void enqueue(fipsd_nostr_runtime *r, GoChannel *queue, message *m)
{
    if (!m || go_channel_try_send(queue, m)) {
        discard(m);
        /* A lost EOSE/CLOSED/AUTH would masquerade as healthy progress. Fail
         * the actor visibly, never infer an empty refetch or a live boundary. */
        FIPSD_ERROR("nostr", "rendezvous mailbox exhausted; stopping relay runtime");
        r->cancel.cancel(r->cancel.context);
    }
}
static size_t relay_index(const fipsd_nostr_runtime *r, const NostrRelay *relay)
{
    for (size_t i = 0; i < r->relay_count; ++i) if (!strcmp(r->relays[i].url, nostr_relay_get_url_const(relay))) return i;
    return RELAY_MAX;
}
static void middleware(NostrIncomingEvent *in, void *user)
{
    callback_source *source = user; fipsd_nostr_runtime *r = source->owner;
    if (!source->id && in->event->kind == NOSTR_KIND_FIPS_TRAVERSAL) {
        fipsd_signal_router_route(&r->router, in->event, mono(), wall()); return;
    }
    message *m = new_message(EVENT);
    if (!m) { nostr_event_free(in->event); enqueue(r, r->incoming, NULL); return; }
    m->event = in->event; m->relay = relay_index(r, in->relay); m->connection = (uintptr_t)in->relay; m->query = source->id;
    m->live = nostr_simple_pool_get_callback_live_since(source->pool) != 0;
    enqueue(r, r->incoming, m);
}
static void subscription(NostrRelay *relay, const char *state, const char *reason, void *user)
{
    callback_source *source = user; fipsd_nostr_runtime *r = source->owner;
    bool closed = !strcmp(state, "CLOSED");
    message *m = new_message(closed ? CLOSED : EOSE);
    if (m) {
        m->relay = relay_index(r, relay); m->connection = (uintptr_t)relay; m->query = source->id;
        if (reason) snprintf(m->reason, sizeof(m->reason), "%s", reason);
    }
    enqueue(r, r->incoming, m);
}
static void auth_challenge(NostrRelay *relay, const char *challenge, void *user)
{
    (void)challenge; fipsd_nostr_runtime *r = user; message *m = new_message(AUTH);
    if (m) { m->relay = relay_index(r, relay); m->connection = (uintptr_t)relay; }
    enqueue(r, r->commands, m);
}
static void connection_state(NostrRelay *relay, NostrRelayConnectionState old, NostrRelayConnectionState state, void *user)
{
    (void)old;
    if (state != NOSTR_RELAY_STATE_DISCONNECTED) return;
    fipsd_nostr_runtime *r = user; message *m = new_message(DISCONNECTED);
    if (m) { m->relay = relay_index(r, relay); m->connection = (uintptr_t)relay; }
    enqueue(r, r->commands, m);
}
static bool nip(const RelayInformationDocument *doc, int n)
{
    for (int i = 0; i < doc->supported_nips_count; ++i) if (doc->supported_nips[i] == n) return true;
    return false;
}
bool fipsd_nostr_relay_capable(const RelayInformationDocument *doc, bool signals)
{
    (void)signals; /* 21059 is NIP-01 ephemeral; NIP-59 is client-side crypto. */
    if (!doc || !nip(doc, 1) || !nip(doc, 40)) return false;
    const RelayLimitationDocument *l = doc->limitation;
    return !l || ((!l->auth_required || nip(doc, 42)) && !l->payment_required && !l->min_pow_difficulty &&
        (!l->max_filters || l->max_filters >= 2) && (!l->max_subscriptions || l->max_subscriptions >= 2));
}
fipsd_nostr_refetch_outcome fipsd_nostr_refetch_decide(bool complete, bool any,
    bool matching, bool valid, bool room, bool cached, uint64_t old, uint64_t next)
{
    if (!complete) return FIPSD_REFETCH_SKIPPED;
    if (!any) return FIPSD_REFETCH_EVICTED;
    if (!matching) return FIPSD_REFETCH_SKIPPED;
    if (cached && next <= old) return FIPSD_REFETCH_SAME_ADVERT;
    return valid && room ? FIPSD_REFETCH_REFRESHED : FIPSD_REFETCH_SKIPPED;
}
static peer_slot *peer(fipsd_nostr_runtime *r, const char *key, bool insert)
{
    peer_slot *empty = NULL; uint64_t now = wall();
    for (size_t i = 0; i < r->peer_cap; ++i) {
        peer_slot *p = &r->peers[i];
        if (p->used && !strcmp(p->key, key)) return p;
        if (p->used && !p->configured && p->expires <= now) {
            free(p->advert); *p = (peer_slot){0};
        }
        if (!p->used && !empty) empty = p;
    }
    if (!insert || !empty) return NULL;
    *empty = (peer_slot){.used = true}; memcpy(empty->key, key, 65); return empty;
}
static bool cache(fipsd_nostr_runtime *r, const NostrEvent *e, bool deliver, bool live)
{
    fipsd_nostr_advert a; uint64_t expires, now = wall();
    if (!key_valid(e->pubkey) || !strcmp(e->pubkey, r->key) ||
        nostr_event_validate(e, NULL) != NOSTR_EVENT_VALIDATION_OK ||
        fipsd_nostr_advert_parse(e, r->app, now, fn_mul1000(r->advert_ttl), &a, &expires) != FIPSD_NOSTR_OK) return false;
    peer_slot *p = peer(r, e->pubkey, r->open);
    if (!p || (r->disabled && !p->configured)) return false;
    uint64_t created = (uint64_t)e->created_at > now / 1000 ? now / 1000 : (uint64_t)e->created_at;
    if (p->advert && (created < p->created || (created == p->created && strcmp(e->id, p->id) >= 0))) return true;
    if (!p->advert) p->advert = malloc(sizeof(a));
    if (!p->advert) return false;
    p->delivered = false;
    *p->advert = a; p->created = created; p->expires = expires; memcpy(p->id, e->id, 65);
    if (r->driver.advert_cached) r->driver.advert_cached(r->driver.user, p->key, live);
    /* History is cached first. EOSE publishes its catch-up candidates; live
     * events can produce effects immediately. Cache replacement is idempotent. */
    const fipsd_nostr_failure *failure = fipsd_nostr_failure_get(&r->failures, p->key);
    if (deliver && live && !r->disabled && (!failure || failure->cooldown_ms <= now) && r->driver.dial_candidate) {
        p->delivered = true; r->driver.dial_candidate(r->driver.user, p->key, p->advert);
    }
    return true;
}
static void catchup(fipsd_nostr_runtime *r)
{
    if (r->disabled || !r->driver.dial_candidate) return;
    for (size_t i = 0; i < r->peer_cap; ++i) {
        peer_slot *p = &r->peers[i];
        const fipsd_nostr_failure *failure = p->used ? fipsd_nostr_failure_get(&r->failures, p->key) : NULL;
        if (p->used && p->advert && p->expires > wall() && !p->delivered &&
            (!failure || failure->cooldown_ms <= wall())) {
            p->delivered = true;
            r->driver.dial_candidate(r->driver.user, p->key, p->advert);
        }
    }
}
static bool filter_add(NostrFilters *all, NostrFilter *f)
{
    bool ok = f && nostr_filters_add(all, f);
    if (f) { if (!ok) nostr_filter_clear(f); free(f); }
    return ok;
}
static NostrFilter *advert_filter(fipsd_nostr_runtime *r, const char *only, bool fetch)
{
    NostrFilter *f = nostr_filter_new(); if (!f) return NULL;
    int kind = NOSTR_KIND_FIPS_OVERLAY_ADVERT; nostr_filter_set_kinds(f, &kind, 1);
    NostrTags *tags = nostr_tags_new(1, nostr_tag_new("d", FIPSD_NOSTR_IDENTIFIER, NULL));
    nostr_filter_set_tags(f, tags);
    if (only) nostr_filter_set_authors(f, &only, 1);
    else if (!r->open) {
        const char *keys[PEER_MAX]; size_t n = 0;
        for (size_t i = 0; i < r->peer_cap; ++i) if (r->peers[i].used) keys[n++] = r->peers[i].key;
        /* Empty author arrays mean wildcard in NIP-01, not no authors. */
        if (!n) keys[n++] = r->key;
        nostr_filter_set_authors(f, keys, n);
    }
    uint64_t now = wall() / 1000;
    nostr_filter_set_since_i64(f, fetch ? 0 : (int64_t)(now > r->advert_ttl ? now - r->advert_ttl : 0));
    if (fetch) { nostr_filter_set_until_i64(f, (int64_t)now); nostr_filter_set_limit(f, 1); }
    /* No aggregate limit on history: the bounded replaceable cache, not an
     * arbitrary page truncation, controls admission. Open discovery is #d scoped. */
    return f;
}
static bool subscribe_main(fipsd_nostr_runtime *r)
{
    NostrSimplePool *pool = r->main_source.pool;
    nostr_simple_pool_stop(pool); /* no concurrent filter/subscription mutation */
    NostrFilters *fs = nostr_filters_new(); if (!fs) return false;
    bool ok = filter_add(fs, advert_filter(r, NULL, false));
    NostrFilter *s = nostr_filter_new();
    if (s) {
        int kind = NOSTR_KIND_FIPS_TRAVERSAL; nostr_filter_set_kinds(s, &kind, 1);
        nostr_filter_set_tags(s, nostr_tags_new(1, nostr_tag_new("p", r->key, NULL)));
        nostr_filter_set_since_i64(s, (int64_t)(wall() / 1000)); nostr_filter_set_limit_zero(s, true);
    }
    ok = filter_add(fs, s) && ok;
    const char *urls[RELAY_MAX]; size_t n = 0;
    for (size_t i = 0; i < r->relay_count; ++i) if (r->relays[i].usable) urls[n++] = r->relays[i].url;
    if (ok && n) {
        /* Unique=false: middleware's signal replay set is independent of the
         * advert cache and refetches must see an unchanged advert again. */
        nostr_simple_pool_subscribe_async(pool, urls, n, *fs, false);
        nostr_simple_pool_start(pool);
        ok = atomic_load(&pool->running);
    } else ok = false;
    nostr_filters_free(fs); r->subscribed = ok; return ok;
}
static bool published(fipsd_nostr_runtime *r, relay_slot *s, NostrEvent *event)
{
    Error *err = NULL;
    bool ok = nostr_relay_publish_and_wait(s->relay, event, 1000, &err);
    if (!ok) FIPSD_WARN("nostr", "publish kind %d refused by %s: %s", event->kind, s->url, err ? err->message : "no OK");
    if (r->driver.publish_result) r->driver.publish_result(r->driver.user, s->url, event->kind, ok);
    if (err) free_error(err);
    return ok;
}
static void publish_local(fipsd_nostr_runtime *r, relay_slot *only)
{
    if (!r->advertise) return;
    fipsd_nostr_advert a = r->local;
    if (!r->driver.local_advert || !r->driver.local_advert(r->driver.user, &a)) return;
    NostrEvent *e = NULL, *inbox = NULL;
    if (fipsd_nostr_advert_build(r->identity, &a, r->app, wall() / 1000, r->advert_ttl, &e) != FIPSD_NOSTR_OK) {
        FIPSD_WARN("nostr", "local advert refused (no bounded usable endpoints/relay/STUN inputs)"); return;
    }
    if (r->inbox.count) fipsd_nostr_inbox_build(r->identity, &r->inbox, wall() / 1000, &inbox);
    for (size_t i = 0; i < r->relay_count && !canceled(r); ++i) {
        relay_slot *s = &r->relays[i];
        if (!s->usable || !s->live || (only && s != only) || !nostr_relay_is_connected(s->relay)) continue;
        if (s->adverts) published(r, s, e);
        if (s->signals && inbox) published(r, s, inbox);
    }
    nostr_event_free(e); nostr_event_free(inbox);
}
static _Thread_local const fips_identity *auth_identity;
static void sign_auth(NostrEvent *e, Error **err)
{
    if (!auth_identity || fn_sign(e, auth_identity) != FIPSD_NOSTR_OK)
        *err = new_error(1, "FIPS AUTH signing failed");
}
static void authenticate(fipsd_nostr_runtime *r, relay_slot *s)
{
    uint64_t generation; char *challenge = nostr_relay_get_auth_challenge(s->relay, &generation);
    if (!challenge || s->auth_generation == generation) { free(challenge); return; }
    free(challenge); s->auth_generation = generation;
    Error *err = NULL; auth_identity = r->identity;
    nostr_relay_auth(s->relay, sign_auth, &err); auth_identity = NULL;
    if (r->driver.publish_result) r->driver.publish_result(r->driver.user, s->url, NOSTR_KIND_CLIENT_AUTHENTICATION, err == NULL);
    if (err) { FIPSD_WARN("nostr", "AUTH %s: %s", s->url, err->message); free_error(err); }

}
static void finish_query(fipsd_nostr_runtime *, bool);
static NostrRelay *create_relay(fipsd_nostr_runtime *r, const char *url)
{
    Error *err = NULL; NostrRelay *relay = nostr_relay_new(r->cancel.context, url, &err);
    if (err) free_error(err);
    if (relay) {
        nostr_relay_set_auto_reconnect(relay, false);
        nostr_relay_set_auth_callback(relay, auth_challenge, r);
        nostr_relay_set_state_callback(relay, connection_state, r);
    }
    return relay;
}
static void retry(fipsd_nostr_runtime *r, relay_slot *s)
{
    if (s->retry_at || canceled(r)) return;
    /* close() is terminal in libnostr: a closed relay cannot be dialled again.
     * Retire its subscriptions while the pool worker is joined, then install
     * a fresh relay before releasing the old identity (generation fence).
     * All accepted old EVENTs remain queued; stale control frames are inert. */
    if (r->query_source.pool) finish_query(r, false);
    bool resume = r->subscribed;
    nostr_simple_pool_stop(r->main_source.pool);
    NostrRelay *old = s->relay, *next = create_relay(r, s->url);
    if (!next) { r->cancel.cancel(r->cancel.context); return; }
    nostr_relay_set_auth_callback(old, NULL, NULL);
    nostr_relay_set_state_callback(old, NULL, NULL);
    nostr_simple_pool_remove_relay(r->main_source.pool, s->url);
    s->relay = next; s->auth_generation = 0; s->live = false;
    nostr_simple_pool_add_relay(r->main_source.pool, nostr_relay_ref(next));
    nostr_relay_unref(old);
    s->retry_at = fn_add(mono(), s->backoff);
    s->backoff = s->backoff < RETRY_MAX_MS / 2 ? s->backoff * 2 : RETRY_MAX_MS;
    if (resume && !subscribe_main(r)) r->cancel.cancel(r->cancel.context);
}
static void connect_relay(fipsd_nostr_runtime *r, relay_slot *s)
{
    s->retry_at = 0; Error *err = NULL;
    if (!nostr_relay_connect_with_context(s->relay, r->cancel.context, &err)) {
        FIPSD_WARN("nostr", "connect %s: %s", s->url, err ? err->message : "failed");
        if (err) free_error(err); retry(r, s); return;
    }
    authenticate(r, s); publish_local(r, s);
}
static session_slot *find_session(session_slot *slots, size_t cap, const char *id)
{
    uint64_t now = mono();
    for (size_t i = 0; i < cap; ++i) {
        session_slot *s = &slots[i];
        if (s->expires && s->expires <= now) { free(s->offer); *s = (session_slot){0}; }
        if (s->expires && !strcmp(s->session, id)) return s;
    }
    return NULL;
}
static session_slot *reserve_session(fipsd_nostr_runtime *r, const fipsd_nostr_traversal *p, bool outbound)
{
    session_slot *slots = outbound ? r->outgoing : r->sessions;
    size_t cap = outbound ? r->outgoing_cap : r->session_cap;
    if (find_session(slots, cap, p->session_id)) return NULL;
    size_t active = 0, sender = 0; session_slot *empty = NULL;
    for (size_t i = 0; i < cap; ++i) {
        session_slot *s = &slots[i];
        if (outbound && s->active && s->offer && !strcmp(s->offer->recipient_npub, p->recipient_npub)) return NULL;
        if (!s->expires && !empty) empty = s;
        if (s->active && !s->outbound) { ++active; if (!strcmp(s->sender, p->sender_npub)) ++sender; }
    }
    if (!empty || (!outbound && ((r->incoming_cap && active >= r->incoming_cap) ||
        (r->sender_cap && sender >= r->sender_cap)))) return NULL;
    /* Refuse when full: never evict a remembered unexpired session to admit an
     * attacker-selected fresh session (upstream 2026-11-12 replay guarantee). */
    *empty = (session_slot){.expires = fn_add(mono(), fn_mul1000(r->replay)), .active = true, .outbound = outbound};
    memcpy(empty->sender, p->sender_npub, sizeof(empty->sender));
    memcpy(empty->session, p->session_id, sizeof(empty->session));
    if (outbound) {
        empty->offer = malloc(sizeof(*p));
        if (!empty->offer) { *empty = (session_slot){0}; return NULL; }
        *empty->offer = *p;
    }
    return empty;
}
static void awaiting(fipsd_nostr_runtime *r)
{
    bool any = false;
    for (size_t i = 0; i < r->outgoing_cap; ++i) if (r->outgoing[i].active) any = true;
    fipsd_signal_router_awaiting(&r->router, any);
}
static void receive_signal(fipsd_nostr_runtime *r, NostrEvent *e)
{
    fipsd_nostr_traversal p; fipsd_nostr_freshness fresh;
    fipsd_nostr_status status = fipsd_nostr_traversal_open(r->identity, e, wall(), fn_mul1000(r->signal_ttl), &p, &fresh);
    if (status != FIPSD_NOSTR_OK) { nostr_event_free(e); return; }
    if (p.answer) {
        session_slot *s = NULL;
        for (size_t i = 0; i < r->outgoing_cap; ++i) {
            session_slot *candidate = &r->outgoing[i];
            if (candidate->active && candidate->outbound && candidate->offer &&
                !strcmp(candidate->offer->nonce, p.in_reply_to) &&
                !strcmp(candidate->offer->recipient_npub, p.sender_npub)) { s = candidate; break; }
        }
        if (s && fipsd_nostr_traversal_answer_for_offer(s->offer, &p, wall(), fn_mul1000(r->signal_ttl), &fresh) == FIPSD_NOSTR_OK) {
            uint64_t now = wall(), received = p.offer_received_at;
            uint64_t distance = received > now ? received - now : now - received;
            if (p.has_offer_received_at && distance <= fn_add(fn_mul1000(r->signal_ttl), FIPSD_NOSTR_SKEW_MS) &&
                now <= INT64_MAX && received <= INT64_MAX && p.issued_at <= INT64_MAX && s->offer->issued_at <= INT64_MAX) {
                int64_t first = (int64_t)received - (int64_t)s->offer->issued_at;
                int64_t second = (int64_t)p.issued_at - (int64_t)now;
                int64_t skew = first / 2 + second / 2 + (first % 2 + second % 2) / 2;
                fips_peer_identity sender;
                if (fips_peer_identity_from_npub(r->secp, p.sender_npub, strlen(p.sender_npub), &sender) == FIPS_IDENTITY_OK) {
                    char hex[65]; sodium_bin2hex(hex, sizeof(hex), sender.pubkey.bytes, 32);
                    fipsd_nostr_failure_skew(&r->failures, hex, skew, now);
                }
            }
            free(s->offer); *s = (session_slot){0};
            if (r->driver.traversal_answer) r->driver.traversal_answer(r->driver.user, &p);
        }
    } else if (!r->disabled) {
        bool allowed = r->open;
        for (size_t i = 0; !allowed && i < r->peer_cap; ++i) {
            peer_slot *q = &r->peers[i]; fips_peer_identity identity;
            if (q->used && q->configured && fips_peer_identity_from_npub(r->secp, p.sender_npub,
                strlen(p.sender_npub), &identity) == FIPS_IDENTITY_OK) {
                char hex[65]; sodium_bin2hex(hex, sizeof(hex), identity.pubkey.bytes, 32);
                allowed = !strcmp(hex, q->key);
            }
        }
        session_slot *s = allowed ? reserve_session(r, &p, false) : NULL;
        if (r->driver.signal_admission) r->driver.signal_admission(r->driver.user, p.session_id, s != NULL);
        if (s && (!r->driver.traversal_offer || !r->driver.traversal_offer(r->driver.user, &p))) {
            s->active = false;
            FIPSD_DEBUG("nostr", "traversal offer refused by driver (8hvs6.3 attachment seam)");
        }
    }
    awaiting(r); nostr_event_free(e);
}
static void finish_query(fipsd_nostr_runtime *r, bool complete)
{
    if (!r->query_source.pool) return;
    peer_slot *p = peer(r, r->query_peer, false);
    bool valid = false; uint64_t next = 0, expires; fipsd_nostr_advert advert;
    if (r->query_best) {
        next = (uint64_t)r->query_best->created_at;
        if (next > wall() / 1000) next = wall() / 1000;
        valid = fipsd_nostr_advert_parse(r->query_best, r->app, wall(), fn_mul1000(r->advert_ttl), &advert, &expires) == FIPSD_NOSTR_OK;
    }
    if (!p && valid) p = peer(r, r->query_peer, true);
    fipsd_nostr_refetch_outcome outcome = fipsd_nostr_refetch_decide(complete && !r->query_invalid,
        r->query_any, r->query_best != NULL, valid, p != NULL,
        p && p->advert, p ? p->created : 0, next);
    if (outcome == FIPSD_REFETCH_REFRESHED && !cache(r, r->query_best, false, false)) outcome = FIPSD_REFETCH_SKIPPED;
    if (outcome == FIPSD_REFETCH_EVICTED && p) {
        free(p->advert); p->advert = NULL; p->expires = 0; p->created = 0; p->id[0] = 0;
    }
    if (outcome == FIPSD_REFETCH_REFRESHED || outcome == FIPSD_REFETCH_EVICTED)
        fipsd_nostr_failure_refresh(&r->failures, r->query_peer);
    nostr_simple_pool_set_subscription_callback(r->query_source.pool, NULL, NULL);
    nostr_simple_pool_stop(r->query_source.pool);
    nostr_simple_pool_free(r->query_source.pool); r->query_source.pool = NULL;
    nostr_event_free(r->query_best); r->query_best = NULL; r->query_deadline = 0;
    if (r->driver.refetch_result) r->driver.refetch_result(r->driver.user, r->query_peer, outcome);
}
static void start_query(fipsd_nostr_runtime *r, const char *key)
{
    if (r->query_source.pool || r->query_serial == UINT_MAX) {
        if (r->driver.refetch_result) r->driver.refetch_result(r->driver.user, key, FIPSD_REFETCH_SKIPPED);
        return;
    }
    uint64_t expected = 0; const char *urls[RELAY_MAX]; size_t n = 0;
    for (size_t i = 0; i < r->relay_count; ++i) if (r->relays[i].usable && r->relays[i].adverts) {
        /* Absence requires every configured compatible advert relay. A relay
         * offline at fetch start is failure, never evidence of absence. */
        if (!nostr_relay_is_connected(r->relays[i].relay)) goto skipped;
        expected |= UINT64_C(1) << i; urls[n++] = r->relays[i].url;
    }
    if (!n) goto skipped;
    NostrSimplePool *pool = nostr_simple_pool_new(); if (!pool) goto skipped;
    r->query_source = (callback_source){r, pool, ++r->query_serial};
    r->query_eose = 0; r->query_expected = expected; r->query_any = false; r->query_invalid = false;
    memcpy(r->query_peer, key, 65);
    r->query_deadline = fn_add(mono(), 10000); /* failure deadline, NEVER successful completion */
    nostr_simple_pool_set_auto_unsub_on_eose(pool, true);
    nostr_simple_pool_set_event_middleware_ex(pool, middleware, &r->query_source);
    nostr_simple_pool_set_subscription_callback(pool, subscription, &r->query_source);
    for (size_t i = 0; i < r->relay_count; ++i) if (expected & (UINT64_C(1) << i))
        nostr_simple_pool_add_relay(pool, nostr_relay_ref(r->relays[i].relay));
    NostrFilters *fs = nostr_filters_new();
    if (!fs || !filter_add(fs, advert_filter(r, key, true))) {
        if (fs) nostr_filters_free(fs); finish_query(r, false); return;
    }
    nostr_simple_pool_subscribe_async(pool, urls, n, *fs, false); nostr_filters_free(fs);
    nostr_simple_pool_start(pool);
    if (!atomic_load(&pool->running)) finish_query(r, false);
    return;
skipped:
    if (r->driver.refetch_result) r->driver.refetch_result(r->driver.user, key, FIPSD_REFETCH_SKIPPED);
}
static void send_signal(fipsd_nostr_runtime *r, message *m)
{
    fipsd_nostr_traversal *p = m->signal; NostrEvent *e = NULL;
    session_slot *s = NULL;
    if (!p->answer && !(s = reserve_session(r, p, true))) return;
    awaiting(r);
    if (fipsd_nostr_traversal_build(r->identity, m->peer, p, wall() / 1000, r->signal_ttl, &e) != FIPSD_NOSTR_OK) {
        if (s) { free(s->offer); *s = (session_slot){0}; }
        awaiting(r); return;
    }
    bool accepted = false;
    peer_slot *target = peer(r, m->peer, false);
    for (size_t i = 0; i < r->relay_count && !canceled(r); ++i) {
        relay_slot *relay = &r->relays[i]; bool selected = relay->signals;
        /* Remote hints are bounded and may choose only probed configured
         * relays: never let an advert turn into arbitrary network destinations. */
        if (target && target->advert && target->advert->has_relays) {
            selected = false;
            for (size_t j = 0; j < target->advert->relay_count; ++j)
                if (!strcmp(relay->url, target->advert->relays[j])) selected = true;
        }
        if (relay->usable && selected && nostr_relay_is_connected(relay->relay)) {
            bool ok = published(r, relay, e); accepted = accepted || ok;
        }
    }
    nostr_event_free(e);
    if (s && !accepted) { free(s->offer); *s = (session_slot){0}; awaiting(r); }
}
static void process(fipsd_nostr_runtime *r, message *m)
{
    if (m->query) {
        if (!r->query_source.pool || m->query != r->query_source.id) return;
        if (m->kind == EVENT) {
            r->query_any = true;
            if (nostr_event_validate(m->event, NULL) != NOSTR_EVENT_VALIDATION_OK) r->query_invalid = true;
            else if (m->event->pubkey && !strcmp(m->event->pubkey, r->query_peer) &&
                (!r->query_best || m->event->created_at > r->query_best->created_at)) {
                nostr_event_free(r->query_best); r->query_best = m->event; m->event = NULL;
            }
        } else if (m->kind == EOSE && m->relay < r->relay_count) {
            r->query_eose |= UINT64_C(1) << m->relay;
            if (r->query_eose == r->query_expected) finish_query(r, true);
        } else if (m->kind == CLOSED) finish_query(r, false);
        return;
    }
    relay_slot *s = m->relay < r->relay_count ? &r->relays[m->relay] : NULL;
    if (s && m->connection && m->connection != (uintptr_t)s->relay && m->kind != EVENT && m->kind != CLOSED) return;
    switch (m->kind) {
    case EVENT: cache(r, m->event, true, m->live); break;
    case EOSE:
        if (s) {
            s->live = true; s->backoff = 1000; catchup(r);
            if (r->driver.boundary) r->driver.boundary(r->driver.user, s->url, true, NULL);
            publish_local(r, s);
        }
        break;
    case CLOSED:
        if (s) {
            FIPSD_WARN("nostr", "CLOSED %s: %s", s->url, m->reason);
            if (r->driver.boundary) r->driver.boundary(r->driver.user, s->url, false, m->reason);
            s->live = false;
        }
        break;
    case DISCONNECTED:
        if (s && s->usable && !nostr_relay_is_connected(s->relay) && !s->retry_at) {
            if (r->driver.boundary) r->driver.boundary(r->driver.user, s->url, false, "connection lost");
            retry(r, s);
        }
        break;
    case AUTH: if (s && s->usable) authenticate(r, s); break;
    case TRACK: {
        peer_slot *p = peer(r, m->peer, true);
        if (p) { p->configured = true; if (!subscribe_main(r)) r->cancel.cancel(r->cancel.context); }
        break;
    }
    case REFETCH: start_query(r, m->peer); break;
    case SEND: send_signal(r, m); break;
    case FINISH: {
        session_slot *q = find_session(r->sessions, r->session_cap, m->reason); if (q) q->active = false;
        q = find_session(r->outgoing, r->outgoing_cap, m->reason); if (q) { free(q->offer); *q = (session_slot){0}; }
        awaiting(r); break;
    }
    case RESULT:
        if (m->success) fipsd_nostr_failure_success(&r->failures, m->peer, wall());
        else if (m->mismatch) fipsd_nostr_failure_mismatch(&r->failures, m->peer, wall(),
            r->mismatch_ms);
        else {
            fipsd_nostr_failure_decision decision = fipsd_nostr_failure_record(&r->failures, m->peer, wall());
            if (decision.warn) FIPSD_WARN("nostr", "peer traversal failed; streak=%u", decision.streak);
            if (decision.crossed_threshold) start_query(r, m->peer);
        }
        break;
    }
}
static bool startup(fipsd_nostr_runtime *r)
{
    NostrSimplePool *pool = nostr_simple_pool_new();
    if (!pool) return false;
    r->main_source = (callback_source){r, pool, 0};
    nostr_simple_pool_set_event_middleware_ex(pool, middleware, &r->main_source);
    nostr_simple_pool_set_subscription_callback(pool, subscription, &r->main_source);
    nostr_simple_pool_set_auto_unsub_on_eose(pool, false);
    if (!nostr_simple_pool_set_recovery_window(pool, (unsigned)r->advert_ttl) ||
        !nostr_simple_pool_set_closed_backoff(pool, 1000, (unsigned)RETRY_MAX_MS)) return false;
    size_t usable = 0;
    for (size_t i = 0; i < r->relay_count && !canceled(r); ++i) {
        relay_slot *s = &r->relays[i];
        char info_url[FIPSD_NOSTR_URL_MAX + 3];
        bool tls = !strncmp(s->url, "wss://", 6);
        snprintf(info_url, sizeof(info_url), "%s%s", tls ? "https://" : "http://", s->url + (tls ? 6 : 5));
        RelayInformationDocument *info = nostr_nip11_fetch_info_cancellable(info_url, canceled, r);
        s->usable = fipsd_nostr_relay_capable(info, s->signals);
        FIPSD_INFO("nostr", "NIP-11 %s: nip1=%d nip40=%d nip42=%d auth=%d capable=%d", s->url,
            info && nip(info, 1), info && nip(info, 40), info && nip(info, 42),
            info && info->limitation ? info->limitation->auth_required : 0, s->usable);
        nostr_nip11_free_info(info);
        if (!s->usable) {
            if (r->driver.boundary) r->driver.boundary(r->driver.user, s->url, false, "NIP-11 incompatible or unavailable");
            continue;
        }
        s->relay = create_relay(r, s->url);
        if (!s->relay) return false;
        /* One retry owner: do not race pool redial/message-loop reconnect
         * with CLOSED backoff. All failed attempts rearm only their deadline. */
        nostr_simple_pool_add_relay(pool, nostr_relay_ref(s->relay)); ++usable;
        connect_relay(r, s);
    }
    if (!usable || canceled(r) || !subscribe_main(r)) return false;
    if (r->advertise) { r->ticker = create_ticker((size_t)fn_mul1000(r->refresh)); if (!r->ticker) return false; }
    return true;
}
static void *run(void *user)
{
    fipsd_nostr_runtime *r = user;
    if (!startup(r)) FIPSD_ERROR("nostr", "rendezvous startup failed; no compatible relay runtime");
    else while (!canceled(r)) {
        uint64_t now = mono(), deadline = UINT64_MAX;
        if (r->query_deadline) deadline = r->query_deadline;
        for (size_t i = 0; i < r->relay_count; ++i)
            if (r->relays[i].retry_at && r->relays[i].retry_at < deadline) deadline = r->relays[i].retry_at;
        for (size_t i = 0; i < r->session_cap; ++i)
            if (r->sessions[i].expires && r->sessions[i].expires < deadline) deadline = r->sessions[i].expires;
        for (size_t i = 0; i < r->outgoing_cap; ++i)
            if (r->outgoing[i].expires && r->outgoing[i].expires < deadline) deadline = r->outgoing[i].expires;
        void *payload = NULL;
        GoSelectCase cases[] = {
            {.op = GO_SELECT_RECEIVE, .chan = go_context_done(r->cancel.context)},
            {.op = GO_SELECT_RECEIVE, .chan = r->commands, .recv_buf = &payload},
            {.op = GO_SELECT_RECEIVE, .chan = r->incoming, .recv_buf = &payload},
            {.op = GO_SELECT_RECEIVE, .chan = r->router.signals, .recv_buf = &payload},
            {.op = GO_SELECT_RECEIVE, .chan = r->ticker ? r->ticker->c : NULL}
        };
        GoSelectResult selected = go_select_timeout(cases, sizeof(cases) / sizeof(cases[0]),
            deadline == UINT64_MAX ? UINT64_MAX : deadline > now ? deadline - now : 0);
        if (selected.selected_case == 0) break;
        if (selected.selected_case == 1 || selected.selected_case == 2) { process(r, payload); discard(payload); }
        else if (selected.selected_case == 3) receive_signal(r, payload);
        else if (selected.selected_case == 4) publish_local(r, NULL);
        now = mono();
        if (r->query_deadline && r->query_deadline <= now) finish_query(r, false);
        for (size_t i = 0; i < r->relay_count && !canceled(r); ++i)
            if (r->relays[i].retry_at && r->relays[i].retry_at <= now) connect_relay(r, &r->relays[i]);
        /* Reap timed-out reservations, without a periodic relay query. */
        find_session(r->sessions, r->session_cap, ""); find_session(r->outgoing, r->outgoing_cap, ""); awaiting(r);
    }
    if (r->ticker) { stop_ticker(r->ticker); r->ticker = NULL; }
    if (r->query_source.pool) finish_query(r, false);
    if (r->main_source.pool) {
        nostr_simple_pool_set_subscription_callback(r->main_source.pool, NULL, NULL);
        nostr_simple_pool_stop(r->main_source.pool);
        for (size_t i = 0; i < r->relay_count; ++i) if (r->relays[i].relay) {
            nostr_relay_set_auth_callback(r->relays[i].relay, NULL, NULL);
            nostr_relay_set_state_callback(r->relays[i].relay, NULL, NULL);
            nostr_relay_request_close(r->relays[i].relay);
        }
        nostr_simple_pool_free(r->main_source.pool); r->main_source.pool = NULL;
    }
    for (size_t i = 0; i < r->relay_count; ++i) if (r->relays[i].relay) {
        nostr_relay_unref(r->relays[i].relay); r->relays[i].relay = NULL;
    }
    return NULL;
}
static int start(fipsd_subsystem *sub, fipsd_runtime *host)
{
    fipsd_nostr_runtime *r = sub->user;
    if (!r->identity && r->node) r->identity = fipsd_node_identity(r->node);
    if (!r->identity || r->started) return -1;
    fips_xonly_pubkey key = fips_identity_pubkey(r->identity);
    sodium_bin2hex(r->key, sizeof(r->key), key.bytes, 32);
    r->cancel = go_context_with_cancel(fipsd_runtime_context(host));
    if (!r->cancel.context) return -1;
    if (!fipsd_signal_router_init(&r->router, r->key, mono(), fn_mul1000(r->replay))) return -1;
    r->router_ready = true;
    if (pthread_create(&r->thread, NULL, run, r)) return -1;
    r->started = true; return 0;
}
static void stop(fipsd_subsystem *sub, fipsd_runtime *host)
{
    (void)host; fipsd_nostr_runtime *r = sub->user;
    if (!r->started) return;
    r->cancel.cancel(r->cancel.context);
    pthread_join(r->thread, NULL); r->started = false;
}
static uint64_t setting(const fipsd_config *c, const char *name)
{
    char path[160]; snprintf(path, sizeof(path), "node.rendezvous.nostr.%s", name);
    return fipsd_config_uint(fipsd_config_get(c, path));
}
static bool add_relays(fipsd_nostr_runtime *r, const fipsd_config *config, const char *path, bool signals)
{
    const fipsd_config_value *list = fipsd_config_get(config, path);
    if (fipsd_config_count(list) > FIPSD_NOSTR_RELAYS_MAX) return false;
    for (size_t i = 0; i < fipsd_config_count(list); ++i) {
        char url[FIPSD_NOSTR_URL_MAX + 1];
        if (!fn_relay_normalize(fipsd_config_string(fipsd_config_at(list, i)), url)) return false;
        size_t j = 0;
        while (j < r->relay_count && strcmp(r->relays[j].url, url)) ++j;
        if (j == r->relay_count) {
            if (j == RELAY_MAX) return false;
            memcpy(r->relays[j].url, url, strlen(url) + 1); r->relays[j].backoff = 1000; ++r->relay_count;
        }
        if (signals) {
            r->relays[j].signals = true;
            memcpy(r->inbox.relays[r->inbox.count++], url, strlen(url) + 1);
        } else r->relays[j].adverts = true;
    }
    return true;
}
fipsd_nostr_runtime *fipsd_nostr_runtime_create(const fipsd_config *c, const fips_identity *id,
    const fipsd_node_context *node, fipsd_rendezvous_driver driver)
{
    fipsd_nostr_runtime *r = calloc(1, sizeof(*r)); if (!r) return NULL;
    r->subsystem = (fipsd_subsystem){"nostr-rendezvous", start, stop, r};
    r->identity = id; r->node = node; r->driver = driver;
    const char *app = fipsd_config_string(fipsd_config_get(c, "node.rendezvous.nostr.app"));
    const char *policy = fipsd_config_string(fipsd_config_get(c, "node.rendezvous.nostr.policy"));
    r->app = app ? strdup(app) : NULL; r->open = policy && !strcmp(policy, "open");
    r->disabled = !policy || !strcmp(policy, "disabled");
    r->advertise = fipsd_config_bool(fipsd_config_get(c, "node.rendezvous.nostr.advertise"));
    r->advert_ttl = setting(c, "advert_ttl_secs"); r->refresh = setting(c, "advert_refresh_secs");
    r->signal_ttl = setting(c, "signal_ttl_secs"); r->replay = setting(c, "replay_window_secs");
    r->peer_cap = (size_t)setting(c, "advert_cache_max_entries");
    r->session_cap = (size_t)setting(c, "seen_sessions_max_entries");
    r->outgoing_cap = (size_t)setting(c, "open_discovery_max_pending");
    if (!r->outgoing_cap || r->outgoing_cap > r->peer_cap) r->outgoing_cap = r->peer_cap;
    r->incoming_cap = (size_t)setting(c, "max_concurrent_incoming_offers");
    r->sender_cap = (size_t)setting(c, "max_concurrent_offers_per_npub");
    r->mismatch_ms = fn_mul1000(setting(c, "protocol_mismatch_cooldown_secs"));
    if (!r->app || strlen(r->app) > 256 || r->advert_ttl < 2 || r->advert_ttl > UINT_MAX ||
        !r->signal_ttl || r->signal_ttl > UINT_MAX || !r->replay || r->replay > UINT_MAX || !r->peer_cap || r->peer_cap > PEER_MAX ||
        !r->session_cap || r->session_cap > 65536 || !r->refresh) goto fail;
    if (r->refresh >= r->advert_ttl) r->refresh = r->advert_ttl > 1 ? r->advert_ttl / 2 : 1;
    r->peers = calloc(r->peer_cap, sizeof(*r->peers)); r->sessions = calloc(r->session_cap, sizeof(*r->sessions));
    r->outgoing = calloc(r->outgoing_cap, sizeof(*r->outgoing));
    r->incoming = go_channel_create(4096); r->commands = go_channel_create(64);
    if (!r->peers || !r->sessions || !r->outgoing || !r->incoming || !r->commands ||
        fips_identity_init_secp(fipsd_host_os_entropy(), &r->secp) != FIPS_IDENTITY_OK ||
        !fipsd_nostr_failure_init(&r->failures, (size_t)setting(c, "failure_state_max_entries"),
            (uint32_t)setting(c, "failure_streak_threshold"), fn_mul1000(setting(c, "extended_cooldown_secs")),
            fn_mul1000(setting(c, "warn_log_interval_secs")))) goto fail;
    if (!add_relays(r, c, "node.rendezvous.nostr.advert_relays", false) ||
        !add_relays(r, c, "node.rendezvous.nostr.dm_relays", true) || !r->relay_count) goto fail;
    r->local.has_relays = true; r->local.relay_count = r->inbox.count;
    memcpy(r->local.relays, r->inbox.relays, sizeof(r->local.relays));
    const fipsd_config_value *stun = fipsd_config_get(c, "node.rendezvous.nostr.stun_servers");
    r->local.stun_count = fipsd_config_count(stun); r->local.has_stun = true;
    if (r->local.stun_count > FIPSD_NOSTR_STUN_MAX) goto fail;
    for (size_t i = 0; i < r->local.stun_count; ++i) {
        const char *s = fipsd_config_string(fipsd_config_at(stun, i));
        if (!s || strlen(s) > FIPSD_NOSTR_URL_MAX) goto fail;
        memcpy(r->local.stun[i], s, strlen(s) + 1);
    }
    const fipsd_config_value *peers = fipsd_config_get(c, "peers");
    for (size_t i = 0; i < fipsd_config_count(peers); ++i) {
        const fipsd_config_value *p = fipsd_config_at(peers, i);
        const char *npub = fipsd_config_string(fipsd_config_member(p, "npub")); fips_peer_identity identity;
        if (!npub || fips_peer_identity_from_npub(r->secp, npub, strlen(npub), &identity) != FIPS_IDENTITY_OK) goto fail;
        char key[65]; sodium_bin2hex(key, sizeof(key), identity.pubkey.bytes, 32);
        peer_slot *slot = peer(r, key, true); if (!slot) goto fail; slot->configured = true;
    }
    return r;
fail:
    fipsd_nostr_runtime_destroy(r); return NULL;
}
fipsd_subsystem *fipsd_nostr_runtime_subsystem(fipsd_nostr_runtime *r) { return r ? &r->subsystem : NULL; }
void fipsd_nostr_runtime_destroy(fipsd_nostr_runtime *r)
{
    if (!r) return;
    if (r->started) stop(&r->subsystem, NULL);
    if (r->cancel.context) { r->cancel.cancel(r->cancel.context); go_context_unref(r->cancel.context); }
    if (r->router_ready) fipsd_signal_router_clear(&r->router);
    if (r->peers) for (size_t i = 0; i < r->peer_cap; ++i) free(r->peers[i].advert);
    if (r->sessions) for (size_t i = 0; i < r->session_cap; ++i) free(r->sessions[i].offer);
    if (r->outgoing) for (size_t i = 0; i < r->outgoing_cap; ++i) free(r->outgoing[i].offer);
    GoChannel *queues[] = {r->commands, r->incoming};
    for (size_t i = 0; i < 2; ++i) if (queues[i]) {
        void *m; while (go_channel_try_receive(queues[i], &m) == 0) discard(m); go_channel_unref(queues[i]);
    }
    fips_identity_destroy_secp(r->secp); fipsd_nostr_failure_clear(&r->failures);
    free(r->peers); free(r->sessions); free(r->outgoing); free(r->app); free(r);
}
static message *peer_command(message_kind kind, const char *key)
{
    if (!key_valid(key)) return NULL;
    message *m = new_message(kind); if (m) {
        uint8_t bytes[32]; sodium_hex2bin(bytes, 32, key, 64, NULL, NULL, NULL);
        sodium_bin2hex(m->peer, sizeof(m->peer), bytes, 32);
    }
    return m;
}
static bool command(fipsd_nostr_runtime *r, message *m)
{
    if (!m) return false;
    if (!r || go_channel_try_send(r->commands, m)) { discard(m); return false; }
    return true;
}
bool fipsd_nostr_runtime_track_peer(fipsd_nostr_runtime *r, const char *key) { return command(r, peer_command(TRACK, key)); }
bool fipsd_nostr_runtime_refetch(fipsd_nostr_runtime *r, const char *key) { return command(r, peer_command(REFETCH, key)); }
bool fipsd_nostr_runtime_peer_result(fipsd_nostr_runtime *r, const char *key, bool success, bool mismatch)
{
    message *m = peer_command(RESULT, key); if (m) { m->success = success; m->mismatch = mismatch; } return command(r, m);
}
bool fipsd_nostr_runtime_send_signal(fipsd_nostr_runtime *r, const char *key, const fipsd_nostr_traversal *p)
{
    message *m = peer_command(SEND, key);
    if (m && p) { m->signal = malloc(sizeof(*p)); if (m->signal) *m->signal = *p; }
    if (!m || !m->signal) { discard(m); return false; }
    return command(r, m);
}
bool fipsd_nostr_runtime_session_finished(fipsd_nostr_runtime *r, const char *id)
{
    if (!id || strlen(id) > FIPSD_NOSTR_SESSION_MAX) return false;
    message *m = new_message(FINISH); if (m) memcpy(m->reason, id, strlen(id) + 1); return command(r, m);
}
