#include "nostr/driver.h"
#include "log.h"
#include "nostr/runtime.h"
#include "nostr/traversal_io.h"
#include "transport/tcp/tcp.h"
#include "transport/tor/tor.h"
#include "transport/udp/udp.h"
#include <select.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {
    fipsd_transport *transport;
    fipsd_nostr_transport kind;
    bool advertise, is_public, has_external;
    char external[FIPSD_NOSTR_ADDR_MAX + 1];
    uint16_t advertised_port;
} advert_slot;
struct fipsd_nostr_adverts {
    advert_slot slots[FIPSD_TRANSPORT_MAX];
    size_t count, signal_count;
    GoChannel *signals[2 * FIPSD_TRANSPORT_MAX], *stop, *done; /* signals borrowed from transports */
    struct fipsd_nostr_runtime *runtime;
    fipsd_runtime *host;
    bool started;
};
static bool dial(void *user, const char *hex, const fipsd_nostr_advert *advert)
{
    fipsd_nostr_node_driver *d = user;
    fips_xonly_pubkey key; fips_peer_identity peer;
    if (sodium_hex2bin(key.bytes, 32, hex, 64, NULL, NULL, NULL) ||
        fips_peer_identity_from_pubkey(fipsd_node_secp(d->node), &key, &peer) != FIPS_IDENTITY_OK) return false;
    if (fipsd_node_handlers_learn_identity(d->handlers, &peer)) return false;
    bool queued = false;
    for (size_t i = 0; i < advert->endpoint_count; ++i) {
        const fipsd_nostr_endpoint *e = &advert->endpoints[i];
        if (!strcmp(e->addr, "nat")) {
            if (e->transport == FIPSD_NOSTR_UDP && fipsd_traversal_io_connect(d->traversal, hex)) queued = true;
            continue;
        }
        const char *type = e->transport == FIPSD_NOSTR_UDP ? "udp" : e->transport == FIPSD_NOSTR_TCP ? "tcp" : "tor";
        for (size_t j = 0; j < d->transports->count; ++j) {
            fipsd_transport *t = d->transports->items[j]; fips_transport_addr addr;
            if (strcmp(t->type, type) || !t->auto_connect) continue;
            if (fips_transport_addr_from_string(e->addr, strlen(e->addr), &addr) == FIPS_OK &&
                fipsd_node_handlers_connect(d->handlers, &peer, t->id, &addr) == 0) queued = true;
        }
    }
    return queued;
}
/* One operational transport's inputs -> its endpoint. UDP is operational once
 * its bound socket finished the STUN advert round; TCP and Tor when bound. */
