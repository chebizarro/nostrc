#ifndef FIPSD_LOG_H
#define FIPSD_LOG_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Structured daemon logging.
 *
 * One line per record: `<UTC RFC 3339 ms> <LEVEL> <module>: <message>`.
 * Records above the configured level are dropped before formatting. The
 * sink receives complete lines, serialized by the logger's mutex.
 *
 * Fiber contract: a record is formatted on the caller's stack without locks,
 * then the sink write runs under a short nsync mutex. The file sink performs
 * exactly one append(2) per line (plus a rename pair when it rolls), no
 * fsync and no retry loop, so a fiber is never blocked on a slow disk for
 * longer than a single write. Callers that cannot afford even that (hot
 * paths) must check fipsd_log_enabled() first and keep records terse.
 */

typedef enum fipsd_log_level {
    FIPSD_LOG_ERROR = 0,
    FIPSD_LOG_WARN,
    FIPSD_LOG_INFO,
    FIPSD_LOG_DEBUG,
    FIPSD_LOG_TRACE
} fipsd_log_level;

/* Defaults from upstream `node.log_max_size_mb` (10) / `node.log_max_files` (4). */
#define FIPSD_LOG_DEFAULT_MAX_BYTES (UINT64_C(10) << 20)
#define FIPSD_LOG_DEFAULT_MAX_FILES 4u
/* Clamps documented in configuration.md: size 0 raised to 1 MiB, files 0
 * raised to 1 and anything above 100 lowered to 100. */
#define FIPSD_LOG_MIN_MAX_BYTES (UINT64_C(1) << 20)
#define FIPSD_LOG_MAX_FILES_LIMIT 100u

/* Longest formatted line, including the trailing newline. Longer messages
 * are truncated and marked with "...". */
#define FIPSD_LOG_LINE_MAX 1024

/* Injectable output. `write` is called with the logger mutex held and one
 * complete line (terminated by '\n'); return 0 or -1 with errno. Failures are
 * ignored by the logger: a log write must never take the daemon down.
 * `close` (optional) runs once from fipsd_log_destroy(). */
typedef struct fipsd_log_sink {
    int (*write)(void *ctx, const char *line, size_t len);
    void (*close)(void *ctx);
    void *ctx;
} fipsd_log_sink;

typedef struct fipsd_log fipsd_log;
typedef struct fipsd_log_file fipsd_log_file;

/* NULL sink logs to stderr. */
fipsd_log *fipsd_log_create(fipsd_log_level level, const fipsd_log_sink *sink);
void fipsd_log_destroy(fipsd_log *log);

void fipsd_log_set_level(fipsd_log *log, fipsd_log_level level);
fipsd_log_level fipsd_log_get_level(const fipsd_log *log);
bool fipsd_log_enabled(const fipsd_log *log, fipsd_log_level level);

/* Case-insensitive `error|warn|warning|info|debug|trace`. */
bool fipsd_log_level_parse(const char *name, fipsd_log_level *out);
const char *fipsd_log_level_name(fipsd_log_level level);

void fipsd_log_write(fipsd_log *log, fipsd_log_level level, const char *module,
                     const char *fmt, ...) __attribute__((format(printf, 4, 5)));
void fipsd_log_vwrite(fipsd_log *log, fipsd_log_level level, const char *module,
                      const char *fmt, va_list ap);

/* Process-wide default logger used by the FIPSD_* macros. Until one is set
 * (or after it is cleared with NULL) a built-in stderr logger at INFO is used.
 * Set it before starting threads or fibers; the pointer is read atomically
 * but the logger it names must outlive every user. */
void fipsd_log_set_default(fipsd_log *log);
fipsd_log *fipsd_log_default(void);

#define FIPSD_LOG(lvl, module, ...)                                        \
    do {                                                                   \
        fipsd_log *fipsd_log_tmp_ = fipsd_log_default();                   \
        if (fipsd_log_enabled(fipsd_log_tmp_, (lvl)))                      \
            fipsd_log_write(fipsd_log_tmp_, (lvl), (module), __VA_ARGS__); \
    } while (0)
#define FIPSD_ERROR(module, ...) FIPSD_LOG(FIPSD_LOG_ERROR, module, __VA_ARGS__)
#define FIPSD_WARN(module, ...) FIPSD_LOG(FIPSD_LOG_WARN, module, __VA_ARGS__)
#define FIPSD_INFO(module, ...) FIPSD_LOG(FIPSD_LOG_INFO, module, __VA_ARGS__)
#define FIPSD_DEBUG(module, ...) FIPSD_LOG(FIPSD_LOG_DEBUG, module, __VA_ARGS__)
#define FIPSD_TRACE(module, ...) FIPSD_LOG(FIPSD_LOG_TRACE, module, __VA_ARGS__)

/*
 * Size-rolled log file, mirroring upstream `fips::utils::logfile`:
 *  - the live file keeps `path`; rolled files are `path.1` (newest) to
 *    `path.<max_files>` (oldest); nothing else in the directory is touched;
 *  - a write that would take a non-empty file past the limit rolls first:
 *    files numbered past max_files are deleted, `.n` -> `.n+1`, live -> `.1`,
 *    and the live file is recreated on the next write;
 *  - a roll that fails leaves the current file in use and retries after
 *    another max_bytes have been written;
 *  - writes are unbuffered appends, so the line logged before exit is on disk;
 *  - a symlink at `path` is refused (O_NOFOLLOW) rather than followed.
 * Missing parent directories are created (mode 0750). Bytes already in the
 * file count toward the cap. Limits are clamped as documented above.
 * The file is not internally locked: it is driven through a fipsd_log, whose
 * mutex serializes writes.
 */
fipsd_log_file *fipsd_log_file_open(const char *path, uint64_t max_bytes,
                                    unsigned max_files, char *err, size_t errlen);
void fipsd_log_file_set_limits(fipsd_log_file *file, uint64_t max_bytes, unsigned max_files);
const char *fipsd_log_file_path(const fipsd_log_file *file);
uint64_t fipsd_log_file_max_bytes(const fipsd_log_file *file);
unsigned fipsd_log_file_max_files(const fipsd_log_file *file);
/* Sink whose close() closes and frees the file. */
fipsd_log_sink fipsd_log_file_sink(fipsd_log_file *file);
/* Close without a logger owning it (e.g. after a failed logger creation). */
void fipsd_log_file_close(fipsd_log_file *file);

uint64_t fipsd_log_clamp_max_bytes(uint64_t max_bytes);
unsigned fipsd_log_clamp_max_files(unsigned max_files);

#endif /* FIPSD_LOG_H */
