/* fips-gateway: the FIPS outbound LAN gateway, the port of upstream
 * src/bin/fips-gateway.rs (nostrc-iq1n9.3). Lets unmodified LAN hosts reach
 * mesh destinations through DNS-allocated virtual IPs and kernel nftables
 * NAT, and exposes LAN services to the mesh through inbound port forwards.
 * Linux only: the build adds this target on Linux alone. */
#define _GNU_SOURCE 1
#include "fiber_errno.h"
#include "config/config.h"
#include "gateway/conntrack.h"
#include "gateway/control.h"
#include "gateway/dns.h"
#include "gateway/nat.h"
#include "gateway/net.h"
#include "gateway/pool.h"
#include "gateway/shutdown.h"
#include "gateway/state.h"
#include "log.h"
#include "privilege.h"
#include "runtime.h"
#include "fips-gateway-version.h"
#include "fipsd_build_info.h"
#include <blocking_executor.h>
#include <go.h>
#include <libgo/fiber.h>
#include <select.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MODULE "gateway"
#define EXIT_OK 0
#define EXIT_FAIL 1
#define EXIT_USAGE 2
/* How long to wait for the daemon's TUN, the upstream probe's attempts and
 * the pool tick interval. */
#define TUN_WAIT_MS 30000u
#define MAX_PROBE_ATTEMPTS 5u
#define PROBE_TIMEOUT_MS 1000u
#define PROBE_RETRY_DELAY_MS 1000u
#define TICK_INTERVAL_MS 10000u

static const char *short_version(void)
{
    return FIPSD_GIT_REV[0] ? FIPS_GATEWAY_VERSION " (rev " FIPSD_GIT_REV ")" : FIPS_GATEWAY_VERSION;
}
static void print_help(FILE *out)
{
    fputs("FIPS outbound LAN gateway\n\n"
          "Usage: fips-gateway [OPTIONS]\n\n"
          "Options:\n"
          "  -c, --config <FILE>      Path to configuration file (overrides default search paths)\n"
          "  -l, --log-level <LEVEL>  Log level (trace, debug, info, warn, error) [default: info]\n"
          "                           FIPSD_LOG_LEVEL, or RUST_LOG holding a bare level, overrides it.\n"
          "  -V                       Print the short version\n"
          "      --version            Print the long version: short version plus build target triple\n"
          "  -h, --help               Print help\n", out);
}

/* ---- start-up signals ---------------------------------------------------- */
/* Registered before set-up: until the runtime's handlers take over, a stop
 * is held here, so the start-up waits end at once. */
static int startup_pipe[2] = {-1, -1};
static volatile sig_atomic_t startup_signal;
static void on_startup_signal(int sig)
{
    startup_signal = sig;
    if (startup_pipe[1] >= 0) { ssize_t n = write(startup_pipe[1], "x", 1); (void)n; }
}
static void install_startup_signals(void)
{
    if (pipe(startup_pipe) == 0) {
        for (int i = 0; i < 2; ++i) (void)fcntl(startup_pipe[i], F_SETFD, FD_CLOEXEC);
        (void)fcntl(startup_pipe[1], F_SETFL, O_NONBLOCK);
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_startup_signal;
    sigemptyset(&sa.sa_mask);
    (void)sigaction(SIGTERM, &sa, NULL);
    (void)sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
}
static int startup_exit(void)
{
    FIPSD_INFO(MODULE, "Received %s during start-up, exiting", startup_signal == SIGINT ? "SIGINT" : "SIGTERM");
    return EXIT_OK;
}
/* Wait ms or until a stop arrives; true when stopped. */
static bool startup_sleep(unsigned ms)
{
    struct pollfd p = {.fd = startup_pipe[0], .events = POLLIN};
    return startup_signal || (startup_pipe[0] >= 0 && poll(&p, 1, (int)ms) > 0);
}

/* ---- the gateway --------------------------------------------------------- */
typedef struct gateway {
    fipsd_runtime *rt;
    char pool_cidr[64], lan[64], tun[64], dns_listen[128], dns_upstream[128];
    uint32_t ttl; uint64_t grace;
    fipsd_gw_shared_pool pool;
    fipsd_gw_nat_driver *nat;
    GoChannel *events, *nat_exit, *dns_done;
    fipsd_gw_dns_resolver resolver;
    fipsd_gw_dns_server dns;
    fipsd_gw_control control; bool control_bound;
    fipsd_gw_snapshot_slot snapshots;
    fipsd_gw_snapshot_config snap_config;
    fipsd_gw_net net; fipsd_gw_ndp_log ndp_log;
    fipsd_gw_ct_reader conntrack; fipsd_gw_ct_read_log ct_log;
    fipsd_gw_save_log save_log;
    fipsd_gw_task resolver_task, tick_task;
    fipsd_gw_instant start_time;
    atomic_int exit_code;
    fipsd_subsystem subsystem;
    fipsd_gw_stopped stopped;
    char dns_cause[256];
} gateway;

static uint64_t mono_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}
static int emit(void *user, const fipsd_gw_pool_event *e)
{
    gateway *g = user;
    fipsd_gw_pool_event *copy = malloc(sizeof *copy);
    if (!copy) return -1;
    *copy = *e;
    if (go_channel_send(g->events, copy) != 0) { free(copy); return -1; }
    return 0;
}
static void fail(gateway *g, const char *what)
{
    FIPSD_ERROR(MODULE, "%s; exiting so the service manager restarts the gateway", what);
    atomic_store(&g->exit_code, EXIT_FAIL);
    fipsd_runtime_shutdown(g->rt);
}

