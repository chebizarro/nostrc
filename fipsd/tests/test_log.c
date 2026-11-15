#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1 /* mkdtemp on macOS */
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s (errno %s)\n", __FILE__, __LINE__, #expr, strerror(errno)); return 1; \
} } while (0)

/* ── capture sink ──────────────────────────────────────────────────────── */

typedef struct capture {
    char lines[16][FIPSD_LOG_LINE_MAX];
    size_t count;
    int closed;
} capture;

static int capture_write(void *ctx, const char *line, size_t len)
{
    capture *c = ctx;
    if (c->count >= 16 || len >= FIPSD_LOG_LINE_MAX)
        return -1;
    memcpy(c->lines[c->count], line, len);
    c->lines[c->count][len] = '\0';
    c->count++;
    return 0;
}

static void capture_close(void *ctx)
{
    ((capture *)ctx)->closed++;
}

/* `2026-11-15T12:34:56.789Z LEVEL module: message\n` */
static int well_formed(const char *line, const char *level, const char *module, const char *msg)
{
    if (strlen(line) < 25 || line[4] != '-' || line[7] != '-' || line[10] != 'T'
        || line[13] != ':' || line[16] != ':' || line[19] != '.' || line[23] != 'Z'
        || line[24] != ' ')
        return 0;
    for (int i = 0; i < 23; i++)
        if (i != 4 && i != 7 && i != 10 && i != 13 && i != 16 && i != 19
            && (line[i] < '0' || line[i] > '9'))
            return 0;
    char expect[FIPSD_LOG_LINE_MAX];
    snprintf(expect, sizeof(expect), " %-5s %s: %s\n", level, module, msg);
    return strcmp(line + 24, expect) == 0;
}

static int test_levels_and_format(void)
{
    capture cap;
    memset(&cap, 0, sizeof(cap));
    fipsd_log_sink sink = {capture_write, capture_close, &cap};
    fipsd_log *log = fipsd_log_create(FIPSD_LOG_INFO, &sink);
    CHECK(log != NULL);
    CHECK(fipsd_log_get_level(log) == FIPSD_LOG_INFO);
    CHECK(fipsd_log_enabled(log, FIPSD_LOG_ERROR) && fipsd_log_enabled(log, FIPSD_LOG_INFO));
    CHECK(!fipsd_log_enabled(log, FIPSD_LOG_DEBUG) && !fipsd_log_enabled(log, FIPSD_LOG_TRACE));
    CHECK(!fipsd_log_enabled(NULL, FIPSD_LOG_ERROR));

    fipsd_log_write(log, FIPSD_LOG_ERROR, "test", "boom %d", 1);
    fipsd_log_write(log, FIPSD_LOG_WARN, "test", "careful");
    fipsd_log_write(log, FIPSD_LOG_INFO, "test", "hello %s", "world");
    fipsd_log_write(log, FIPSD_LOG_DEBUG, "test", "hidden");
    fipsd_log_write(log, FIPSD_LOG_TRACE, "test", "hidden");
    CHECK(cap.count == 3);
    CHECK(well_formed(cap.lines[0], "ERROR", "test", "boom 1"));
    CHECK(well_formed(cap.lines[1], "WARN", "test", "careful"));
    CHECK(well_formed(cap.lines[2], "INFO", "test", "hello world"));

    fipsd_log_set_level(log, FIPSD_LOG_TRACE);
    fipsd_log_write(log, FIPSD_LOG_DEBUG, "mod", "d");
    fipsd_log_write(log, FIPSD_LOG_TRACE, NULL, "t");
    CHECK(cap.count == 5);
    CHECK(well_formed(cap.lines[3], "DEBUG", "mod", "d"));
    CHECK(well_formed(cap.lines[4], "TRACE", "fipsd", "t"));

    fipsd_log_set_level(log, FIPSD_LOG_ERROR);
    fipsd_log_write(log, FIPSD_LOG_WARN, "mod", "dropped");
    CHECK(cap.count == 5);

    /* Truncation keeps the line bounded and marked. */
    fipsd_log_set_level(log, FIPSD_LOG_INFO);
    char big[4096];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    fipsd_log_write(log, FIPSD_LOG_INFO, "mod", "%s", big);
    CHECK(cap.count == 6);
    size_t len = strlen(cap.lines[5]);
    CHECK(len == FIPSD_LOG_LINE_MAX - 1);
    CHECK(strcmp(cap.lines[5] + len - 4, "...\n") == 0);

    /* Default logger and macros. */
    fipsd_log *fallback = fipsd_log_default();
    CHECK(fallback != NULL && fallback != log);
    fipsd_log_set_default(log);
    CHECK(fipsd_log_default() == log);
    FIPSD_DEBUG("macro", "dropped at info");
    FIPSD_WARN("macro", "kept %d", 7);
    CHECK(cap.count == 7);
    CHECK(well_formed(cap.lines[6], "WARN", "macro", "kept 7"));
    fipsd_log_destroy(log); /* also clears the default it was */
    CHECK(cap.closed == 1);
    CHECK(fipsd_log_default() == fallback);

    fipsd_log_level lvl;
    CHECK(fipsd_log_level_parse("Debug", &lvl) && lvl == FIPSD_LOG_DEBUG);
    CHECK(fipsd_log_level_parse("WARNING", &lvl) && lvl == FIPSD_LOG_WARN);
    CHECK(fipsd_log_level_parse("warn", &lvl) && lvl == FIPSD_LOG_WARN);
    CHECK(fipsd_log_level_parse("trace", &lvl) && lvl == FIPSD_LOG_TRACE);
    CHECK(fipsd_log_level_parse("error", &lvl) && lvl == FIPSD_LOG_ERROR);
    CHECK(!fipsd_log_level_parse("loud", &lvl));
    CHECK(!fipsd_log_level_parse(NULL, &lvl));
    CHECK(strcmp(fipsd_log_level_name(FIPSD_LOG_INFO), "INFO") == 0);

    CHECK(fipsd_log_clamp_max_bytes(0) == FIPSD_LOG_MIN_MAX_BYTES);
    CHECK(fipsd_log_clamp_max_bytes(FIPSD_LOG_DEFAULT_MAX_BYTES) == FIPSD_LOG_DEFAULT_MAX_BYTES);
    CHECK(fipsd_log_clamp_max_files(0) == 1);
    CHECK(fipsd_log_clamp_max_files(500) == FIPSD_LOG_MAX_FILES_LIMIT);
    CHECK(fipsd_log_clamp_max_files(4) == 4);
    return 0;
}

