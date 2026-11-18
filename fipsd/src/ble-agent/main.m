/* fips-ble-agent (nostrc-gfi8h.6.3; upstream `fips --ble-agent`): owns this
 * login session's Bluetooth radio through CoreBluetooth and lends it to fipsd
 * over the daemon's BLE agent socket. macOS never gives CoreBluetooth to a
 * root launchd daemon, so the per-user LaunchAgent com.fips.ble-agent runs
 * this in every GUI session (packaging/macos). The daemon accepts one agent at
 * a time; the others are told BUSY and stand by.
 *
 * The main thread runs the session loop (session.c); CoreBluetooth runs on
 * the radio's dispatch queue (radio.m); SIGTERM/SIGINT stop both, and the
 * radio gives its L2CAP PSMs back to bluetoothd on the way out. */
#import <Foundation/Foundation.h>
#include "fips-ble-agent-version.h"
#include "fipsd_build_info.h"
#include "radio.h"
#include "session.h"
#include "transport/ble/agent_proto.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LOG_ROLL_BYTES (10L * 1024 * 1024) /* the previous log is kept as .1 beyond this */
static const char *usage =
    "Usage: fips-ble-agent [options]\n"
    "Owns this login session's Bluetooth radio (CoreBluetooth) and lends it to fipsd\n"
    "over the daemon's BLE agent socket. Started by the per-user LaunchAgent\n"
    "com.fips.ble-agent; needs the user in the `fips` group and Bluetooth permission.\n"
    "\n"
    "  --socket PATH      the daemon's socket (default: $" "FIPSD_BLE_AGENT_SOCKET, else " FIPSD_BLE_AGENT_SOCKET_PATH ")\n"
    "  --daemon-uid UID   serve only a daemon running as UID (default 0, the root launchd daemon)\n"
    "  --log-file FILE    log here (default: stdout on a terminal, else ~/Library/Logs/fips/ble-agent.log)\n"
    "  --verbose          debug logging, including every FIPS advert passed on\n"
    "  --probe SECONDS    diagnostics, no daemon: power up, publish an L2CAP listener, advertise the FIPS\n"
    "                     service and scan every advertiser for SECONDS, then report; exits 1 unless up\n"
    "  --version          print the version and exit\n"
    "  --help             print this and exit\n"
    "  --ble-agent        accepted and ignored (upstream's `fips --ble-agent`)\n";

typedef struct logger { pthread_mutex_t mu; FILE *out; int level; } logger;
static void log_line(void *ctx, int level, const char *msg)
{
    logger *l = ctx;
    if (level < l->level) return;
    static const char *names[] = {"DEBUG", "INFO", "WARN", "ERROR"};
    struct timespec now; clock_gettime(CLOCK_REALTIME, &now);
    struct tm tm; gmtime_r(&now.tv_sec, &tm);
    char ts[32]; strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", &tm);
    pthread_mutex_lock(&l->mu);
    fprintf(l->out, "%s.%03ldZ %-5s %s\n", ts, now.tv_nsec / 1000000L, names[level < 0 ? 0 : level > 3 ? 3 : level], msg);
    fflush(l->out);
    pthread_mutex_unlock(&l->mu);
}
static void logf_(logger *l, int level, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void logf_(logger *l, int level, const char *fmt, ...)
{
    char msg[1024];
    va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
    log_line(l, level, msg);
}
/* Each user's agent logs in a directory of its own: a shared one would let
 * another account plant a file or link that this agent appends to. */
static FILE *open_log(const char *path)
{
    struct stat st;
    if (!lstat(path, &st) && S_ISREG(st.st_mode) && st.st_size > LOG_ROLL_BYTES) {
        char old[1024];
        snprintf(old, sizeof old, "%s.1", path);
        (void)rename(path, old);
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0600);
    return fd < 0 ? NULL : fdopen(fd, "a");
}
static FILE *default_log(void)
{
    const char *home = getenv("HOME");
    if (!home || !*home) return NULL;
    char dir[1024], path[1100];
    snprintf(dir, sizeof dir, "%s/Library/Logs", home); (void)mkdir(dir, 0700);
    snprintf(dir, sizeof dir, "%s/Library/Logs/fips", home); (void)mkdir(dir, 0700);
    snprintf(path, sizeof path, "%s/ble-agent.log", dir);
    return open_log(path);
}
static dispatch_source_t on_signal(int sig, dispatch_queue_t q, dispatch_block_t handler)
{
    signal(sig, SIG_IGN); /* delivered to the source instead */
    dispatch_source_t s = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, (uintptr_t)sig, 0, q);
    dispatch_source_set_event_handler(s, handler);
    dispatch_resume(s);
    return s;
}
static bool parse_uint(const char *s, unsigned long max, unsigned long *out)
{
    char *end = NULL;
    errno = 0;
    unsigned long v = s && *s ? strtoul(s, &end, 10) : 0;
    if (!s || !*s || errno || *end || v > max) return false;
    *out = v;
    return true;
}

