#ifdef __linux__
#define _GNU_SOURCE 1
#else
#define _DARWIN_C_SOURCE 1
#endif
#include "gateway/net.h"
#include "log.h"
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <poll.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <sys/socket.h>
#endif

#define MODULE "gateway"
extern char **environ;

static int read_line(const char *path, char *buf, size_t cap)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, cap - 1);
    int e = errno;
    close(fd);
    if (n < 0) { errno = e; return -1; }
    buf[n] = 0;
    for (ssize_t i = n - 1; i >= 0 && (buf[i] == '\n' || buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r'); --i) buf[i] = 0;
    size_t lead = strspn(buf, " \t");
    if (lead) memmove(buf, buf + lead, strlen(buf + lead) + 1);
    return 0;
}
int fipsd_gw_ipv6_forwarding(const char *path)
{
    char v[32];
    if (read_line(path ? path : FIPSD_GW_SYSCTL_CONF "/all/forwarding", v, sizeof v) != 0) return -1;
    return !strcmp(v, "1");
}
int fipsd_gw_proxy_on(const char *conf, const char *lan_interface)
{
    char path[4096], v[32];
    int all = -1, own = -1;
    snprintf(path, sizeof path, "%s/all/proxy_ndp", conf);
    if (read_line(path, v, sizeof v) == 0) all = strcmp(v, "0") != 0;
    snprintf(path, sizeof path, "%s/%s/proxy_ndp", conf, lan_interface);
    if (read_line(path, v, sizeof v) == 0) own = strcmp(v, "0") != 0;
    if (all == 1 || own == 1) return 1;
    if (all < 0 && own < 0) return -1;
    return 0;
}
void fipsd_gw_check_proxy(const char *lan_interface)
{
    switch (fipsd_gw_proxy_on(FIPSD_GW_SYSCTL_CONF, lan_interface)) {
    case 1: FIPSD_DEBUG(MODULE, "Proxy NDP is enabled: interface=%s", lan_interface); break;
    case 0: FIPSD_WARN(MODULE, "Proxy NDP is disabled, so LAN hosts that resolve pool addresses on-link get no answer. "
                       "Enable with: sysctl -w net.ipv6.conf.all.proxy_ndp=1: interface=%s", lan_interface); break;
    default: FIPSD_WARN(MODULE, "Could not check the proxy NDP state: interface=%s", lan_interface); break;
    }
}
uint32_t fipsd_gw_iface_index(const char *name) { return name && *name ? if_nametoindex(name) : 0; }

#ifdef __linux__
static uint64_t mono_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}
#endif
int fipsd_gw_wait_interface(const char *name, unsigned timeout_ms, int cancel_fd)
{
#ifdef __linux__
    /* Subscribe before the first check, so a link that appears between the
     * check and the wait is still announced. */
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_ROUTE);
    if (fd >= 0) {
        struct sockaddr_nl local = {.nl_family = AF_NETLINK, .nl_groups = RTMGRP_LINK};
        if (bind(fd, (struct sockaddr *)&local, sizeof local) != 0) { close(fd); fd = -1; }
    }
    if (fd < 0) return errno ? errno : EIO;
    uint64_t deadline = mono_ms() + timeout_ms;
    int rc = ETIMEDOUT;
    char buf[8192];
    for (;;) {
        if (fipsd_gw_iface_index(name)) { rc = 0; break; }
        uint64_t now = mono_ms();
        if (now >= deadline) break;
        struct pollfd p[2] = {{.fd = fd, .events = POLLIN}, {.fd = cancel_fd, .events = POLLIN}};
        int n = poll(p, cancel_fd >= 0 ? 2 : 1, (int)(deadline - now));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { rc = errno; break; }
        if (n == 0) continue; /* the deadline: the loop's check decides */
        if (cancel_fd >= 0 && (p[1].revents & (POLLIN | POLLHUP))) { rc = ECANCELED; break; }
        /* Drain the notifications; which link changed does not matter, the
         * lookup by name does. ENOBUFS (an overrun) only means a re-check. */
        while (recv(fd, buf, sizeof buf, 0) > 0) {}
    }
    close(fd);
    return rc;
#else
    (void)timeout_ms; (void)cancel_fd;
    return fipsd_gw_iface_index(name) ? 0 : ENOSYS;
