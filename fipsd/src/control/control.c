#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "fiber_errno.h"
#include "control/control.h"
#include "log.h"
#include "transport/sockos.h"
#include <libgo/fiber.h>
#include <go.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#if defined(_WIN32)
#include <netinet/in.h>
#else
#include "privilege.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

int fipsd_control_resolve_path(const char *override, const fipsd_control_path_env *env,
                              char *out, size_t cap)
{
    const char *path = override;
    int n;
    if (path) n = snprintf(out, cap, "%s", path);
    else if (env->run_exists) n = snprintf(out, cap, "/run/fips/control.sock");
    else if ((env->macos || env->freebsd) && (env->var_run_exists || (env->macos && env->privileged)))
        n = snprintf(out, cap, "/var/run/fips/control.sock");
    else if (env->xdg_runtime_dir && *env->xdg_runtime_dir)
        n = snprintf(out, cap, "%s/fips/control.sock", env->xdg_runtime_dir);
    else n = snprintf(out, cap, "/tmp/fips-control.sock");
    if (n <= 0 || (size_t)n >= cap) { errno = ENAMETOOLONG; return -1; }
    return 0;
}
bool fipsd_control_resolve_port(const char *value, uint16_t *port)
{
    *port = FIPSD_CONTROL_DEFAULT_PORT;
    if (!value) return true;
    const char *p = value + (*value == '+');
    if (!*p) return false;
    uint32_t n = 0;
    for (; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        n = n * 10 + (uint32_t)(*p - '0');
        if (n > UINT16_MAX) return false;
    }
    *port = (uint16_t)n;
    return true;
}
#if !defined(_WIN32)
static bool is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}
static int unix_addr(const char *path, struct sockaddr_un *a)
{
    if (!*path || strlen(path) >= sizeof(a->sun_path)) { errno = ENAMETOOLONG; return -1; }
    memset(a, 0, sizeof(*a)); a->sun_family = AF_UNIX;
    memcpy(a->sun_path, path, strlen(path) + 1);
    return 0;
}
static int socket_new(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 || fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
        int e = errno; close(fd); errno = e; return -1;
    }
#ifdef SO_NOSIGPIPE
    int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) < 0) {
        int e = errno; close(fd); errno = e; return -1;
    }
