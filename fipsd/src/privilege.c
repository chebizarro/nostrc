#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1 /* O_NOFOLLOW, _SC_NPROCESSORS_ONLN on macOS */
#include "privilege.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(_WIN32)
#include <io.h> /* umask */
#else
#include <grp.h>
#endif

mode_t fipsd_privilege_set_umask(void)
{
    return umask(FIPSD_UMASK);
}

#if !defined(_WIN32)
static void set_err(char *err, size_t errlen, const char *what, const char *path, int e)
{
    if (err && errlen)
        snprintf(err, errlen, "%s %s: %s", what, path, strerror(e));
}

static int system_group_resolver(void *ctx, const char *name, struct group *grp,
                                 char *buf, size_t cap, struct group **result)
{
    (void)ctx;
    return getgrnam_r(name, grp, buf, cap, result);
}

static int valid_group_name(const char *name)
{
    if (!name || !((*name >= 'a' && *name <= 'z') ||
                   (*name >= 'A' && *name <= 'Z') || *name == '_'))
        return 0;
    size_t len = strlen(name);
    if (len > 255) return 0;
    for (size_t i = 1; i < len; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' ||
              (c == '$' && i == len - 1)))
            return 0;
    }
    return 1;
}

int fipsd_privilege_resolve_group_with(const char *name, gid_t *gid, char *err, size_t errlen,
                                     fipsd_group_resolver resolve, void *ctx)
{
    if (err && errlen)
        err[0] = '\0';
    if (!valid_group_name(name) || !gid || !resolve) {
        set_err(err, errlen, "cannot resolve group", name ? name : "(null)", EINVAL);
        errno = EINVAL;
        return -1;
    }
    long hint = sysconf(_SC_GETGR_R_SIZE_MAX);
    const size_t max_cap = 1u << 20;
    size_t cap = hint > 0 && (size_t)hint <= max_cap ? (size_t)hint : 1024;
    unsigned retries = 0;
    for (;;) {
        char *buf = malloc(cap);
        if (!buf) {
            set_err(err, errlen, "cannot resolve group", name, ENOMEM);
            return -1;
        }
        struct group grp, *result = NULL;
        int rc = resolve(ctx, name, &grp, buf, cap, &result);
        if (rc == ERANGE && cap < max_cap) {
            free(buf);
            cap = cap > max_cap / 2 ? max_cap : cap * 2;
            continue;
        }
        if (rc == 0 && result) {
            *gid = result->gr_gid;
            free(buf);
            return 1;
        }
        free(buf);
        if (rc == EINVAL || rc == ENOMEM) {
            set_err(err, errlen, "cannot resolve group", name, rc);
            errno = rc;
            return -1;
        }
        if (rc != 0 && rc != ENOENT && retries++ == 0) continue;
        *gid = getegid();
        FIPSD_WARN("privilege", "cannot resolve group '%s': %s; using effective group %u",
                   name, rc == 0 || rc == ENOENT ? "group not found" : strerror(rc),
                   (unsigned)*gid);
        return 0;
    }
}

int fipsd_privilege_resolve_group(const char *name, gid_t *gid, char *err, size_t errlen)
{
    return fipsd_privilege_resolve_group_with(name, gid, err, errlen, system_group_resolver, NULL);
}

int fipsd_privilege_secure_socket(const char *path, gid_t gid, char *err, size_t errlen)
{
    if (err && errlen)
        err[0] = '\0';
    struct stat st;
    if (lstat(path, &st) != 0) {
        set_err(err, errlen, "cannot stat socket", path, errno);
        return -1;
    }
    if (!S_ISSOCK(st.st_mode)) {
        set_err(err, errlen, "not a socket", path, S_ISLNK(st.st_mode) ? ELOOP : ENOTSOCK);
        return -1;
    }
    /* A socket path cannot be opened, so chown/chmod go by name; the lstat
     * above has already refused a symlink and the daemon owns the directory
     * (0750) in the canonical layouts. */
    if (st.st_gid != gid && lchown(path, (uid_t)-1, gid) != 0) {
        set_err(err, errlen, "cannot set group of socket", path, errno);
        return -1;
    }
    if (chmod(path, FIPSD_SOCKET_MODE) != 0) {
        set_err(err, errlen, "cannot set mode of socket", path, errno);
        return -1;
    }
    return 0;
}

