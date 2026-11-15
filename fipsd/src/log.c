#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1 /* O_NOFOLLOW, _SC_NPROCESSORS_ONLN on macOS */
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <nsync.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct fipsd_log {
    nsync_mu mu;              /* serializes sink writes */
    _Atomic int level;
    fipsd_log_sink sink;
};

/* ── sinks ─────────────────────────────────────────────────────────────── */

static int write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

static int stderr_sink_write(void *ctx, const char *line, size_t len)
{
    (void)ctx;
    return write_all(STDERR_FILENO, line, len);
}

static const fipsd_log_sink stderr_sink = {stderr_sink_write, NULL, NULL};

/* nsync_mu zero-initialized is a valid unlocked mutex, so this fallback
 * needs no constructor and is usable from the first FIPSD_* macro. */
static fipsd_log fallback_log = {{0}, FIPSD_LOG_INFO, {stderr_sink_write, NULL, NULL}};
static _Atomic(fipsd_log *) default_log = NULL;

/* ── logger ────────────────────────────────────────────────────────────── */

fipsd_log *fipsd_log_create(fipsd_log_level level, const fipsd_log_sink *sink)
{
    fipsd_log *log = calloc(1, sizeof(*log));
    if (!log)
        return NULL;
    nsync_mu_init(&log->mu);
    atomic_store(&log->level, (int)level);
    log->sink = sink && sink->write ? *sink : stderr_sink;
    return log;
}

void fipsd_log_destroy(fipsd_log *log)
{
    if (!log)
        return;
    fipsd_log *expected = log;
    atomic_compare_exchange_strong(&default_log, &expected, NULL);
    if (log->sink.close)
        log->sink.close(log->sink.ctx);
    free(log);
}

void fipsd_log_set_level(fipsd_log *log, fipsd_log_level level)
{
    atomic_store(&log->level, (int)level);
}

fipsd_log_level fipsd_log_get_level(const fipsd_log *log)
{
    return (fipsd_log_level)atomic_load(&log->level);
}

bool fipsd_log_enabled(const fipsd_log *log, fipsd_log_level level)
{
    return log && (int)level <= atomic_load_explicit(&log->level, memory_order_relaxed);
}

static const char *const level_names[] = {"ERROR", "WARN", "INFO", "DEBUG", "TRACE"};

const char *fipsd_log_level_name(fipsd_log_level level)
{
    return level <= FIPSD_LOG_TRACE ? level_names[level] : "?";
}

bool fipsd_log_level_parse(const char *name, fipsd_log_level *out)
{
    if (!name || !out)
        return false;
    for (int i = 0; i <= FIPSD_LOG_TRACE; i++) {
        if (strcasecmp(name, level_names[i]) == 0) {
            *out = (fipsd_log_level)i;
            return true;
        }
    }
    if (strcasecmp(name, "warning") == 0) {
        *out = FIPSD_LOG_WARN;
        return true;
    }
    return false;
}

static size_t format_timestamp(char *buf, size_t cap)
{
    struct timespec ts;
    struct tm tm;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0 || !gmtime_r(&ts.tv_sec, &tm))
        return (size_t)snprintf(buf, cap, "0000-00-00T00:00:00.000Z");
    size_t n = strftime(buf, cap, "%Y-%m-%dT%H:%M:%S", &tm);
    if (n == 0)
        return 0;
    n += (size_t)snprintf(buf + n, cap - n, ".%03ldZ", ts.tv_nsec / 1000000L);
    return n;
}

void fipsd_log_vwrite(fipsd_log *log, fipsd_log_level level, const char *module,
                      const char *fmt, va_list ap)
{
    if (!fipsd_log_enabled(log, level))
        return;
    char line[FIPSD_LOG_LINE_MAX];
    size_t n = format_timestamp(line, sizeof(line));
    int h = snprintf(line + n, sizeof(line) - n, " %-5s %s: ",
                     fipsd_log_level_name(level), module ? module : "fipsd");
    if (h < 0)
        return;
    n += (size_t)h;
    if (n >= sizeof(line) - 1)
        n = sizeof(line) - 2;
    int m = vsnprintf(line + n, sizeof(line) - 1 - n, fmt, ap);
    if (m < 0)
        m = 0;
    if ((size_t)m >= sizeof(line) - 1 - n) {
        /* Truncated: keep the marker inside the buffer, newline after it. */
        n = sizeof(line) - 2;
        memcpy(line + n - 3, "...", 3);
    } else {
        n += (size_t)m;
    }
    line[n++] = '\n';

    /* Brief critical section: one sink write. Never spans a gof_* call. */
    nsync_mu_lock(&log->mu);
    (void)log->sink.write(log->sink.ctx, line, n);
    nsync_mu_unlock(&log->mu);
}

void fipsd_log_write(fipsd_log *log, fipsd_log_level level, const char *module,
                     const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fipsd_log_vwrite(log, level, module, fmt, ap);
    va_end(ap);
}

void fipsd_log_set_default(fipsd_log *log)
{
    atomic_store(&default_log, log);
}

fipsd_log *fipsd_log_default(void)
{
    fipsd_log *log = atomic_load_explicit(&default_log, memory_order_acquire);
    return log ? log : &fallback_log;
}

/* ── rolling file ──────────────────────────────────────────────────────── */

struct fipsd_log_file {
    char *path;
    int fd;           /* -1 between a roll and the next write */
    uint64_t len;     /* bytes in the live file */
    uint64_t max;     /* configured cap */
    uint64_t limit;   /* cap in force; raised after a failed roll */
    unsigned keep;
};