#endif
    return fd;
}
/* Called on the starting thread, never on a scheduler worker. */
static int private_parent(const char *path, gid_t gid, char *err, size_t cap)
{
    char parent[PATH_MAX];
    snprintf(parent, sizeof(parent), "%s", path);
    char *slash = strrchr(parent, '/');
    if (!slash || slash == parent) return 0;
    *slash = 0;
    bool created = !is_dir(parent);
    /* Recursive creation, refusing symlink components we would create through.
     * Existing shared/custom parents are not chmod/chgrp'd. */
    if (created) {
        for (char *p = parent + (parent[0] == '/'); ; ++p) {
            if (*p != '/' && *p) continue;
            char saved = *p; *p = 0;
            if (mkdir(parent, FIPSD_SOCKET_DIR_MODE) != 0 && errno != EEXIST) return -1;
            struct stat st;
            if (lstat(parent, &st) != 0 || !S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
            *p = saved;
            if (!saved) break;
        }
    }
    char xdg[PATH_MAX] = "";
    const char *env = getenv("XDG_RUNTIME_DIR");
    if (env && *env) snprintf(xdg, sizeof(xdg), "%s/fips", env);
    bool canonical = !strcmp(parent, "/run/fips") || !strcmp(parent, "/var/run/fips") ||
        (*xdg && !strcmp(parent, xdg));
    if (created || canonical) return fipsd_privilege_secure_private_dir(parent, gid, err, cap);
    return 0;
}
static int bind_listener(fipsd_control_service *s)
{
    char err[512] = "";
    gid_t gid = getegid();
    int group = fipsd_privilege_resolve_group(FIPSD_SOCKET_GROUP, &gid, err, sizeof(err));
    if (group < 0) { FIPSD_ERROR("control", "%s", err); return -1; }
    if (private_parent(s->path, gid, err, sizeof(err)) != 0) {
        FIPSD_ERROR("control", "cannot secure socket parent: %s %s", err, strerror(errno)); return -1;
    }
    struct sockaddr_un address;
    if (unix_addr(s->path, &address) != 0) return -1;
    struct stat st;
    if (lstat(s->path, &st) == 0) {
        /* Do not remove arbitrary files or symlinks, even though upstream's
         * stale-path cleanup is broader. Never steal a live daemon's socket. */
        if (!S_ISSOCK(st.st_mode) || (st.st_uid != geteuid() && geteuid() != 0)) { errno = EEXIST; return -1; }
        int probe = socket_new();
        if (probe < 0) return -1;
        int rc = connect(probe, (struct sockaddr *)&address, sizeof(address));
        int e = errno; close(probe);
        if (rc == 0 || e != ECONNREFUSED) { errno = EADDRINUSE; return -1; }
        struct stat again;
        if (lstat(s->path, &again) != 0 || again.st_dev != st.st_dev || again.st_ino != st.st_ino) {
            errno = EEXIST; return -1;
        }
        if (unlink(s->path) != 0) return -1;
    } else if (errno != ENOENT) return -1;
    int fd = socket_new();
    if (fd < 0) return -1;
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0) { close(fd); return -1; }
    if (lstat(s->path, &st) != 0) { close(fd); return -1; }
    s->bound = true; s->socket_device = (uint64_t)st.st_dev; s->socket_inode = (uint64_t)st.st_ino;
    if (fipsd_privilege_secure_socket(s->path, gid, err, sizeof(err)) != 0 || listen(fd, 128) != 0) {
        FIPSD_ERROR("control", "cannot secure/listen: %s", err); close(fd); return -1;
    }
    return fd;
}
#else
/* Upstream's Windows control socket: TCP bound to 127.0.0.1 only, and every
 * accepted peer checked to be loopback. Exclusive, so no other socket of any
 * account can bind the port while the daemon holds it (Windows otherwise
 * lets SO_REUSEADDR take over a bound port). There is no ACL: any local user
 * can connect, as upstream documents. */
static gof_socket_t bind_listener(fipsd_control_service *s, uint16_t port)
{
    gof_socket_t fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == GOF_INVALID_SOCKET) { errno = gof_socket_errno(); return GOF_INVALID_SOCKET; }
    BOOL one = TRUE;
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    int len = (int)sizeof a;
    if (setsockopt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&one, (int)sizeof one) != 0 ||
        bind(fd, (struct sockaddr *)&a, (int)sizeof a) != 0 || listen(fd, 128) != 0 ||
        getsockname(fd, (struct sockaddr *)&a, &len) != 0) {
        errno = gof_socket_errno(); fipsd_socket_close(fd); return GOF_INVALID_SOCKET;
    }
    /* A SOCKET value is recycled at once on Windows: clear any cancellation
     * latched on the last socket that had it before this one is used. */
    if (fipsd_socket_prepare(fd) != 0 || gof_io_reset_fd(fd) != 0) {
        fipsd_socket_close(fd); return GOF_INVALID_SOCKET;
    }
    s->port = ntohs(a.sin_port);
    snprintf(s->path, sizeof s->path, "127.0.0.1:%u", (unsigned)s->port);
    s->bound = true;
    return fd;
}
static bool loopback_peer(const struct sockaddr_storage *peer)
{
    if (peer->ss_family == AF_INET)
        return (ntohl(((const struct sockaddr_in *)peer)->sin_addr.s_addr) >> 24) == 127;
    if (peer->ss_family == AF_INET6) {
        const uint8_t *b = ((const struct sockaddr_in6 *)peer)->sin6_addr.s6_addr;
        static const uint8_t loop[16] = {[15] = 1};
        static const uint8_t mapped[12] = {[10] = 0xff, [11] = 0xff};
        return !memcmp(b, loop, 16) || (!memcmp(b, mapped, 12) && b[12] == 127);
    }
    return false;
}
#endif

