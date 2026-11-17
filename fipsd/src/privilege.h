#ifndef FIPSD_PRIVILEGE_H
#define FIPSD_PRIVILEGE_H

#include <stddef.h>
#include <sys/types.h>
#if !defined(_WIN32)
#include <grp.h>
#endif

/*
 * Process privilege, umask and `fips` group handling.
 *
 * Ownership and modes follow upstream docs/reference/control-socket.md: the
 * control socket is group `fips`, mode 0770; a private parent directory the
 * daemon created, or a canonical runtime directory a service manager
 * pre-created (/run/fips, /var/run/fips, $XDG_RUNTIME_DIR/fips), is group
 * `fips`, mode 0750. Shared or custom parents such as /tmp are left alone;
 * deciding which directory qualifies is the control-socket owner's job
 * (nostrc-9keoz.3), these helpers only apply the ownership and mode.
 *
 * Capability expectations (docs/how-to/run-as-unprivileged-user.md): the
 * daemon runs as root by default. Unprivileged operation needs
 * CAP_NET_ADMIN on Linux for the TUN device (file capability on the binary,
 * `AmbientCapabilities=CAP_NET_ADMIN`, `NoNewPrivileges=no`), a `fips`
 * system user in the `fips` group, a config directory readable (and, for a
 * persistent identity, writable) by that user, and `RuntimeDirectory=fips`
 * so /run/fips exists as fips:fips 0750. Privilege dropping itself is
 * delegated to the service manager (`User=`/`Group=`) in this release; the
 * hook below exists so a later configuration key can take it over.
 */

#define FIPSD_SOCKET_GROUP "fips"
#define FIPSD_SOCKET_MODE 0770
#define FIPSD_SOCKET_DIR_MODE 0750
/* Files 0640, directories 0750 by default: nothing the daemon creates is
 * world-readable, and the explicit chmods above widen only what must be. */
#define FIPSD_UMASK 0027

/* Applies FIPSD_UMASK; returns the previous mask. Call before any thread. */
mode_t fipsd_privilege_set_umask(void);

/* Windows has no `fips` group or Unix socket modes; only the umask applies
 * (the CRT's, for files the daemon creates). */
#if !defined(_WIN32)

/* Returns 1 with the named gid, or 0 with getegid() and one WARN naming the
 * group when absent (0/NULL or ENOENT) or NSS fails after one immediate retry.
 * ERANGE grows the buffer up to 1 MiB before retry/fallback. Invalid names
 * (ASCII letter/underscore followed by letters, digits, '_', '-', '.', with
 * optional final '$', at most 255 bytes) and allocation failures return -1
 * with a message in err. Call during startup, not on scheduler workers. */
int fipsd_privilege_resolve_group(const char *name, gid_t *gid, char *err, size_t errlen);

/* Per-call resolver seam; same result/error convention as getgrnam_r.
 * No process-global resolver override or cached gid. */
typedef int (*fipsd_group_resolver)(void *ctx, const char *name, struct group *grp,
                                  char *buf, size_t cap, struct group **result);
int fipsd_privilege_resolve_group_with(const char *name, gid_t *gid, char *err, size_t errlen,
                                     fipsd_group_resolver resolve, void *ctx);

/* Sets the bound socket at `path` to group `gid` and FIPSD_SOCKET_MODE.
 * Refuses anything that is not a socket (symlinks included). Returns 0 or -1
 * with a message. The caller is the socket's owner, so chgrp succeeds for
 * root or for an owner that is a member of the group. */
int fipsd_privilege_secure_socket(const char *path, gid_t gid, char *err, size_t errlen);

/* Sets the directory at `path` to group `gid` and FIPSD_SOCKET_DIR_MODE,
 * operating on an opened descriptor (O_DIRECTORY | O_NOFOLLOW) so a swapped
 * path cannot redirect the change. Returns 0 or -1 with a message. */
int fipsd_privilege_secure_private_dir(const char *path, gid_t gid, char *err, size_t errlen);

typedef struct fipsd_privilege_drop_config {
    const char *user;  /* NULL: keep the current user */
    const char *group; /* NULL: keep the current group */
} fipsd_privilege_drop_config;

/* Privilege-drop hook guarded by configuration. With both fields NULL (the
 * only configuration this release produces) it is a no-op returning 0. A
 * request to switch identity returns -1 with a message naming the
 * service-manager route, rather than half-dropping. */
int fipsd_privilege_drop(const fipsd_privilege_drop_config *cfg, char *err, size_t errlen);
#endif /* !_WIN32 */

#endif /* FIPSD_PRIVILEGE_H */