static int probe(logger *lg, unsigned long seconds, bool verbose)
{
    fba_cb_config rc = {.probe = true, .log_adverts = verbose, .log = log_line, .log_ctx = lg};
    fba_cb_radio *radio = fba_cb_radio_create(&rc);
    if (!radio) return 1;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    dispatch_queue_t q = dispatch_queue_create("com.fips.ble-agent.signals", DISPATCH_QUEUE_SERIAL);
    dispatch_source_t term = on_signal(SIGTERM, q, ^{ dispatch_semaphore_signal(done); });
    dispatch_source_t intr = on_signal(SIGINT, q, ^{ dispatch_semaphore_signal(done); });
    fba_cb_radio_start(radio, NULL);
    /* The probe's length is what was asked for: a diagnostic, not a wait for an outcome. */
    (void)dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, (int64_t)seconds * NSEC_PER_SEC));
    fba_cb_probe p = fba_cb_radio_probe(radio);
    fba_cb_radio_destroy(radio);
    dispatch_source_cancel(term); dispatch_source_cancel(intr);
    printf("Bluetooth permission: %s\n", fba_cb_authorization_name(p.authorization));
    printf("central manager: %s; peripheral manager: %s\n", fba_cb_state_name(p.central_state), fba_cb_state_name(p.manager_state));
    if (p.published > 0) printf("L2CAP listener: published on psm 0x%04x (OS-assigned)\n", p.published);
    else printf("L2CAP listener: %s\n", p.published == 0 ? "publish failed" : "not published");
    printf("radio: %s; advertising the FIPS service: %s; scanning: %s\n", p.up ? "up" : "down",
        p.advertising ? "yes" : p.advertise_failed ? "failed" : "no", p.scanning ? "yes" : "no");
    printf("heard: %zu adverts from %zu devices, %zu advertising the FIPS service\n", p.adverts, p.devices, p.fips_devices);
    return p.up && p.published > 0 && p.advertising ? 0 : 1;
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        const char *socket = getenv("FIPSD_BLE_AGENT_SOCKET"), *log_file = NULL;
        unsigned long uid = 0, seconds = 0;
        bool verbose = false, probing = false;
        for (int i = 1; i < argc; ++i) {
            const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
            if (!strcmp(a, "--help") || !strcmp(a, "-h")) { fputs(usage, stdout); return 0; }
            if (!strcmp(a, "--version") || !strcmp(a, "-V")) {
                printf("fips-ble-agent %s\n", FIPSD_GIT_REV[0] ? FIPS_BLE_AGENT_VERSION " (rev " FIPSD_GIT_REV ")" : FIPS_BLE_AGENT_VERSION);
                return 0;
            }
            if (!strcmp(a, "--ble-agent")) continue;
            if (!strcmp(a, "--verbose") || !strcmp(a, "-v")) { verbose = true; continue; }
            if (!strcmp(a, "--socket") && v && *v) { socket = v; ++i; continue; }
            if (!strcmp(a, "--log-file") && v && *v) { log_file = v; ++i; continue; }
            if (!strcmp(a, "--daemon-uid") && parse_uint(v, (uid_t)-1 - 1u, &uid)) { ++i; continue; }
            if (!strcmp(a, "--probe") && parse_uint(v, 3600, &seconds) && seconds) { probing = true; ++i; continue; }
            fprintf(stderr, "fips-ble-agent: bad argument '%s'\n%s", a, usage);
            return 2;
        }
        if (!socket || !*socket) socket = FIPSD_BLE_AGENT_SOCKET_PATH;
        const char *level = getenv("FIPS_BLE_AGENT_LOG");
        logger lg = {.out = stdout, .level = verbose || (level && !strcmp(level, "debug")) ? FBA_LOG_DEBUG : FBA_LOG_INFO};
        pthread_mutex_init(&lg.mu, NULL);
        if (log_file || (!probing && !isatty(STDOUT_FILENO))) {
            FILE *f = log_file ? open_log(log_file) : default_log();
            if (!f && log_file) { fprintf(stderr, "fips-ble-agent: cannot open log file %s: %s\n", log_file, strerror(errno)); return 1; }
            if (f) lg.out = f;
        }
        signal(SIGPIPE, SIG_IGN); /* a daemon or peer that hung up is an error return, not a signal */
        logf_(&lg, FBA_LOG_INFO, "FIPS BLE agent %s starting (Bluetooth permission: %s)", FIPS_BLE_AGENT_VERSION,
            fba_cb_authorization_name(fba_cb_authorization()));
        if (probing) return probe(&lg, seconds, verbose);

        logf_(&lg, FBA_LOG_INFO, "BLE agent: serving the daemon at %s (uid %lu)", socket, uid);
        fba_cb_config rc = {.log_adverts = verbose, .log = log_line, .log_ctx = &lg};
        fba_cb_radio *radio = fba_cb_radio_create(&rc);
        fba_agent_config ac = {.socket_path = socket, .check_uid = true, .daemon_uid = (uid_t)uid, .log = log_line, .log_ctx = &lg};
        fba_agent *agent = radio ? fba_agent_create(&ac, &fba_cb_radio_ops, radio) : NULL;
        if (!agent) { logf_(&lg, FBA_LOG_ERROR, "FIPS BLE agent failed to start"); fba_cb_radio_destroy(radio); return 1; }
        /* The stop signal is the agent's own to honour, or launchd's bootout on
         * upgrade and uninstall would wait out its timeout and kill it. */
        dispatch_queue_t q = dispatch_queue_create("com.fips.ble-agent.signals", DISPATCH_QUEUE_SERIAL);
        dispatch_source_t term = on_signal(SIGTERM, q, ^{ fba_agent_stop(agent); });
        dispatch_source_t intr = on_signal(SIGINT, q, ^{ fba_agent_stop(agent); });
        fba_cb_radio_start(radio, agent);
        fba_agent_run(agent);
        logf_(&lg, FBA_LOG_INFO, "FIPS BLE agent stopping");
        dispatch_source_cancel(term); dispatch_source_cancel(intr);
        fba_cb_radio_destroy(radio); /* gives the PSMs back; closes channels, ending their pumps */
        fba_agent_destroy(agent);
        return 0;
    }
}