/* Blocking work, run off the fibers. */
typedef struct ct_read { gateway *g; fipsd_gw_ct_snapshot snapshot; } ct_read;
static void *read_conntrack(void *arg)
{
    ct_read *r = arg;
    fipsd_gw_ct_read_logged(&r->g->conntrack, &r->g->ct_log, &r->snapshot);
    return NULL;
}
typedef struct save_job { const fipsd_gw_pool_state *state; int rc; char err[256]; } save_job;
static void *save_state(void *arg)
{
    save_job *j = arg;
    j->rc = fipsd_gw_state_save(FIPSD_GW_STATE_DIR, j->state, fipsd_gw_is_tmpfs, NULL, j->err, sizeof j->err);
    return NULL;
}
typedef struct ndp_job { gateway *g; bool add; struct in6_addr addr; int rc; char err[1024]; } ndp_job;
static void *proxy_ndp(void *arg)
{
    ndp_job *j = arg;
    j->rc = j->add ? fipsd_gw_net_add_proxy_ndp(&j->g->net, &j->addr, j->err, sizeof j->err)
                   : fipsd_gw_net_remove_proxy_ndp(&j->g->net, &j->addr, j->err, sizeof j->err);
    return NULL;
}

static void dns_fiber(void *arg)
{
    gateway *g = arg;
    gof_set_name("gw-dns");
    int rc = fipsd_gw_dns_serve(&g->dns);
    int e = fipsd_io_errno();
    bool shutting_down = go_context_is_canceled(fipsd_runtime_context(g->rt));
    if (!shutting_down) {
        /* The resolver ends only on shutdown, so this means .fips resolution
         * has stopped. */
        snprintf(g->dns_cause, sizeof g->dns_cause, "%s", rc == 0 ? "the resolver returned without an error" : strerror(e));
        (void)go_channel_try_send(g->dns_done, g);
    }
    fipsd_gw_task_end(&g->resolver_task, rc != 0);
}
static int spawn_on(void *user, void (*fn)(void *), void *arg) { return fipsd_runtime_spawn(user, fn, arg); }
static void control_fiber(void *arg)
{
    gateway *g = arg;
    gof_set_name("gw-control");
    fipsd_gw_control_accept_loop(&g->control, spawn_on, g->rt);
}
static void tick_fiber(void *arg)
{
    gateway *g = arg;
    gof_set_name("gw-tick");
    GoContext *ctx = fipsd_runtime_context(g->rt);
    uint64_t next = mono_ms(); /* the first tick at once, as an interval's */
    bool running = true;
    while (running) {
        uint64_t now_ms = mono_ms();
        void *token = NULL;
        GoSelectCase done = {.op = GO_SELECT_RECEIVE, .chan = go_context_done(ctx), .recv_buf = &token};
        GoSelectResult r = go_select_timeout(&done, 1, next > now_ms ? next - now_ms : 0);
        if (r.selected_case == 0 || go_context_is_canceled(ctx)) break;
        next += TICK_INTERVAL_MS;
        if (next <= mono_ms()) next = mono_ms() + TICK_INTERVAL_MS;
        /* Removals the NAT table has applied, taken before the conntrack
         * read, so the read follows every removal it is counted against. */
        fipsd_gw_pairs removed = {0};
        fipsd_gw_nat_driver_take_removed(g->nat, &removed);
        fipsd_gw_instant now = fipsd_gw_now();
        /* Read conntrack once, off the fiber and before the pool lock. */
        uint64_t read_started = mono_ms();
        ct_read read = {.g = g};
        (void)go_blocking_submit(read_conntrack, &read);
        uint64_t read_ms = mono_ms() - read_started;
        fipsd_gw_events events = {0};
        fipsd_gw_pool_status status;
        fipsd_gw_mapping_info *info = NULL; size_t ninfo = 0;
        fipsd_gw_pool_state request; memset(&request, 0, sizeof request);
        nsync_mu_lock(&g->pool.mu);
        for (size_t i = 0; i < removed.len; ++i) fipsd_gw_pool_nat_removed(g->pool.pool, &removed.items[i].virtual_ip, &removed.items[i].mesh_addr);
        uint64_t tick_started = mono_ms();
        if (fipsd_gw_pool_tick(g->pool.pool, now, &read.snapshot, &events) != 0) FIPSD_WARN(MODULE, "Pool tick ran out of memory");
        uint64_t tick_ms = mono_ms() - tick_started;
        fipsd_gw_pool_get_status(g->pool.pool, &status);
        if (fipsd_gw_pool_mapping_info(g->pool.pool, now, &info, &ninfo) != 0) { info = NULL; ninfo = 0; }
        bool due = fipsd_gw_pool_mark_request(g->pool.pool, &request);
        nsync_mu_unlock(&g->pool.mu);
        fipsd_gw_ct_snapshot_clear(&read.snapshot);
        fipsd_gw_pairs_clear(&removed);
        FIPSD_DEBUG(MODULE, "Pool tick: mappings=%zu read_ms=%llu tick_ms=%llu", ninfo, (unsigned long long)read_ms, (unsigned long long)tick_ms);
        /* Written off the fiber and without the pool lock; names issued
         * meanwhile fall in the stretch the previous write already covers. */
        if (due) {
            save_job job = {.state = &request};
            (void)go_blocking_submit(save_state, &job);
            (void)fipsd_gw_save_log_report(&g->save_log, job.rc, job.err);
            nsync_mu_lock(&g->pool.mu);
            if (job.rc == 0) fipsd_gw_pool_confirm_mark(g->pool.pool, &request);
            else fipsd_gw_pool_mark_failed(g->pool.pool);
            nsync_mu_unlock(&g->pool.mu);
        }
        fipsd_gw_pool_state_clear(&request);
        uint64_t uptime = fipsd_gw_since(fipsd_gw_now(), g->start_time) / FIPSD_GW_SEC;
        fipsd_gw_snapshot *snapshot = fipsd_gw_snapshot_build(&status, info, ninfo, fipsd_gw_nat_driver_mapping_count(g->nat),
                                                              uptime, &g->snap_config);
        if (snapshot) fipsd_gw_snapshot_publish(&g->snapshots, snapshot);
        for (size_t i = 0; i < events.len && running; ++i)
            if (emit(g, &events.items[i]) != 0) running = false; /* the channel closed: shutdown */
        fipsd_gw_events_clear(&events);
    }
    fipsd_gw_task_end(&g->tick_task, false);
}
static void loop_fiber(void *arg)
{
    gateway *g = arg;
    gof_set_name("gw-loop");
    GoContext *ctx = fipsd_runtime_context(g->rt);
    FIPSD_INFO(MODULE, "fips-gateway running");
    for (;;) {
        void *nat_token = NULL, *event = NULL, *dns_token = NULL, *done_token = NULL;
        GoSelectCase cases[] = {
            {.op = GO_SELECT_RECEIVE, .chan = g->nat_exit, .recv_buf = &nat_token},
            {.op = GO_SELECT_RECEIVE, .chan = g->events, .recv_buf = &event},
            {.op = GO_SELECT_RECEIVE, .chan = g->dns_done, .recv_buf = &dns_token},
            {.op = GO_SELECT_RECEIVE, .chan = go_context_done(ctx), .recv_buf = &done_token},
        };
        int selected = go_select(cases, 4);
        if (go_context_is_canceled(ctx) || selected == 3) { free(event); break; }
        if (selected == 0) {
            /* Without the worker the gateway would answer .fips queries with
             * addresses that have no rules. */
            fail(g, "Gateway NAT worker stopped"); break;
        }
        if (selected == 2) {
            FIPSD_ERROR(MODULE, "Gateway DNS resolver stopped: cause=%s", g->dns_cause);
            fail(g, "Gateway DNS resolver stopped"); break;
        }
        if (selected != 1) continue;
        /* The channel closes only at shutdown, which the context reports first. */
        if (!event) break;
        fipsd_gw_pool_event e = *(fipsd_gw_pool_event *)event;
        free(event);
        fipsd_gw_nat_command command = {e.kind == FIPSD_GW_EVENT_MAPPING_CREATED ? FIPSD_GW_NAT_ADD : FIPSD_GW_NAT_REMOVE,
                                        e.virtual_ip, e.mesh_addr};
        if (fipsd_gw_nat_driver_submit(g->nat, &command) != 0) { fail(g, "Gateway NAT worker stopped"); break; }
        ndp_job job = {.g = g, .add = command.kind == FIPSD_GW_NAT_ADD, .addr = e.virtual_ip};
        (void)go_blocking_submit(proxy_ndp, &job);
        fipsd_gw_report_proxy_ndp(fipsd_gw_ndp_observe(&g->ndp_log, job.add, &job.addr, job.rc, job.err), job.add, &job.addr, job.rc, job.err);
    }
}
static int gateway_start(fipsd_subsystem *self, fipsd_runtime *rt)
{
    gateway *g = self->user;
    fipsd_gw_fibers_started();
    fipsd_gw_task_begin(&g->resolver_task);
    if (fipsd_runtime_spawn(rt, dns_fiber, g) != 0) { fipsd_gw_task_end(&g->resolver_task, true); return -1; }
    if (g->control_bound && fipsd_runtime_spawn(rt, control_fiber, g) != 0)
        FIPSD_WARN(MODULE, "Failed to start the gateway control socket; continuing without it");
    fipsd_gw_task_begin(&g->tick_task);
    if (fipsd_runtime_spawn(rt, tick_fiber, g) != 0) { fipsd_gw_task_end(&g->tick_task, true); return -1; }
    if (fipsd_runtime_spawn(rt, loop_fiber, g) != 0) return -1;
    return 0;
}
static void hook_signal(void *user) { gateway *g = user; fipsd_gw_dns_server_stop(&g->dns); }
static void hook_control(void *user) { gateway *g = user; if (g->control_bound) fipsd_gw_control_stop(&g->control); }
static void hook_abort(void *user, fipsd_gw_task *task)
{
    gateway *g = user;
    if (task == &g->resolver_task) fipsd_gw_dns_server_stop(&g->dns);
}
static void gateway_stop(fipsd_subsystem *self, fipsd_runtime *rt)
{
    (void)rt;
    gateway *g = self->user;
    FIPSD_INFO(MODULE, "fips-gateway shutting down");
    fipsd_gw_stop_hooks hooks = {.events = g->events, .signal = hook_signal, .abort_control = hook_control, .abort = hook_abort, .user = g};
    g->stopped = fipsd_gw_stop_tasks(&hooks, fipsd_gw_task_ended(&g->resolver_task) ? NULL : &g->resolver_task, &g->tick_task,
                                     FIPSD_GW_STOP_TIMEOUT_MS);
}

