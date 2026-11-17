#define _POSIX_C_SOURCE 200809L
#include <fips/core.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "cli.h"
#include "config/config.h"
#include "control/control.h"
#include "host_os.h"
#include "log.h"
#include "privilege.h"
#include "runtime.h"
#include "node/context.h"
#include "node/handlers/handlers.h"
#include "tun/lifecycle.h"
#include "tun/status.h"
#include "ipv6/shim.h"
#include <fips/proto/framing.h>
#include "native/api.h"
#include "dns/service.h"
#include "transport/udp/udp.h"
#include "transport/tcp/tcp.h"
#include "transport/tor/tor.h"
#include "transport/nym/nym.h"
#include "transport/ble/ble.h"
#include "nostr/runtime.h"
#include "mdns/mdns.h"

#define MODULE "main"

/* Open --log-file before parsing so startup failures reach the requested sink. */
static fipsd_log *open_logger(const fipsd_options *opts, fipsd_log_file **out_file)
{
    fipsd_log_level level = FIPSD_LOG_INFO;
    const char *env_level = getenv("FIPSD_LOG_LEVEL");
    if (env_level && *env_level && !fipsd_log_level_parse(env_level, &level)) {
        fprintf(stderr, "fipsd: ignoring FIPSD_LOG_LEVEL=%s: expected error, warn, info, debug or trace\n",
                env_level);
        level = FIPSD_LOG_INFO;
    }
    fipsd_log *log;
    *out_file = NULL;
    if (opts->log_file) {
        char err[512];
        fipsd_log_file *file = fipsd_log_file_open(opts->log_file, FIPSD_LOG_DEFAULT_MAX_BYTES,
                                                   FIPSD_LOG_DEFAULT_MAX_FILES, err, sizeof(err));
        if (!file) {
            /* Fatal: a daemon that silently ran without the log it was told
             * to keep would present as exactly the missing-logs problem the
             * file exists to solve. */
            fprintf(stderr, "fipsd: %s\n", err);
            return NULL;
        }
        fipsd_log_sink sink = fipsd_log_file_sink(file);
        log = fipsd_log_create(level, &sink);
        if (!log) {
            fipsd_log_file_close(file);
            fputs("fipsd: cannot allocate the logger\n", stderr);
            return NULL;
        }
        *out_file = file;
        fipsd_log_set_default(log);
        /* Logged first: an operator looking for output that is no longer on
         * stderr needs the path and the limits that govern it. */
        FIPSD_INFO(MODULE, "logging to file %s (max %" PRIu64 " bytes, %u rolled files kept)",
                   fipsd_log_file_path(file), fipsd_log_file_max_bytes(file),
                   fipsd_log_file_max_files(file));
    } else {
        log = fipsd_log_create(level, NULL);
        if (!log) {
            fputs("fipsd: cannot allocate the logger\n", stderr);
            return NULL;
        }
        fipsd_log_set_default(log);
    }
    return log;
}

