/* Gateway network setup tests (nostrc-iq1n9.3), from upstream
 * src/gateway/net.rs and tests/net.rs: proxy NDP is read from all/ or the
 * LAN interface, proxy NDP failures latch per direction with the address
 * taken out, and the `ip` commands run through an injected runner. */
#include "test_gateway.h"
#include "gateway/net.h"
#include <errno.h>
#include <sys/stat.h>

static void write_file(const char *path, const char *text) { FILE *f = fopen(path, "w"); CHECK(f); fputs(text, f); fclose(f); }
static void proxy_ndp_is_on_when_all_or_the_lan_interface_sets_it(void)
{
    char conf[] = "/tmp/fipsgw-conf-XXXXXX"; CHECK(mkdtemp(conf));
    char all[128], lan[128], p[256];
    snprintf(all, sizeof all, "%s/all", conf); snprintf(lan, sizeof lan, "%s/lan0", conf);
    CHECKM(fipsd_gw_proxy_on(conf, "lan0") == -1, "neither file reads");
    CHECK(mkdir(all, 0755) == 0 && mkdir(lan, 0755) == 0);
    snprintf(p, sizeof p, "%s/proxy_ndp", all); write_file(p, "0\n");
    snprintf(p, sizeof p, "%s/proxy_ndp", lan); write_file(p, "0\n");
    CHECK(fipsd_gw_proxy_on(conf, "lan0") == 0);
    write_file(p, "1\n");
    CHECKM(fipsd_gw_proxy_on(conf, "lan0") == 1, "the interface's own setting");
    write_file(p, "0\n");
    snprintf(p, sizeof p, "%s/proxy_ndp", all); write_file(p, "1\n");
    CHECKM(fipsd_gw_proxy_on(conf, "lan0") == 1, "all/ covers every interface");
    CHECKM(fipsd_gw_proxy_on(conf, "missing0") == 1, "all/ alone");
    write_file(p, "0\n");
    CHECKM(fipsd_gw_proxy_on(conf, "missing0") == 0, "all/ off and the interface unknown");
    unlink(p); snprintf(p, sizeof p, "%s/proxy_ndp", lan); unlink(p); rmdir(all); rmdir(lan); rmdir(conf);
}
static void ipv6_forwarding_is_read_from_the_sysctl(void)
{
    char path[] = "/tmp/fipsgw-fwd-XXXXXX"; int fd = mkstemp(path); CHECK(fd >= 0); close(fd);
    write_file(path, "1\n"); CHECK(fipsd_gw_ipv6_forwarding(path) == 1);
    write_file(path, "0\n"); CHECK(fipsd_gw_ipv6_forwarding(path) == 0);
    unlink(path); CHECK(fipsd_gw_ipv6_forwarding(path) == -1);
}
static void proxy_ndp_failures_log_once_per_change_of_outcome(void)
{
    fipsd_gw_ndp_log log = {0};
    struct in6_addr a = gw_v6("fd01::1"), b = gw_v6("fd01::2");
    CHECK(fipsd_gw_ndp_observe(&log, true, &a, -1, "Failed to add proxy NDP: RTNETLINK answers: Operation not permitted for fd01::1") == FIPSD_GW_RETRY_FAILED);
    CHECKM(fipsd_gw_ndp_observe(&log, true, &b, -1, "Failed to add proxy NDP: RTNETLINK answers: Operation not permitted for fd01::2") == FIPSD_GW_RETRY_REPEATED,
           "an error naming its address counts as the same error for every address");
    CHECKM(fipsd_gw_ndp_observe(&log, false, &a, -1, "Failed to remove proxy NDP: x") == FIPSD_GW_RETRY_FAILED, "the directions latch apart");
    CHECK(fipsd_gw_ndp_observe(&log, true, &a, 0, NULL) == FIPSD_GW_RETRY_RECOVERED);
    CHECK(fipsd_gw_ndp_observe(&log, true, &a, 0, NULL) == FIPSD_GW_RETRY_CLEAN);
    CHECK(fipsd_gw_ndp_observe(&log, false, &a, -1, "Failed to remove proxy NDP: x") == FIPSD_GW_RETRY_REPEATED);
}

