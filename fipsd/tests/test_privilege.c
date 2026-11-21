#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1 /* mkdtemp on macOS */
#include "test_tmpdir.h"
#include "privilege.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s (errno %s)\n", __FILE__, __LINE__, #expr, strerror(errno)); return 1; \
} } while (0)

static int mode_of(const char *path, mode_t *mode, gid_t *gid)
{
    struct stat st;
    if (lstat(path, &st) != 0)
        return -1;
    *mode = st.st_mode;
    *gid = st.st_gid;
    return 0;
}

typedef struct resolver_case {
    int codes[3];
    unsigned calls, found_at;
    size_t caps[32];
    int bad_name;
} resolver_case;

static int resolve_fake(void *ctx, const char *name, struct group *grp,
                        char *buf, size_t cap, struct group **result)
{
    resolver_case *c = ctx;
    unsigned i = c->calls++;
    if (strcmp(name, "test-fips")) c->bad_name = 1;
    if (i < 32) c->caps[i] = cap;
    memset(buf, 0, cap);
    *result = NULL;
    int rc = c->codes[i < 3 ? i : 2];
    if (!rc && c->calls == c->found_at) {
        grp->gr_gid = 1234;
        *result = grp;
    }
    return rc;
}

typedef struct warnings {
    unsigned count;
    char line[FIPSD_LOG_LINE_MAX];
} warnings;

static int capture_warning(void *ctx, const char *line, size_t len)
{
    warnings *w = ctx;
    ++w->count;
    snprintf(w->line, sizeof(w->line), "%.*s", (int)len, line);
    return 0;
}

static int test_resolver(void)
{
    warnings w = {0};
    fipsd_log_sink sink = {.write = capture_warning, .ctx = &w};
    fipsd_log *log = fipsd_log_create(FIPSD_LOG_WARN, &sink);
    CHECK(log);
    fipsd_log_set_default(log);
    struct {
        int codes[3];
        unsigned found_at, calls;
        int outcome;
        const char *warning;
    } cases[] = {
        {{0}, 1, 1, 1, NULL},
        {{0}, 0, 1, 0, "group not found"},
        {{ENOENT}, 0, 1, 0, "group not found"},
        {{EIO, 0}, 2, 2, 1, NULL},
        {{EIO, EIO}, 0, 2, 0, strerror(EIO)},
        {{EAGAIN, EAGAIN}, 0, 2, 0, strerror(EAGAIN)},
        {{ERANGE, ERANGE, 0}, 3, 3, 1, NULL},
        {{EIO, ENOENT}, 0, 2, 0, "group not found"},
        {{EINVAL}, 0, 1, -1, NULL},
        {{ENOMEM}, 0, 1, -1, NULL},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        resolver_case c = {.found_at = cases[i].found_at};
        memcpy(c.codes, cases[i].codes, sizeof(c.codes));
        memset(&w, 0, sizeof(w));
        char err[256] = "stale";
        gid_t gid = (gid_t)-1;
        CHECK(fipsd_privilege_resolve_group_with("test-fips", &gid, err, sizeof(err),
                                                resolve_fake, &c) == cases[i].outcome);
        CHECK(c.calls == cases[i].calls && !c.bad_name);
        CHECK(w.count == (cases[i].warning ? 1u : 0u));
        if (cases[i].outcome >= 0) {
            CHECK(gid == (cases[i].outcome ? 1234 : getegid()) && !err[0]);
        } else CHECK(err[0] && gid == (gid_t)-1);
        if (cases[i].warning) {
            CHECK(strstr(w.line, "WARN") && strstr(w.line, "test-fips"));
            CHECK(strstr(w.line, cases[i].warning) && strstr(w.line, "using effective group"));
        }
        if (c.codes[0] == ERANGE) {
            CHECK(c.caps[1] == c.caps[0] * 2 && c.caps[2] == c.caps[1] * 2);
        }
    }
    /* Persistent ERANGE is bounded, then gets one final retry at the cap. */
    resolver_case c = {.codes = {ERANGE, ERANGE, ERANGE}};
    char err[256]; gid_t gid = (gid_t)-1;
    memset(&w, 0, sizeof(w));
    CHECK(fipsd_privilege_resolve_group_with("test-fips", &gid, err, sizeof(err),
                                            resolve_fake, &c) == 0);
    CHECK(c.calls >= 2 && c.calls <= 32 && gid == getegid());
    CHECK(c.caps[c.calls - 1] == (1u << 20) && c.caps[c.calls - 2] == (1u << 20));
    CHECK(w.count == 1 && strstr(w.line, strerror(ERANGE)));

    const char *invalid[] = {NULL, "", "bad group", "bad/group", ":garbage", "-fips", "fips\n"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        c.calls = 0;
        CHECK(fipsd_privilege_resolve_group_with(invalid[i], &gid, err, sizeof(err),
                                                resolve_fake, &c) == -1);
        CHECK(errno == EINVAL && err[0] && c.calls == 0);
    }
    fipsd_log_set_default(NULL);
    fipsd_log_destroy(log);
    return 0;
}