typedef struct connection {
    fipsd_control_service *service;
    nsync_mu mu; /* fd shutdown/close only; never held across gof I/O */
    gof_socket_t fd;
#if defined(_WIN32)
    bool reading; /* protected by mu: the request is still being read */
    int cancelled; /* protected by mu: 0, CANCEL_READ, or CANCEL_ALL */
#endif
    _Atomic unsigned references;
    _Atomic bool read_timeout, execution_timeout;
    GoChannel *phases; /* read, execution/encode, write complete (capacity 3) */
} connection;
#if defined(_WIN32)
enum { CANCEL_READ = 1, CANCEL_ALL = 2 };
#endif
static void release_connection(connection *c)
{
    if (atomic_fetch_sub(&c->references, 1) != 1) return;
    go_channel_free(c->phases);
    atomic_fetch_sub(&c->service->connections, 1);
    free(c);
}
/* POSIX: shutdown(2) ends a parked read or write. On Windows shutdown()
 * leaves a pending overlapped receive pending, so the socket's operations are
 * cancelled through libgo instead (ECANCELED, latched until reset). A read
 * deadline cancels only while the request is still being read, and the
 * connection fiber clears that latch before it writes the reply, so a read
 * timeout still answers "read timeout" as it does on POSIX. */
static void shutdown_connection(connection *c, int how)
{
    nsync_mu_lock(&c->mu);
#if defined(_WIN32)
    if (c->fd != GOF_INVALID_SOCKET && (how != SHUT_RD || c->reading)) {
        (void)gof_io_cancel_fd(c->fd);
        if (c->cancelled < (how == SHUT_RD ? CANCEL_READ : CANCEL_ALL))
            c->cancelled = how == SHUT_RD ? CANCEL_READ : CANCEL_ALL;
    }
#else
    if (c->fd >= 0) (void)shutdown(c->fd, how);
#endif
    nsync_mu_unlock(&c->mu);
}
/* gof_read/write have no deadlines. This companion selects completion against
 * cancellation and the actual upstream I/O budget, then shuts down (not closes)
 * the fd. The I/O owner alone closes it; reuse can never redirect a timeout.
 * These are protocol deadlines, not delay-based synchronization or polling. */