/* ---- set-up -------------------------------------------------------------- */
static const char *cfg_str(const fipsd_config *c, const char *path, const char *fallback)
{
    const char *s = fipsd_config_string(fipsd_config_get(c, path));
    return s ? s : fallback;
}
static void report_loaded(const fipsd_gw_loaded *loaded)
{
    if (loaded->kind == FIPSD_GW_LOADED_ABSENT) FIPSD_INFO(MODULE, "No pool state found; the pool starts at a random address");
    else if (loaded->kind == FIPSD_GW_LOADED_INVALID) FIPSD_WARN(MODULE, "Pool state unusable; the pool starts at a random address: reason=%s", loaded->reason);
    else {
        uint64_t held = 0;
        for (size_t i = 0; i < loaded->state.held_len; ++i) held += (uint64_t)(loaded->state.held[i][1] - loaded->state.held[i][0]) + 1;
        FIPSD_INFO(MODULE, "Pool state restored; holding the previous run's addresses: from=%u stretch=%u held=%llu hold_secs=%llu",
                   loaded->state.from, loaded->state.span, (unsigned long long)held, (unsigned long long)loaded->state.hold_secs);
    }
}
static bool report_conntrack_source(gateway *g)
{
    fipsd_gw_ct_probe probe = fipsd_gw_ct_probe_reader(&g->conntrack);
    if (probe.found) {
        FIPSD_INFO(MODULE, "Conntrack source: %s; session pinning is on", fipsd_gw_ct_source_name(probe.source));
        return true;
    }
    if (probe.why.netlink_tried)
        FIPSD_WARN(MODULE, "No conntrack source is readable; session pinning is off, and no mapping counts as carrying traffic until reads succeed: proc_error=%s netlink_error=%s",
                   strerror(probe.why.proc_errno), strerror(probe.why.netlink_errno));
    else
        FIPSD_WARN(MODULE, "No conntrack source is readable; session pinning is off, and no mapping counts as carrying traffic until reads succeed: proc_error=%s",
                   strerror(probe.why.proc_errno));
    return false;
}
static int nat_exit_channel_notify(void *user) { gateway *g = user; return go_channel_try_send(g->nat_exit, g); }
static void on_nat_exit(void *user) { (void)nat_exit_channel_notify(user); }
static void drain_events(gateway *g)
{
    void *item = NULL;
    while (g->events && go_channel_try_receive(g->events, &item) == 0) { free(item); item = NULL; }
}