int fipsd_privilege_secure_private_dir(const char *path, gid_t gid, char *err, size_t errlen)
{
    if (err && errlen)
        err[0] = '\0';
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        set_err(err, errlen, "cannot open directory", path, errno);
        return -1;
    }
    struct stat st;
    int rc = -1;
    if (fstat(fd, &st) != 0) {
        set_err(err, errlen, "cannot stat directory", path, errno);
    } else if (st.st_gid != gid && fchown(fd, (uid_t)-1, gid) != 0) {
        set_err(err, errlen, "cannot set group of directory", path, errno);
    } else if (fchmod(fd, FIPSD_SOCKET_DIR_MODE) != 0) {
        set_err(err, errlen, "cannot set mode of directory", path, errno);
    } else {
        rc = 0;
    }
    close(fd);
    return rc;
}

int fipsd_privilege_socket_parent(const char *path, gid_t gid, char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    char parent[PATH_MAX];
    if (!path || !*path) {
        errno = EINVAL;
        set_err(err, errlen, "invalid socket path", path ? path : "(null)", errno);
        return -1;
    }
    if (strlen(path) >= sizeof parent) {
        errno = ENAMETOOLONG;
        set_err(err, errlen, "socket path too long", path, errno);
        return -1;
    }
    memcpy(parent, path, strlen(path) + 1);
    char *slash = strrchr(parent, '/');
    if (!slash || slash == parent) return 0;
    *slash = 0;
    struct stat st;
    int rc = lstat(parent, &st);
    if (rc == 0 && !S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        set_err(err, errlen, "not a socket directory", parent, errno);
        return -1;
    }
    int created = rc != 0;
    if (created) {
        for (char *p = parent + (parent[0] == '/');; ++p) {
            if (*p != '/' && *p) continue;
            char saved = *p; *p = 0;
            int made = mkdir(parent, FIPSD_SOCKET_DIR_MODE);
            if (made != 0 && errno != EEXIST) {
                set_err(err, errlen, "cannot create directory", parent, errno);
                return -1;
            }
            /* Ancestors may be symlinks (macOS /var -> private/var), but
             * the private socket directory itself must be a real directory. */
            if ((saved ? stat(parent, &st) : lstat(parent, &st)) != 0) {
                set_err(err, errlen, "cannot stat directory", parent, errno);
                return -1;
            }
            if (!S_ISDIR(st.st_mode)) {
                errno = ENOTDIR;
                set_err(err, errlen, "not a directory", parent, errno);
                return -1;
            }
            /* A new intermediate directory also needs group traversal, even
             * under a restrictive caller umask. Never change existing ancestors. */
            if (made == 0 && saved && fipsd_privilege_secure_private_dir(parent, gid, err, errlen) != 0)
                return -1;
            *p = saved;
            if (!saved) break;
        }
    }
    char xdg[PATH_MAX] = "";
    const char *env = getenv("XDG_RUNTIME_DIR");
    if (env && *env) snprintf(xdg, sizeof xdg, "%s/fips", env);
    int canonical = !strcmp(parent, "/run/fips") || !strcmp(parent, "/var/run/fips") ||
        (*xdg && !strcmp(parent, xdg));
    return created || canonical ? fipsd_privilege_secure_private_dir(parent, gid, err, errlen) : 0;
}

int fipsd_privilege_drop(const fipsd_privilege_drop_config *cfg, char *err, size_t errlen)
{
    if (err && errlen)
        err[0] = '\0';
    if (!cfg || (!cfg->user && !cfg->group))
        return 0;
    if (err && errlen)
        snprintf(err, errlen,
                 "dropping privileges to %s:%s is not supported by this release; run the "
                 "daemon under the service manager's User=/Group= instead",
                 cfg->user ? cfg->user : "-", cfg->group ? cfg->group : "-");
    errno = ENOTSUP;
    return -1;
}
#endif /* !_WIN32 */