static void deadline_fiber(void *arg)
{
    connection *c = arg;
    GoContext *ctx = fipsd_runtime_context(c->service->runtime);
    for (unsigned phase = 0; phase < 3; ++phase) {
        void *token = NULL;
        GoSelectCase cases[] = {
            {.op=GO_SELECT_RECEIVE, .chan=c->phases, .recv_buf=&token},
            {.op=GO_SELECT_RECEIVE, .chan=go_context_done(ctx), .recv_buf=&token}
        };
        GoSelectResult r = go_select_timeout(cases, 2, FIPSD_CONTROL_IO_TIMEOUT_MS);
        if (r.selected_case == 1 || go_context_is_canceled(ctx)) { shutdown_connection(c, SHUT_RDWR); break; }
        if (r.selected_case == -1) {
            if (phase < 2) {
                if (phase == 0) {
                    atomic_store(&c->read_timeout, true); shutdown_connection(c, SHUT_RD);
                } else atomic_store(&c->execution_timeout, true);
                /* Execution providers select the root and their absolute
                 * deadline; shutting down a socket cannot interrupt a callback.
                 * Wait for that phase's acknowledgement before timing write. */
                if (go_select(cases, 2) != 0) { shutdown_connection(c, SHUT_RDWR); break; }
            } else { shutdown_connection(c, SHUT_RDWR); break; }
        }
    }
    release_connection(c);
}
static ssize_t write_socket(gof_socket_t fd, const char *data, size_t len)
{
#if defined(MSG_NOSIGNAL) && !defined(_WIN32)
    /* Linux has no SO_NOSIGPIPE. Use the equivalent fiber I/O wrapper rather
     * than changing the process-wide SIGPIPE disposition. */
    struct iovec iov = {.iov_base=(void *)data, .iov_len=len};
    struct msghdr msg = {.msg_iov=&iov, .msg_iovlen=1};
    return gof_sendmsg(fd, &msg, MSG_NOSIGNAL, -1);
#else
    return gof_write(fd, data, len);
#endif
}
static uint64_t monotonic_ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) abort();
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}
static void connection_fiber(void *arg)
{
    connection *c = arg;
    char line[FIPSD_CONTROL_MAX_REQUEST + 1];
    size_t len = 0;
    json_t *response = NULL;
    /* Read a byte at a time: bounded (4097 bytes), never consume a second
     * request. This also makes the 4096-byte newline-inclusive bound exact. */
    while (len < sizeof(line)) {
        ssize_t n = gof_read(c->fd, line + len, 1);
        if (n == 0) break;
        if (n < 0) {
            if (fipsd_io_errno() == EINTR) continue;
            response = fipsd_control_error("read error: socket read failed"); break;
        }
        if (line[len++] == '\n') break;
    }
#if defined(_WIN32)
    /* Reading is over: a read deadline can no longer cancel, and one that did
     * must not fail the reply. No operation is pending on the socket. */
    nsync_mu_lock(&c->mu);
    c->reading = false;
    if (c->cancelled == CANCEL_READ && gof_io_reset_fd(c->fd) == 0) c->cancelled = 0;
    nsync_mu_unlock(&c->mu);
#endif
    /* Reading is finished BEFORE executing any provider or JSON renderer. */
    go_channel_send(c->phases, c);
    if (atomic_load(&c->read_timeout)) { json_decref(response); response = fipsd_control_error("read timeout"); }
    fipsd_control_request_context request_context = {
        .shutdown=fipsd_runtime_context(c->service->runtime),
        .deadline_ns=monotonic_ns() + UINT64_C(5000000000)
    };
    if (!response) response = fipsd_control_parse_context(&c->service->provider, line, len, &request_context);
    if (atomic_load(&c->execution_timeout) || monotonic_ns() >= request_context.deadline_ns) {
        json_decref(response); response = fipsd_control_error("query timeout");
    }
    if (go_context_is_canceled(fipsd_runtime_context(c->service->runtime))) {
        json_decref(response); response = fipsd_control_error("node shutting down");
    }
    char *encoded = fipsd_control_encode(response);
    json_decref(response);
    if (atomic_load(&c->execution_timeout) || monotonic_ns() >= request_context.deadline_ns) {
        free(encoded);
        response = fipsd_control_error("query timeout");
        encoded = fipsd_control_encode(response); json_decref(response);
    }
    go_channel_send(c->phases, c); /* write deadline starts after execution/encode */
    if (encoded) {
        size_t size = strlen(encoded), sent = 0;
        /* Append the single delimiter in the same allocation. */
        encoded[size++] = '\n';
        while (sent < size) {
            ssize_t n = write_socket(c->fd, encoded + sent, size - sent);
            if (n < 0 && fipsd_io_errno() == EINTR) continue;
            if (n <= 0) break;
            sent += (size_t)n;
        }
        free(encoded);
    }
    go_channel_send(c->phases, c);
    nsync_mu_lock(&c->mu);
#if defined(_WIN32)
    /* An orderly close, so the client reads the reply and then end of file. */
    (void)shutdown(c->fd, SD_SEND);
#endif
    fipsd_socket_close(c->fd); c->fd = GOF_INVALID_SOCKET;
    nsync_mu_unlock(&c->mu);
    release_connection(c);
}
static void accept_fiber(void *arg)
{
    fipsd_control_service *s = arg;
    GoContext *ctx = fipsd_runtime_context(s->runtime);
    nsync_mu_lock(&s->listener_mu);
    s->accept_waiter = gof_hook_current();
    nsync_mu_unlock(&s->listener_mu);
    while (!go_context_is_canceled(ctx)) {
#if defined(_WIN32)
        struct sockaddr_storage peer;
        socklen_t peer_len = (socklen_t)sizeof peer;
        memset(&peer, 0, sizeof peer);
        gof_socket_t fd = gof_accept(s->listener, (struct sockaddr *)&peer, &peer_len, -1);
#else
        int fd = gof_accept(s->listener, NULL, NULL, -1);
#endif
        if (fd == GOF_INVALID_SOCKET) {
            if (fipsd_io_errno() == EINTR) continue;
            if (!go_context_is_canceled(ctx)) {
                FIPSD_ERROR("control", "accept failed: %s", strerror(fipsd_io_errno()));
                fipsd_runtime_shutdown(s->runtime);
            }
            break;
        }
        if (go_context_is_canceled(ctx)) { fipsd_socket_close(fd); break; }
#if defined(_WIN32)
        /* Bound to 127.0.0.1 so nothing else can reach it; checked anyway, as
         * upstream does. The accepted SOCKET may reuse a cancelled one's value. */
        if (!loopback_peer(&peer)) {
            FIPSD_WARN("control", "rejected non-localhost control connection");
            fipsd_socket_close(fd); continue;
        }
        if (gof_io_reset_fd(fd) != 0) { fipsd_socket_close(fd); continue; }
#else
        if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) { close(fd); continue; }
#ifdef SO_NOSIGPIPE
        int one = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) < 0) { close(fd); continue; }