int main(int argc, char **argv)
{
    fipsd_options opts;
    fipsd_cli_action action = fipsd_cli_parse(argc, argv, &opts, stderr);
    switch (action) {
    case FIPSD_CLI_HELP:
        fipsd_cli_print_help(stdout);
        return fipsd_cli_exit_code(action);
    case FIPSD_CLI_VERSION_SHORT:
    case FIPSD_CLI_VERSION_LONG:
        fipsd_cli_print_version(stdout, action == FIPSD_CLI_VERSION_LONG);
        return fipsd_cli_exit_code(action);
    case FIPSD_CLI_USAGE_ERROR:
        return fipsd_cli_exit_code(action);
    case FIPSD_CLI_RUN:
        break;
    }

    /* The log file is opened before anything else so that every later
     * start-up error, including a configuration error, is logged there. */
    fipsd_log_file *log_file = NULL;
    fipsd_log *log = open_logger(&opts, &log_file);
    if (!log)
        return FIPSD_EXIT_FAILURE;
    FIPSD_INFO(MODULE, "fipsd %s starting (libfips-core %s)", fipsd_cli_short_version(),
               fips_core_version());

    mode_t previous_umask = fipsd_privilege_set_umask();
    FIPSD_DEBUG(MODULE, "umask %04o (was %04o)", (unsigned)FIPSD_UMASK, (unsigned)previous_umask);

    fipsd_config_log held = {0};
    fipsd_config *config = fipsd_config_load(opts.config_path, &held);
    if (!config) {
        fipsd_config_log_replay(&held, log);
        FIPSD_ERROR(MODULE, "Failed to load configuration%s%s: %s",
                    opts.config_path ? " from " : "", opts.config_path ? opts.config_path : "", held.error);
        fipsd_config_log_clear(&held);
        fipsd_log_set_default(NULL);
        fipsd_log_destroy(log);
        return FIPSD_EXIT_FAILURE;
    }
    const char *configured_file = fipsd_config_string(fipsd_config_get(config, "node.log_file"));
    if (!opts.log_file && configured_file) {
        fipsd_options configured_opts = opts;
        configured_opts.log_file = configured_file;
        fipsd_log_set_default(NULL);
        fipsd_log_destroy(log);
        log = open_logger(&configured_opts, &log_file);
        if (!log) {
            fipsd_config_free(config);
            fipsd_config_log_clear(&held);
            return FIPSD_EXIT_FAILURE;
        }
    }
    fipsd_config_apply_logging(config, log, log_file);
    fipsd_config_log_replay(&held, log);
    fipsd_config_log_clear(&held);
    FIPSD_INFO(MODULE, "configuration loaded");
    if (!fipsd_config_enable_acl(config, NULL)) {
        FIPSD_ERROR(MODULE, "cannot allocate ACL snapshot");
        fipsd_config_free(config);
        fipsd_log_set_default(NULL); fipsd_log_destroy(log);
        if (log_file) fipsd_log_file_close(log_file);
        return FIPSD_EXIT_FAILURE;
    }
    GoSwap *snapshots = go_swap_create(config, fipsd_config_free);
    if (!snapshots) {
        fipsd_config_free(config);
        FIPSD_ERROR(MODULE, "cannot allocate configuration snapshot");
        fipsd_log_set_default(NULL);
        fipsd_log_destroy(log);
        return FIPSD_EXIT_FAILURE;
    }
    fipsd_config_service service;
    fipsd_config_service_init(&service, snapshots, opts.config_path, log, log_file);

    /* Protocol handlers run on the node's dispatcher fiber (nostrc-9keoz.6);
     * the control provider reads their published snapshots. */
    fipsd_node_handlers_config handlers_config;
    fipsd_node_handlers_config_default(&handlers_config);
    fipsd_node_handlers *handlers = fipsd_node_handlers_create(&handlers_config);
    fipsd_node_options node_options = {0};
    if (handlers) fipsd_node_handlers_install(handlers, &node_options);
    fipsd_node *node = handlers ? fipsd_node_create(snapshots, &node_options) : NULL;
    fipsd_transport *udp[FIPSD_TRANSPORT_MAX] = {0};
    size_t udp_count = 0;
    bool udp_ok = node && fipsd_udp_register_config(fipsd_node_transports(node), config, udp, &udp_count) == 0;
    fipsd_transport *tcp[FIPSD_TRANSPORT_MAX] = {0};
    size_t tcp_count = 0;
    bool tcp_ok = udp_ok && fipsd_tcp_register_config(fipsd_node_transports(node), config, tcp, &tcp_count) == 0;
    /* Proxied stream owners (nostrc-gfi8h.4): Tor always, Nym when built in. */
    fipsd_transport *tor[FIPSD_TRANSPORT_MAX] = {0}, *nym[FIPSD_TRANSPORT_MAX] = {0};
    size_t tor_count = 0, nym_count = 0;
    bool tor_ok = tcp_ok && fipsd_tor_register_config(fipsd_node_transports(node), config, tor, &tor_count) == 0;
    bool nym_ok = tor_ok && fipsd_nym_register_config(fipsd_node_transports(node), config, nym, &nym_count) == 0;
    /* BLE (nostrc-gfi8h.6.1) over this build's platform backend; without one a
     * configured instance is ignored with a warning, as upstream. */
    fipsd_transport *ble[FIPSD_TRANSPORT_MAX] = {0};
    size_t ble_count = 0;
    bool ble_ok = nym_ok && fipsd_ble_register_config(fipsd_node_transports(node), config, ble, &ble_count) == 0;
    /* Asking for the subsystem seals the transport registry. */
    fipsd_subsystem *node_subsystem = udp_ok ? fipsd_node_subsystem(node) : NULL;
    fipsd_control_service control;
    fipsd_control_provider provider = handlers ? fipsd_node_handlers_provider(handlers) : (fipsd_control_provider){0};
    bool tun_enabled = fipsd_config_bool(fipsd_config_get(config, "tun.enabled"));
    fipsd_tun_config tun_config = {
        .name = fipsd_config_string(fipsd_config_get(config, "tun.name")),
        .mtu = (uint16_t)fipsd_config_uint(fipsd_config_get(config, "tun.mtu")),
        .transport_mtu = fipsd_tun_transport_mtu(config)};
    fipsd_tun *tun = tun_enabled ? fipsd_tun_create(&tun_config, NULL, NULL) : NULL;
    fipsd_tun_service tun_service;
    fipsd_ipv6_shim *shim = NULL;
    if (tun) {
        fipsd_tun_service_init(&tun_service, tun, node);
        provider = fipsd_tun_provider(tun, provider);
        /* The packet adapter between the device's frames and the node's
         * sessions (nostrc-9nxwk.2); seeded with the configured transport
         * budget, the device's own MTU input. */
        uint32_t budget = tun_config.transport_mtu > UINT16_MAX ? UINT16_MAX : tun_config.transport_mtu;
        fipsd_ipv6_shim_config shim_config = {.tun = tun, .handlers = handlers,
            .payload_limit = fips_framing_payload_limit((uint16_t)budget),
            .host_queue = (size_t)fipsd_config_uint(fipsd_config_get(config, "node.buffers.host_channel")),
            .firewall_scope_check = true};
        shim = fipsd_ipv6_shim_create(&shim_config);
        if (shim) fipsd_tun_service_set_shim(&tun_service, shim);
    }
    /* Native datagram API (nostrc-9nxwk.4): off by default; layers its
     * flows and listeners onto the control snapshot for show_native_flows. */
    bool native_enabled = fipsd_config_bool(fipsd_config_get(config, "node.native_api.enabled"));
    fipsd_native_api *native = native_enabled && node && handlers ? fipsd_native_api_create(snapshots, node, handlers) : NULL;
    if (native) provider = fipsd_native_api_provider(native, provider);
    /* The .fips DNS responder (nostrc-9nxwk.3): on by default; it reads the
     * TUN's mesh side at start, so it registers after the TUN service. */
    bool dns_enabled = fipsd_config_bool(fipsd_config_get(config, "dns.enabled"));
    fipsd_dns_service *dns = dns_enabled && handlers ? fipsd_dns_service_create(snapshots, handlers, tun) : NULL;
    fipsd_control_service_init(&control, snapshots, handlers ? &provider : NULL);
    bool control_enabled = fipsd_config_bool(fipsd_config_get(config, "node.control.enabled"));
    bool nostr_enabled = fipsd_config_bool(fipsd_config_get(config, "node.rendezvous.nostr.enabled"));
    fipsd_nostr_node_driver nostr_driver;
    fipsd_nostr_runtime *rendezvous = nostr_enabled && node ? fipsd_nostr_runtime_create(config, NULL,
        fipsd_node_get_context(node), fipsd_nostr_node_driver_init(&nostr_driver,
            fipsd_node_get_context(node), handlers, fipsd_node_transports(node))) : NULL;
    /* mDNS LAN rendezvous (nostrc-9nxwk.5): off by default, as upstream. */
    bool lan_enabled = fipsd_config_bool(fipsd_config_get(config, "node.rendezvous.lan.enabled"));
    fipsd_mdns_service *mdns = lan_enabled && node && handlers ? fipsd_mdns_service_create(snapshots, node, handlers) : NULL;


    /* Host adapters for the core (time, entropy) are linked and ready for the
     * node subsystem; nothing reads them in the skeleton. */
    fips_clock clock = fipsd_host_os_clock();
    fips_entropy entropy = fipsd_host_os_entropy();
    (void)clock;
    (void)entropy;

    int code = FIPSD_EXIT_OK;
    fipsd_runtime_config cfg;
    fipsd_runtime_config_default(&cfg);
    fipsd_runtime *rt = fipsd_runtime_create(&cfg);
    if (!rt) {
        FIPSD_ERROR(MODULE, "cannot allocate the runtime");
        code = FIPSD_EXIT_FAILURE;
    } else if (!node_subsystem || !udp_ok || !tcp_ok || !nym_ok || !ble_ok || (tun_enabled && !tun) || (tun && !shim) ||
               fipsd_runtime_register(rt, &service.subsystem) != 0 ||
               fipsd_runtime_register(rt, node_subsystem) != 0 ||
               (tun && fipsd_runtime_register(rt, &tun_service.subsystem) != 0) ||
               (dns_enabled && (!dns || fipsd_runtime_register(rt, fipsd_dns_service_subsystem(dns)) != 0)) ||
               (native_enabled && (!native || fipsd_runtime_register(rt, fipsd_native_api_subsystem(native)) != 0)) ||
               (nostr_enabled && (!rendezvous || fipsd_runtime_register(rt, fipsd_nostr_runtime_subsystem(rendezvous)) != 0)) ||
               (lan_enabled && (!mdns || fipsd_runtime_register(rt, fipsd_mdns_service_subsystem(mdns)) != 0)) ||
               (control_enabled && fipsd_runtime_register(rt, &control.subsystem) != 0) ||
               fipsd_runtime_start(rt) != 0) {
        FIPSD_ERROR(MODULE, "start-up failed");
        code = FIPSD_EXIT_FAILURE;
    } else {
        GoRefPtr node_status = fipsd_node_status_load(fipsd_node_status_slot(node));
        const fipsd_node_status *state = node_status.ptr;
        if (!state || !state->ready)
            FIPSD_WARN(MODULE, "node not ready (state=%d); waiting for configured transports; host remains available",
                       state ? (int)state->state : -1);
        if (fipsd_node_handlers_dial_configured(handlers) != 0)
            FIPSD_WARN(MODULE, "configured peers could not be queued for dialing");
        go_refptr_release(&node_status);
        FIPSD_INFO(MODULE, "fipsd running");
        int status = fipsd_runtime_run(rt);
        if (status != FIPSD_RUN_OK) {
            FIPSD_ERROR(MODULE, "shutdown did not complete within the grace period");
            code = FIPSD_EXIT_FAILURE;
        }
    }
    fipsd_runtime_destroy(rt);
    fipsd_ipv6_shim_destroy(shim);
    fipsd_native_api_destroy(native);
    fipsd_dns_service_destroy(dns);
    fipsd_tun_destroy(tun);
    fipsd_nostr_runtime_destroy(rendezvous);
    fipsd_mdns_service_destroy(mdns);
    fipsd_node_destroy(node);
    fipsd_node_handlers_destroy(handlers);
    for (size_t i = 0; i < udp_count; ++i) fipsd_udp_destroy(udp[i]);
    for (size_t i = 0; i < tcp_count; ++i) fipsd_tcp_destroy(tcp[i]);
    for (size_t i = 0; i < tor_count; ++i) fipsd_tor_destroy(tor[i]);
    for (size_t i = 0; i < nym_count; ++i) fipsd_nym_destroy(nym[i]);
    for (size_t i = 0; i < ble_count; ++i) fipsd_ble_destroy(ble[i]);
    fipsd_control_service_clear(&control);
    go_swap_free(snapshots);
    FIPSD_INFO(MODULE, "fipsd shutdown complete (exit %d)", code);
    fipsd_log_set_default(NULL);
    fipsd_log_destroy(log);
    return code;
}
