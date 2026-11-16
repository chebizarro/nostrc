/* Opt-in privileged IPv6 shim smoke (nostrc-9nxwk.2): one node with a REAL
 * TUN device and the shim running on it, driven by the host's own IPv6 stack
 * through ping. Skips (77) unless geteuid()==0 and
 * FIPSD_TUN_PRIVILEGED_SMOKE=1. Proves on this host: the kernel routes
 * fd00::/8 into the device, the shim reads the kernel's packets and answers
 * an unknown mesh destination with ICMPv6 Destination Unreachable that the
 * kernel accepts (ping reports it), and the node's own address answers (via
 * lo on Linux, hairpinned through the shim on macOS utun). A two-node ping
 * across real TUNs needs two network namespaces: see fipsd/README.md. */
#define _POSIX_C_SOURCE 200809L
#include "ipv6/shim.h"
#include "tun/lifecycle.h"
#include "tun/status.h"
#include "node/context.h"
#include "node/handlers/handlers.h"
#include "transport/loopback.h"
#include "config/config.h"
#include <fips/proto/framing.h>
#include <fips/identity/address.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
static fipsd_network_snapshot *network(void *arg) { (void)arg; return calloc(1, sizeof(fipsd_network_snapshot)); }
static int ping(const char *address)
{
    pid_t child = fork(); assert(child >= 0);
    if (!child) {
#ifdef __linux__
        execlp("ping", "ping", "-6", "-c", "1", "-W", "3", address, (char *)NULL);
#else
        execl("/sbin/ping6", "ping6", "-c", "1", "-i", "1", address, (char *)NULL);
#endif
        _exit(127);
    }
    int result = 0; assert(waitpid(child, &result, 0) == child);
    return WIFEXITED(result) ? WEXITSTATUS(result) : -1;
}
int main(void)
{
    if (geteuid() != 0) { puts("SKIP: a real TUN and ping across it require root (geteuid()!=0); the fake-device two-node proof covers ordinary runs"); return 77; }
    const char *opt = getenv("FIPSD_TUN_PRIVILEGED_SMOKE");
    if (!opt || strcmp(opt, "1")) { puts("SKIP: privileged smoke also requires explicit FIPSD_TUN_PRIVILEGED_SMOKE=1"); return 77; }
    const char *yaml = "node: {identity: {persistent: false}, control: {enabled: false}}\ntun: {enabled: true, name: fipsSmoke0}\n";
    fipsd_config *config = fipsd_config_parse(yaml, strlen(yaml), NULL); assert(config);
    GoSwap *configs = go_swap_create(config, fipsd_config_free); assert(configs);
    fipsd_node_handlers_config hcfg; fipsd_node_handlers_config_default(&hcfg);
    fipsd_node_handlers *h = fipsd_node_handlers_create(&hcfg); assert(h);
    fipsd_netmon_source source = {.injected = true, .read = network};
    fipsd_node_options options = {.netmon_source = &source, .workers = 2};
    fipsd_node_handlers_install(h, &options);
    fipsd_node *node = fipsd_node_create(configs, &options); assert(node);
    fipsd_loopback_network *net = fipsd_loopback_network_create(); assert(net);
    fipsd_transport *transport = fipsd_loopback_transport_create(net, (fips_transport_id){7}, "loop", "loop:a", 1357); assert(transport);
    assert(!fipsd_transport_register(fipsd_node_transports(node), transport));
    fipsd_tun_config cfg = {.name = "fipsSmoke0", .mtu = 1280, .transport_mtu = 1357};
    fipsd_tun *tun = fipsd_tun_create(&cfg, NULL, NULL); assert(tun);
    fipsd_tun_service service; fipsd_tun_service_init(&service, tun, node);
    fipsd_ipv6_shim_config scfg = {.tun = tun, .handlers = h, .payload_limit = fips_framing_payload_limit(1357), .firewall_scope_check = true};
    fipsd_ipv6_shim *shim = fipsd_ipv6_shim_create(&scfg); assert(shim);
    fipsd_tun_service_set_shim(&service, shim);
    fipsd_runtime_config rc; fipsd_runtime_config_default(&rc); rc.workers = 2; rc.handle_signals = false;
    fipsd_runtime *rt = fipsd_runtime_create(&rc); assert(rt);
    assert(!fipsd_runtime_register(rt, fipsd_node_subsystem(node)) && !fipsd_runtime_register(rt, &service.subsystem));
    assert(!fipsd_runtime_start(rt));
    fipsd_tun_status status = fipsd_tun_status_read(tun);
    int failures = 0;
    if (status.state != FIPSD_TUN_UP) { fprintf(stderr, "TUN not up (state %d, error %s)\n", status.state, strerror(status.error)); failures = 1; }
    else {
        printf("TUN %s up: %s mtu %u\n", status.name, status.address, status.mtu);
        int self = ping(status.address);
        printf("ping own address: exit %d\n", self);
        if (self != 0) ++failures;
        /* An unknown fd00::/8 address: the kernel routes it into the TUN, the
         * shim answers Destination Unreachable, and ping fails promptly with it. */
        const char *unknown = "fd00::dead:beef";
        int stranger = ping(unknown);
        fipsd_ipv6_shim_counters c = fipsd_ipv6_shim_counters_read(shim);
        printf("ping %s: exit %d; shim read=%llu unreachable=%llu hairpinned=%llu written=%llu host_drops=%llu\n", unknown, stranger,
            (unsigned long long)c.read, (unsigned long long)c.unreachable, (unsigned long long)c.hairpinned,
            (unsigned long long)c.written, (unsigned long long)c.host_drops);
        if (stranger == 0 || c.read == 0 || c.unreachable == 0 || c.written == 0) ++failures;
#ifdef __APPLE__
        if (c.hairpinned == 0) { fputs("macOS utun did not hairpin the self-addressed ping\n", stderr); ++failures; }
#endif
        if (fipsd_tun_status_read(tun).host_drops != 0) ++failures;
    }
    fipsd_runtime_shutdown(rt); assert(fipsd_runtime_run(rt) == FIPSD_RUN_OK);
    fipsd_runtime_destroy(rt); fipsd_ipv6_shim_destroy(shim); fipsd_tun_destroy(tun); fipsd_node_destroy(node); fipsd_node_handlers_destroy(h);
    fipsd_loopback_transport_destroy(transport); fipsd_loopback_network_destroy(net); go_swap_free(configs);
    puts(failures ? "privileged IPv6 shim smoke: FAILED" : "privileged IPv6 shim smoke: real TUN, kernel-routed packets, ICMPv6 unreachable accepted by the kernel: ok");
    return failures ? 1 : 0;
}
