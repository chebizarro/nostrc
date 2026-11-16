/* The dns subsystem: upstream's start_dns/run_responder over this daemon's
 * runtime, handlers and TUN owner. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "dns/service.h"
#include "host_os.h"
#include "log.h"
#include <libgo/fiber.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MODULE "dns"

struct fipsd_dns_service {
    fipsd_subsystem subsystem;
    GoSwap *configs;
    fipsd_node_handlers *handlers;
    fipsd_tun *tun;
    char *hosts_path;
    fipsd_dns_identity_sink sink; void *sink_user;
    bool mesh_override;
    fipsd_dns_mesh_side mesh;
    GoChannel *events;
    fips_secp *secp;
    int fd;                 /* bound at start; owned by the fiber once spawned */
    bool fiber_owns_fd;
    struct sockaddr_storage local; socklen_t local_len;
    bool wildcard;
    uint32_t ttl;
    fipsd_hosts_reloader reloader; bool reloader_ready;
    GoRefPtr seen;          /* the configuration snapshot the alias base was built from */
    bool has_seen;
    atomic_bool stopping;
};

static void learn(void *user, const fips_peer_identity *peer)
{
    fipsd_dns_service *s = user;
    if (fipsd_node_handlers_learn_identity(s->handlers, peer) != 0)
        FIPSD_DEBUG(MODULE, "resolved identity not queued: the dispatcher's command queue is full");
}
static void post(fipsd_dns_service *s, const fipsd_dns_event *e)
{
    if (!s->events) return;
    fipsd_dns_event *owned = malloc(sizeof *owned);
    if (!owned) return;
    *owned = *e;
    if (go_channel_try_send(s->events, owned) != 0) free(owned);
}
/* Bring the host map up to date before answering a query: apply the newest
 * configuration's peer aliases if not seen yet, then re-read the hosts file
 * if its mtime changed. */
