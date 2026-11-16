#ifndef FIPSD_GATEWAY_NET_H
#define FIPSD_GATEWAY_NET_H
/* Network setup for the gateway, the port of upstream src/gateway/net.rs
 * (nostrc-iq1n9.3): the IPv6 forwarding and proxy NDP prerequisites, the
 * `local <pool> dev lo` route and the proxy NDP entries for virtual IPs.
 * Routes and neighbour entries are made with `ip`, as upstream does; the
 * runner is injected so tests can stand in for it. Every call here blocks:
 * a fiber reaches them through go_blocking_submit. */
#include "gateway/nat.h"

#define FIPSD_GW_SYSCTL_CONF "/proc/sys/net/ipv6/conf"

/* 1 forwarding is on, 0 off, -1 unreadable (errno). path NULL: the sysctl. */
int fipsd_gw_ipv6_forwarding(const char *path);
/* Whether proxy NDP is on for lan_interface under conf: 1 on (all/ or the
 * interface's own proxy_ndp is set), 0 off, -1 when neither file reads. */
int fipsd_gw_proxy_on(const char *conf, const char *lan_interface);
/* Warn when proxy NDP is off for lan_interface. */
void fipsd_gw_check_proxy(const char *lan_interface);
/* The interface's index, 0 when it does not exist. */
uint32_t fipsd_gw_iface_index(const char *name);
/* Wait up to timeout_ms for interface name to exist, woken by rtnetlink link
 * notifications rather than by polling. cancel_fd (or -1) readable ends the
 * wait. 0 found, ETIMEDOUT, ECANCELED, or another errno. */
int fipsd_gw_wait_interface(const char *name, unsigned timeout_ms, int cancel_fd);

/* Run argv: its exit status (0 success), or -1 with errno when it could not
 * run. Its stderr is captured into err. */
typedef int (*fipsd_gw_cmd_fn)(void *user, const char *const *argv, char *err, size_t cap);
int fipsd_gw_cmd_spawn(void *user, const char *const *argv, char *err, size_t cap);

typedef struct fipsd_gw_net {
    char lan_interface[64];
    char pool_cidr[64];
    struct in6_addr *proxy; size_t nproxy, cap; /* proxy NDP entries added this run */
    bool route_added;
    fipsd_gw_cmd_fn run; void *user;
} fipsd_gw_net;
void fipsd_gw_net_init(fipsd_gw_net *net, const char *lan_interface, const char *pool_cidr, fipsd_gw_cmd_fn run, void *user);
void fipsd_gw_net_clear(fipsd_gw_net *net);
/* `ip -6 route add local <pool> dev lo`; an existing route is not an error.
 * 0, or -1 with err. */
int fipsd_gw_net_add_pool_route(fipsd_gw_net *net, char *err, size_t cap);
int fipsd_gw_net_add_proxy_ndp(fipsd_gw_net *net, const struct in6_addr *addr, char *err, size_t cap);
int fipsd_gw_net_remove_proxy_ndp(fipsd_gw_net *net, const struct in6_addr *addr, char *err, size_t cap);
/* Remove every proxy entry and the route added this run. */
void fipsd_gw_net_cleanup(fipsd_gw_net *net);

/* Proxy NDP outcomes, latched per direction; an error naming its address
 * counts as the same error for every address. */
typedef struct fipsd_gw_ndp_log { fipsd_gw_retry_log add, remove; } fipsd_gw_ndp_log;
fipsd_gw_retry_report fipsd_gw_ndp_observe(fipsd_gw_ndp_log *log, bool add, const struct in6_addr *addr, int rc, const char *err);
/* Log an outcome as upstream's report_proxy_ndp. */
void fipsd_gw_report_proxy_ndp(fipsd_gw_retry_report report, bool add, const struct in6_addr *addr, int rc, const char *err);
#endif