/* A runner that records argv lines and answers from a script. */
typedef struct runner { char lines[32][256]; size_t n; int status; const char *stderr_text; int spawn_errno; } runner;
static int run(void *user, const char *const *argv, char *err, size_t cap)
{
    runner *r = user;
    CHECK(r->n < 32);
    size_t at = 0;
    for (const char *const *a = argv; *a; ++a) at += (size_t)snprintf(r->lines[r->n] + at, sizeof r->lines[0] - at, "%s%s", a == argv ? "" : " ", *a);
    r->n++;
    if (r->spawn_errno) { errno = r->spawn_errno; return -1; }
    snprintf(err, cap, "%s", r->stderr_text ? r->stderr_text : "");
    return r->status;
}
static void the_ip_commands_are_upstreams_and_benign_failures_pass(void)
{
    runner r = {.n = 0};
    fipsd_gw_net net; fipsd_gw_net_init(&net, "br-lan", "fd01::/112", run, &r);
    char err[512];
    struct in6_addr a = gw_v6("fd01::5");
    CHECK(fipsd_gw_net_add_pool_route(&net, err, sizeof err) == 0 && net.route_added);
    CHECK(fipsd_gw_net_add_proxy_ndp(&net, &a, err, sizeof err) == 0 && net.nproxy == 1);
    CHECK(!strcmp(r.lines[0], "ip -6 route add local fd01::/112 dev lo"));
    CHECK(!strcmp(r.lines[1], "ip -6 neigh add proxy fd01::5 dev br-lan"));
    /* An existing entry or route is not an error, and is not ours to remove. */
    r.status = 2; r.stderr_text = "RTNETLINK answers: File exists\n";
    fipsd_gw_net other; fipsd_gw_net_init(&other, "br-lan", "fd01::/112", run, &r);
    CHECK(fipsd_gw_net_add_pool_route(&other, err, sizeof err) == 0 && !other.route_added);
    CHECK(fipsd_gw_net_add_proxy_ndp(&other, &a, err, sizeof err) == 0 && other.nproxy == 0);
    /* A real failure is reported with its stderr. */
    r.stderr_text = "RTNETLINK answers: Operation not permitted\n";
    CHECK(fipsd_gw_net_add_proxy_ndp(&other, &a, err, sizeof err) != 0);
    CHECKM(!strcmp(err, "Failed to add proxy NDP: RTNETLINK answers: Operation not permitted"), "%s", err);
    CHECK(fipsd_gw_net_add_pool_route(&other, err, sizeof err) != 0 && !strcmp(err, "Failed to add pool route: RTNETLINK answers: Operation not permitted"));
    /* An entry already gone is a removal. */
    r.stderr_text = "RTNETLINK answers: No such file or directory\n";
    CHECK(fipsd_gw_net_remove_proxy_ndp(&net, &a, err, sizeof err) == 0 && net.nproxy == 0);
    r.stderr_text = "RTNETLINK answers: Operation not permitted\n";
    CHECK(fipsd_gw_net_remove_proxy_ndp(&net, &a, err, sizeof err) != 0 && !strcmp(err, "Failed to remove proxy NDP: RTNETLINK answers: Operation not permitted"));
    /* An `ip` that cannot run at all. */
    r.spawn_errno = ENOENT;
    CHECK(fipsd_gw_net_add_proxy_ndp(&other, &a, err, sizeof err) != 0 && strstr(err, strerror(ENOENT)));
    fipsd_gw_net_clear(&net); fipsd_gw_net_clear(&other);
}
static void cleanup_removes_only_what_this_run_added(void)
{
    runner r = {.n = 0};
    fipsd_gw_net net; fipsd_gw_net_init(&net, "br-lan", "fd01::/112", run, &r);
    char err[512];
    struct in6_addr a = gw_v6("fd01::5"), b = gw_v6("fd01::6");
    CHECK(fipsd_gw_net_add_pool_route(&net, err, sizeof err) == 0);
    CHECK(fipsd_gw_net_add_proxy_ndp(&net, &a, err, sizeof err) == 0 && fipsd_gw_net_add_proxy_ndp(&net, &b, err, sizeof err) == 0);
    r.n = 0;
    fipsd_gw_net_cleanup(&net);
    CHECK(r.n == 3);
    CHECK(!strcmp(r.lines[0], "ip -6 neigh del proxy fd01::6 dev br-lan") && !strcmp(r.lines[1], "ip -6 neigh del proxy fd01::5 dev br-lan"));
    CHECK(!strcmp(r.lines[2], "ip -6 route del local fd01::/112 dev lo"));
    CHECK(!net.route_added && net.nproxy == 0);
    r.n = 0;
    fipsd_gw_net_cleanup(&net);
    CHECKM(r.n == 0, "a second cleanup has nothing left");
    fipsd_gw_net_clear(&net);
}
static void the_spawn_runner_reports_status_and_stderr(void)
{
    const char *ok[] = {"sh", "-c", "exit 0", NULL}, *bad[] = {"sh", "-c", "echo oops >&2; exit 3", NULL}, *none[] = {"/nonexistent/fips-ip", NULL};
    char err[128];
    CHECK(fipsd_gw_cmd_spawn(NULL, ok, err, sizeof err) == 0);
    CHECK(fipsd_gw_cmd_spawn(NULL, bad, err, sizeof err) == 3 && !strcmp(err, "oops\n"));
    CHECK(fipsd_gw_cmd_spawn(NULL, none, err, sizeof err) == -1 || fipsd_gw_cmd_spawn(NULL, none, err, sizeof err) == 127);
}
static void the_interface_wait_returns_at_once_for_an_existing_link(void)
{
#ifdef __APPLE__
    const char *lo = "lo0";
#else
    const char *lo = "lo";
#endif
    CHECK(fipsd_gw_iface_index(lo) != 0);
    CHECK(fipsd_gw_wait_interface(lo, 1000, -1) == 0);
    CHECK(fipsd_gw_iface_index("fipsgw-absent9") == 0);
#ifdef __linux__
    /* A cancel descriptor already readable ends the wait for a missing link. */
    int p[2]; CHECK(pipe(p) == 0); CHECK(write(p[1], "x", 1) == 1);
    CHECK(fipsd_gw_wait_interface("fipsgw-absent9", 10000, p[0]) == ECANCELED);
    close(p[0]); close(p[1]);
#endif
}

int gw_suite_net(void)
{
    RUN(proxy_ndp_is_on_when_all_or_the_lan_interface_sets_it);
    RUN(ipv6_forwarding_is_read_from_the_sysctl);
    RUN(proxy_ndp_failures_log_once_per_change_of_outcome);
    RUN(the_ip_commands_are_upstreams_and_benign_failures_pass);
    RUN(cleanup_removes_only_what_this_run_added);
    RUN(the_spawn_runner_reports_status_and_stderr);
    RUN(the_interface_wait_returns_at_once_for_an_existing_link);
    return 0;
}
