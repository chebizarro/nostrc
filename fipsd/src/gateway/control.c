#ifdef __linux__
#define _GNU_SOURCE 1
#else
#define _DARWIN_C_SOURCE 1
#endif
#include "fiber_errno.h"
#include "gateway/control.h"
#include "control/protocol.h"
#include "log.h"
#include "privilege.h"
#include <fips/identity/node_addr.h>
#include <libgo/fiber.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define MODULE "gateway"
#define MAX_CONNECTIONS 32u
#define MAX_DEPTH 64u

fipsd_gw_snapshot *fipsd_gw_snapshot_build(const fipsd_gw_pool_status *status, fipsd_gw_mapping_info *mappings, size_t nmappings,
                                           size_t nat_mappings, uint64_t uptime_secs, const fipsd_gw_snapshot_config *config)
{
    fipsd_gw_snapshot *s = calloc(1, sizeof *s);
    if (!s) { free(mappings); return NULL; }
    atomic_init(&s->refs, 1);
    s->pool = *status; s->mappings = mappings; s->nmappings = nmappings; s->nat_mappings = nat_mappings;
    s->uptime_secs = uptime_secs;
    snprintf(s->dns_listen, sizeof s->dns_listen, "%s", config->dns_listen ? config->dns_listen : "");
    snprintf(s->pool_cidr, sizeof s->pool_cidr, "%s", config->pool_cidr ? config->pool_cidr : "");
    snprintf(s->lan_interface, sizeof s->lan_interface, "%s", config->lan_interface ? config->lan_interface : "");
    snprintf(s->dns_upstream, sizeof s->dns_upstream, "%s", config->dns_upstream ? config->dns_upstream : "");
    s->dns_ttl = config->dns_ttl; s->pool_grace_period = config->pool_grace_period;
    return s;
}
void fipsd_gw_snapshot_release(fipsd_gw_snapshot *s)
{
    if (!s || atomic_fetch_sub(&s->refs, 1) != 1) return;
    free(s->mappings); free(s);
}
void fipsd_gw_snapshot_slot_init(fipsd_gw_snapshot_slot *slot) { nsync_mu_init(&slot->mu); slot->current = NULL; }
void fipsd_gw_snapshot_publish(fipsd_gw_snapshot_slot *slot, fipsd_gw_snapshot *s)
{
    nsync_mu_lock(&slot->mu);
    fipsd_gw_snapshot *old = slot->current;
    slot->current = s;
    nsync_mu_unlock(&slot->mu);
    fipsd_gw_snapshot_release(old);
}
fipsd_gw_snapshot *fipsd_gw_snapshot_acquire(fipsd_gw_snapshot_slot *slot)
{
    nsync_mu_lock(&slot->mu);
    fipsd_gw_snapshot *s = slot->current;
    if (s) atomic_fetch_add(&s->refs, 1);
    nsync_mu_unlock(&slot->mu);
    return s;
}
void fipsd_gw_snapshot_slot_clear(fipsd_gw_snapshot_slot *slot) { fipsd_gw_snapshot_publish(slot, NULL); }