static void refresh_hosts(fipsd_dns_service *s)
{
    GoRefPtr cfg = go_swap_load(s->configs);
    if (cfg.ptr && (!s->has_seen || cfg.ptr != s->seen.ptr)) {
        fipsd_hosts_map base; fipsd_hosts_map_init(&base);
        /* The node already warned about each alias it skipped from the same
         * list; warning again would log it twice. */
        if (fipsd_hosts_map_from_config(&base, cfg.ptr, s->secp, true) && fipsd_hosts_reloader_set_base(&s->reloader, &base)) {
            if (s->has_seen) go_refptr_release(&s->seen);
            s->seen = cfg; s->has_seen = true;
        } else { fipsd_hosts_map_clear(&base); go_refptr_release(&cfg); }
    } else go_refptr_release(&cfg);
    (void)fipsd_hosts_reloader_check_reload(&s->reloader);
}
static void responder(void *arg)
{
    fipsd_dns_service *s = arg;
    int fd = s->fd;
    gof_set_name("dns");
    uint8_t *buf = malloc(FIPSD_DNS_MAX_DATAGRAM), *reply = malloc(FIPSD_DNS_MAX_REPLY);
    fipsd_dns_screen *screen = malloc(sizeof *screen);
    const struct in6_addr *mesh_addr = s->mesh.has_addr ? &s->mesh.addr : NULL;
    while (buf && reply && screen && !atomic_load(&s->stopping)) {
        struct sockaddr_storage src; socklen_t src_len = 0; fipsd_dns_arrival arrival;
        ssize_t n = fipsd_dns_recv_with_pktinfo(fd, buf, FIPSD_DNS_MAX_DATAGRAM, &src, &src_len, &arrival, -1);
        if (n < 0) {
            if (errno == ECANCELED || atomic_load(&s->stopping)) break;
            FIPSD_DEBUG(MODULE, "DNS socket recv error: %s", strerror(errno));
            continue;
        }
        if (atomic_load(&s->stopping)) break;
        const struct sockaddr *from = (const struct sockaddr *)&src;
        char text[64]; fipsd_dns_sockaddr_text(from, text, sizeof text);
        uint16_t src_port = from->sa_family == AF_INET6 ? ntohs(((struct sockaddr_in6 *)&src)->sin6_port)
                          : from->sa_family == AF_INET ? ntohs(((struct sockaddr_in *)&src)->sin_port) : 0;
        fipsd_dns_event event = {.kind = FIPSD_DNS_EVENT_MESH};
        if (fipsd_dns_mesh_query(&arrival, from, &s->mesh)) {
            FIPSD_TRACE(MODULE, "DNS query from the mesh, dropping: src=%s ifindex=%u", text, arrival.has_ifindex ? arrival.ifindex : 0);
            post(s, &event); continue;
        }
        fipsd_dns_reply_path path = fipsd_dns_reply_path_for(from, arrival.has_ifindex, arrival.ifindex,
            s->mesh.has_ifindex, s->mesh.ifindex, s->wildcard, FIPSD_DNS_STRICT_PIN);
        fipsd_dns_screen_datagram(buf, (size_t)n, src_port, screen);
        size_t len = 0; fips_peer_identity identity; bool resolved = false;
        char rcode_text[8];
        event.path = path.kind;
        if (screen->kind == FIPSD_DNS_SCREEN_DROP) {
            FIPSD_DEBUG(MODULE, "DNS datagram dropped: src=%s len=%zd reason=%s", text, n, fipsd_dns_drop_reason_str(screen->reason));
            event.kind = FIPSD_DNS_EVENT_DROPPED; event.reason = screen->reason;
            post(s, &event); continue;
        }
        if (screen->kind == FIPSD_DNS_SCREEN_REPLY) {
            FIPSD_DEBUG(MODULE, "DNS query refused: src=%s len=%zd rcode=%s", text, n, fipsd_dns_rcode_name(screen->rcode, rcode_text, sizeof rcode_text));
            memcpy(reply, screen->reply, screen->reply_len); len = screen->reply_len;
            event.kind = FIPSD_DNS_EVENT_REFUSED; event.rcode = screen->rcode; event.id = (uint16_t)((buf[0] << 8) | buf[1]);
        } else {
            /* For a query only: apply any new peer-alias base, then check for
             * hosts file changes (one stat). */
            refresh_hosts(s);
            len = fipsd_dns_answer(&screen->query, s->ttl, fipsd_hosts_reloader_hosts(&s->reloader), s->secp,
                                   reply, FIPSD_DNS_MAX_REPLY, &identity, &resolved);
            event.kind = FIPSD_DNS_EVENT_ANSWERED; event.rcode = fipsd_dns_rcode_from_header(reply[3]);
            event.resolved = resolved; event.id = screen->query.id;
        }
        if (resolved) {
            char addr[40]; (void)fips_node_addr_short_hex(&identity.node_addr, addr, sizeof addr);
            FIPSD_DEBUG(MODULE, "DNS resolved .fips name, registering identity: node_addr=%s", addr);
            s->sink(s->sink_user, &identity);
        }
        const char *withheld = NULL;
        ssize_t sent = fipsd_dns_send_reply(fd, reply, len, from, src_len, path, mesh_addr, &withheld);
        if (sent < 0) FIPSD_DEBUG(MODULE, "DNS send error: %s", strerror(errno));
        else if (withheld) FIPSD_DEBUG(MODULE, "DNS reply to %s withheld: reason=%s", text, withheld);
        event.sent = sent;
        post(s, &event);
    }
    free(buf); free(reply); free(screen);
    (void)gof_io_reset_fd(fd);
    close(fd);
}