#endif
#endif
        /* Only this accept fiber increments; both owners must exit before
         * release_connection returns the slot. Refuse before allocating stacks. */
        if (atomic_load(&s->connections) >= FIPSD_CONTROL_MAX_CONNECTIONS) { fipsd_socket_close(fd); continue; }
        connection *c = calloc(1, sizeof(*c));
        if (!c) { fipsd_socket_close(fd); continue; }
        c->fd = fd; c->service = s; atomic_init(&c->references, 2); atomic_init(&c->read_timeout, false); atomic_init(&c->execution_timeout, false);
#if defined(_WIN32)
        c->reading = true;
#endif
        c->phases = go_channel_create(3);
        if (!c->phases) { fipsd_socket_close(fd); free(c); continue; }
        atomic_fetch_add(&s->connections, 1);
        if (fipsd_runtime_spawn(s->runtime, deadline_fiber, c) != 0) {
            fipsd_socket_close(fd); release_connection(c); release_connection(c); continue;
        }
        if (fipsd_runtime_spawn(s->runtime, connection_fiber, c) != 0) {
            go_channel_send(c->phases, c); go_channel_send(c->phases, c); go_channel_send(c->phases, c);
            nsync_mu_lock(&c->mu); fipsd_socket_close(c->fd); c->fd = GOF_INVALID_SOCKET; nsync_mu_unlock(&c->mu);
            release_connection(c);
        }
    }
    nsync_mu_lock(&s->listener_mu);
    s->accept_waiter = NULL;
    fipsd_socket_close(s->listener); s->listener = GOF_INVALID_SOCKET;
    fipsd_socket_close(s->cancel_socket); s->cancel_socket = GOF_INVALID_SOCKET;
    nsync_mu_unlock(&s->listener_mu);
}
#if defined(_WIN32)
static int start(fipsd_subsystem *subsystem, fipsd_runtime *rt)
{
    fipsd_control_service *s = subsystem->user;
    s->runtime = rt;
    GoRefPtr config = go_swap_load(s->configs);
    const char *value = fipsd_config_string(fipsd_config_get(config.ptr, "node.control.socket_path"));
    uint16_t port;
    if (!fipsd_control_resolve_port(value, &port))
        FIPSD_WARN("control", "invalid control port '%s'; using the default %u", value, (unsigned)port);
    go_refptr_release(&config);
    s->listener = bind_listener(s, port);
    if (s->listener == GOF_INVALID_SOCKET) {
        FIPSD_ERROR("control", "cannot bind 127.0.0.1:%u: %s", (unsigned)port, strerror(errno)); return -1;
    }
    if (fipsd_runtime_spawn(rt, accept_fiber, s) != 0) {
        fipsd_socket_close(s->listener); s->listener = GOF_INVALID_SOCKET; return -1;
    }
    FIPSD_INFO("control", "control socket listening at %s", s->path);
    return 0;
}
static void stop(fipsd_subsystem *subsystem, fipsd_runtime *rt)
{
    (void)rt;
    fipsd_control_service *s = subsystem->user;
    /* Cancel the pending AcceptEx: the accept fiber's gof_accept returns
     * ECANCELED (latched, so a cancel that lands before the accept is issued
     * is honoured too) and the fiber closes the listener itself. */
    nsync_mu_lock(&s->listener_mu);
    if (s->listener != GOF_INVALID_SOCKET && gof_io_cancel_fd(s->listener) != 0)
        FIPSD_ERROR("control", "cannot cancel listener: %s", strerror(errno));
    nsync_mu_unlock(&s->listener_mu);
}
#else
static int start(fipsd_subsystem *subsystem, fipsd_runtime *rt)
{
    fipsd_control_service *s = subsystem->user;
    s->runtime = rt;
    GoRefPtr config = go_swap_load(s->configs);
    const char *override = fipsd_config_string(fipsd_config_get(config.ptr, "node.control.socket_path"));
    fipsd_control_path_env env = {.run_exists=is_dir("/run/fips"), .var_run_exists=is_dir("/var/run/fips"),
        .privileged=geteuid() == 0, .xdg_runtime_dir=getenv("XDG_RUNTIME_DIR")};
#if defined(__APPLE__)
    env.macos = true;
#elif defined(__FreeBSD__)
    env.freebsd = true;
#endif
    int rc = fipsd_control_resolve_path(override, &env, s->path, sizeof(s->path));
    go_refptr_release(&config);
    if (rc == 0) s->listener = bind_listener(s);
    if (rc != 0 || s->listener < 0) {
        FIPSD_ERROR("control", "cannot bind %s: %s", s->path, strerror(errno)); return -1;
    }
    s->cancel_socket = socket_new();
    if (s->cancel_socket < 0 || fipsd_runtime_spawn(rt, accept_fiber, s) != 0) {
        close(s->listener); s->listener = -1;
        if (s->cancel_socket >= 0) close(s->cancel_socket);
        s->cancel_socket = -1; return -1;
    }
    FIPSD_INFO("control", "control socket listening at %s", s->path);
    return 0;
}
static void stop(fipsd_subsystem *subsystem, fipsd_runtime *rt)
{
    (void)rt;
    fipsd_control_service *s = subsystem->user;
    /* Keep the descriptor SLOT reserved until accept returns. Replacing the
     * listening socket with our preallocated non-listening socket makes
     * gof_accept return an error on its next readiness retry, even if the
     * pathname was removed/replaced or the backlog is full. Unlike close(),
     * dup2 cannot let another subsystem reuse the fd under the accept fiber.
     * No filesystem lookup, allocation, host polling or lock across gof I/O. */
    nsync_mu_lock(&s->listener_mu);
    if (s->listener >= 0) {
        int rc;
        do { rc = dup2(s->cancel_socket, s->listener); } while (rc < 0 && errno == EINTR);
        if (rc < 0) FIPSD_ERROR("control", "cannot cancel listener: %s", strerror(errno));
        else {
            (void)fcntl(s->listener, F_SETFD, FD_CLOEXEC);
            /* dup2 removes the old poll registration without a readiness
             * event. Wake explicitly: indefinite gof_accept no longer retries
             * periodically. The scheduler retains a wake racing arm/park;
             * listener_mu keeps the handle alive until this wake completes. */
            if (s->accept_waiter) gof_hook_make_runnable(s->accept_waiter);
        }
    }
    nsync_mu_unlock(&s->listener_mu);
}
#endif
void fipsd_control_service_init(fipsd_control_service *s, GoSwap *configs, const fipsd_control_provider *provider)
{
    *s = (fipsd_control_service){.subsystem={"control", start, stop, s}, .configs=configs,
        .listener=GOF_INVALID_SOCKET, .cancel_socket=GOF_INVALID_SOCKET};
    if (provider) s->provider = *provider;
}
void fipsd_control_service_clear(fipsd_control_service *s)
{
    if (!s->bound) return;
#if !defined(_WIN32)
    struct stat st;
    if (lstat(s->path, &st) == 0 && S_ISSOCK(st.st_mode) && (uint64_t)st.st_dev == s->socket_device &&
        (uint64_t)st.st_ino == s->socket_inode) (void)unlink(s->path);
#endif
    s->bound = false;
}