/* ── rolling file ──────────────────────────────────────────────────────── */

static int exists(const char *dir, const char *name, off_t *size)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    struct stat st;
    if (lstat(path, &st) != 0)
        return 0;
    if (size)
        *size = st.st_size;
    return 1;
}

/* First decimal number in the file's first line, or -1. */
static long first_seq(const char *dir, const char *name)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    char line[FIPSD_LOG_LINE_MAX];
    long seq = -1;
    if (fgets(line, sizeof(line), f)) {
        char *p = strstr(line, "seq=");
        if (p)
            seq = strtol(p + 4, NULL, 10);
    }
    fclose(f);
    return seq;
}

static long write_until_bytes(fipsd_log *log, long seq, uint64_t total)
{
    /* Exactly 1000-byte lines: 24 (timestamp) + 13 (" INFO  roll: ") +
     * 11 ("seq=NNNNNN ") + 951 (pad) + 1 (newline). */
    char pad[952];
    memset(pad, 'p', sizeof(pad) - 1);
    pad[sizeof(pad) - 1] = '\0';
    for (uint64_t written = 0; written < total; seq++) {
        fipsd_log_write(log, FIPSD_LOG_INFO, "roll", "seq=%06ld %s", seq, pad);
        written += 1000;
    }
    return seq;
}