const char *fipsd_gw_mapping_state_str(fipsd_gw_mapping_state state)
{
    switch (state) {
    case FIPSD_GW_MAPPING_ALLOCATED: return "Allocated";
    case FIPSD_GW_MAPPING_ACTIVE: return "Active";
    case FIPSD_GW_MAPPING_DRAINING: return "Draining";
    }
    return "Allocated";
}
static json_t *uint_json(uint64_t v) { return json_integer(v > (uint64_t)INT64_MAX ? (json_int_t)INT64_MAX : (json_int_t)v); }
json_t *fipsd_gw_control_show_gateway(const fipsd_gw_snapshot *s)
{
    json_t *d = json_object();
    if (!d) return NULL;
    json_object_set_new(d, "pool_total", uint_json(s->pool.total));
    json_object_set_new(d, "pool_allocated", uint_json(s->pool.allocated));
    json_object_set_new(d, "pool_active", uint_json(s->pool.active));
    json_object_set_new(d, "pool_draining", uint_json(s->pool.draining));
    json_object_set_new(d, "pool_free", uint_json(s->pool.free));
    json_object_set_new(d, "pool_evicted", uint_json(s->pool.evicted));
    json_object_set_new(d, "pool_releasing", uint_json(s->pool.releasing));
    json_object_set_new(d, "pool_refused", uint_json(s->pool.refused));
    json_object_set_new(d, "nat_mappings", uint_json(s->nat_mappings));
    json_object_set_new(d, "dns_listen", json_string(s->dns_listen));
    json_object_set_new(d, "uptime_secs", uint_json(s->uptime_secs));
    json_object_set_new(d, "pool_cidr", json_string(s->pool_cidr));
    json_object_set_new(d, "lan_interface", json_string(s->lan_interface));
    json_object_set_new(d, "dns_upstream", json_string(s->dns_upstream));
    json_object_set_new(d, "dns_ttl", uint_json(s->dns_ttl));
    json_object_set_new(d, "pool_grace_period", uint_json(s->pool_grace_period));
    return fipsd_control_ok(d);
}
json_t *fipsd_gw_control_show_mappings(const fipsd_gw_snapshot *s)
{
    json_t *list = json_array();
    if (!list) return NULL;
    for (size_t i = 0; i < s->nmappings; ++i) {
        const fipsd_gw_mapping_info *m = &s->mappings[i];
        char vip[48], mesh[48], node[40] = "";
        (void)fips_node_addr_format(&m->node_addr, node, sizeof node);
        json_t *row = json_object();
        json_object_set_new(row, "virtual_ip", json_string(fipsd_gw_ip6_text(&m->virtual_ip, vip, sizeof vip)));
        json_object_set_new(row, "mesh_addr", json_string(fipsd_gw_ip6_text(&m->mesh_addr, mesh, sizeof mesh)));
        json_object_set_new(row, "node_addr", json_string(node));
        json_object_set_new(row, "dns_name", json_string(m->dns_name));
        json_object_set_new(row, "state", json_string(fipsd_gw_mapping_state_str(m->state)));
        json_object_set_new(row, "sessions", uint_json(m->session_count));
        json_object_set_new(row, "age_secs", uint_json(m->age_secs));
        json_object_set_new(row, "last_ref_secs", uint_json(m->last_ref_secs));
        json_object_set_new(row, "used", json_boolean(m->used));
        json_array_append_new(list, row);
    }
    return fipsd_control_ok(json_pack("{s:o}", "mappings", list));
}
json_t *fipsd_gw_control_dispatch(const char *command, const fipsd_gw_snapshot *s)
{
    if (!s) return fipsd_control_error("gateway not yet initialized");
    if (!strcmp(command, "show_gateway")) return fipsd_gw_control_show_gateway(s);
    if (!strcmp(command, "show_mappings")) return fipsd_gw_control_show_mappings(s);
    size_t len = strlen(command) + 32;
    char *msg = malloc(len);
    if (!msg) return NULL;
    snprintf(msg, len, "unknown command: %s", command);
    json_t *r = fipsd_control_error(msg);
    free(msg);
    return r;
}
/* Bound recursion before Jansson parses on a small fiber stack, as the
 * daemon does: containers outside strings are counted. */
static bool bounded_depth(const char *line, size_t len)
{
    size_t depth = 0;
    bool quoted = false, escaped = false;
    for (size_t i = 0; i < len; ++i) {
        char c = line[i];
        if (quoted) { if (escaped) escaped = false; else if (c == '\\') escaped = true; else if (c == '"') quoted = false; }
        else if (c == '"') quoted = true;
        else if (c == '[' || c == '{') { if (++depth > MAX_DEPTH) return false; }
        else if ((c == ']' || c == '}') && depth) --depth;
    }
    return true;
}
char *fipsd_gw_control_answer(const char *line, size_t len, fipsd_gw_snapshot_slot *slot)
{
    json_t *response = NULL;
    /* The request is the line without its surrounding whitespace. */
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' ' || line[len - 1] == '\t')) --len;
    while (len && (*line == ' ' || *line == '\t' || *line == '\r' || *line == '\n')) { ++line; --len; }
    if (!len) response = fipsd_control_error("empty request");
    else if (!bounded_depth(line, len)) response = fipsd_control_error("invalid request: nesting limit exceeded");
    else {
        json_error_t error;
        json_t *request = json_loadb(line, len, JSON_DECODE_ANY | JSON_REJECT_DUPLICATES, &error);
        const json_t *command = request ? json_object_get(request, "command") : NULL;
        if (!request) {
            char msg[256];
            snprintf(msg, sizeof msg, "invalid request: %s at line %d column %d", error.text, error.line, error.column);
            response = fipsd_control_error(msg);
        } else if (!json_is_object(request) || !json_is_string(command)) {
            response = fipsd_control_error("invalid request: missing or non-string 'command'");
        } else if (strlen(json_string_value(command)) != json_string_length(command)) {
            response = fipsd_control_error("invalid request: NUL in command");
        } else {
            fipsd_gw_snapshot *s = fipsd_gw_snapshot_acquire(slot);
            response = fipsd_gw_control_dispatch(json_string_value(command), s);
            fipsd_gw_snapshot_release(s);
        }
        json_decref(request);
    }
    char *out = response ? fipsd_control_encode(response) : NULL;
    json_decref(response);
    return out;
}