static bool slot_endpoint(fipsd_nostr_node_driver *d, const advert_slot *s, fipsd_nostr_endpoint *e)
{
    fipsd_transport *t = s->transport;
    fipsd_nostr_advert_source src = {.transport = s->kind, .advertise = true, .is_public = s->is_public,
        .external = s->has_external ? s->external : NULL, .advertised_port = s->advertised_port};
    char reflexive[65], bound[FIPSD_NOSTR_ADDR_MAX + 1]; bool has_reflexive = false;
    if (s->kind == FIPSD_NOSTR_UDP) {
        if (!fipsd_traversal_io_udp_observed(d->traversal, t, reflexive, &has_reflexive)) return false;
        if (has_reflexive) src.reflexive = reflexive;
    } else if (t->ops->state(t) != FIPSD_TRANSPORT_BOUND) return false;
    fips_transport_addr a; const char *text; size_t len;
    if (s->kind != FIPSD_NOSTR_TOR && t->ops->local_addr && t->ops->local_addr(t, &a) &&
        fips_transport_addr_as_str(&a, &text, &len) == FIPS_OK && len < sizeof bound) {
        memcpy(bound, text, len); bound[len] = 0; src.local = bound;
    }
    if (s->kind == FIPSD_NOSTR_TOR) src.onion = fipsd_tor_advertised_onion(t);
    const char *why = NULL;
    if (fipsd_nostr_advert_source_endpoint(&src, e, &why)) return true;
    if (why) FIPSD_WARN("nostr", "advert: %s: %s", t->name, why);
    return false;
}
static bool local(void *user, fipsd_nostr_advert *out)
{
    fipsd_nostr_node_driver *d = user; struct fipsd_nostr_adverts *a = d->adverts;
    for (size_t i = 0; a && i < a->count; ++i) {
        fipsd_nostr_endpoint e;
        if (!a->slots[i].advertise || !slot_endpoint(d, &a->slots[i], &e)) continue;
        if (out->endpoint_count == FIPSD_NOSTR_ENDPOINTS_MAX) {
            FIPSD_WARN("nostr", "advert: more than %u endpoints; %s and later transports omitted",
                (unsigned)FIPSD_NOSTR_ENDPOINTS_MAX, a->slots[i].transport->name);
            break;
        }
        out->endpoints[out->endpoint_count++] = e;
    }
    return out->endpoint_count != 0;
}
/* transports.<type> for a single instance, transports.<type>.<name> for a named one. */
static const fipsd_config_value *instance(const fipsd_config *c, const fipsd_transport *t)
{
    char path[64]; snprintf(path, sizeof path, "transports.%s", t->type);
    const fipsd_config_value *all = fipsd_config_get(c, path), *named = fipsd_config_member(all, t->name);
    return named ? named : all;
}
static bool adverts_prepare(fipsd_nostr_node_driver *d, const fipsd_config *c, struct fipsd_nostr_runtime *r)
{
    struct fipsd_nostr_adverts *a = d->adverts = calloc(1, sizeof *a);
    if (!a || !(a->stop = go_channel_create(1)) || !(a->done = go_channel_create(1))) return false;
    a->runtime = r;
    for (size_t i = 0; i < d->transports->count && a->count < FIPSD_TRANSPORT_MAX; ++i) {
        fipsd_transport *t = d->transports->items[i]; fipsd_nostr_transport kind;
        if (!strcmp(t->type, "udp")) kind = FIPSD_NOSTR_UDP;
        else if (!strcmp(t->type, "tcp")) kind = FIPSD_NOSTR_TCP;
        else if (fipsd_tor_is(t)) kind = FIPSD_NOSTR_TOR;
        else continue;
        const fipsd_config_value *v = instance(c, t);
        advert_slot *s = &a->slots[a->count++];
        *s = (advert_slot){.transport = t, .kind = kind,
            .advertise = fipsd_config_bool(fipsd_config_member(v, "advertise_on_nostr")),
            .is_public = kind == FIPSD_NOSTR_UDP && fipsd_config_bool(fipsd_config_member(v, "public"))};
        if (!s->advertise) continue;
        if (kind == FIPSD_NOSTR_TOR) {
            s->advertised_port = fipsd_tor_advertised_port(t);
            a->signals[a->signal_count++] = fipsd_tor_advert_updates(t);
            a->signals[a->signal_count++] = fipsd_tor_bound(t);
            continue;
        }
        const char *raw = fipsd_config_string(fipsd_config_member(v, "external_addr"));
        s->has_external = fipsd_nostr_advert_external(raw, fipsd_config_string(fipsd_config_member(v, "bind_addr")), s->external);
        if (raw && !s->has_external)
            FIPSD_WARN("nostr", "%s: external_addr '%s' is neither a socket address nor an IP with a parseable bind_addr port; not advertised",
                t->name, raw);
        if (kind == FIPSD_NOSTR_TCP) a->signals[a->signal_count++] = fipsd_tcp_bound(t);
    }
    return true;
}
/* Event-driven advert inputs: a listener (re)bind or a Tor onion turning
 * advertisable asks the actor to rebuild. UDP observation asks from
 * traversal_io. No timer: the actor's refresh ticker only renews the TTL. */
