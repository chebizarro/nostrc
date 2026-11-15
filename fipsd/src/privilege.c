#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1 /* O_NOFOLLOW, _SC_NPROCESSORS_ONLN on macOS */
#include "privilege.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void set_err(char *err, size_t errlen, const char *what, const char *path, int e)
{
    if (err && errlen)
        snprintf(err, errlen, "%s %s: %s", what, path, strerror(e));
}

mode_t fipsd_privilege_set_umask(void)
{
    return umask(FIPSD_UMASK);
}

int fipsd_privilege_resolve_group(const char *name, gid_t *gid, char *err, size_t errlen)
{
    if (err && errlen)
        err[0] = '\0';
    if (!name || !*name || !gid) {
        set_err(err, errlen, "cannot resolve group", name ? name : "(null)", EINVAL);
        return -1;
    }
    long hint = sysconf(_SC_GETGR_R_SIZE_MAX);
    size_t cap = hint > 0 ? (size_t)hint : 1024;
    for (;;) {
        char *buf = malloc(cap);
        if (!buf) {
            set_err(err, errlen, "cannot resolve group", name, ENOMEM);
            return -1;
        }
        struct group grp, *result = NULL;
        int rc = getgrnam_r(name, &grp, buf, cap, &result);
        if (rc == ERANGE && cap < (1u << 20)) {
            free(buf);
            cap *= 2;
            continue;
        }
        if (rc != 0) {
            free(buf);
            set_err(err, errlen, "cannot resolve group", name, rc);
            return -1;
        }
        if (!result) {
            free(buf);
            return 0;
        }
        *gid = result->gr_gid;
        free(buf);
        return 1;
    }
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