uint64_t fipsd_log_clamp_max_bytes(uint64_t max_bytes)
{
    return max_bytes < FIPSD_LOG_MIN_MAX_BYTES ? FIPSD_LOG_MIN_MAX_BYTES : max_bytes;
}

unsigned fipsd_log_clamp_max_files(unsigned max_files)
{
    if (max_files < 1)
        return 1;
    return max_files > FIPSD_LOG_MAX_FILES_LIMIT ? FIPSD_LOG_MAX_FILES_LIMIT : max_files;
}

static void set_err(char *err, size_t errlen, const char *what, const char *path)
{
    if (err && errlen)
        snprintf(err, errlen, "%s %s: %s", what, path, strerror(errno));
}

static int mkdir_parents(const char *path)
{
    char buf[PATH_MAX];
    size_t len = strlen(path);
    if (len >= sizeof(buf)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(buf, path, len + 1);
    char *slash = strrchr(buf, '/');
    if (!slash || slash == buf)
        return 0; /* no parent, or the root */
    *slash = '\0';
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(buf, 0750) != 0 && errno != EEXIST)
            return -1;
        *p = '/';
    }
    if (mkdir(buf, 0750) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

/* Open the live file for appending and learn its current length. */
static int file_reopen(fipsd_log_file *file)
{
    int fd = open(file->path, O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0640);
    if (fd < 0)
        return -1;
    struct stat st;
    if (fstat(fd, &st) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    file->fd = fd;
    file->len = (uint64_t)st.st_size;
    return 0;
}

static int numbered(const fipsd_log_file *file, unsigned n, char *buf, size_t cap)
{
    int w = snprintf(buf, cap, "%s.%u", file->path, n);
    if (w < 0 || (size_t)w >= cap) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int file_roll(fipsd_log_file *file)
{
    char from[PATH_MAX], to[PATH_MAX];
    if (file->fd >= 0) {
        close(file->fd);
        file->fd = -1;
    }
    /* Delete files numbered past keep (a lowered limit leaves them behind);
     * stop at the first gap, since rolls only ever leave a contiguous run. */
    for (unsigned n = file->keep + 1;; n++) {
        if (numbered(file, n, from, sizeof(from)) != 0 || unlink(from) != 0)
            break;
    }
    for (unsigned n = file->keep - 1; n >= 1; n--) {
        if (numbered(file, n, from, sizeof(from)) != 0
            || numbered(file, n + 1, to, sizeof(to)) != 0)
            return -1;
        if (rename(from, to) != 0 && errno != ENOENT)
            return -1;
    }
    if (numbered(file, 1, to, sizeof(to)) != 0 || rename(file->path, to) != 0)
        return -1;
    file->len = 0;
    file->limit = file->max;
    return 0;
}

static int file_append(fipsd_log_file *file, const char *buf, size_t len)
{
    if (file->fd < 0 && file_reopen(file) != 0)
        return -1;
    if (write_all(file->fd, buf, len) != 0)
        return -1;
    file->len += len;
    return 0;
}

static int file_sink_write(void *ctx, const char *line, size_t len)
{
    fipsd_log_file *file = ctx;
    if (file->len > 0 && file->len + len > file->limit && file_roll(file) != 0) {
        /* Keep the current file; try rolling again after another cap's worth. */
        int rc = file_append(file, line, len);
        file->limit = file->len + file->max;
        return rc;
    }
    return file_append(file, line, len);
}

static void file_sink_close(void *ctx)
{
    fipsd_log_file_close(ctx);
}

fipsd_log_file *fipsd_log_file_open(const char *path, uint64_t max_bytes,
                                    unsigned max_files, char *err, size_t errlen)
{
    if (err && errlen)
        err[0] = '\0';
    if (!path || !*path) {
        errno = EINVAL;
        set_err(err, errlen, "cannot open log file", path ? path : "(null)");
        return NULL;
    }
    fipsd_log_file *file = calloc(1, sizeof(*file));
    if (!file) {
        set_err(err, errlen, "cannot open log file", path);
        return NULL;
    }
    file->path = strdup(path);
    file->fd = -1;
    file->max = fipsd_log_clamp_max_bytes(max_bytes);
    file->limit = file->max;
    file->keep = fipsd_log_clamp_max_files(max_files);
    if (!file->path || mkdir_parents(path) != 0 || file_reopen(file) != 0) {
        set_err(err, errlen, "cannot open log file", path);
        fipsd_log_file_close(file);
        return NULL;
    }
    return file;
}

void fipsd_log_file_set_limits(fipsd_log_file *file, uint64_t max_bytes, unsigned max_files)
{
    file->max = fipsd_log_clamp_max_bytes(max_bytes);
    file->limit = file->max;
    file->keep = fipsd_log_clamp_max_files(max_files);
}

const char *fipsd_log_file_path(const fipsd_log_file *file)
{
    return file->path;
}

uint64_t fipsd_log_file_max_bytes(const fipsd_log_file *file)
{
    return file->max;
}

unsigned fipsd_log_file_max_files(const fipsd_log_file *file)
{
    return file->keep;
}

fipsd_log_sink fipsd_log_file_sink(fipsd_log_file *file)
{
    return (fipsd_log_sink){file_sink_write, file_sink_close, file};
}

void fipsd_log_file_close(fipsd_log_file *file)
{
    if (!file)
        return;
    if (file->fd >= 0)
        close(file->fd);
    free(file->path);
    free(file);
}

void fipsd_log_file_apply_limits(fipsd_log *log, fipsd_log_file *file, uint64_t max_bytes, unsigned max_files)
{
    nsync_mu_lock(&log->mu);
    fipsd_log_file_set_limits(file,max_bytes,max_files);
    nsync_mu_unlock(&log->mu);
}