static void watch(void *arg)
{
    struct fipsd_nostr_adverts *a = arg;
    for (;;) {
        GoSelectCase cases[2 + 2 * FIPSD_TRANSPORT_MAX]; void *v = NULL;
        cases[0] = (GoSelectCase){.op = GO_SELECT_RECEIVE, .chan = a->stop, .recv_buf = &v};
        cases[1] = (GoSelectCase){.op = GO_SELECT_RECEIVE, .chan = go_context_done(fipsd_runtime_context(a->host)), .recv_buf = &v};
        for (size_t i = 0; i < a->signal_count; ++i)
            cases[i + 2] = (GoSelectCase){.op = GO_SELECT_RECEIVE, .chan = a->signals[i], .recv_buf = &v};
        if (go_select(cases, a->signal_count + 2) < 2) break;
        if (!fipsd_nostr_runtime_refresh_advert(a->runtime)) FIPSD_WARN("nostr", "advert input change refused (rendezvous mailbox full)");
    }
    go_channel_close(a->done);
}
static bool prepare(void *user, const fipsd_config *c, struct fipsd_nostr_runtime *r)
{
    fipsd_nostr_node_driver *d = user;
    d->traversal = fipsd_traversal_io_create(c, d, r);
    return d->traversal != NULL && adverts_prepare(d, c, r);
}
static void adverts_stop(struct fipsd_nostr_adverts *a)
{
    if (!a || !a->started) return;
    go_channel_close(a->stop); void *unused = NULL; (void)go_channel_receive(a->done, &unused); a->started = false;
}
static int start(void *user, fipsd_runtime *rt, const fips_identity *id)
{
    fipsd_nostr_node_driver *d = user; struct fipsd_nostr_adverts *a = d->adverts;
    if (fipsd_traversal_io_start(d->traversal, rt, id)) return -1;
    a->host = rt;
    if (a->signal_count && fipsd_runtime_spawn(rt, watch, a)) { fipsd_traversal_io_stop(d->traversal); return -1; }
    a->started = a->signal_count != 0; return 0;
}
static void stop(void *user)
{
    fipsd_nostr_node_driver *d = user;
    adverts_stop(d->adverts); fipsd_traversal_io_stop(d->traversal);
}
static void destroy(void *user)
{
    fipsd_nostr_node_driver *d = user; fipsd_traversal_io_destroy(d->traversal); d->traversal = NULL;
    struct fipsd_nostr_adverts *a = d->adverts; d->adverts = NULL;
    if (!a) return;
    adverts_stop(a);
    if (a->stop) go_channel_unref(a->stop);
    if (a->done) go_channel_unref(a->done);
    free(a);
}
static bool traversal_signal(void *user, const fipsd_nostr_traversal *p)
{ return fipsd_traversal_io_signal(((fipsd_nostr_node_driver *)user)->traversal, p); }
static void published_signal(void *user, const char *id, bool ok, bool repair)
{ fipsd_traversal_io_published(((fipsd_nostr_node_driver *)user)->traversal, id, ok, repair); }
fipsd_rendezvous_driver fipsd_nostr_node_driver_init(fipsd_nostr_node_driver *d,
    const fipsd_node_context *node, fipsd_node_handlers *handlers, const fipsd_transport_registry *transports)
{
    *d = (fipsd_nostr_node_driver){.node = node, .handlers = handlers, .transports = transports};
    return (fipsd_rendezvous_driver){.user = d, .dial_candidate = dial, .local_advert = local,
        .prepare = prepare, .start = start, .stop = stop, .destroy = destroy,
        .traversal_offer = traversal_signal, .traversal_answer = traversal_signal, .signal_result = published_signal};
}

bool fipsd_nostr_node_driver_established(fipsd_nostr_node_driver *d, const fipsd_established_traversal *e)
{
    fipsd_transport *t = fipsd_transport_find(d->transports, e->transport);
    fips_transport_addr current;
    if (!t || strcmp(t->type, "udp") || !fipsd_udp_address(t, &current) ||
        !fips_transport_addr_equal(&current, &e->local_addr)) return false;
    return !fipsd_node_handlers_learn_identity(d->handlers, &e->peer) &&
        !fipsd_node_handlers_connect(d->handlers, &e->peer, e->transport, &e->remote_addr);
}