static bool parse_bind(const char *text, uint16_t port, struct sockaddr_storage *out, socklen_t *len)
{
    memset(out, 0, sizeof *out);
    struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)out;
    struct sockaddr_in *v4 = (struct sockaddr_in *)out;
    if (text && inet_pton(AF_INET6, text, &v6->sin6_addr) == 1) {
        v6->sin6_family = AF_INET6; v6->sin6_port = htons(port); *len = sizeof *v6; return true;
    }
    if (text && inet_pton(AF_INET, text, &v4->sin_addr) == 1) {
        v4->sin_family = AF_INET; v4->sin_port = htons(port); *len = sizeof *v4; return true;
    }
    return false;
}
static int start(fipsd_subsystem *subsystem, fipsd_runtime *rt)
{
    fipsd_dns_service *s = subsystem->user;
    atomic_store(&s->stopping, false);
    GoRefPtr cfg = go_swap_load(s->configs);
    const fipsd_config *config = cfg.ptr;
    const char *bind_addr = fipsd_config_string(fipsd_config_get(config, "dns.bind_addr"));
    uint64_t port = fipsd_config_uint(fipsd_config_get(config, "dns.port"));
    uint64_t ttl = fipsd_config_uint(fipsd_config_get(config, "dns.ttl"));
    s->ttl = fipsd_dns_clamp_ttl(ttl > UINT32_MAX ? UINT32_MAX : (uint32_t)ttl);
    struct sockaddr_storage bind; socklen_t bind_len;
    if (!parse_bind(bind_addr, (uint16_t)port, &bind, &bind_len)) {
        FIPSD_WARN(MODULE, "Invalid dns.bind_addr; DNS responder not started: addr=%s", bind_addr ? bind_addr : "(unset)");
        go_refptr_release(&cfg); return 0;
    }
    char text[64];
    s->fd = fipsd_dns_bind_socket((struct sockaddr *)&bind, bind_len);
    if (s->fd < 0) {
        FIPSD_WARN(MODULE, "Failed to start DNS responder: bind=%s error=%s",
                   fipsd_dns_sockaddr_text((struct sockaddr *)&bind, text, sizeof text), strerror(errno));
        go_refptr_release(&cfg); return 0;
    }
    /* Read the bound address back off the socket: a port-0 configuration
     * resolves to the kernel-assigned port here. */
    s->local_len = sizeof s->local;
    if (getsockname(s->fd, (struct sockaddr *)&s->local, &s->local_len) != 0) { s->local = bind; s->local_len = bind_len; }
    s->wildcard = fipsd_dns_wildcard_bind((struct sockaddr *)&s->local);
    if (!s->mesh_override) {
        s->mesh = (fipsd_dns_mesh_side){0};
        if (s->tun && fipsd_tun_fd(s->tun) >= 0) {
            /* The name comes from the device the TUN path actually created,
             * not the configured one: macOS and FreeBSD assign utunN/tunN of
             * their own choosing. */
            const char *name = fipsd_tun_name(s->tun);
            uint32_t index = fipsd_dns_lookup_ifindex(name);
            s->mesh.has_ifindex = index != 0; s->mesh.ifindex = index;
            s->mesh.has_addr = true; memcpy(&s->mesh.addr, fipsd_tun_address(s->tun).bytes, 16);
            if (!index) FIPSD_WARN(MODULE, "Mesh interface index unresolved; DNS mesh filter falls back to the destination address: device=%s", name ? name : "?");
        }
    }
    if (!s->secp && fips_identity_init_secp(fipsd_host_os_entropy(), &s->secp) != FIPS_IDENTITY_OK) {
        FIPSD_ERROR(MODULE, "cannot initialise the secp256k1 context"); goto fail;
    }
    fipsd_hosts_map base; fipsd_hosts_map_init(&base);
    if (!fipsd_hosts_map_from_config(&base, config, s->secp, false)) { FIPSD_ERROR(MODULE, "cannot build the alias map"); goto fail; }
    if (s->reloader_ready) { fipsd_hosts_reloader_clear(&s->reloader); s->reloader_ready = false; }
    if (!fipsd_hosts_reloader_init(&s->reloader, &base, s->hosts_path, s->secp)) { FIPSD_ERROR(MODULE, "cannot load the hosts file map"); goto fail; }
    s->reloader_ready = true;
    if (s->has_seen) go_refptr_release(&s->seen);
    s->seen = cfg; s->has_seen = true; /* retained: the base was built from it */
    if (gof_io_reset_fd(s->fd) != 0) { FIPSD_ERROR(MODULE, "cannot register the socket for cancellation: %s", strerror(errno)); goto fail_seen; }
    s->fiber_owns_fd = true;
    if (fipsd_runtime_spawn(rt, responder, s) != 0) { s->fiber_owns_fd = false; FIPSD_ERROR(MODULE, "cannot spawn the responder fiber"); goto fail_seen; }
    FIPSD_INFO(MODULE, "DNS responder started for .fips domain (auto-reload enabled): bind=%s hosts=%zu mesh_ifindex=%u",
               fipsd_dns_sockaddr_text((struct sockaddr *)&s->local, text, sizeof text),
               fipsd_hosts_map_len(fipsd_hosts_reloader_hosts(&s->reloader)), s->mesh.has_ifindex ? s->mesh.ifindex : 0);
    return 0;
fail_seen:
    go_refptr_release(&s->seen); s->has_seen = false; cfg.ptr = NULL;
fail:
    if (cfg.ptr) go_refptr_release(&cfg);
    close(s->fd); s->fd = -1;
    return -1;
}
static void stop(fipsd_subsystem *subsystem, fipsd_runtime *rt)
{
    (void)rt;
    fipsd_dns_service *s = subsystem->user;
    atomic_store(&s->stopping, true);
    /* Owner-directed cancellation: the fiber's pending receive returns
     * ECANCELED and it closes the socket on its way out. */
    if (s->fiber_owns_fd) (void)gof_io_cancel_fd(s->fd);
}