static int test_socket_parent(void)
{
    const char *dir = fipsd_test_tmpdir("/link/made/fips/control.sock");
    char real[PATH_MAX], link[PATH_MAX], path[PATH_MAX], made[PATH_MAX];
    char err[512];
    snprintf(real, sizeof real, "%s/real", dir);
    snprintf(link, sizeof link, "%s/link", dir);
    CHECK(mkdir(real, 0700) == 0 && symlink(real, link) == 0);
    snprintf(path, sizeof path, "%s/link/made/fips/control.sock", dir);
    mode_t mask = umask(0077);
    int rc = fipsd_privilege_socket_parent(path, getegid(), err, sizeof err);
    umask(mask);
    CHECK(rc == 0 && !err[0]);
    snprintf(made, sizeof made, "%s/real/made/fips", dir);
    mode_t mode; gid_t gid;
    CHECK(mode_of(made, &mode, &gid) == 0 && S_ISDIR(mode) &&
          (mode & 0777) == FIPSD_SOCKET_DIR_MODE && gid == getegid());
    /* Existing custom parents stay untouched, including shared directories. */
    CHECK(chmod(made, 0711) == 0);
    CHECK(fipsd_privilege_socket_parent(path, getegid(), err, sizeof err) == 0);
    CHECK(mode_of(made, &mode, &gid) == 0 && (mode & 0777) == 0711);
    char *old_xdg = getenv("XDG_RUNTIME_DIR") ? strdup(getenv("XDG_RUNTIME_DIR")) : NULL;
    char runtime[PATH_MAX]; snprintf(runtime, sizeof runtime, "%s/real/made", dir);
    CHECK(setenv("XDG_RUNTIME_DIR", runtime, 1) == 0);
    snprintf(path, sizeof path, "%s/real/made/fips/api.sock", dir);
    CHECK(fipsd_privilege_socket_parent(path, getegid(), err, sizeof err) == 0);
    CHECK(mode_of(made, &mode, &gid) == 0 && (mode & 0777) == FIPSD_SOCKET_DIR_MODE);
    if (old_xdg) { CHECK(setenv("XDG_RUNTIME_DIR", old_xdg, 1) == 0); free(old_xdg); }
    else CHECK(unsetenv("XDG_RUNTIME_DIR") == 0);
    /* Ancestors may be links; the socket directory itself may not. */
    snprintf(path, sizeof path, "%s/link/control.sock", dir);
    CHECK(fipsd_privilege_socket_parent(path, getegid(), err, sizeof err) == -1 && errno == ENOTDIR && err[0]);
    CHECK(fipsd_privilege_socket_parent("control.sock", getegid(), err, sizeof err) == 0 && !err[0]);
    CHECK(fipsd_privilege_socket_parent("/control.sock", getegid(), err, sizeof err) == 0);
    CHECK(fipsd_privilege_socket_parent(NULL, getegid(), err, sizeof err) == -1 && errno == EINVAL);
    char too_long[PATH_MAX + 1]; memset(too_long, 'x', sizeof too_long); too_long[PATH_MAX] = 0;
    CHECK(fipsd_privilege_socket_parent(too_long, getegid(), err, sizeof err) == -1 && errno == ENAMETOOLONG);
    CHECK(rmdir(made) == 0);
    snprintf(made, sizeof made, "%s/real/made", dir);
    CHECK(mode_of(made, &mode, &gid) == 0 && (mode & 0777) == FIPSD_SOCKET_DIR_MODE && gid == getegid());
    CHECK(rmdir(made) == 0);
    CHECK(mode_of(real, &mode, &gid) == 0 && (mode & 0777) == 0700);
    CHECK(unlink(link) == 0);
    /* A non-directory ancestor must fail with a useful diagnostic. */
    int fd = open(link, O_CREAT | O_EXCL | O_WRONLY, 0600); CHECK(fd >= 0); close(fd);
    snprintf(path, sizeof path, "%s/link/missing/api.sock", dir);
    CHECK(fipsd_privilege_socket_parent(path, getegid(), err, sizeof err) == -1 && errno == ENOTDIR && err[0]);
    CHECK(unlink(link) == 0 && rmdir(real) == 0 && rmdir(dir) == 0);
    return 0;
}