static int test_rolling(void)
{
    char tmpl[] = "/tmp/fipsd-log-test.XXXXXX";
    char *dir = mkdtemp(tmpl);
    CHECK(dir != NULL);
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/nested/dir/fipsd.log", dir);
    char err[512];

    /* Limits are clamped: 0 bytes -> 1 MiB, 2 files kept. */
    fipsd_log_file *file = fipsd_log_file_open(path, 0, 2, err, sizeof(err));
    CHECK(file != NULL);
    CHECK(fipsd_log_file_max_bytes(file) == FIPSD_LOG_MIN_MAX_BYTES);
    CHECK(fipsd_log_file_max_files(file) == 2);
    CHECK(strcmp(fipsd_log_file_path(file), path) == 0);
    fipsd_log_sink sink = fipsd_log_file_sink(file);
    fipsd_log *log = fipsd_log_create(FIPSD_LOG_INFO, &sink);
    CHECK(log != NULL);

    char nested[PATH_MAX];
    snprintf(nested, sizeof(nested), "%s/nested/dir", dir);
    const uint64_t cap = FIPSD_LOG_MIN_MAX_BYTES;

    /* Less than one cap: a single live file, nothing rolled. */
    long seq = write_until_bytes(log, 0, cap / 2);
    off_t size = 0;
    CHECK(exists(nested, "fipsd.log", &size) && size > 0 && (uint64_t)size <= cap);
    CHECK(!exists(nested, "fipsd.log.1", NULL));

    /* Past the cap: live -> .1, live recreated; every file within the cap. */
    seq = write_until_bytes(log, seq, cap);
    CHECK(exists(nested, "fipsd.log", &size) && (uint64_t)size <= cap);
    CHECK(exists(nested, "fipsd.log.1", &size) && (uint64_t)size <= cap && size > 0);
    CHECK(!exists(nested, "fipsd.log.2", NULL));

    /* Two more caps: .1 -> .2, .2 (oldest) dropped beyond keep=2. */
    seq = write_until_bytes(log, seq, 2 * cap);
    CHECK(exists(nested, "fipsd.log", &size) && (uint64_t)size <= cap);
    CHECK(exists(nested, "fipsd.log.1", &size) && (uint64_t)size <= cap);
    CHECK(exists(nested, "fipsd.log.2", &size) && (uint64_t)size <= cap);
    CHECK(!exists(nested, "fipsd.log.3", NULL));
    /* Newest-first numbering: .2 is older than .1, which is older than live. */
    long live = first_seq(nested, "fipsd.log"), one = first_seq(nested, "fipsd.log.1"),
         two = first_seq(nested, "fipsd.log.2");
    CHECK(two >= 0 && one > two && live > one);

    /* Lowering keep deletes the files numbered past it on the next roll. */
    fipsd_log_file_set_limits(file, 0, 1);
    seq = write_until_bytes(log, seq, cap + cap / 4);
    CHECK(exists(nested, "fipsd.log", NULL));
    CHECK(exists(nested, "fipsd.log.1", NULL));
    CHECK(!exists(nested, "fipsd.log.2", NULL));
    fipsd_log_destroy(log); /* closes the file through the sink */

    /* Reopening counts existing bytes toward the cap. */
    file = fipsd_log_file_open(path, 0, 1, err, sizeof(err));
    CHECK(file != NULL);
    sink = fipsd_log_file_sink(file);
    log = fipsd_log_create(FIPSD_LOG_INFO, &sink);
    CHECK(log != NULL);
    CHECK(exists(nested, "fipsd.log", &size));
    long before = first_seq(nested, "fipsd.log");
    write_until_bytes(log, seq, cap - (uint64_t)size + 1000);
    CHECK(first_seq(nested, "fipsd.log") > before); /* rolled: new live file */
    CHECK(first_seq(nested, "fipsd.log.1") == before);
    fipsd_log_destroy(log);

    /* A symlink at the path is refused. */
    char link[PATH_MAX], target[PATH_MAX];
    snprintf(link, sizeof(link), "%s/link.log", dir);
    snprintf(target, sizeof(target), "%s/target.log", dir);
    CHECK(symlink(target, link) == 0);
    CHECK(fipsd_log_file_open(link, 0, 1, err, sizeof(err)) == NULL);
    CHECK(strstr(err, "cannot open log file") != NULL && strstr(err, link) != NULL);
    CHECK(!exists(dir, "target.log", NULL));

    /* A parent that cannot be created is an error with the path in it. */
    char blocked[PATH_MAX];
    snprintf(blocked, sizeof(blocked), "%s/link.log/child.log", dir);
    CHECK(fipsd_log_file_open(blocked, 0, 1, err, sizeof(err)) == NULL);
    CHECK(strstr(err, "child.log") != NULL);
    CHECK(fipsd_log_file_open("", 0, 1, err, sizeof(err)) == NULL);

    /* Cleanup. */
    const char *names[] = {"fipsd.log", "fipsd.log.1", "fipsd.log.2", "fipsd.log.3"};
    for (size_t i = 0; i < 4; i++) {
        char p[PATH_MAX];
        snprintf(p, sizeof(p), "%s/%s", nested, names[i]);
        unlink(p);
    }
    unlink(link);
    CHECK(rmdir(nested) == 0);
    snprintf(nested, sizeof(nested), "%s/nested", dir);
    CHECK(rmdir(nested) == 0);
    CHECK(rmdir(dir) == 0);
    return 0;
}

int main(void)
{
    int rc = test_levels_and_format();
    if (rc)
        return rc;
    return test_rolling();
}