/* ---- the socket -------------------------------------------------------- */
static uint64_t mono_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}
static int stream_socket(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 || fcntl(fd, F_SETFL, O_NONBLOCK) < 0) { int e = errno; close(fd); errno = e; return -1; }
#ifdef SO_NOSIGPIPE
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    return fd;
}
static bool is_dir(const char *path) { struct stat st; return stat(path, &st) == 0 && S_ISDIR(st.st_mode); }
/* Create the parent if missing and give it the policy's group and mode when
 * this service created it or it is a canonical runtime directory. */
static int secure_parent(const char *path, gid_t gid, char *err, size_t cap)
{
    char parent[PATH_MAX];
    snprintf(parent, sizeof parent, "%s", path);
    char *slash = strrchr(parent, '/');
    if (!slash || slash == parent) return 0;
    *slash = 0;
    bool created = !is_dir(parent);
    if (created) {
        for (char *p = parent + (parent[0] == '/');; ++p) {
            if (*p != '/' && *p) continue;
            char saved = *p; *p = 0;
            if (mkdir(parent, FIPSD_SOCKET_DIR_MODE) != 0 && errno != EEXIST) { snprintf(err, cap, "cannot create %s: %s", parent, strerror(errno)); return -1; }
            struct stat st;
            /* Ancestors may be symlinks (macOS /var -> private/var), as with upstream's
             * create_dir_all; the socket directory itself must be a real directory. */
            if ((saved ? stat(parent, &st) : lstat(parent, &st)) != 0 || !S_ISDIR(st.st_mode)) { snprintf(err, cap, "%s is not a directory", parent); errno = ENOTDIR; return -1; }
            *p = saved;
            if (!saved) break;
        }
    }
    bool canonical = !strcmp(parent, "/run/fips") || !strcmp(parent, "/var/run/fips");
    if (created || canonical) return fipsd_privilege_secure_private_dir(parent, gid, err, cap);
    return 0;
}
int fipsd_gw_control_bind(fipsd_gw_control *c, const char *path, fipsd_gw_snapshot_slot *slot, char *err, size_t cap)
{
    memset(c, 0, sizeof *c);
    c->listener = -1; c->slot = slot;
    atomic_init(&c->stopping, false); atomic_init(&c->connections, 0);
    if (strlen(path) >= sizeof c->path) { snprintf(err, cap, "socket path too long"); errno = ENAMETOOLONG; return -1; }
    snprintf(c->path, sizeof c->path, "%s", path);
    gid_t gid = getegid();
    /* Best-effort, as upstream's: a host without the group, or whose group
     * database cannot answer, keeps the socket usable by its owner. */
    int group = fipsd_privilege_resolve_group(FIPSD_SOCKET_GROUP, &gid, err, cap);
    if (group < 0) { FIPSD_WARN(MODULE, "%s; the gateway control socket keeps effective group %u", err, (unsigned)getegid()); gid = getegid(); }
    else if (!group) FIPSD_WARN(MODULE, "fips group absent; the gateway control socket keeps effective group %u", (unsigned)gid);
    if (cap) err[0] = 0;
    if (secure_parent(path, gid, err, cap) != 0) return -1;
    struct sockaddr_un a;
    memset(&a, 0, sizeof a); a.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof a.sun_path) { snprintf(err, cap, "socket path too long"); errno = ENAMETOOLONG; return -1; }
    memcpy(a.sun_path, path, strlen(path) + 1);
    struct stat st;
    if (lstat(path, &st) == 0) {
        /* Never remove another kind of file, nor steal a live gateway's socket. */
        if (!S_ISSOCK(st.st_mode) || (st.st_uid != geteuid() && geteuid() != 0)) { snprintf(err, cap, "%s exists and is not a stale socket", path); errno = EEXIST; return -1; }
        int probe = stream_socket();
        if (probe < 0) { snprintf(err, cap, "socket: %s", strerror(errno)); return -1; }
        int rc = connect(probe, (struct sockaddr *)&a, sizeof a), e = errno;
        close(probe);
        if (rc == 0 || (e != ECONNREFUSED && e != ENOENT)) { snprintf(err, cap, "%s is in use by another fips-gateway", path); errno = EADDRINUSE; return -1; }
        if (unlink(path) != 0 && errno != ENOENT) { snprintf(err, cap, "cannot remove the stale socket %s: %s", path, strerror(errno)); return -1; }
    } else if (errno != ENOENT) { snprintf(err, cap, "%s: %s", path, strerror(errno)); return -1; }
    int fd = stream_socket();
    if (fd < 0) { snprintf(err, cap, "socket: %s", strerror(errno)); return -1; }
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0 || lstat(path, &st) != 0) {
        int e = errno; close(fd); snprintf(err, cap, "bind %s: %s", path, strerror(e)); errno = e; return -1;
    }
    c->bound = true; c->device = (uint64_t)st.st_dev; c->inode = (uint64_t)st.st_ino;
    if (fipsd_privilege_secure_socket(path, gid, err, cap) != 0 || listen(fd, 128) != 0) {
        int e = errno; close(fd); fipsd_gw_control_clear(c); errno = e; return -1;
    }
    (void)gof_io_reset_fd(fd);
    c->listener = fd;
    FIPSD_INFO(MODULE, "Gateway control socket listening: path=%s", path);
    return 0;
}
static ssize_t io_recv(int fd, char *buf, size_t cap, uint64_t deadline)
{
    for (;;) {
        uint64_t now = mono_ms();
        if (now >= deadline) { fipsd_io_set_errno(ETIMEDOUT); return -1; }
        int left = (int)(deadline - now);
        if (fipsd_gw_on_fiber()) {
            ssize_t n = gof_recvfrom(fd, buf, cap, 0, NULL, NULL, left);
            if (n < 0 && fipsd_io_errno() == EINTR) continue;
            return n;
        }
        ssize_t n = recv(fd, buf, cap, 0);
        if (n >= 0) return n;
        if (errno == EINTR) continue; /* errno-after-park: allow -- thread-only fallback; fiber branch cannot reach this read */
        if (errno != EAGAIN && errno != EWOULDBLOCK) return -1; /* errno-after-park: allow -- thread-only fallback; fiber branch cannot reach this read */
        struct pollfd p = {.fd = fd, .events = POLLIN};
        if (poll(&p, 1, left) == 0) { fipsd_io_set_errno(ETIMEDOUT); return -1; }
    }
}
static int io_send_all(int fd, const char *data, size_t len, uint64_t deadline)
{
#ifdef MSG_NOSIGNAL
    int flags = MSG_NOSIGNAL;
#else
    int flags = 0;
#endif
    while (len) {
        uint64_t now = mono_ms();
        if (now >= deadline) { fipsd_io_set_errno(ETIMEDOUT); return -1; }
        int left = (int)(deadline - now);
        ssize_t n;
        if (fipsd_gw_on_fiber()) n = gof_sendto(fd, data, len, flags, NULL, 0, left);
        else {
            n = send(fd, data, len, flags);
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { /* errno-after-park: allow -- thread-only fallback; fiber branch cannot reach this read */
                struct pollfd p = {.fd = fd, .events = POLLOUT};
                if (poll(&p, 1, left) == 0) { fipsd_io_set_errno(ETIMEDOUT); return -1; }
                continue;
            }
        }
        if (n < 0 && fipsd_io_errno() == EINTR) continue;
        if (n <= 0) return -1;
        data += n; len -= (size_t)n;
    }
    return 0;
}
void fipsd_gw_control_serve_fd(fipsd_gw_control *c, int fd)
{
    char line[FIPSD_GW_CONTROL_MAX_REQUEST + 1];
    size_t len = 0;
    const char *failure = NULL;
    uint64_t deadline = mono_ms() + FIPSD_GW_CONTROL_IO_TIMEOUT_MS;
    /* One line, within the size limit and the deadline. */
    for (;;) {
        ssize_t n = io_recv(fd, line + len, sizeof line - len, deadline);
        if (n < 0) { failure = fipsd_io_errno() == ETIMEDOUT ? "read timeout" : "read error"; break; }
        if (n == 0) break;
        char *nl = memchr(line + len, '\n', (size_t)n);
        len += (size_t)n;
        if (nl) { len = (size_t)(nl - line) + 1; break; }
        if (len > FIPSD_GW_CONTROL_MAX_REQUEST) { failure = "read error: request too large"; break; }
        if (len == sizeof line) { failure = "read error: request too large"; break; }
    }
    if (len > FIPSD_GW_CONTROL_MAX_REQUEST) failure = "read error: request too large";
    char *out;
    if (failure) {
        json_t *r = fipsd_control_error(failure);
        out = r ? fipsd_control_encode(r) : NULL;
        json_decref(r);
    } else out = fipsd_gw_control_answer(line, len, c->slot);
    if (out) {
        size_t olen = strlen(out);
        char *framed = malloc(olen + 2);
        if (framed) {
            memcpy(framed, out, olen); framed[olen] = '\n';
            uint64_t write_deadline = mono_ms() + FIPSD_GW_CONTROL_IO_TIMEOUT_MS;
            if (io_send_all(fd, framed, olen + 1, write_deadline) != 0)
                FIPSD_DEBUG(MODULE, "Gateway control socket write failed or timed out");
            free(framed);
        }
        free(out);
    }
    (void)shutdown(fd, SHUT_RDWR);
    (void)gof_io_reset_fd(fd);
    close(fd);
}
typedef struct conn { fipsd_gw_control *c; int fd; } conn;
static void conn_fiber(void *arg)
{
    conn *k = arg;
    gof_set_name("gw-control");
    fipsd_gw_control_serve_fd(k->c, k->fd);
    atomic_fetch_sub(&k->c->connections, 1);
    free(k);
}
void fipsd_gw_control_accept_loop(fipsd_gw_control *c, fipsd_gw_spawn_fn spawn, void *user)
{
    while (!atomic_load(&c->stopping)) {
        int fd = fipsd_gw_on_fiber() ? gof_accept(c->listener, NULL, NULL, -1) : accept(c->listener, NULL, NULL);
        if (atomic_load(&c->stopping)) { if (fd >= 0) close(fd); break; }
        if (fd < 0) {
            if (fipsd_io_errno() == ECANCELED) break;
            if (fipsd_io_errno() == EINTR || fipsd_io_errno() == EAGAIN || fipsd_io_errno() == EWOULDBLOCK || fipsd_io_errno() == ECONNABORTED) {
                if (!fipsd_gw_on_fiber() && (fipsd_io_errno() == EAGAIN || fipsd_io_errno() == EWOULDBLOCK)) {
                    struct pollfd p = {.fd = c->listener, .events = POLLIN};
                    (void)poll(&p, 1, -1);
                }
                continue;
            }
            FIPSD_WARN(MODULE, "Gateway control socket accept failed: error=%s", strerror(fipsd_io_errno()));
            continue;
        }
        if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 || fcntl(fd, F_SETFL, O_NONBLOCK) < 0) { close(fd); continue; }
#ifdef SO_NOSIGPIPE
        int one = 1;
        (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        (void)gof_io_reset_fd(fd);
        if (atomic_fetch_add(&c->connections, 1) >= MAX_CONNECTIONS) {
            atomic_fetch_sub(&c->connections, 1);
            FIPSD_DEBUG(MODULE, "Gateway control connection refused: too many connections");
            (void)gof_io_reset_fd(fd); close(fd); continue;
        }
        if (!spawn) { fipsd_gw_control_serve_fd(c, fd); atomic_fetch_sub(&c->connections, 1); continue; }
        conn *k = malloc(sizeof *k);
        if (k) { k->c = c; k->fd = fd; }
        if (!k || spawn(user, conn_fiber, k) != 0) {
            free(k);
            atomic_fetch_sub(&c->connections, 1);
            (void)gof_io_reset_fd(fd); close(fd);
        }
    }
}
void fipsd_gw_control_stop(fipsd_gw_control *c)
{
    atomic_store(&c->stopping, true);
    if (c->listener >= 0) { (void)gof_io_cancel_fd(c->listener); (void)shutdown(c->listener, SHUT_RDWR); }
}
void fipsd_gw_control_clear(fipsd_gw_control *c)
{
    if (c->listener >= 0) { (void)gof_io_reset_fd(c->listener); close(c->listener); c->listener = -1; }
    if (!c->bound) return;
    /* Remove only the inode this service bound, never a replacement. */
    struct stat st;
    if (lstat(c->path, &st) == 0 && (uint64_t)st.st_dev == c->device && (uint64_t)st.st_ino == c->inode) (void)unlink(c->path);
    c->bound = false;
}