#endif
}

int fipsd_gw_cmd_spawn(void *user, const char *const *argv, char *err, size_t cap)
{
    (void)user;
    if (cap) err[0] = 0;
    int pipefd[2];
    if (pipe(pipefd) != 0) return -1;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], 2);
    posix_spawn_file_actions_addclose(&actions, pipefd[0]);
    pid_t pid;
    int rc = posix_spawnp(&pid, argv[0], &actions, NULL, (char *const *)argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipefd[1]);
    if (rc != 0) { close(pipefd[0]); errno = rc; return -1; }
    size_t at = 0;
    char sink[256];
    for (;;) {
        char *dst = at + 1 < cap ? err + at : sink;
        size_t room = at + 1 < cap ? cap - at - 1 : sizeof sink;
        ssize_t n = read(pipefd[0], dst, room);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        if (dst != sink) { at += (size_t)n; err[at] = 0; }
    }
    close(pipefd[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) if (errno != EINTR) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

void fipsd_gw_net_init(fipsd_gw_net *net, const char *lan_interface, const char *pool_cidr, fipsd_gw_cmd_fn run, void *user)
{
    memset(net, 0, sizeof *net);
    snprintf(net->lan_interface, sizeof net->lan_interface, "%s", lan_interface);
    snprintf(net->pool_cidr, sizeof net->pool_cidr, "%s", pool_cidr);
    net->run = run ? run : fipsd_gw_cmd_spawn; net->user = user;
}
void fipsd_gw_net_clear(fipsd_gw_net *net) { free(net->proxy); net->proxy = NULL; net->nproxy = net->cap = 0; }
static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == ' ' || s[n - 1] == '\r' || s[n - 1] == '\t')) s[--n] = 0;
}
/* Run argv: 0 success, otherwise the exit status with its stderr in out, or
 * -1 with the spawn error in out. */
static int run(fipsd_gw_net *net, const char *const *argv, char *out, size_t cap)
{
    int status = net->run(net->user, argv, out, cap);
    if (status < 0) snprintf(out, cap, "%s", strerror(errno));
    return status;
}
int fipsd_gw_net_add_pool_route(fipsd_gw_net *net, char *err, size_t cap)
{
    const char *argv[] = {"ip", "-6", "route", "add", "local", net->pool_cidr, "dev", "lo", NULL};
    char out[1024];
    int status = run(net, argv, out, sizeof out);
    if (status < 0) { snprintf(err, cap, "%s", out); return -1; }
    if (status) {
        /* "File exists": the route is already present, not an error. */
        if (strstr(out, "File exists")) { FIPSD_DEBUG(MODULE, "Pool route already exists: cidr=%s", net->pool_cidr); return 0; }
        trim(out);
        snprintf(err, cap, "Failed to add pool route: %s", out);
        return -1;
    }
    net->route_added = true;
    FIPSD_INFO(MODULE, "Added local pool route: cidr=%s", net->pool_cidr);
    return 0;
}
int fipsd_gw_net_add_proxy_ndp(fipsd_gw_net *net, const struct in6_addr *addr, char *err, size_t cap)
{
    char text[48]; fipsd_gw_ip6_text(addr, text, sizeof text);
    const char *argv[] = {"ip", "-6", "neigh", "add", "proxy", text, "dev", net->lan_interface, NULL};
    char out[1024];
    int status = run(net, argv, out, sizeof out);
    if (status < 0) { snprintf(err, cap, "%s", out); return -1; }
    if (status) {
        if (strstr(out, "File exists")) { FIPSD_DEBUG(MODULE, "Proxy NDP entry already exists: addr=%s", text); return 0; }
        trim(out);
        snprintf(err, cap, "Failed to add proxy NDP: %s", out);
        return -1;
    }
    if (net->nproxy == net->cap) {
        size_t c = net->cap ? net->cap * 2 : 16;
        struct in6_addr *grown = realloc(net->proxy, c * sizeof *grown);
        if (grown) { net->proxy = grown; net->cap = c; }
    }
    if (net->nproxy < net->cap) net->proxy[net->nproxy++] = *addr;
    FIPSD_DEBUG(MODULE, "Added proxy NDP entry: addr=%s iface=%s", text, net->lan_interface);
    return 0;
}
int fipsd_gw_net_remove_proxy_ndp(fipsd_gw_net *net, const struct in6_addr *addr, char *err, size_t cap)
{
    char text[48]; fipsd_gw_ip6_text(addr, text, sizeof text);
    const char *argv[] = {"ip", "-6", "neigh", "del", "proxy", text, "dev", net->lan_interface, NULL};
    char out[1024];
    int status = run(net, argv, out, sizeof out);
    for (size_t i = 0; i < net->nproxy;) {
        if (fipsd_gw_ip6_eq(&net->proxy[i], addr)) net->proxy[i] = net->proxy[--net->nproxy];
        else ++i;
    }
    if (status < 0) { snprintf(err, cap, "%s", out); return -1; }
    /* "No such file": the entry was already gone. */
    if (status && !strstr(out, "No such file")) { trim(out); snprintf(err, cap, "Failed to remove proxy NDP: %s", out); return -1; }
    return 0;
}
void fipsd_gw_net_cleanup(fipsd_gw_net *net)
{
    size_t failed = 0;
    char last[1024] = "";
    while (net->nproxy) {
        struct in6_addr addr = net->proxy[net->nproxy - 1];
        char err[1024];
        if (fipsd_gw_net_remove_proxy_ndp(net, &addr, err, sizeof err) != 0) { failed++; snprintf(last, sizeof last, "%s", err); }
    }
    if (failed) FIPSD_WARN(MODULE, "Failed to remove proxy NDP entries at shutdown: failed=%zu error=%s", failed, last);
    if (net->route_added) {
        const char *argv[] = {"ip", "-6", "route", "del", "local", net->pool_cidr, "dev", "lo", NULL};
        char out[1024];
        int status = run(net, argv, out, sizeof out);
        trim(out);
        if (status == 0) FIPSD_INFO(MODULE, "Removed pool route: cidr=%s", net->pool_cidr);
        else if (status > 0) FIPSD_WARN(MODULE, "Failed to remove pool route: error=%s", out);
        else FIPSD_WARN(MODULE, "Failed to run ip route del: error=%s", out);
        net->route_added = false;
    }
}

