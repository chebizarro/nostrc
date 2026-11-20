#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "nostr/traversal_io.h"
#include "nostr/traversal_machine.h"
#include "nostr/stun.h"
#include "nostr/runtime.h"
#include "nostr/internal.h"
#include "blocking_executor.h"
#include "log.h"
#include <select.h>
#include <sodium.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#define TARGET_MAX 8u
#define PREFIX_MAX 32u
typedef struct { int family; uint8_t ip[16], mask[16]; } prefix;
typedef struct udp_path {
    struct fipsd_traversal_io *owner;
    fipsd_transport *transport;
    fips_transport_addr bound;
    fipsd_stun stun;
    size_t server;
    bool observed, loopback, snapshot_ready;
    prefix prefixes[PREFIX_MAX]; size_t prefix_count;
    fipsd_nostr_traversal addresses; /* worker owned */
    fipsd_nostr_traversal snapshot; /* snapshot_mu; actor reads */
} udp_path;
typedef struct {
    bool used, completed, resend_pending, resent;
    char key[65];
    fips_peer_identity peer;
    udp_path *path;
    fipsd_tr_machine machine;
    fipsd_nostr_traversal local;
    fips_transport_addr targets[TARGET_MAX], selected;
    size_t target_count;
    uint8_t hash[16];
} session;
typedef enum { CONNECT, SIGNAL, PUBLISH_RESULT } command_kind;
typedef struct { command_kind kind; char key[129]; bool ok, repair; fipsd_nostr_traversal signal; } command;
typedef struct { udp_path *path; fips_transport_addr from; size_t len; uint8_t bytes[]; } datagram;
struct fipsd_traversal_io {
    fipsd_nostr_node_driver *driver;
    fipsd_nostr_runtime *relay;
    fipsd_runtime *host;
    const fips_identity *identity;
    char npub[64]; fips_node_addr ours;
    GoChannel *commands, *packets, *stop, *done;
    bool started, share_local;
    nsync_mu snapshot_mu;
    udp_path paths[FIPSD_TRANSPORT_MAX]; size_t path_count;
    char servers[FIPSD_NOSTR_STUN_MAX][FIPSD_NOSTR_URL_MAX + 1]; size_t server_count;
    fipsd_tr_config timing;
    session *sessions; size_t capacity, incoming_capacity;
};
static uint64_t wall(void)
{ struct timespec t; clock_gettime(CLOCK_REALTIME, &t); return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000; }
static uint64_t mono(void) { return fipsd_transport_now_ms(); }
static void nonce(char out[129])
{ uint8_t bytes[16]; randombytes_buf(bytes, sizeof bytes); sodium_bin2hex(out, 129, bytes, sizeof bytes); }
static bool endpoint(const fips_transport_addr *a, fipsd_udp_endpoint *ep)
{
    if (!a->len || a->len > 256) return false;
    char text[257]; memcpy(text, a->bytes, a->len); text[a->len] = 0;
    return fipsd_udp_endpoint_numeric(text, ep);
}
static const uint8_t *ip_bytes(const struct sockaddr *a)
{
    return a->sa_family == AF_INET ? (const uint8_t *)&((const struct sockaddr_in *)a)->sin_addr :
        ((const struct sockaddr_in6 *)a)->sin6_addr.s6_addr;
}
static bool same_ip(const fips_transport_addr *a, const fips_transport_addr *b)
{
    fipsd_udp_endpoint x, y;
    return endpoint(a, &x) && endpoint(b, &y) && x.address.ss_family == y.address.ss_family &&
        !memcmp(ip_bytes((struct sockaddr *)&x.address), ip_bytes((struct sockaddr *)&y.address), x.address.ss_family == AF_INET ? 4 : 16);
}
static bool unspecified(int family, const uint8_t *ip)
{ static const uint8_t zero[16] = {0}; return !memcmp(ip, zero, family == AF_INET ? 4 : 16); }
static bool is_loopback(int family, const uint8_t *ip)
{
    static const uint8_t loop6[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
    return family == AF_INET ? ip[0] == 127 : !memcmp(ip, loop6, 16);
}
static bool private_ip(int family, const uint8_t *ip)
{
    return family == AF_INET ? ip[0] == 10 || (ip[0] == 172 && (ip[1] & 0xf0) == 16) ||
        (ip[0] == 192 && ip[1] == 168) : (ip[0] & 0xfe) == 0xfc;
}
static bool forbidden(int family, const uint8_t *ip, bool loop)
{
    if (unspecified(family, ip) || (!loop && is_loopback(family, ip))) return true;
    return family == AF_INET ? ip[0] >= 224 || (ip[0] == 169 && ip[1] == 254) ||
        (ip[0] == 100 && (ip[1] & 0xc0) == 64) : ip[0] == 0xff || (ip[0] == 0xfe && (ip[1] & 0xc0) == 0x80);
}
static bool address(const fips_transport_addr *a, fipsd_nostr_address *out)
{
    fipsd_udp_endpoint ep; if (!endpoint(a, &ep)) return false;
    const struct sockaddr *sa = (const struct sockaddr *)&ep.address;
    *out = (fipsd_nostr_address){.protocol = "udp"};
    out->port = ntohs(sa->sa_family == AF_INET ? ((const struct sockaddr_in *)sa)->sin_port : ((const struct sockaddr_in6 *)sa)->sin6_port);
    return inet_ntop(sa->sa_family, ip_bytes(sa), out->ip, sizeof out->ip) != NULL;
}
static void *local_addresses(void *arg)
{
    udp_path *p = arg; fipsd_udp_endpoint bound;
    if (!endpoint(&p->bound, &bound)) return NULL;
    int family = bound.address.ss_family;
    const uint8_t *own = ip_bytes((struct sockaddr *)&bound.address);
    p->loopback = is_loopback(family, own); p->prefix_count = 0;
    p->addresses = (fipsd_nostr_traversal){0};
    fipsd_nostr_address base; if (!address(&p->bound, &base)) return NULL;
    struct ifaddrs *list = NULL; if (getifaddrs(&list)) return NULL;
    for (struct ifaddrs *i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || !i->ifa_netmask || !(i->ifa_flags & IFF_UP) || i->ifa_addr->sa_family != family ||
            (p->transport->interface_name && strcmp(i->ifa_name, p->transport->interface_name))) continue;
        const uint8_t *ip = ip_bytes(i->ifa_addr), *mask = ip_bytes(i->ifa_netmask);
        size_t bytes = family == AF_INET ? 4 : 16;
        if ((!unspecified(family, own) && memcmp(own, ip, bytes)) || forbidden(family, ip, p->loopback)) continue;
        if (p->prefix_count < PREFIX_MAX) {
            prefix *net = &p->prefixes[p->prefix_count++]; net->family = family;
            memcpy(net->ip, ip, bytes); memcpy(net->mask, mask, bytes);
        }
        if (p->owner->share_local && p->addresses.local_count < FIPSD_NOSTR_CANDIDATES_MAX) {
            fipsd_nostr_address a = base;
            if (inet_ntop(family, ip, a.ip, sizeof a.ip)) p->addresses.local[p->addresses.local_count++] = a;
        }
    }
    freeifaddrs(list); return NULL;
}
static bool target(udp_path *p, const fipsd_nostr_address *a, fips_transport_addr *out)
{
    if (strcmp(a->protocol, "udp") || !a->port) return false;
    char text[96]; snprintf(text, sizeof text, strchr(a->ip, ':') ? "[%s]:%u" : "%s:%u", a->ip, a->port);
    fipsd_udp_endpoint ep, local;
    if (!fipsd_udp_endpoint_numeric(text, &ep) || !endpoint(&p->bound, &local) || ep.address.ss_family != local.address.ss_family) return false;
    int family = ep.address.ss_family; const uint8_t *ip = ip_bytes((struct sockaddr *)&ep.address);
    if (forbidden(family, ip, p->loopback)) return false;
    if (private_ip(family, ip) || is_loopback(family, ip)) {
        bool onlink = false;
        for (size_t i = 0; i < p->prefix_count; ++i) {
            const prefix *net = &p->prefixes[i]; bool match = family == net->family;
            for (size_t j = 0; match && j < (family == AF_INET ? 4u : 16u); ++j)
                if ((ip[j] & net->mask[j]) != (net->ip[j] & net->mask[j])) match = false;
            onlink |= match;
        }
        if (!onlink) return false;
    }
    return fipsd_udp_endpoint_addr(&ep, out);
}
static void plan(session *s, const fipsd_nostr_traversal *remote)
{
    s->target_count = 0;
    for (size_t i = 0; i < remote->local_count + (remote->has_reflexive ? 1u : 0u) && s->target_count < TARGET_MAX; ++i) {
        const fipsd_nostr_address *a = i < remote->local_count ? &remote->local[i] : &remote->reflexive;
        fips_transport_addr to; if (!target(s->path, a, &to)) continue;
        bool duplicate = false;
        for (size_t j = 0; j < s->target_count; ++j) if (fips_transport_addr_equal(&s->targets[j], &to)) duplicate = true;
        if (!duplicate) s->targets[s->target_count++] = to;
    }
}
static fipsd_udp_class classify(void *arg, const uint8_t *bytes, size_t n, const fips_transport_addr *from)
{
    udp_path *p = arg;
    if (!fipsd_is_punch_packet(bytes, n) && !fipsd_stun_packet(bytes, n)) return FIPSD_UDP_FMP;
    if (!from || n > 2048 || go_channel_is_closed(p->owner->stop)) return FIPSD_UDP_DISCARD;
    datagram *d = malloc(sizeof(*d) + n);
    if (d) {
        d->path = p; d->from = *from; d->len = n; memcpy(d->bytes, bytes, n);
        if (go_channel_try_send(p->owner->packets, d)) free(d);
    }
    return FIPSD_UDP_DISCARD;
}
static void finish(fipsd_traversal_io *io, session *s)
{
    if (*s->local.session_id && !fipsd_nostr_runtime_session_finished(io->relay, s->local.session_id))
        FIPSD_WARN("nostr", "traversal reservation release refused");
}
static void effects(fipsd_traversal_io *io, session *s, unsigned actions)
{
    if (actions & (FIPSD_TR_SEND_OFFER | FIPSD_TR_SEND_ANSWER)) {
        if (!fipsd_nostr_runtime_send_signal(io->relay, s->key, &s->local)) {
            actions |= fipsd_tr_step(&s->machine, (fipsd_tr_input){.event = FIPSD_TR_ANSWERED}, mono());
        }
    }
    if (actions & FIPSD_TR_SEND_PUNCH) {
        uint8_t bytes[FIPSD_PUNCH_SIZE]; fipsd_punch_build(false, s->machine.sequence - 1, s->local.session_id, bytes);
        for (size_t i = 0; i < s->target_count; ++i) (void)s->path->transport->ops->send(s->path->transport, &s->targets[i], bytes, sizeof bytes);
    }
    if (actions & FIPSD_TR_HANDOFF) {
        /* A punch proves reachability, not identity. IK authenticates before
         * udp.authenticated promotes this SAME local tuple to a connected peer. */
        fipsd_established_traversal established = {.peer = s->peer, .transport = s->path->transport->id,
            .local_addr = s->path->bound, .remote_addr = s->selected};
        memcpy(established.session_id, s->local.session_id, sizeof established.session_id);
        bool ok = fipsd_nostr_node_driver_established(io->driver, &established);
        if (!fipsd_nostr_runtime_peer_result(io->relay, s->key, ok, false)) FIPSD_WARN("nostr", "traversal result refused");
        s->completed = ok; finish(io, s);
        if (!ok) actions |= fipsd_tr_step(&s->machine, (fipsd_tr_input){.event = FIPSD_TR_CANCEL}, mono());
    }
    if (actions & FIPSD_TR_FAILED) {
        if (!fipsd_nostr_runtime_peer_result(io->relay, s->key, false, false)) FIPSD_WARN("nostr", "traversal failure refused");
    }
    if (actions & FIPSD_TR_FINISHED) finish(io, s);
}
static udp_path *usable_path(fipsd_traversal_io *io)
{
    for (size_t i = 0; i < io->path_count; ++i) {
        udp_path *p = &io->paths[i];
        if (p->observed && p->transport->auto_connect && (p->addresses.has_reflexive || p->addresses.local_count)) return p;
    }
    return NULL;
}
static void payload(fipsd_traversal_io *io, session *s)
{
    s->local = s->path->addresses;
    memcpy(s->local.sender_npub, io->npub, sizeof io->npub);
    (void)fips_peer_identity_npub(&s->peer, s->local.recipient_npub, sizeof s->local.recipient_npub);
    s->local.issued_at = wall(); s->local.expires_at = fn_add(s->local.issued_at, io->timing.answer_ms);
    nonce(s->local.nonce);
}
static void begin(fipsd_traversal_io *io, session *s)
{
    s->path = usable_path(io);
    if (!s->path) {
        for (size_t i = 0; i < io->path_count; ++i) if (!io->paths[i].observed) return;
        /* No observation can still complete: release the pending admission. */
        (void)fipsd_nostr_runtime_peer_result(io->relay, s->key, false, false);
        finish(io, s); *s = (session){0}; return;
    }
    payload(io, s); memcpy(s->local.session_id, s->local.nonce, sizeof s->local.session_id);
    uint8_t p[FIPSD_PUNCH_SIZE]; fipsd_punch_build(false, 0, s->local.session_id, p); memcpy(s->hash, p + 8, 16);
    s->resend_pending = s->resent = false;
    effects(io, s, fipsd_tr_step(&s->machine, (fipsd_tr_input){.event = FIPSD_TR_OFFER, .initiator = true}, mono()));
}
static session *find_peer(fipsd_traversal_io *io, const char *key)
{
    for (size_t i = 0; i < io->capacity; ++i) if (io->sessions[i].used && !strcmp(io->sessions[i].key, key)) return &io->sessions[i];
    return NULL;
}
static session *reserve(fipsd_traversal_io *io, const char *key, const fips_peer_identity *peer, bool initiator)
{
    /* Retained incoming cooldowns cannot consume outbound admission slots. */
    size_t first = initiator ? io->incoming_capacity : 0;
    size_t end = initiator ? io->capacity : io->incoming_capacity;
    for (size_t i = first; i < end; ++i) if (!io->sessions[i].used) {
        session *s = &io->sessions[i]; *s = (session){.used = true, .peer = *peer};
        memcpy(s->key, key, 65); fipsd_tr_init(&s->machine, &io->timing); return s;
    }
    return NULL;
}
static void process(fipsd_traversal_io *io, const command *c)
{
    if (c->kind == CONNECT) {
        if (find_peer(io, c->key)) return;
        fips_xonly_pubkey pk; fips_peer_identity peer;
        if (sodium_hex2bin(pk.bytes, 32, c->key, 64, NULL, NULL, NULL) ||
            fips_peer_identity_from_pubkey(fipsd_node_secp(io->driver->node), &pk, &peer) != FIPS_IDENTITY_OK ||
            !memcmp(peer.node_addr.bytes, io->ours.bytes, 16)) return;
        session *s = reserve(io, c->key, &peer, true);
        if (s) begin(io, s);
        else (void)fipsd_nostr_runtime_peer_result(io->relay, c->key, false, false);
        return;
    }
    if (c->kind == PUBLISH_RESULT) {
        for (size_t i = 0; i < io->capacity; ++i) {
            session *s = &io->sessions[i];
            if (!s->used || strcmp(s->local.session_id, c->key)) continue;
            if (c->repair) { s->resend_pending = false; s->resent = c->ok; }
            /* An accepted answer can overtake a queued repair in the relay
             * actor. A missing consumed reservation must not cancel punching. */
            if (!c->ok) effects(io, s, fipsd_tr_step(&s->machine,
                (fipsd_tr_input){.event = FIPSD_TR_ANSWERED, .repair = c->repair}, mono()));
        }
        return;
    }
    const fipsd_nostr_traversal *p = &c->signal; fips_peer_identity peer;
    if (fips_peer_identity_from_npub(fipsd_node_secp(io->driver->node), p->sender_npub, strlen(p->sender_npub), &peer) != FIPS_IDENTITY_OK ||
        !memcmp(peer.node_addr.bytes, io->ours.bytes, 16)) return;
    char key[65]; sodium_bin2hex(key, sizeof key, peer.pubkey.bytes, 32);
    session *s = find_peer(io, key);
    if (p->answer) {
        if (!s || s->machine.state != FIPSD_TR_ANSWER || strcmp(s->local.session_id, p->session_id)) return;
        plan(s, p);
        effects(io, s, fipsd_tr_step(&s->machine, (fipsd_tr_input){.event = FIPSD_TR_ANSWERED,
            .accepted = p->accepted && s->target_count != 0}, mono()));
        return;
    }
    if (s) {
        bool active = s->machine.initiator && (s->machine.state == FIPSD_TR_ANSWER ||
            s->machine.state == FIPSD_TR_PUNCHING || s->machine.state == FIPSD_TR_SETTLING);
        if (fipsd_tr_suppress(io->ours.bytes, peer.node_addr.bytes, active)) {
            if (s->machine.state == FIPSD_TR_ANSWER && !s->resent && !s->resend_pending)
                s->resend_pending = fipsd_nostr_runtime_resend_signal(io->relay, s->key, &s->local);
            (void)fipsd_nostr_runtime_session_finished(io->relay, p->session_id); return;
        }
        if (!active) { (void)fipsd_nostr_runtime_session_finished(io->relay, p->session_id); return; }
        effects(io, s, fipsd_tr_step(&s->machine, (fipsd_tr_input){.event = FIPSD_TR_CANCEL}, mono()));
        *s = (session){0};
    }
    s = reserve(io, key, &peer, false);
    if (!s || !(s->path = usable_path(io))) {
        if (s) *s = (session){0};
        (void)fipsd_nostr_runtime_session_finished(io->relay, p->session_id); return;
    }
    payload(io, s); s->local.answer = true;
    memcpy(s->local.session_id, p->session_id, sizeof s->local.session_id);
    memcpy(s->local.in_reply_to, p->nonce, sizeof s->local.in_reply_to);
    plan(s, p); s->local.accepted = s->target_count != 0;
    if (!s->local.accepted) {
        s->local.has_reason = true; strcpy(s->local.reason, "no usable UDP candidates");
    }
    s->local.has_offer_received_at = true; s->local.offer_received_at = wall();
    s->local.has_punch = s->local.accepted;
    s->local.punch = (fipsd_nostr_punch){fn_add(wall(), io->timing.start_delay_ms), io->timing.interval_ms, io->timing.duration_ms};
    uint8_t bytes[FIPSD_PUNCH_SIZE]; fipsd_punch_build(false, 0, s->local.session_id, bytes); memcpy(s->hash, bytes + 8, 16);
    effects(io, s, fipsd_tr_step(&s->machine, (fipsd_tr_input){.event = FIPSD_TR_OFFER}, mono()));
    if (!s->local.accepted) effects(io, s, fipsd_tr_step(&s->machine, (fipsd_tr_input){.event = FIPSD_TR_ANSWERED}, mono()));
}
static void observed(udp_path *p)
{
    fipsd_traversal_io *io = p->owner; p->observed = true;
    nsync_mu_lock(&io->snapshot_mu); p->snapshot = p->addresses; p->snapshot_ready = true; nsync_mu_unlock(&io->snapshot_mu);
    if (!fipsd_nostr_runtime_refresh_advert(io->relay)) FIPSD_WARN("nostr", "observed-address advert update refused");
    for (size_t i = 0; i < io->capacity; ++i) {
        session *s = &io->sessions[i];
        if (s->used && !s->path && s->machine.state == FIPSD_TR_IDLE) begin(io, s);
    }
}
static void next_server(udp_path *p)
{
    fipsd_traversal_io *io = p->owner;
    while (p->server < io->server_count) {
        fips_transport_addr name, numeric;
        if (fipsd_stun_server(io->servers[p->server], &name) && fipsd_udp_resolve_address(p->transport, &name, &numeric)) {
            uint8_t id[12]; randombytes_buf(id, sizeof id);
            fipsd_stun_begin(&p->stun, &numeric, id, mono(), FIPSD_STUN_ADVERT_MS); return;
        }
        ++p->server;
    }
    observed(p);
}
static void bound(udp_path *p)
{
    fips_transport_addr current; if (!fipsd_udp_address(p->transport, &current)) return;
    fipsd_traversal_io *io = p->owner;
    for (size_t i = 0; i < io->capacity; ++i) if (io->sessions[i].used && io->sessions[i].path == p) {
        effects(io, &io->sessions[i], fipsd_tr_step(&io->sessions[i].machine, (fipsd_tr_input){.event = FIPSD_TR_CANCEL}, mono()));
        io->sessions[i] = (session){0};
    }
    nsync_mu_lock(&io->snapshot_mu); p->snapshot_ready = false; nsync_mu_unlock(&io->snapshot_mu);
    p->bound = current; p->observed = false; p->server = 0; p->stun = (fipsd_stun){0};
    go_blocking_submit(local_addresses, p); next_server(p);
}
static void packet(fipsd_traversal_io *io, datagram *d)
{
    udp_path *p = d->path;
    if (p->stun.active && mono() < p->stun.deadline && fipsd_stun_receive(&p->stun, &d->from, d->bytes, d->len)) {
        p->addresses.has_reflexive = address(&p->stun.mapped, &p->addresses.reflexive);
        p->addresses.has_stun = true; strcpy(p->addresses.stun, io->servers[p->server]); observed(p); return;
    }
    fipsd_punch_packet punch; if (!fipsd_punch_parse(d->bytes, d->len, &punch)) return;
    for (size_t i = 0; i < io->capacity; ++i) {
        session *s = &io->sessions[i];
        if (!s->used || s->path != p || memcmp(s->hash, punch.hash, 16)) continue;
        unsigned rank = 0;
        for (size_t j = 0; j < s->target_count; ++j) {
            if (fips_transport_addr_equal(&s->targets[j], &d->from)) { rank = 2; break; }
            if (same_ip(&s->targets[j], &d->from)) rank = 1;
        }
        if (s->machine.state == FIPSD_TR_ESTABLISHED) rank = fips_transport_addr_equal(&s->selected, &d->from) ? 2 : 0;
        if (!rank) return;
        unsigned actions = fipsd_tr_step(&s->machine, (fipsd_tr_input){.event = FIPSD_TR_PUNCH, .rank = rank, .probe = !punch.ack}, mono());
        if (s->machine.state != FIPSD_TR_ESTABLISHED || (actions & FIPSD_TR_HANDOFF)) s->selected = d->from;
        if (actions & FIPSD_TR_SEND_ACK) {
            uint8_t bytes[FIPSD_PUNCH_SIZE]; fipsd_punch_build(true, punch.sequence, s->local.session_id, bytes);
            (void)p->transport->ops->send(p->transport, &d->from, bytes, sizeof bytes);
        }
        effects(io, s, actions); return;
    }
}
static void run(void *arg)
{
    fipsd_traversal_io *io = arg;
    while (!go_channel_is_closed(io->stop) && !go_context_is_canceled(fipsd_runtime_context(io->host))) {
        uint64_t deadline = UINT64_MAX, now = mono();
        for (size_t i = 0; i < io->path_count; ++i) {
            udp_path *p = &io->paths[i];
            if (p->stun.active) {
                fipsd_stun_action a = fipsd_stun_tick(&p->stun, now);
                if (a == FIPSD_STUN_SEND) (void)fipsd_stun_send(&p->stun, p->transport);
                if (a == FIPSD_STUN_FAILED) { ++p->server; next_server(p); }
                uint64_t next = fipsd_stun_deadline(&p->stun); if (next < deadline) deadline = next;
            }
        }
        for (size_t i = 0; i < io->capacity; ++i) {
            session *s = &io->sessions[i]; if (!s->used) continue;
            unsigned actions = fipsd_tr_step(&s->machine, (fipsd_tr_input){.event = FIPSD_TR_TIMEOUT}, mono());
            effects(io, s, actions);
            if (actions & FIPSD_TR_RETRY) begin(io, s);
            else if ((s->machine.state == FIPSD_TR_IDLE && s->path) || s->machine.state == FIPSD_TR_CANCELLED) { *s = (session){0}; continue; }
            uint64_t next = fipsd_tr_deadline(&s->machine); if (next < deadline) deadline = next;
        }
        void *value = NULL;
        GoSelectCase cases[FIPSD_TRANSPORT_MAX + 4] = {
            {.op = GO_SELECT_RECEIVE, .chan = io->stop},
            {.op = GO_SELECT_RECEIVE, .chan = go_context_done(fipsd_runtime_context(io->host))},
            {.op = GO_SELECT_RECEIVE, .chan = io->commands, .recv_buf = &value},
            {.op = GO_SELECT_RECEIVE, .chan = io->packets, .recv_buf = &value}
        };
        for (size_t i = 0; i < io->path_count; ++i) cases[i + 4] = (GoSelectCase){.op = GO_SELECT_RECEIVE, .chan = fipsd_udp_bound(io->paths[i].transport)};
        now = mono();
        GoSelectResult selected = deadline == UINT64_MAX ?
            (GoSelectResult){.selected_case = go_select(cases, io->path_count + 4)} :
            go_select_timeout(cases, io->path_count + 4, deadline > now ? deadline - now : 0);
        if (selected.selected_case == 0 || selected.selected_case == 1) break;
        if (selected.selected_case == 2 && value) { process(io, value); free(value); }
        else if (selected.selected_case == 3 && value) { packet(io, value); free(value); }
        else if (selected.selected_case >= 4) bound(&io->paths[selected.selected_case - 4]);
    }
    go_channel_close(io->done);
}
static uint64_t setting(const fipsd_config *c, const char *name)
{ char path[160]; snprintf(path, sizeof path, "node.rendezvous.nostr.%s", name); return fipsd_config_uint(fipsd_config_get(c, path)); }
fipsd_traversal_io *fipsd_traversal_io_create(const fipsd_config *c, fipsd_nostr_node_driver *d, fipsd_nostr_runtime *r)
{
    fipsd_traversal_io *io = calloc(1, sizeof *io); if (!io) return NULL;
    io->driver = d; io->relay = r; nsync_mu_init(&io->snapshot_mu);
    io->commands = go_channel_create(64); io->packets = go_channel_create(128);
    io->stop = go_channel_create(1); io->done = go_channel_create(1);
    uint64_t outgoing = setting(c, "open_discovery_max_pending"), incoming = setting(c, "max_concurrent_incoming_offers");
    if (outgoing > 4096 || incoming > 4096 || !(outgoing + incoming) || outgoing + incoming > 4096) goto fail;
    io->incoming_capacity = (size_t)incoming;
    io->capacity = (size_t)(outgoing + incoming);
    io->sessions = calloc(io->capacity, sizeof *io->sessions);
    if (!io->commands || !io->packets || !io->stop || !io->done || !io->sessions) goto fail;
    io->share_local = fipsd_config_bool(fipsd_config_get(c, "node.rendezvous.nostr.share_local_candidates"));
    fipsd_tr_config_default(&io->timing);
    io->timing.answer_ms = fn_mul1000(setting(c, "signal_ttl_secs"));
    io->timing.attempt_ms = fn_mul1000(setting(c, "attempt_timeout_secs"));
    io->timing.start_delay_ms = setting(c, "punch_start_delay_ms");
    io->timing.interval_ms = setting(c, "punch_interval_ms"); io->timing.duration_ms = setting(c, "punch_duration_ms");
    io->timing.cooldown_ms = fn_mul1000(setting(c, "extended_cooldown_secs"));
    io->timing.failure_threshold = (uint32_t)setting(c, "failure_streak_threshold");
    io->timing.retries = (uint32_t)fipsd_config_uint(fipsd_config_get(c, "node.retry.max_retries"));
    io->timing.retry_ms = fn_mul1000(fipsd_config_uint(fipsd_config_get(c, "node.retry.base_interval_secs")));
    io->timing.max_retry_ms = fn_mul1000(fipsd_config_uint(fipsd_config_get(c, "node.retry.max_backoff_secs")));
    if (!io->timing.answer_ms || !io->timing.attempt_ms || !io->timing.retry_ms) goto fail;
    const fipsd_config_value *servers = fipsd_config_get(c, "node.rendezvous.nostr.stun_servers");
    io->server_count = fipsd_config_count(servers); if (io->server_count > FIPSD_NOSTR_STUN_MAX) goto fail;
    for (size_t i = 0; i < io->server_count; ++i) {
        const char *s = fipsd_config_string(fipsd_config_at(servers, i));
        if (!s || strlen(s) > FIPSD_NOSTR_URL_MAX) goto fail;
        strcpy(io->servers[i], s);
    }
    for (size_t i = 0; i < d->transports->count; ++i) {
        fipsd_transport *t = d->transports->items[i]; if (strcmp(t->type, "udp")) continue;
        udp_path *p = &io->paths[io->path_count++]; p->owner = io; p->transport = t;
        if (!fipsd_udp_register_datagram_classifier(t, "nostr-traversal", classify, p)) goto fail;
    }
    return io;
fail:
    fipsd_traversal_io_destroy(io); return NULL;
}
int fipsd_traversal_io_start(fipsd_traversal_io *io, fipsd_runtime *rt, const fips_identity *id)
{
    io->host = rt; io->identity = id; io->ours = fips_identity_node_addr(id);
    if (fips_identity_npub(id, io->npub, sizeof io->npub) != FIPS_IDENTITY_OK || fipsd_runtime_spawn(rt, run, io)) return -1;
    io->started = true; return 0;
}
void fipsd_traversal_io_stop(fipsd_traversal_io *io)
{
    if (!io || !io->started) return;
    go_channel_close(io->stop); void *unused = NULL; (void)go_channel_receive(io->done, &unused); io->started = false;
}
void fipsd_traversal_io_destroy(fipsd_traversal_io *io)
{
    if (!io) return;
    fipsd_traversal_io_stop(io);
    for (size_t i = 0; i < io->path_count; ++i)
        (void)fipsd_udp_unregister_datagram_classifier(io->paths[i].transport, "nostr-traversal", &io->paths[i]);
    GoChannel *queues[] = {io->commands, io->packets, io->stop, io->done};
    for (size_t i = 0; i < 4; ++i) if (queues[i]) {
        void *p; while (go_channel_try_receive(queues[i], &p) == 0) free(p); go_channel_unref(queues[i]);
    }
    free(io->sessions); free(io);
}
static bool enqueue(fipsd_traversal_io *io, command *c)
{
    if (!c) return false;
    if (!io || go_channel_is_closed(io->stop) || go_channel_try_send(io->commands, c)) { free(c); return false; }
    return true;
}
bool fipsd_traversal_io_connect(fipsd_traversal_io *io, const char *key)
{
    if (!key || strlen(key) != 64) return false;
    command *c = calloc(1, sizeof *c); if (c) { c->kind = CONNECT; strcpy(c->key, key); } return enqueue(io, c);
}
bool fipsd_traversal_io_signal(fipsd_traversal_io *io, const fipsd_nostr_traversal *p)
{
    command *c = calloc(1, sizeof *c); if (c) { c->kind = SIGNAL; c->signal = *p; } return enqueue(io, c);
}
void fipsd_traversal_io_published(fipsd_traversal_io *io, const char *id, bool ok, bool repair)
{
    command *c = calloc(1, sizeof *c); if (c) { c->kind = PUBLISH_RESULT; strcpy(c->key, id); c->ok = ok; c->repair = repair; }
    if (!enqueue(io, c)) FIPSD_WARN("nostr", "traversal publish-result mailbox exhausted");
}
bool fipsd_traversal_io_udp_observed(fipsd_traversal_io *io, const fipsd_transport *t, char reflexive[65], bool *has_reflexive)
{
    bool ready = false;
    if (!io || !t || !reflexive || !has_reflexive) return false;
    *has_reflexive = false;
    nsync_mu_lock(&io->snapshot_mu);
    for (size_t i = 0; i < io->path_count; ++i) {
        udp_path *p = &io->paths[i];
        if (p->transport != t) continue;
        ready = p->snapshot_ready && t->ops->state(t) == FIPSD_TRANSPORT_BOUND;
        if (ready && p->snapshot.has_reflexive) {
            memcpy(reflexive, p->snapshot.reflexive.ip, sizeof p->snapshot.reflexive.ip); *has_reflexive = true;
        }
    }
    nsync_mu_unlock(&io->snapshot_mu); return ready;
}
