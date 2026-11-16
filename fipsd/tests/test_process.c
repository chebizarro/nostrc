/* Drives the real fipsd executable. No sleeps: the parent reads the child's
 * output until the start-up line appears, or blocks SIGTERM in the child so a
 * stop sent immediately is held until the daemon's handler is installed. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "test_tmpdir.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s (errno %s)\n", __FILE__, __LINE__, #expr, strerror(errno)); return 1; \
} } while (0)

extern char **environ;

typedef struct child {
    pid_t pid;
    FILE *stderr_stream;
} child;

/* Spawns fipsd with argv, stderr on a pipe. When block_sigterm, the child
 * starts with SIGTERM blocked (posix_spawn sets the mask at exec). */
static int spawn(const char *fipsd, char *const argv[], int block_sigterm, child *out)
{
    int fds[2];
    if (pipe(fds) != 0)
        return -1;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, fds[0]);
    posix_spawn_file_actions_addclose(&actions, fds[1]);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t mask;
    sigemptyset(&mask);
    if (block_sigterm)
        sigaddset(&mask, SIGTERM);
    posix_spawnattr_setsigmask(&attr, &mask);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK);
    int rc = posix_spawn(&out->pid, fipsd, &actions, &attr, argv, environ);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);
    if (rc != 0) {
        close(fds[0]);
        errno = rc;
        return -1;
    }
    out->stderr_stream = fdopen(fds[0], "r");
    return out->stderr_stream ? 0 : -1;
}

/* Reads lines until one contains `until` (or EOF), appending to buf. */
static int read_until(child *c, const char *until, char *buf, size_t cap)
{
    size_t len = strlen(buf);
    char line[2048];
    while (fgets(line, sizeof(line), c->stderr_stream)) {
        size_t n = strlen(line);
        if (len + n < cap) {
            memcpy(buf + len, line, n + 1);
            len += n;
        }
        if (until && strstr(line, until))
            return 1;
    }
    return 0;
}

static int wait_exit(child *c, int *status)
{
    fclose(c->stderr_stream);
    int st;
    if (waitpid(c->pid, &st, 0) != c->pid)
        return -1;
    *status = st;
    return 0;
}

static int read_file(const char *path, char *buf, size_t cap)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    size_t n = fread(buf, 1, cap - 1, f);
    buf[n] = '\0';
    fclose(f);
    return 0;
}

