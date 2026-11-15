#include "internal.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#define V(p) fipsd_config_get(c,p)
#define CU(p) fipsd_config_uint(V(p))
#define CB(p) fipsd_config_bool(V(p))
#define N(p) fipsd_config_number(V(p))
#define CS(p) fipsd_config_string(V(p))
#define BAD(...) cfg_error(log,"invalid configuration: " __VA_ARGS__)
#define MEMBER(v,k) fipsd_config_member(v,k)
#define STR(v,k) fipsd_config_string(MEMBER(v,k))
#define NUM(v,k) fipsd_config_uint(MEMBER(v,k))
#define BOOL(v,k) fipsd_config_bool(MEMBER(v,k))

static bool exists(const fipsd_config_value *v) { return v && v->kind != CFG_NULL; }
static bool named(const fipsd_config_value *v)
{
    return v && v->named_transport;
}
static size_t instances(const fipsd_config_value *v)
{ return !exists(v) ? 0 : named(v) ? v->count : 1; }
static const fipsd_config_value *instance(const fipsd_config_value *v, size_t i)
{ return named(v) ? v->items[i] : v; }
static const char *label(const fipsd_config_value *v, size_t i, const char *fallback)
{ return named(v) ? v->keys[i] : fallback; }
static bool interface_name(const char *name, const char *iface, fipsd_config_log *log)
{
    if (!iface || !*iface) return BAD("transport `%s` has an empty `interface`",name);
    size_t len=strlen(iface);
    if (len>15) return BAD("transport `%s` interface `%s` is %zu bytes; the kernel limit is 15, so no such interface can exist",name,iface,len);
    for (const unsigned char *p=(const unsigned char *)iface;*p;++p) {
        if (*p=='/' || isspace(*p)) return BAD("transport `%s` interface `%s` contains a character no interface name may hold",name,iface);
        if (iscntrl(*p)) return BAD("transport `%s` interface contains a control character",name);
    }
    return true;
}
static bool loopback(const char *addr)
{
    if (!addr) return false;
    if (!strncmp(addr,"127.",4) || !strncmp(addr,"localhost:",10)) return true;
    return !strncmp(addr,"[::1]:",6) || !strncmp(addr,"[0:0:0:0:0:0:0:1]:",18);
}
static bool grants(const fipsd_config *c, fipsd_config_log *log)
{
    const fipsd_config_value *list=V("node.native_api.grants");
    size_t owners[768];
    for (size_t i=0;i<768;++i) owners[i]=SIZE_MAX;
    for (size_t i=0;i<list->count;++i) {
        const fipsd_config_value *g=list->items[i], *ports=MEMBER(g,"ports");
        if (exists(MEMBER(g,"uid")) == exists(MEMBER(g,"gid")))
            return BAD("node.native_api.grants[%zu] must name exactly one of `uid` and `gid`: a grant is for one principal",i);
        if (!ports->count && !BOOL(g,"keys") && !BOOL(g,"learn"))
            return BAD("node.native_api.grants[%zu] grants nothing: it names no port and neither `keys` nor `learn`",i);
        for (size_t j=0;j<ports->count;++j) {
            uint64_t port=ports->items[j]->u;
            if (port<256 || port>1023) return BAD("node.native_api.grants[%zu] grants port %llu, but a grant covers only ports 256 to 1023: ports below 256 are refused to everyone, and ports from 1024 need no grant",i,(unsigned long long)port);
            if (owners[port-256]!=SIZE_MAX) return BAD("node.native_api.grants[%zu] grants port %llu, which node.native_api.grants[%zu] already grants: a port has one grantee",i,(unsigned long long)port,owners[port-256]);
            owners[port-256]=i;
            if (CB("tun.enabled") && port==256) return BAD("node.native_api.grants[%zu] grants port 256, which the IPv6 adapter holds while `tun.enabled` is true",i);
        }
    }
    return true;
}
bool cfg_validate(fipsd_config *c, fipsd_config_log *log)
{
    const fipsd_config_value *ether=V("transports.ethernet"), *udp=V("transports.udp"),
        *tcp=V("transports.tcp"), *tor=V("transports.tor"), *ble=V("transports.ble"), *peers=V("peers");
    for (size_t i=0;i<instances(ether);++i) {
        const char *iface=STR(instance(ether,i),"interface"), *name=label(ether,i,"ethernet");
        if (!interface_name(name,iface,log)) return false;
        for (size_t j=0;j<i;++j) if (!strcmp(iface,STR(instance(ether,j),"interface")))
            return BAD("transports `%s` and `%s` both bind interface `%s`",label(ether,j,"ethernet"),name,iface);
    }
    for (size_t i=0;i<instances(udp);++i) {
        const char *iface=STR(instance(udp,i),"interface");
        if (iface && !interface_name(label(udp,i,"udp"),iface,log)) return false;
    }
#ifdef __APPLE__
    if (instances(ble)>1) return BAD("%zu BLE transports are configured; this platform has a single Bluetooth radio, so configure one",instances(ble));
#else
    (void)ble;
#endif
    bool advert=false, nat=false;
    const fipsd_config_value *ts[]={udp,tcp,tor};
    for (size_t t=0;t<3;++t) for (size_t i=0;i<instances(ts[t]);++i) {
        const fipsd_config_value *v=instance(ts[t],i);
        if (BOOL(v,"advertise_on_nostr")) {
            advert=true;
            if (t==0 && !BOOL(v,"public")) nat=true;
        }
    }
    bool nostr=CB("node.rendezvous.nostr.enabled");
    if (advert && !nostr) return BAD("at least one transport has `advertise_on_nostr = true`, but `node.rendezvous.nostr.enabled` is false");
    for (size_t i=0;i<peers->count;++i) if (BOOL(peers->items[i],"via_nostr") && !nostr)
        return BAD("at least one peer has `via_nostr = true`, but `node.rendezvous.nostr.enabled` is false");
    for (size_t i=0;i<peers->count;++i) {
        const fipsd_config_value *p=peers->items[i];
        if (!fipsd_config_count(MEMBER(p,"addresses")) && !BOOL(p,"via_nostr"))
            return BAD("peers[%zu] (%s): must specify at least one address, or set `via_nostr = true` to resolve endpoints from the Nostr advert",i,STR(p,"npub"));
    }
    if (nostr && nat && !fipsd_config_count(V("node.rendezvous.nostr.dm_relays")))
        return BAD("NAT UDP advert publishing requires `node.rendezvous.nostr.dm_relays` to be non-empty");
    if (nostr && nat && !fipsd_config_count(V("node.rendezvous.nostr.stun_servers")))
        return BAD("NAT UDP advert publishing requires `node.rendezvous.nostr.stun_servers` to be non-empty");
    if (CB("node.netmon.enabled")) {
        if (!CU("node.netmon.poll_interval_secs")) return BAD("`node.netmon.poll_interval_secs` must be at least 1; it is the backstop period behind the kernel event source, and the only detection signal at all on a platform without one");
        uint64_t debounce=CU("node.netmon.debounce_ms"), dead=CU("node.link_dead_timeout_secs");
        uint64_t worst=debounce>UINT64_MAX/8 ? UINT64_MAX : debounce*8;
        uint64_t timeout=dead>UINT64_MAX/1000 ? UINT64_MAX : dead*1000;
        if (timeout && worst>=timeout) return BAD("`node.netmon.debounce_ms` = %llu can hold a report for up to %llums across 8 settling rounds, which meets or exceeds `node.link_dead_timeout_secs` = %llus: the peering would be reaped before the medium change was ever acted on",(unsigned long long)debounce,(unsigned long long)worst,(unsigned long long)dead);
    }
    if (!CU("node.buffers.host_channel")) return BAD("node.buffers.host_channel is 0 but must be at least 1: a queue toward the host that holds nothing drops every packet its reader is not already waiting for");
    if (!CU("node.native_api.backlog")) return BAD("node.native_api.backlog is 0 but must be at least 1: a listener with no backlog admits no flow, so every arrival would be dropped");
    if (!CU("node.native_api.max_flows")) return BAD("node.native_api.max_flows is 0 but must be at least 1: a node that can hold no flow refuses every client connect and drops every arrival");
    uint64_t pending=CU("node.native_api.pending_per_flow");
    if (!pending) return BAD("node.native_api.pending_per_flow is 0 but must be at least 1: a flow that can hold nothing loses its peer's opening datagram between the arrival being announced and the client taking the flow");
    if (pending>64) return BAD("node.native_api.pending_per_flow is %llu but must not exceed 64: the whole batch is written onto a socket pair the client cannot read yet, and a larger one would not fit the send buffer",(unsigned long long)pending);
    if (!grants(c,log)) return false;
    for (size_t i=0;i<instances(udp);++i) {
        const fipsd_config_value *v=instance(udp,i);
        const char *bind=STR(v,"bind_addr");
        if (BOOL(v,"outbound_only") || !loopback(bind)) continue;
        for (size_t j=0;j<peers->count;++j) {
            const fipsd_config_value *addrs=MEMBER(peers->items[j],"addresses");
            for (size_t a=0;a<addrs->count;++a) if (!strcmp(STR(addrs->items[a],"transport"),"udp") && !loopback(STR(addrs->items[a],"addr")))
                return BAD("transports.udp[%s].bind_addr is loopback (%s) but at least one peer has a non-loopback UDP address; fips cannot reach external peers from a loopback-bound socket. Use bind_addr: \"0.0.0.0:2121\" (with kernel-firewall hardening if exposure is a concern), or set outbound_only: true.",label(udp,i,"(unnamed)"),bind);
        }
    }
    for (size_t i=0;i<peers->count;++i) {
        const fipsd_config_value *p=peers->items[i], *addrs=MEMBER(p,"addresses");
        for (size_t a=0;a<addrs->count;++a) {
            const char *transport=STR(addrs->items[a],"transport"), *addr=STR(addrs->items[a],"addr");
#ifdef __APPLE__
            if (instances(ble) && !strcmp(transport,"ble") && strncmp(addr,"ble0/",5))
                return BAD("BLE peer address \"%s\" for %s cannot be dialled on macOS: CoreBluetooth hides link addresses, so only scan-discovered peers (adapter \"ble0\") are reachable; remove it and enable `transports.ble.auto_connect`",addr,STR(p,"npub"));
#endif
            const char *slash=strchr(transport,'/');
            if (!slash || slash==transport || !slash[1]) continue;
            char kind[256]; size_t n=(size_t)(slash-transport);
            if (n>=sizeof(kind)) return BAD("peer transport kind too long");
            memcpy(kind,transport,n); kind[n]=0;
            const char *want=slash+1;
            if (strcmp(kind,"udp")) return BAD("peer `%s` has address `%s` on transport `%s`, but only `udp` resolves an instance name; for any other type the dialer would have to pick an arbitrary instance, which is the wrong-lane dial the syntax exists to prevent. Drop the `/%s` qualifier to match any instance of `%s`.",STR(p,"npub"),addr,transport,want,kind);
            if (!named(udp) || !MEMBER(udp,want)) {
                char known[2048]="no named udp instances are configured (the udp transport is a single unnamed instance)";
                if (named(udp)) {
                    strcpy(known,"configured udp instances are: ");
                    for (size_t j=0;j<udp->count;++j) {
                        size_t used=strlen(known);
                        snprintf(known+used,sizeof(known)-used,"%s%s",j ? ", " : "",udp->keys[j]);
                    }
                }
                return BAD("peer `%s` has address `%s` on transport `%s`, but no udp transport is configured under the instance name `%s`; a qualified name is never substituted, so this address would be skipped at every dial and the peer reached only over its other addresses, if it has any. %s.",STR(p,"npub"),addr,transport,want,known);
            }
        }
    }
    if (!CU("node.rekey.after_messages")) return BAD("`node.rekey.after_messages` must be at least 1; 0 fires the message-count trigger on every poll instead of disabling it. Use a very large value to effectively disable the message-count trigger.");
    if (CU("node.rekey.after_secs")<=15) return BAD("`node.rekey.after_secs` is %llu, but must be greater than the per-session rekey jitter of 15s; each session offsets the interval by a random value in [-15, +15] seconds, so a smaller interval saturates to zero and rekeys on sight for roughly half of sessions. Use a very large value to effectively disable the timer trigger.",(unsigned long long)CU("node.rekey.after_secs"));
    if (exists(V("node.rate_limit.established_handshake_burst")) && !CU("node.rate_limit.established_handshake_burst"))
        return BAD("`node.rate_limit.established_handshake_burst` is 0, which refuses every rekey and restart msg1 from an established peer rather than disabling the limit. Omit the key to derive it from `node.limits.max_peers`, or set a positive burst.");
    double rate=N("node.rate_limit.established_handshake_rate");
    if (exists(V("node.rate_limit.established_handshake_rate")) && !(isfinite(rate) && rate>0))
        return BAD("`node.rate_limit.established_handshake_rate` is %g, but must be a finite value greater than 0; a non-positive or non-finite refill rate never replenishes the established-link bucket, so rekey msg1 stops being admitted once the initial burst is spent. Omit the key to derive it from `node.limits.max_peers` and `node.rekey.after_secs`.",rate);
    if (!CU("node.rate_limit.session_setup_burst")) return BAD("`node.rate_limit.session_setup_burst` is 0, which refuses every inbound SessionSetup rather than disabling the limit. Set a positive burst; a very large value effectively disables it.");
    rate=N("node.rate_limit.session_setup_rate");
    if (!(isfinite(rate) && rate>0)) return BAD("`node.rate_limit.session_setup_rate` is %g, but must be a finite value greater than 0; a non-positive or non-finite refill rate never replenishes a link's setup bucket, so that link stops establishing sessions once its initial burst is spent.",rate);
    uint64_t ttl=CU("node.rendezvous.nostr.signal_ttl_secs"), replay=CU("node.rendezvous.nostr.replay_window_secs");
    uint64_t fresh=ttl>UINT64_MAX-120 ? UINT64_MAX : ttl+120;
    if (fresh>=replay) return BAD("`node.rendezvous.nostr.signal_ttl_secs` is %llu, which with 60s of clock-skew grace on each side makes a traversal signal acceptable over a %llus window, but `node.rendezvous.nostr.replay_window_secs` is %llu. The freshness window must be strictly narrower than the replay window, or a session id evicted from the replay cache is still fresh enough to be accepted a second time. Raise `replay_window_secs` above %llu; `signal_ttl_secs` runs at no less than 120 whatever is configured, so lowering it below 120 does not help.",(unsigned long long)ttl,(unsigned long long)fresh,(unsigned long long)replay,(unsigned long long)fresh);
    uint64_t offers=CU("node.rendezvous.nostr.max_concurrent_offers_per_npub");
    if (offers>SIZE_MAX/8) return BAD("`node.rendezvous.nostr.max_concurrent_offers_per_npub` is %llu, which exceeds the maximum %llu permits a semaphore can hold. Use a value at or below `max_concurrent_incoming_offers`, which is the outer bound anything larger is inert against.",(unsigned long long)offers,(unsigned long long)(SIZE_MAX/8));
    double margin=N("node.path.switch.margin");
    if (!isfinite(margin) || margin<1) return BAD("`node.path.switch.margin` = %g must be a finite number of at least 1.0: a standby must beat the active path's score by this factor, and anything less makes two equal paths swap after every dwell",margin);
    const char *positive[]={"node.path.switch.dwell_secs","node.path.probe.active_ms","node.path.probe.standby_ms","node.path.min_samples"};
    for (size_t i=0;i<4;++i) if (!CU(positive[i])) return BAD("`%s` must be greater than zero",positive[i]);
    if (CU("node.path.min_samples")>255) return BAD("`node.path.min_samples` = %llu must be at most 255",(unsigned long long)CU("node.path.min_samples"));
    const fipsd_config_value *forwards=V("gateway.port_forwards");
    for (size_t i=0;i<fipsd_config_count(forwards);++i) {
        const fipsd_config_value *pf=forwards->items[i];
        uint64_t port=NUM(pf,"listen_port");
        const char *proto=STR(pf,"proto");
        if (!port) return BAD("port_forward listen_port must be non-zero");
        for (size_t j=0;j<i;++j) if (NUM(forwards->items[j],"listen_port")==port && !strcmp(STR(forwards->items[j],"proto"),proto))
            return BAD("duplicate port_forward (%s %llu) — each (listen_port, proto) must be unique",!strcmp(proto,"tcp") ? "Tcp" : "Udp",(unsigned long long)port);
        const char *target=STR(pf,"target"), *end=strchr(target,']');
        char host[256]; struct in6_addr ip;
        size_t n=end ? (size_t)(end-target) : 0;
        if (*target!='[' || n<2 || n>=sizeof(host) || end[1]!=':') return BAD("invalid IPv6 socket address syntax");
        memcpy(host,target+1,n-1); host[n-1]=0;
        char *stop; unsigned long destport=strtoul(end+2,&stop,10);
        if (inet_pton(AF_INET6,host,&ip)!=1 || !end[2] || *stop || destport>65535) return BAD("invalid IPv6 socket address syntax");
    }
    return true;
}
