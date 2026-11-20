/* Opt-in privileged Windows TUN smoke (nostrc-iq1n9.6.2), the Windows
 * counterpart of test_tun_privileged.c and test_ipv6_privileged.c. Skips (77)
 * unless the process token is elevated AND FIPSD_TUN_PRIVILEGED_SMOKE=1; then
 * wintun.dll must sit beside this executable.
 *
 * `device`: a real Wintun adapter, twice in turn: the node's /128 (preferred
 * at once), the IPv6 MTU and the fd00::/8 route are read back through IP
 * Helper and through PowerShell (Get-NetIPAddress, Get-NetRoute,
 * Get-NetIPInterface, Get-NetAdapter), the address answers ping, the second
 * round's rows are removed behind the device's back before a stop that must
 * still succeed, and after stop the adapter is gone.
 * `shim`: one node with the IPv6 shim on a real adapter: ping of the node's
 * own address succeeds, and a ping of an fd00::/8 address no key names is
 * routed by Windows into the adapter, read by the shim and answered with an
 * ICMPv6 Destination Unreachable that Windows hands to ping. */
#include "ipv6/shim.h"
#include "tun/lifecycle.h"
#include "tun/status.h"
#include "tun/internal.h"
#include "tun/wintun.h"
#include "tun/winnet.h"
#include "node/context.h"
#include "node/handlers/handlers.h"
#include "transport/loopback.h"
#include "config/config.h"
#include "host_os.h"
#include <fips/proto/framing.h>
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define NAME "fipsSmoke0"
static bool elevated(void)
{
    HANDLE token = NULL; TOKEN_ELEVATION e = {0}; DWORD n = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    bool ok = GetTokenInformation(token, TokenElevation, &e, sizeof e, &n) && e.TokenIsElevated;
    CloseHandle(token);
    return ok;
}
/* Run a command line; its exit code, with stdout+stderr in out. */
static int run(const char *command, char *out, size_t cap)
{
    char line[1024];
    snprintf(line, sizeof line, "%s 2>&1", command);
    FILE *p = _popen(line, "r");
    if (!p) return -1;
    size_t n = 0;
    while (n + 1 < cap && fgets(out + n, (int)(cap - n), p)) n += strlen(out + n);
    out[n] = 0;
    return _pclose(p);
}
static bool powershell(const char *script, const char *want)
{
    char command[768], out[4096];
    snprintf(command, sizeof command, "powershell -NoProfile -NonInteractive -Command \"%s\"", script);
    int rc = run(command, out, sizeof out);
    char *end = out + strlen(out);
    while (end > out && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ')) *--end = 0;
    printf("  %s\n    -> %s\n", script, out);
    return rc == 0 && !strcmp(out, want);
}
static int ping(const char *address, char *out, size_t cap)
{
    char command[160];
    snprintf(command, sizeof command, "ping -6 -n 1 -w 3000 %s", address);
    int rc = run(command, out, cap);
    printf("  %s -> exit %d\n%s", command, rc, out);
    return rc;
}
/* After stop: no IP interface is left on the adapter at once (the rows, the
 * name probe and routing depend on that), and Plug and Play then removes the
 * interface itself. IP Helper has no notification for that last step, so
 * this test (only) waits for it, bounded, in 20 ms steps. */
static int removed(uint64_t luid)
{
    uint16_t wide[32]; bool in_use = true;
    assert(fipsd_winnet_wide(NAME, wide, 32));
    if (fipsd_winnet_alias_in_use(wide, &in_use) || in_use) { fputs("the stopped adapter is still in use\n", stderr); return 1; }
    uint32_t mtu = 0, code = luid ? fipsd_winnet_get_mtu(luid, &mtu) : ERROR_NOT_FOUND;
    if (code != ERROR_NOT_FOUND && code != ERROR_FILE_NOT_FOUND) { fputs("the stopped adapter kept its IPv6 interface\n", stderr); return 1; }
    uint64_t left = 0;
    for (unsigned waited = 0; !fipsd_winnet_alias_luid(wide, &left); waited += 20) {
        if (waited >= 5000) { fputs("the adapter's interface was not removed within 5 s of stop\n", stderr); return 1; }
        Sleep(20);
    }
    return 0;
}
static int device_round(fips_identity *id, unsigned round)
{
    fipsd_tun_config cfg = {.name = NAME, .mtu = 1280, .transport_mtu = 1357};
    fipsd_tun *t = fipsd_tun_create(&cfg, NULL, NULL); assert(t);
    int failures = 0;
    if (fipsd_tun_start(t, id)) { perror("TUN bring-up"); fipsd_tun_destroy(t); return 1; }
    fipsd_tun_status s = fipsd_tun_status_read(t);
    printf("round %u: %s up, index %u, %s/128, mtu %u, LUID %#llx\n", round, s.name, s.index, s.address, s.mtu,
        (unsigned long long)fipsd_tun_wintun_luid(t));
    uint64_t luid = fipsd_tun_wintun_luid(t);
    fipsd_winnet_address a; fipsd_winnet_route r; bool present = false; uint32_t mtu = 0;
    fips_ipv6_address our = fipsd_tun_address(t);
    assert(fipsd_winnet_node_address(luid, our.bytes, &a) && fipsd_winnet_mesh_route(luid, &r));
    if (fipsd_winnet_has_address(&a, &present) || !present) { fputs("IP Helper: no /128 on the adapter\n", stderr); ++failures; }
    if (fipsd_winnet_has_route(&r, &present) || !present) { fputs("IP Helper: no fd00::/8 route\n", stderr); ++failures; }
    if (fipsd_winnet_get_mtu(luid, &mtu) || mtu != 1280) { fprintf(stderr, "IP Helper: MTU %u\n", mtu); ++failures; }
    char script[512], want[64];
    /* Usable at once: no duplicate address detection on the TUN. */
    snprintf(script, sizeof script, "(Get-NetIPAddress -InterfaceAlias %s -AddressFamily IPv6 -IPAddress %s).AddressState", NAME, s.address);
    if (!powershell(script, "Preferred")) ++failures;
    snprintf(script, sizeof script, "$i = Get-NetIPInterface -InterfaceAlias %s -AddressFamily IPv6; "
        "\\\"$($i.DadTransmits) $($i.RouterDiscovery)\\\"", NAME);
    if (!powershell(script, "0 Disabled")) ++failures;
    snprintf(script, sizeof script, "(Get-NetIPAddress -InterfaceAlias %s -AddressFamily IPv6 -IPAddress %s).PrefixLength", NAME, s.address);
    if (!powershell(script, "128")) ++failures;
    snprintf(script, sizeof script, "(Get-NetRoute -InterfaceAlias %s -DestinationPrefix fd00::/8).RouteMetric", NAME);
    if (!powershell(script, "256")) ++failures;
    snprintf(script, sizeof script, "(Get-NetIPInterface -InterfaceAlias %s -AddressFamily IPv6).NlMtu", NAME);
    if (!powershell(script, "1280")) ++failures;
    snprintf(script, sizeof script, "(Get-NetAdapter -Name %s).InterfaceDescription", NAME);
    if (!powershell(script, "FIPS Tunnel")) ++failures;
    snprintf(want, sizeof want, "%u", s.index);
    snprintf(script, sizeof script, "(Get-NetAdapter -Name %s).ifIndex", NAME);
    if (!powershell(script, want)) ++failures;
    char out[2048];
    if (ping(s.address, out, sizeof out)) ++failures;
    if (round) {
        /* Someone removed the rows already: stop still succeeds. */
        snprintf(script, sizeof script, "Remove-NetIPAddress -InterfaceAlias %s -IPAddress %s -Confirm:$false; "
            "Remove-NetRoute -InterfaceAlias %s -DestinationPrefix fd00::/8 -Confirm:$false; 'removed'", NAME, s.address, NAME);
        if (!powershell(script, "removed")) ++failures;
    }
    if (fipsd_tun_stop(t)) { perror("TUN teardown"); ++failures; }
    if (fipsd_tun_status_read(t).error) { fprintf(stderr, "teardown error %d\n", fipsd_tun_status_read(t).error); ++failures; }
    if (removed(luid)) ++failures;
    snprintf(script, sizeof script, "@(Get-NetAdapter -Name %s -IncludeHidden -ErrorAction SilentlyContinue).Count", NAME);
    if (!powershell(script, "0")) ++failures;
    fipsd_tun_destroy(t);
    return failures;
}
static fipsd_network_snapshot *network(void *arg) { (void)arg; return calloc(1, sizeof(fipsd_network_snapshot)); }
static int shim_smoke(void)
{
    const char *yaml = "node: {identity: {persistent: false}, control: {enabled: false}}\ntun: {enabled: true, name: " NAME "}\n";
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
    fipsd_tun_config cfg = {.name = NAME, .mtu = 1280, .transport_mtu = 1357};
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
    uint64_t luid = fipsd_tun_wintun_luid(tun);
    int failures = 0;
    if (status.state != FIPSD_TUN_UP) { fprintf(stderr, "TUN not up (state %d, error %s)\n", status.state, strerror(status.error)); failures = 1; }
    else {
        char out[2048];
        printf("TUN %s up: %s mtu %u\n", status.name, status.address, status.mtu);
        if (ping(status.address, out, sizeof out)) ++failures; /* local delivery: the address is assigned and preferred */
        const char *unknown = "fd00::dead:beef";
        (void)ping(unknown, out, sizeof out); /* ping's exit code counts an Unreachable as a reply */
        fipsd_ipv6_shim_counters c = fipsd_ipv6_shim_counters_read(shim);
        fipsd_tun_wintun_stats w = fipsd_tun_wintun_stats_read(tun);
        printf("shim read=%llu unreachable=%llu written=%llu host_drops=%llu; wintun received=%llu dropped=%llu\n",
            (unsigned long long)c.read, (unsigned long long)c.unreachable, (unsigned long long)c.written,
            (unsigned long long)c.host_drops, (unsigned long long)w.received, (unsigned long long)w.dropped);
        if (c.read == 0 || c.unreachable == 0 || c.written == 0 || w.received == 0) ++failures;
        if (!strstr(out, "unreachable")) { fputs("ping did not report the shim's Destination Unreachable\n", stderr); ++failures; }
        if (fipsd_tun_status_read(tun).host_drops != 0) ++failures;
    }
    fipsd_runtime_shutdown(rt); assert(fipsd_runtime_run(rt) == FIPSD_RUN_OK);
    if (removed(luid)) ++failures;
    fipsd_runtime_destroy(rt); fipsd_ipv6_shim_destroy(shim); fipsd_tun_destroy(tun); fipsd_node_destroy(node);
    fipsd_node_handlers_destroy(h); fipsd_loopback_transport_destroy(transport); fipsd_loopback_network_destroy(net);
    go_swap_free(configs);
    return failures;
}
int main(int argc, char **argv)
{
    bool shim = argc > 1 && !strcmp(argv[1], "shim");
    if (!elevated()) {
        puts("SKIP: a Wintun adapter needs an elevated (Administrator) token; the fake-session test covers ordinary runs");
        return 77;
    }
    const char *opt = getenv("FIPSD_TUN_PRIVILEGED_SMOKE");
    if (!opt || strcmp(opt, "1")) { puts("SKIP: privileged smoke also requires explicit FIPSD_TUN_PRIVILEGED_SMOKE=1"); return 77; }
    int failures = 0;
    if (shim) failures = shim_smoke();
    else {
        fips_secp *secp = NULL; fips_identity *id = NULL;
        assert(fips_identity_init_secp(fipsd_host_os_entropy(), &secp) == FIPS_IDENTITY_OK);
        for (unsigned round = 0; round < 2; ++round) {
            assert(fips_identity_generate(secp, fipsd_host_os_entropy(), &id) == FIPS_IDENTITY_OK);
            failures += device_round(id, round);
            fips_identity_destroy(id);
        }
        fips_identity_destroy_secp(secp);
    }
    printf("privileged Windows %s smoke: %s\n", shim ? "IPv6 shim" : "TUN device", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