fipsd_dns_service *fipsd_dns_service_create(GoSwap *configs, fipsd_node_handlers *handlers, fipsd_tun *tun)
{
    fipsd_dns_service *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->hosts_path = strdup(FIPSD_HOSTS_DEFAULT_PATH);
    if (!s->hosts_path) { free(s); return NULL; }
    s->configs = configs; s->handlers = handlers; s->tun = tun; s->fd = -1;
    s->sink = learn; s->sink_user = s;
    s->subsystem = (fipsd_subsystem){.name = "dns", .start = start, .stop = stop, .user = s};
    atomic_init(&s->stopping, false);
    return s;
}
fipsd_subsystem *fipsd_dns_service_subsystem(fipsd_dns_service *s) { return &s->subsystem; }
void fipsd_dns_service_destroy(fipsd_dns_service *s)
{
    if (!s) return;
    if (s->has_seen) go_refptr_release(&s->seen);
    if (s->reloader_ready) fipsd_hosts_reloader_clear(&s->reloader);
    if (s->fd >= 0 && !s->fiber_owns_fd) close(s->fd);
    fips_identity_destroy_secp(s->secp);
    free(s->hosts_path);
    free(s);
}
int fipsd_dns_service_set_hosts_path(fipsd_dns_service *s, const char *path)
{
    char *copy = strdup(path);
    if (!copy) return -1;
    free(s->hosts_path); s->hosts_path = copy;
    return 0;
}
void fipsd_dns_service_set_identity_sink(fipsd_dns_service *s, fipsd_dns_identity_sink sink, void *user)
{ s->sink = sink ? sink : learn; s->sink_user = sink ? user : s; }
void fipsd_dns_service_set_mesh_side(fipsd_dns_service *s, const fipsd_dns_mesh_side *mesh)
{ s->mesh_override = mesh != NULL; if (mesh) s->mesh = *mesh; }
void fipsd_dns_service_observe(fipsd_dns_service *s, GoChannel *events) { s->events = events; }
int fipsd_dns_service_local_addr(const fipsd_dns_service *s, struct sockaddr_storage *out, socklen_t *len)
{
    if (!s->fiber_owns_fd) return -1;
    *out = s->local; *len = s->local_len;
    return 0;
}