int main(void)
{
    char err[512];
    CHECK(test_resolver() == 0);
    CHECK(test_socket_parent() == 0);

    /* umask: applied and reported. */
    mode_t before = umask(0022);
    umask(before);
    CHECK(fipsd_privilege_set_umask() == before);
    CHECK(umask(before) == FIPSD_UMASK);

    /* Group lookup. */
    gid_t gid = (gid_t)-1;
    CHECK(fipsd_privilege_resolve_group("fipsd-no-such-group-xyz", &gid, err, sizeof(err)) == 0);
    CHECK(gid == getegid() && !err[0]);
    CHECK(fipsd_privilege_resolve_group("", &gid, err, sizeof(err)) == -1 && err[0] != '\0');
    CHECK(fipsd_privilege_resolve_group(NULL, &gid, err, sizeof(err)) == -1);
    gid_t fips_gid = (gid_t)-1;
    int have_fips = fipsd_privilege_resolve_group(FIPSD_SOCKET_GROUP, &fips_gid, err, sizeof(err));
    CHECK(have_fips >= 0);

    /* Temp layout: <dir>/run/control.sock, parent 0700 to start. */
    const char *dir = fipsd_test_tmpdir("/run/control.sock");
    CHECK(dir != NULL);
    char run[PATH_MAX], sock[PATH_MAX], link[PATH_MAX], file[PATH_MAX];
    snprintf(run, sizeof(run), "%s/run", dir);
    snprintf(sock, sizeof(sock), "%s/run/control.sock", dir);
    snprintf(link, sizeof(link), "%s/run/link.sock", dir);
    snprintf(file, sizeof(file), "%s/run/plain", dir);
    CHECK(mkdir(run, 0700) == 0);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    CHECK(strlen(sock) < sizeof(addr.sun_path));
    strcpy(addr.sun_path, sock);
    CHECK(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);

    /* Own primary group: always permitted, so the mechanics are always tested. */
    gid_t own = getegid();
    mode_t mode;
    gid_t got;
    CHECK(fipsd_privilege_secure_private_dir(run, own, err, sizeof(err)) == 0);
    CHECK(mode_of(run, &mode, &got) == 0 && S_ISDIR(mode));
    CHECK((mode & 07777) == FIPSD_SOCKET_DIR_MODE && got == own);
    CHECK(fipsd_privilege_secure_socket(sock, own, err, sizeof(err)) == 0);
    CHECK(mode_of(sock, &mode, &got) == 0 && S_ISSOCK(mode));
    CHECK((mode & 07777) == FIPSD_SOCKET_MODE && got == own);

    /* The `fips` group, when this machine has it and we may assign it. */
    if (have_fips == 1) {
        int rc = fipsd_privilege_secure_socket(sock, fips_gid, err, sizeof(err));
        if (rc == 0) {
            CHECK(mode_of(sock, &mode, &got) == 0 && got == fips_gid);
            CHECK(fipsd_privilege_secure_private_dir(run, fips_gid, err, sizeof(err)) == 0);
            CHECK(mode_of(run, &mode, &got) == 0 && got == fips_gid);
            printf("fips group %u: socket and directory assigned\n", (unsigned)fips_gid);
        } else {
            printf("fips group %u exists but cannot be assigned by this user (%s); "
                   "group-assignment check skipped\n", (unsigned)fips_gid, err);
        }
    } else {
        printf("no `fips` group on this machine; group-assignment check skipped\n");
    }

    /* Refusals: symlink, regular file, missing path, missing directory. */
    CHECK(symlink(sock, link) == 0);
    CHECK(fipsd_privilege_secure_socket(link, own, err, sizeof(err)) == -1);
    CHECK(strstr(err, "not a socket") != NULL);
    int pfd = open(file, O_WRONLY | O_CREAT | O_EXCL, 0600);
    CHECK(pfd >= 0);
    close(pfd);
    CHECK(fipsd_privilege_secure_socket(file, own, err, sizeof(err)) == -1);
    CHECK(strstr(err, "not a socket") != NULL);
    CHECK(fipsd_privilege_secure_socket(file, own, NULL, 0) == -1); /* no buffer */
    char missing[sizeof run + sizeof "/missing"];
    snprintf(missing, sizeof(missing), "%s/missing", run);
    CHECK(fipsd_privilege_secure_socket(missing, own, err, sizeof(err)) == -1);
    CHECK(fipsd_privilege_secure_private_dir(file, own, err, sizeof(err)) == -1);
    CHECK(fipsd_privilege_secure_private_dir(link, own, err, sizeof(err)) == -1);
    CHECK(fipsd_privilege_secure_private_dir(missing, own, err, sizeof(err)) == -1);
    /* The socket itself is unchanged by the refused calls. */
    CHECK(mode_of(sock, &mode, &got) == 0 && (mode & 07777) == FIPSD_SOCKET_MODE);

    /* Drop hook: a no-op unless configured; configured is refused whole. */
    CHECK(fipsd_privilege_drop(NULL, err, sizeof(err)) == 0);
    fipsd_privilege_drop_config none = {NULL, NULL};
    CHECK(fipsd_privilege_drop(&none, err, sizeof(err)) == 0 && err[0] == '\0');
    fipsd_privilege_drop_config to_fips = {"fips", "fips"};
    CHECK(fipsd_privilege_drop(&to_fips, err, sizeof(err)) == -1 && errno == ENOTSUP);
    CHECK(strstr(err, "fips:fips") != NULL && strstr(err, "User=/Group=") != NULL);
    fipsd_privilege_drop_config group_only = {NULL, "fips"};
    CHECK(fipsd_privilege_drop(&group_only, err, sizeof(err)) == -1);
    CHECK(strstr(err, "-:fips") != NULL);

    close(fd);
    CHECK(unlink(link) == 0 && unlink(file) == 0 && unlink(sock) == 0);
    CHECK(rmdir(run) == 0 && rmdir(dir) == 0);
    return 0;
}
