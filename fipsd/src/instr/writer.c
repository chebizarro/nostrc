#define _XOPEN_SOURCE 700
#include "instr/writer.h"
#include "instr/recorder.h"
#include "control/protocol.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "FIPSD-version.h"

static const char notes[] =
    "# NOTE\tstep durations are WALL CLOCK across await points, not CPU time:\n"
    "# NOTE\ta step that awaits I/O accrues the wait, and other tasks may run\n"
    "# NOTE\tinside that span. That is the intended measure for head-of-line\n"
    "# NOTE\tdelay; do not read a large step as CPU cost.\n"
    "# NOTE\tarm_starvation is measured directly as (entry time - the deadline\n"
    "# NOTE\tthe interval scheduled the tick for). It is NOT derived from\n"
    "# NOTE\ttick_entry_gap, which carries no starvation signal by itself:\n"
    "# NOTE\tunder a steady delay every gap is exactly one tick period.\n";
static void utc(uint64_t secs, char *iso, size_t n, char *compact, size_t m)
{
    time_t t = (time_t)secs; struct tm tm;
    gmtime_r(&t, &tm);
    strftime(iso, n, "%Y-%m-%dT%H:%M:%SZ", &tm);
    strftime(compact, m, "%Y%m%dT%H%M%SZ", &tm);
}
int fipsd_instr_write_header(FILE *f, const char *node, uint64_t period, uint64_t secs)
{
    char iso[32], stamp[32]; utc(secs, iso, sizeof iso, stamp, sizeof stamp);
#if defined(__APPLE__)
    const char *platform = "macos";
#elif defined(__linux__)
    const char *platform = "linux";
#elif defined(__FreeBSD__)
    const char *platform = "freebsd";
#else
    const char *platform = "unknown";
#endif
    return fprintf(f, "# fips tick profile\n# node\t%s\n# build\tfipsd %s\n# platform\t%s\n"
        "# tick_period_secs\t%" PRIu64 "\n# interval_secs\t%u\n# byte_cap\t%u\n"
        "# start_utc\t%s\n# start_unix\t%" PRIu64 "\n%s"
        "ts_unix\tkind\tdomain\tname\tcount\tmax\ttotal\tunit\n",
        node, FIPSD_VERSION, platform, period, FIPSD_PROFILE_INTERVAL, FIPSD_PROFILE_CAP, iso, secs, notes);
}
int fipsd_instr_flush(FILE *f, uint64_t secs)
{
    for (int i = 0; i < FIPSD_INSTR_STEPS; ++i) {
        if (!fipsd_instr_step_emitted(i)) continue;
        fipsd_instr_sample s = fipsd_instr_take_step(i);
        if (fprintf(f, "%" PRIu64 "\tstep\ttick\t%s\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\tus\n",
            secs, fipsd_instr_step_name(i), s.count, s.max / 1000, s.total / 1000) < 0) return -1;
    }
    static const char *const names[] = {"ticks", "peers", "tick_entry_gap", "arm_starvation"};
    static const char *const units[] = {"ticks", "peers", "us", "us"};
    for (int i = 0; i < FIPSD_INSTR_GAUGES; ++i) {
        fipsd_instr_sample s = fipsd_instr_take_gauge(i);
        uint64_t divisor = i >= FIPSD_INSTR_TICK_GAP ? 1000 : 1;
        if (fprintf(f, "%" PRIu64 "\tgauge\ttick\t%s\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%s\n",
            secs, names[i], s.count, s.max / divisor, s.total / divisor, units[i]) < 0) return -1;
    }
    return fflush(f);
}
/* Commands run off-dispatcher. Only control-side lifecycle operations take mu;
 * the writer owns the FILE and never takes mu. stop wakes a condition variable
 * and joins, without waiting for dispatcher progress or a flush interval. */
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t wake_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static bool stop_requested, joinable;
static pthread_t thread;
static char active_path[PATH_MAX];
enum { IDLE, RUNNING, STOPPED_BY_CAP, STOPPED_BY_ERROR };
static _Atomic int state;
static _Atomic unsigned long long bytes;
static const char *state_name(int s)
{
    switch (s) {
    case RUNNING: return "running";
    case STOPPED_BY_CAP: return "stopped_by_cap";
    case STOPPED_BY_ERROR: return "stopped_by_error";
    default: return "idle";
    }
}
static void *write_run(void *arg)
{
    FILE *file = arg;
    for (;;) {
        struct timespec until; clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += FIPSD_PROFILE_INTERVAL;
        pthread_mutex_lock(&wake_mu);
        int rc = 0;
        while (!stop_requested && rc == 0) rc = pthread_cond_timedwait(&wake, &wake_mu, &until);
        bool stop = stop_requested;
        pthread_mutex_unlock(&wake_mu);
        bool failed = fipsd_instr_flush(file, (uint64_t)time(NULL)) != 0;
        long pos = ftell(file);
        if (pos >= 0) atomic_store(&bytes, (unsigned long long)pos);
        bool capped = atomic_load(&bytes) >= FIPSD_PROFILE_CAP;
        if (failed || capped) {
            fipsd_instr_arm(false);
            atomic_store(&state, failed ? STOPPED_BY_ERROR : STOPPED_BY_CAP);
            if (failed) {
                fprintf(stderr, "profile capture stopped: write error on the sink\n");
                (void)fprintf(file, "# capture stopped: write error\n");
            } else (void)fprintf(file, "# capture stopped: byte cap %u reached\n", FIPSD_PROFILE_CAP);
        }
        pos = ftell(file);
        if (pos >= 0) atomic_store(&bytes, (unsigned long long)pos);
        if (stop || failed || capped) break;
    }
    if (fclose(file) != 0) {
        fipsd_instr_arm(false); atomic_store(&state, STOPPED_BY_ERROR);
        fprintf(stderr, "profile capture stopped: error closing the sink\n");
    }
    return NULL;
}
static void reap(void)
{
    if (!joinable) return;
    pthread_mutex_lock(&wake_mu); stop_requested = true;
    pthread_cond_signal(&wake); pthread_mutex_unlock(&wake_mu);
    pthread_join(thread, NULL); joinable = false;
}
static int mkdir_all(const char *path)
{
    char buf[PATH_MAX];
    if (strlen(path) >= sizeof buf) { errno = ENAMETOOLONG; return -1; }
    strcpy(buf, path);
    for (char *p = buf + 1;; ++p) {
        if (*p != '/' && *p) continue;
        char c = *p; *p = 0;
        if (mkdir(buf, 0750) != 0 && errno != EEXIST) return -1;
        *p = c; if (!c) return 0;
    }
}
static bool confined(const char *dir)
{
    if (geteuid() != 0) return true;
    if (dir[0] != '/') return false;
    char probe[PATH_MAX], resolved[PATH_MAX];
    if (strlen(dir) >= sizeof probe) return false;
    strcpy(probe, dir);
    /* Reject parent components before resolving the deepest existing ancestor.
     * The trusted /var/log/fips root must not be writable by weaker users. */
    for (const char *p = dir; *p;) {
        while (*p == '/') ++p;
        const char *end = strchr(p, '/'); size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n == 2 && p[0] == '.' && p[1] == '.') return false;
        p += n;
    }
    while (!realpath(probe, resolved)) {
        char *slash = strrchr(probe, '/');
        if (!slash || slash == probe) return false;
        *slash = 0;
    }
    /* Resolve the root as well (e.g. macOS /var -> /private/var). */
    char root[PATH_MAX];
    if (!realpath("/var/log/fips", root)) {
        if (!realpath("/var/log", root)) return false;
        size_t n = strlen(root);
        if (n + 6 >= sizeof root) return false;
        memcpy(root + n, "/fips", 6);
        /* The root may not yet exist. Only its literal descendants are allowed. */
        char parent[PATH_MAX]; strcpy(parent, root); *strrchr(parent, '/') = 0;
        if (!strcmp(resolved, parent))
            return !strncmp(dir, "/var/log/fips", 13) && (dir[13] == '/' || !dir[13]);
    }
    size_t n = strlen(root);
    return !strncmp(resolved, root, n) && (!resolved[n] || resolved[n] == '/');
}
static json_t *start(const char *dir, const char *node, uint64_t period)
{
    if (!dir[0]) return fipsd_control_error("cannot use empty profile directory");
    if (!confined(dir)) return fipsd_control_error("profile directory must be under /var/log/fips without '..' components");
    if (atomic_load(&state) == RUNNING) return fipsd_control_error("capture already running");
    reap(); active_path[0] = 0; atomic_store(&state, IDLE); atomic_store(&bytes, 0);
    if (mkdir_all(dir)) return fipsd_control_error("cannot use profile directory");
    char iso[32], stamp[32]; uint64_t now = (uint64_t)time(NULL);
    utc(now, iso, sizeof iso, stamp, sizeof stamp);
    int fd = -1;
    for (unsigned n = 0; n <= 16; ++n) {
        int len = n ? snprintf(active_path, sizeof active_path, "%s/profile-%s-%u.tsv", dir, stamp, n) :
            snprintf(active_path, sizeof active_path, "%s/profile-%s.tsv", dir, stamp);
        if (len < 0 || (size_t)len >= sizeof active_path) break;
        fd = open(active_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) { active_path[0] = 0; return fipsd_control_error("cannot create profile file"); }
    FILE *file = fdopen(fd, "w");
    if (!file) { close(fd); unlink(active_path); active_path[0] = 0; return fipsd_control_error("cannot open profile stream"); }
    int len = fipsd_instr_write_header(file, node, period, now);
    if (len < 0 || fflush(file)) {
        fclose(file); unlink(active_path); active_path[0] = 0;
        return fipsd_control_error("cannot write profile header");
    }
    fipsd_instr_reset(); atomic_store(&bytes, (unsigned)len);
    pthread_mutex_lock(&wake_mu); stop_requested = false; pthread_mutex_unlock(&wake_mu);
    atomic_store(&state, RUNNING);
    fipsd_instr_arm(true);
    if (pthread_create(&thread, NULL, write_run, file)) {
        fipsd_instr_arm(false);
        fclose(file); unlink(active_path); active_path[0] = 0;
        atomic_store(&state, IDLE); atomic_store(&bytes, 0);
        return fipsd_control_error("cannot start profile writer thread");
    }
    joinable = true;
    return fipsd_control_ok(json_pack("{s:s,s:s,s:i,s:i}", "state", "running", "path", active_path,
        "interval_secs", FIPSD_PROFILE_INTERVAL, "byte_cap", FIPSD_PROFILE_CAP));
}
static json_t *stop(void)
{
    if (atomic_load(&state) == IDLE)
        return fipsd_control_ok(json_pack("{s:s,s:b}", "state", "idle", "stopped", 0));
    fipsd_instr_arm(false); reap();
    int previous = atomic_load(&state);
    json_t *out = fipsd_control_ok(json_pack("{s:s,s:b,s:b,s:b,s:s,s:I}", "state", "idle", "stopped", 1,
        "stopped_by_cap", previous == STOPPED_BY_CAP, "stopped_by_error", previous == STOPPED_BY_ERROR,
        "path", active_path, "bytes", (json_int_t)atomic_load(&bytes)));
    active_path[0] = 0; atomic_store(&bytes, 0); atomic_store(&state, IDLE);
    return out;
}
json_t *fipsd_instr_command(const char *name, const json_t *params, const char *node, uint64_t period)
{
    if (strcmp(name, "profile_tick_on") && strcmp(name, "profile_tick_off") && strcmp(name, "profile_tick_status")) return NULL;
    pthread_mutex_lock(&mu);
    json_t *out;
    if (!strcmp(name, "profile_tick_on")) {
        const json_t *value = json_object_get(params, "dir");
        const char *dir = json_string_value(value);
        if (dir && strlen(dir) != json_string_length(value)) out = fipsd_control_error("NUL in profile directory");
        else out = start(dir ? dir : "/var/log/fips", node ? node : "", period);
    } else if (!strcmp(name, "profile_tick_off")) out = stop();
    else out = fipsd_control_ok(json_pack("{s:s,s:o,s:I,s:i,s:i}", "state", state_name(atomic_load(&state)),
        "path", active_path[0] ? json_string(active_path) : json_null(), "bytes", (json_int_t)atomic_load(&bytes),
        "byte_cap", FIPSD_PROFILE_CAP, "interval_secs", FIPSD_PROFILE_INTERVAL));
    pthread_mutex_unlock(&mu);
    return out;
}
void fipsd_instr_shutdown(void)
{
    pthread_mutex_lock(&mu); json_t *out = stop(); json_decref(out); pthread_mutex_unlock(&mu);
}