static int test_sigterm_stderr(const char *fipsd, const char *config)
{
    child c;
    char out[16384] = "";
    CHECK(spawn(fipsd, (char *const[]){(char *)fipsd, "-c", (char *)config, NULL}, 0, &c) == 0);
    /* Event: the daemon reports it is running; only then stop it. */
    CHECK(read_until(&c, "fipsd running", out, sizeof(out)) == 1);
    CHECK(strstr(out, "fipsd ") && strstr(out, " starting (libfips-core "));
    CHECK(strstr(out, "fiber runtime started: 1 worker(s)") != NULL); /* FIPSD_WORKERS=1 */
    CHECK(strstr(out, "configuration loaded") != NULL);
    CHECK(kill(c.pid, SIGTERM) == 0);
    read_until(&c, NULL, out, sizeof(out));
    int status;
    CHECK(wait_exit(&c, &status) == 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(strstr(out, "SIGTERM received; shutting down") != NULL);
    CHECK(strstr(out, "runtime stopped") != NULL);
    CHECK(strstr(out, "fipsd shutdown complete (exit 0)") != NULL);
    /* Log lines are shaped `<timestamp> LEVEL module: ...` */
    CHECK(strstr(out, "Z INFO  main: fipsd running") != NULL);
    return 0;
}

static int test_sigint_config_flag(const char *fipsd, const char *dir)
{
    child c;
    char out[16384] = "";
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/reload.yaml", dir);
    int fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
    CHECK(fd >= 0);
    char yaml[PATH_MAX + 128];
    int len = snprintf(yaml, sizeof(yaml),
        "node: {identity: {persistent: true}, control: {socket_path: '%s/control.sock'}}\n", dir);
    CHECK(len > 0 && (size_t)len < sizeof(yaml));
    CHECK(write(fd, yaml, (size_t)len) == (ssize_t)len);
    CHECK(close(fd) == 0);
    CHECK(spawn(fipsd, (char *const[]){(char *)fipsd, "-c", path, NULL}, 0, &c) == 0);
    CHECK(read_until(&c, "fipsd running", out, sizeof(out)) == 1);
    CHECK(strstr(out, "configuration loaded") != NULL);
    FILE *config = fopen(path, "w");
    CHECK(config != NULL);
    CHECK(fputs("node: {identity: {persistent: true}, log_max_files: 7, limits: {max_peers: 999}}\n", config) >= 0);
    CHECK(fclose(config) == 0);
    CHECK(kill(c.pid, SIGHUP) == 0);
    out[0] = 0;
    CHECK(read_until(&c, "configuration reload complete", out, sizeof(out)) == 1);
    /* Refusal warnings are replayed after the completion notification. */
    CHECK(read_until(&c, "requires restart; keeping active value key=node.limits.max_peers", out, sizeof(out)) == 1);
    CHECK(strstr(out, "reloaded configuration key=node.log_max_files") != NULL);
    config = fopen(path, "w");
    CHECK(config != NULL);
    CHECK(fputs("node: [\n", config) >= 0 && fclose(config) == 0);
    CHECK(kill(c.pid, SIGHUP) == 0);
    out[0] = 0;
    CHECK(read_until(&c, "configuration reload failed; keeping active snapshot", out, sizeof(out)) == 1);
    CHECK(unlink(path) == 0);
    CHECK(kill(c.pid, SIGINT) == 0);
    read_until(&c, NULL, out, sizeof(out));
    int status;
    CHECK(wait_exit(&c, &status) == 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(strstr(out, "SIGINT received; shutting down") != NULL);
    return 0;
}

static int test_log_file(const char *fipsd, const char *config)
{
    const char *dir = fipsd_test_tmpdir(NULL);
    CHECK(dir != NULL);
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/logs/fipsd.log", dir);

    /* SIGTERM is blocked at exec and sent at once: the kernel holds it until
     * fipsd unblocks the handled signals right after installing its handler,
     * so the stop is always observed by the handler, never the default action. */
    child c;
    char out[16384] = "";
    CHECK(spawn(fipsd, (char *const[]){(char *)fipsd, "-c", (char *)config, "--log-file", path, NULL}, 1, &c) == 0);
    CHECK(kill(c.pid, SIGTERM) == 0);
    read_until(&c, NULL, out, sizeof(out));
    int status;
    CHECK(wait_exit(&c, &status) == 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(strstr(out, " INFO  ") == NULL); /* nothing logged to stderr */

    char log[16384];
    CHECK(read_file(path, log, sizeof(log)) == 0);
    CHECK(strstr(log, "logging to file ") != NULL && strstr(log, path) != NULL);
    CHECK(strstr(log, "max 10485760 bytes, 4 rolled files kept") != NULL);
    CHECK(strstr(log, " starting (libfips-core ") != NULL);
    CHECK(strstr(log, "fipsd running") != NULL);
    CHECK(strstr(log, "SIGTERM received; shutting down") != NULL);
    CHECK(strstr(log, "fipsd shutdown complete (exit 0)") != NULL);
    /* The log file precedes everything else in the output. */
    CHECK(strstr(log, "logging to file ") < strstr(log, " starting (libfips-core "));
    struct stat st;
    CHECK(stat(path, &st) == 0 && (st.st_mode & 07777) == 0640);

    /* An unopenable log file is fatal with exit 1, before anything runs. */
    char bad[PATH_MAX];
    snprintf(bad, sizeof(bad), "%s/logs/fipsd.log/child.log", dir);
    out[0] = '\0';
    CHECK(spawn(fipsd, (char *const[]){(char *)fipsd, "-c", (char *)config, "--log-file", bad, NULL}, 0, &c) == 0);
    read_until(&c, NULL, out, sizeof(out));
    CHECK(wait_exit(&c, &status) == 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 1);
    CHECK(strstr(out, "fipsd: cannot open log file ") != NULL && strstr(out, bad) != NULL);
    CHECK(strstr(out, "fipsd running") == NULL);

    out[0] = 0;
    char missing[PATH_MAX];
    snprintf(missing, sizeof(missing), "%s/missing.yaml", dir);
    CHECK(spawn(fipsd, (char *const[]){(char *)fipsd, "--log-file", path, "-c", missing, NULL}, 0, &c) == 0);
    read_until(&c, NULL, out, sizeof(out));
    CHECK(wait_exit(&c, &status) == 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 1);
    CHECK(read_file(path, out, sizeof(out)) == 0);
    CHECK(strstr(out, "Failed to load configuration from ") && strstr(out, missing));

    CHECK(unlink(path) == 0);
    snprintf(path, sizeof(path), "%s/logs", dir);
    CHECK(rmdir(path) == 0 && rmdir(dir) == 0);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fputs("usage: fipsd_process_test <path-to-fipsd>\n", stderr);
        return 2;
    }
    const char *fipsd = argv[1];
    setenv("FIPSD_WORKERS", "1", 1);
    const char *dir = fipsd_test_tmpdir("/control.sock");
    char config[PATH_MAX];
    snprintf(config, sizeof(config), "%s/default.yaml", dir);
    FILE *file = fopen(config, "w");
    CHECK(file != NULL);
    CHECK(fprintf(file, "node: {identity: {persistent: true}, control: {socket_path: '%s/control.sock'}}\n", dir) > 0);
    CHECK(fclose(file) == 0);
    int rc = test_sigterm_stderr(fipsd, config);
    if (rc == 0)
        rc = test_sigint_config_flag(fipsd, dir);
    if (rc == 0)
        rc = test_log_file(fipsd, config);
    return rc;
}