int main(int argc, char **argv)
{
    install_startup_signals();
    const char *config_path = NULL, *level_text = "info";
    static const struct option options[] = {
        {"config", required_argument, NULL, 'c'}, {"log-level", required_argument, NULL, 'l'},
        {"version", no_argument, NULL, 1}, {"help", no_argument, NULL, 'h'}, {NULL, 0, NULL, 0}};
    int opt;
    opterr = 0;
    while ((opt = getopt_long(argc, argv, "c:l:Vh", options, NULL)) != -1) {
        switch (opt) {
        case 'c': config_path = optarg; break;
        case 'l': level_text = optarg; break;
        case 'V': printf("fips-gateway %s\n", short_version()); return EXIT_OK;
        case 1: printf("fips-gateway %s\ntarget: %s\n", short_version(), FIPSD_TARGET_TRIPLE); return EXIT_OK;
        case 'h': print_help(stdout); return EXIT_OK;
        default:
            fprintf(stderr, "error: unexpected argument '%s'\n\nUsage: fips-gateway [OPTIONS]\n\nFor more information, try '--help'.\n",
                    argv[optind - 1] ? argv[optind - 1] : "");
            return EXIT_USAGE;
        }
    }
    if (optind < argc) {
        fprintf(stderr, "error: unexpected argument '%s'\n\nUsage: fips-gateway [OPTIONS]\n\nFor more information, try '--help'.\n", argv[optind]);
        return EXIT_USAGE;
    }
    fipsd_log_level level = FIPSD_LOG_INFO;
    if (!fipsd_log_level_parse(level_text, &level)) level = FIPSD_LOG_INFO;
    const char *env = getenv("FIPSD_LOG_LEVEL");
    if (!env || !*env) env = getenv("RUST_LOG");
    fipsd_log_level env_level;
    if (env && *env && fipsd_log_level_parse(env, &env_level)) level = env_level;
    fipsd_log *log = fipsd_log_create(level, NULL);
    if (!log) { fprintf(stderr, "fips-gateway: cannot create the logger\n"); return EXIT_FAIL; }
    fipsd_log_set_default(log);
    (void)fipsd_privilege_set_umask();
    if (sodium_init() < 0) { FIPSD_ERROR(MODULE, "cannot initialise libsodium"); return EXIT_FAIL; }
    FIPSD_INFO(MODULE, "fips-gateway %s starting", short_version());

    static gateway g;
    memset(&g, 0, sizeof g);
    atomic_init(&g.exit_code, EXIT_OK);
    int rc = EXIT_FAIL;
    bool nat_left_running = false;

    fipsd_config_log held = {0};
    fipsd_config *config = fipsd_config_load(config_path, &held);
    fipsd_config_log_replay(&held, log);
    if (!config) {
        if (config_path) FIPSD_ERROR(MODULE, "Failed to load config from %s: %s", config_path, held.error);
        else FIPSD_ERROR(MODULE, "Failed to load config: %s", held.error);
        fipsd_config_log_clear(&held);
        goto out_log;
    }
    fipsd_config_log_clear(&held);
    if (config_path) FIPSD_INFO(MODULE, "Loaded config file: path=%s", config_path);
    if (!fipsd_config_get(config, "gateway")) { FIPSD_ERROR(MODULE, "No gateway section in configuration"); goto out_config; }
    if (!fipsd_config_bool(fipsd_config_get(config, "gateway.enabled"))) {
        FIPSD_ERROR(MODULE, "Gateway section exists but is not enabled (gateway.enabled = false)"); goto out_config;
    }
    snprintf(g.pool_cidr, sizeof g.pool_cidr, "%s", cfg_str(config, "gateway.pool", ""));
    snprintf(g.lan, sizeof g.lan, "%s", cfg_str(config, "gateway.lan_interface", ""));
    snprintf(g.tun, sizeof g.tun, "%s", cfg_str(config, "tun.name", "fips0"));
    snprintf(g.dns_listen, sizeof g.dns_listen, "%s", cfg_str(config, "gateway.dns.listen", "[::1]:5365"));
    snprintf(g.dns_upstream, sizeof g.dns_upstream, "%s", cfg_str(config, "gateway.dns.upstream", "[::1]:5354"));
    uint64_t ttl = fipsd_config_uint(fipsd_config_get(config, "gateway.dns.ttl"));
    g.ttl = fipsd_dns_clamp_ttl(ttl > UINT32_MAX ? UINT32_MAX : (uint32_t)ttl);
    if (g.ttl != ttl)
        FIPSD_WARN(MODULE, "gateway.dns.ttl is above 2147483647, the largest TTL resolvers honour; using 2147483647: configured=%llu used=%u",
                   (unsigned long long)ttl, g.ttl);
    g.grace = fipsd_config_uint(fipsd_config_get(config, "gateway.pool_grace_period"));
    const fipsd_config_value *forwards_cfg = fipsd_config_get(config, "gateway.port_forwards");
    size_t nforwards = fipsd_config_count(forwards_cfg);
    fipsd_gw_port_forward *forwards = nforwards ? calloc(nforwards, sizeof *forwards) : NULL;
    if (nforwards && !forwards) goto out_config;
    for (size_t i = 0; i < nforwards; ++i) {
        const fipsd_config_value *pf = fipsd_config_at(forwards_cfg, i);
        forwards[i].listen_port = (uint16_t)fipsd_config_uint(fipsd_config_member(pf, "listen_port"));
        const char *proto = fipsd_config_string(fipsd_config_member(pf, "proto"));
        forwards[i].proto = proto && !strcmp(proto, "udp") ? FIPSD_GW_PROTO_UDP : FIPSD_GW_PROTO_TCP;
        /* Validated at load: non-zero, unique, an IPv6 socket address. */
        if (!fipsd_gw_parse_target(fipsd_config_string(fipsd_config_member(pf, "target")), &forwards[i].target, &forwards[i].target_port)) {
            FIPSD_ERROR(MODULE, "Invalid gateway.port_forwards: invalid IPv6 socket address syntax"); free(forwards); goto out_config;
        }
    }
    FIPSD_INFO(MODULE, "Gateway config loaded: pool=%s lan_interface=%s port_forwards=%zu", g.pool_cidr, g.lan, nforwards);

    /* --- Prerequisites --- */
    int forwarding = fipsd_gw_ipv6_forwarding(NULL);
    if (forwarding < 0) { FIPSD_ERROR(MODULE, "Could not check IPv6 forwarding state: error=%s", strerror(errno)); goto out_forwards; }
    if (!forwarding) { FIPSD_ERROR(MODULE, "IPv6 forwarding is disabled. Enable with: sysctl -w net.ipv6.conf.all.forwarding=1"); goto out_forwards; }
    FIPSD_DEBUG(MODULE, "IPv6 forwarding is enabled");
    fipsd_gw_check_proxy(g.lan);
    /* The daemon creates its TUN some time after it starts; wait for it,
     * woken by link notifications, so a gateway started with the daemon is
     * not restarted while the TUN is still missing. */
    if (!fipsd_gw_iface_index(g.tun)) {
        FIPSD_INFO(MODULE, "Waiting for the FIPS daemon's TUN interface: interface=%s", g.tun);
        int waited = fipsd_gw_wait_interface(g.tun, TUN_WAIT_MS, startup_pipe[0]);
        if (waited == ECANCELED) { rc = startup_exit(); goto out_forwards; }
        if (waited) {
            FIPSD_ERROR(MODULE, "The FIPS daemon's TUN interface (tun.name) did not appear within %us; is the FIPS daemon running?: error=%s interface=%s",
                        TUN_WAIT_MS / 1000u, strerror(waited), g.tun);
            goto out_forwards;
        }
    }
    if (!fipsd_gw_iface_index(g.lan)) { FIPSD_ERROR(MODULE, "LAN interface not found: error=%s interface=%s", strerror(ENODEV), g.lan); goto out_forwards; }

    /* The daemon's resolver must answer; the resolver later forwards to the
     * address this probe reached. Bounded retries cover a daemon still
     * binding its responder at a cold boot. */
    FIPSD_INFO(MODULE, "Checking DNS upstream reachability: upstream=%s", g.dns_upstream);
    struct sockaddr_storage upstream; socklen_t upstream_len;
    if (!fipsd_gw_dns_parse_addr(g.dns_upstream, &upstream, &upstream_len)) {
        FIPSD_ERROR(MODULE, "Invalid DNS upstream address: upstream=%s", g.dns_upstream); goto out_forwards;
    }
    char last_failure[256] = "";
    bool reachable = false;
    for (unsigned attempt = 1; attempt <= MAX_PROBE_ATTEMPTS && !reachable; ++attempt) {
        if (startup_signal) { rc = startup_exit(); goto out_forwards; }
        if (fipsd_gw_dns_probe_upstream((struct sockaddr *)&upstream, upstream_len, PROBE_TIMEOUT_MS, last_failure, sizeof last_failure) == 0) {
            FIPSD_INFO(MODULE, "DNS upstream is reachable: upstream=%s attempt=%u", g.dns_upstream, attempt);
            reachable = true; break;
        }
        if (attempt < MAX_PROBE_ATTEMPTS) {
            FIPSD_INFO(MODULE, "DNS upstream probe attempt failed; retrying: upstream=%s attempt=%u last=%s", g.dns_upstream, attempt, last_failure);
            if (startup_sleep(PROBE_RETRY_DELAY_MS)) { rc = startup_exit(); goto out_forwards; }
        }
    }
    if (!reachable) {
        FIPSD_ERROR(MODULE, "DNS upstream did not become reachable after exhausting retries — is the FIPS daemon running?: upstream=%s attempts=%u last=%s",
                    g.dns_upstream, MAX_PROBE_ATTEMPTS, last_failure);
        goto out_forwards;
    }

    /* --- The DNS listener, before anything that would need tearing down --- */
    uint16_t listen_port;
    if (fipsd_gw_dns_port_of(g.dns_listen, &listen_port) && listen_port == 5353)
        FIPSD_WARN(MODULE, "gateway.dns.listen uses port 5353, the mDNS port; an mDNS responder (the fips daemon's LAN rendezvous, avahi) will conflict with it; the default is now [::1]:5365");
    char err[1024];
    int dns_fd = fipsd_gw_dns_bind_listener(g.dns_listen, err, sizeof err);
    if (dns_fd < 0) { FIPSD_ERROR(MODULE, "%s", err); goto out_forwards; }
    FIPSD_INFO(MODULE, "Gateway DNS resolver listening: addr=%s", g.dns_listen);

    /* --- The pool, from the previous run's state when there is one --- */
    uint32_t total;
    fipsd_gw_pool_error perr = fipsd_gw_pool_total(g.pool_cidr, &total);
    char perr_text[160];
    if (perr) {
        FIPSD_ERROR(MODULE, "Failed to create virtual IP pool: error=%s", fipsd_gw_pool_error_text(perr, 0, NULL, g.pool_cidr, perr_text, sizeof perr_text));
        close(dns_fd); goto out_forwards;
    }
    fipsd_gw_started started;
    perr = fipsd_gw_state_start_pool(FIPSD_GW_STATE_DIR, g.pool_cidr, total, g.ttl, g.grace, randombytes_uniform(total) + 1,
                                     fipsd_gw_now(), fipsd_gw_is_tmpfs, NULL, &started);
    if (perr) {
        FIPSD_ERROR(MODULE, "Failed to create virtual IP pool: error=%s", fipsd_gw_pool_error_text(perr, 0, NULL, g.pool_cidr, perr_text, sizeof perr_text));
        close(dns_fd); goto out_forwards;
    }
    report_loaded(&started.loaded);
    if (started.carried) (void)fipsd_gw_save_log_report(&g.save_log, started.carried_rc, started.carried_err);
    fipsd_gw_loaded_clear(&started.loaded);
    nsync_mu_init(&g.pool.mu); g.pool.pool = started.pool;
    size_t small = fipsd_gw_small_pool_threshold(g.ttl, g.grace);
    if ((size_t)total < small)
        FIPSD_WARN(MODULE, "The pool is small enough for one LAN host naming new names to exhaust it: addresses=%u threshold=%zu", total, small);
    struct in6_addr network; uint8_t prefix_len;
    fipsd_gw_pool_network(g.pool.pool, &network, &prefix_len);

    /* --- NAT, forwards and the pool route --- */
    fipsd_gw_nat_error nerr;
    fipsd_gw_nat *nat = fipsd_gw_nat_new(g.lan, g.tun, &network, prefix_len, fipsd_gw_nft_system_backend(), &nerr);
    char nerr_text[700];
    if (!nat) {
        FIPSD_ERROR(MODULE, "Failed to create nftables table — do you have CAP_NET_ADMIN?: error=%s", fipsd_gw_nat_error_text(&nerr, nerr_text, sizeof nerr_text));
        goto out_pool;
    }
    if (fipsd_gw_nat_set_port_forwards(nat, forwards, nforwards, &nerr) != 0) {
        FIPSD_ERROR(MODULE, "Failed to install port-forward rules: error=%s", fipsd_gw_nat_error_text(&nerr, nerr_text, sizeof nerr_text));
        (void)fipsd_gw_nat_cleanup(nat, &nerr); fipsd_gw_nat_free(nat); goto out_pool;
    }
    fipsd_gw_net_init(&g.net, g.lan, g.pool_cidr, NULL, NULL);
    if (fipsd_gw_net_add_pool_route(&g.net, err, sizeof err) != 0) {
        FIPSD_ERROR(MODULE, "Failed to add pool route: error=%s", err);
        (void)fipsd_gw_nat_cleanup(nat, &nerr); fipsd_gw_nat_free(nat); goto out_pool;
    }

    /* The NAT table exists, so a kernel with the proc file has loaded
     * nf_conntrack and the probe sees what the first tick will. The pool's
     * quota of reads comes before any name is answered. */
    fipsd_gw_ct_reader_system(&g.conntrack);
    if (report_conntrack_source(&g)) {
        for (unsigned i = 0; i < FIPSD_GW_MAX_ABSENT_READS; ++i) {
            fipsd_gw_ct_snapshot snapshot;
            fipsd_gw_ct_read_logged(&g.conntrack, &g.ct_log, &snapshot);
            fipsd_gw_events events = {0};
            (void)fipsd_gw_pool_tick(g.pool.pool, fipsd_gw_now(), &snapshot, &events);
            fipsd_gw_events_clear(&events);
            fipsd_gw_ct_snapshot_clear(&snapshot);
        }
    } else fipsd_gw_pool_set_evidence(g.pool.pool, false);
    /* The first write with a stretch, after the last step that can end the
     * start; later writes run off the fibers. */
    fipsd_gw_pool_state request;
    if (fipsd_gw_pool_mark_request(g.pool.pool, &request)) {
        int saved = fipsd_gw_state_save(FIPSD_GW_STATE_DIR, &request, fipsd_gw_is_tmpfs, NULL, err, sizeof err);
        (void)fipsd_gw_save_log_report(&g.save_log, saved, err);
        if (saved == 0) fipsd_gw_pool_confirm_mark(g.pool.pool, &request); else fipsd_gw_pool_mark_failed(g.pool.pool);
        fipsd_gw_pool_state_clear(&request);
    }

    /* --- Channels, the NAT worker and the runtime --- */
    g.events = go_channel_create(FIPSD_GW_EVENT_CAPACITY);
    g.nat_exit = go_channel_create(1);
    g.dns_done = go_channel_create(1);
    if (!g.events || !g.nat_exit || !g.dns_done) {
        FIPSD_ERROR(MODULE, "cannot create the gateway channels");
        (void)fipsd_gw_nat_cleanup(nat, &nerr); fipsd_gw_nat_free(nat); goto out_net;
    }
    g.nat = fipsd_gw_nat_driver_start(fipsd_gw_nat_table_of(nat), on_nat_exit, &g);
    if (!g.nat) { FIPSD_ERROR(MODULE, "cannot start the NAT worker"); goto out_net; }
    fipsd_gw_dns_resolver_init(&g.resolver, (struct sockaddr *)&upstream, upstream_len, g.ttl, &g.pool, emit, &g);
    if (fipsd_gw_dns_server_init(&g.dns, dns_fd, g.tun, &g.resolver) != 0) FIPSD_WARN(MODULE, "cannot register the DNS socket for cancellation");
    dns_fd = -1;
    fipsd_gw_snapshot_slot_init(&g.snapshots);
    g.snap_config = (fipsd_gw_snapshot_config){g.pool_cidr, g.lan, g.dns_upstream, g.dns_listen, g.ttl, g.grace};
    g.start_time = fipsd_gw_now();
    if (fipsd_gw_control_bind(&g.control, FIPSD_GW_CONTROL_SOCKET, &g.snapshots, err, sizeof err) == 0) g.control_bound = true;
    else FIPSD_WARN(MODULE, "Failed to bind gateway control socket — continuing without it: error=%s", err);
    fipsd_gw_task_init(&g.resolver_task, "resolver");
    fipsd_gw_task_init(&g.tick_task, "tick");

    fipsd_runtime_config rcfg;
    fipsd_runtime_config_default(&rcfg);
    rcfg.grace_ms = FIPSD_GW_STOP_TIMEOUT_MS;
    g.rt = fipsd_runtime_create(&rcfg);
    g.subsystem = (fipsd_subsystem){.name = "gateway", .start = gateway_start, .stop = gateway_stop, .user = &g};
    if (!g.rt || fipsd_runtime_register(g.rt, &g.subsystem) != 0 || fipsd_runtime_start(g.rt) != 0) {
        FIPSD_ERROR(MODULE, "cannot start the gateway runtime");
        atomic_store(&g.exit_code, EXIT_FAIL);
        g.stopped.tick = FIPSD_GW_TASK_FINISHED;
    } else {
        /* A stop held by the start-up handler before the runtime's took over. */
        if (startup_signal) fipsd_runtime_shutdown(g.rt);
        if (fipsd_runtime_run(g.rt) == FIPSD_RUN_GRACE_ELAPSED) FIPSD_WARN(MODULE, "Some gateway fibers outlived the stop grace period");
    }

    /* --- Shutdown: the exact addresses a client may still hold, then the
     * network and NAT cleanup --- */
    nsync_mu_lock(&g.pool.mu);
    fipsd_gw_pool_status status;
    fipsd_gw_pool_get_status(g.pool.pool, &status);
    FIPSD_INFO(MODULE, "Final pool status: total=%zu allocated=%zu active=%zu draining=%zu free=%zu",
               status.total, status.allocated, status.active, status.draining, status.free);
    fipsd_gw_pool_state final_state;
    int have_final = fipsd_gw_pool_shutdown_state(g.pool.pool, &final_state) == 0;
    nsync_mu_unlock(&g.pool.mu);
    if (g.stopped.tick == FIPSD_GW_TASK_ABANDONED) {
        /* An abandoned tick may still be writing through the same temporary
         * file; its last completed write stands, as after a crash. */
        FIPSD_WARN(MODULE, "Pool tick did not stop; skipping the final pool state write, the last periodic write stands");
    } else if (have_final) {
        fipsd_gw_save_log save_log = g.stopped.tick == FIPSD_GW_TASK_FAILED ? (fipsd_gw_save_log){0} : g.save_log;
        int saved = fipsd_gw_state_save(FIPSD_GW_STATE_DIR, &final_state, fipsd_gw_is_tmpfs, NULL, err, sizeof err);
        (void)fipsd_gw_save_log_report(&save_log, saved, err);
    }
    if (have_final) fipsd_gw_pool_state_clear(&final_state);
    fipsd_gw_net_cleanup(&g.net);
    int nat_stopped = fipsd_gw_nat_driver_shutdown(g.nat, FIPSD_GW_NAT_STOP_TIMEOUT_MS, &nerr);
    if (nat_stopped < 0)
        FIPSD_WARN(MODULE, "Failed to clean up nftables table: error=%s", fipsd_gw_nat_error_text(&nerr, nerr_text, sizeof nerr_text));
    nat_left_running = nat_stopped > 0;
    g.nat = NULL;
    FIPSD_INFO(MODULE, "fips-gateway shutdown complete");
    rc = atomic_load(&g.exit_code);
    if (g.rt) fipsd_runtime_destroy(g.rt);
    if (g.control_bound) fipsd_gw_control_clear(&g.control);
    fipsd_gw_snapshot_slot_clear(&g.snapshots);
    if (g.dns.fd >= 0) { (void)gof_io_reset_fd(g.dns.fd); close(g.dns.fd); }
    goto out_channels;
out_net:
    fipsd_gw_net_cleanup(&g.net);
out_channels:
    drain_events(&g);
    if (g.events) go_channel_free(g.events);
    if (g.nat_exit) go_channel_free(g.nat_exit);
    if (g.dns_done) go_channel_free(g.dns_done);
    fipsd_gw_net_clear(&g.net);
out_pool:
    if (dns_fd >= 0) close(dns_fd);
    fipsd_gw_pool_free(g.pool.pool);
out_forwards:
    free(forwards);
out_config:
    fipsd_config_free(config);
out_log:
    /* A NAT worker the shutdown left behind may still log; the exit reclaims
     * the logger then. The fibers and the blocking executor were joined by
     * fipsd_runtime_run, so nothing else outlives this point. */
    if (!nat_left_running) {
        fipsd_log_set_default(NULL);
        fipsd_log_destroy(log);
    }
    return rc;
}