/* A failure's text with the address's own text taken out. */
static void failure_text(const struct in6_addr *addr, const char *err, char *out, size_t cap)
{
    char a[48]; fipsd_gw_ip6_text(addr, a, sizeof a);
    size_t alen = strlen(a), at = 0;
    for (const char *p = err; *p && at + 1 < cap;) {
        if (alen && !strncmp(p, a, alen)) {
            const char *mark = "<addr>";
            for (const char *m = mark; *m && at + 1 < cap; ++m) out[at++] = *m;
            p += alen;
        } else out[at++] = *p++;
    }
    out[at] = 0;
}
fipsd_gw_retry_report fipsd_gw_ndp_observe(fipsd_gw_ndp_log *log, bool add, const struct in6_addr *addr, int rc, const char *err)
{
    char text[1024];
    if (rc == 0) return fipsd_gw_retry_log_observe(add ? &log->add : &log->remove, NULL);
    failure_text(addr, err ? err : "", text, sizeof text);
    return fipsd_gw_retry_log_observe(add ? &log->add : &log->remove, text);
}
void fipsd_gw_report_proxy_ndp(fipsd_gw_retry_report report, bool add, const struct in6_addr *addr, int rc, const char *err)
{
    char a[48]; fipsd_gw_ip6_text(addr, a, sizeof a);
    if (report == FIPSD_GW_RETRY_FAILED && rc) {
        if (add) FIPSD_ERROR(MODULE, "Failed to add proxy NDP: error=%s virtual_ip=%s", err, a);
        else FIPSD_WARN(MODULE, "Failed to remove proxy NDP: error=%s virtual_ip=%s", err, a);
    } else if (report == FIPSD_GW_RETRY_REPEATED && rc) {
        FIPSD_DEBUG(MODULE, "Failed to %s proxy NDP: error=%s virtual_ip=%s", add ? "add" : "remove", err, a);
    } else if (report == FIPSD_GW_RETRY_RECOVERED) {
        FIPSD_INFO(MODULE, "Proxy NDP %s recovered", add ? "add" : "removal");
    }
}
