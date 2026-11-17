#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#define _DARWIN_C_SOURCE 1
#include "transport/tor/control.h"
#include "transport/tcp/internal.h"
#include "blocking_executor.h"
#include "log.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/un.h>
#include <unistd.h>
#define MODULE "tor"
#define REPLY_WAIT_MS 10000
#define CONNECT_WAIT_MS 10000
#define BACKOFF_CAP_MS 30000u
#define EVENTS "SETEVENTS STATUS_CLIENT NETWORK_LIVENESS BW"
static const char *const snapshot_keys[] = {"status/bootstrap-phase", "status/circuit-established", "traffic/read",
    "traffic/written", "network-liveness", "version", "dormant"};
#define SNAPSHOT_KEYS (sizeof snapshot_keys / sizeof snapshot_keys[0])
typedef struct reply { int code; char status[256]; char data[8][512]; size_t count; } reply;
__attribute__((noinline)) static int io_errno(void) { return errno; }
static bool stopped(tor_control *c)
{ return go_channel_is_closed(c->stop) || (c->root && go_context_is_canceled(c->root)); }
static void control_error(tor_control *c)
{ atomic_fetch_add_explicit(&c->stats->control_errors, 1, memory_order_relaxed); }
int tor_control_init(tor_control *c, const char *owner, fipsd_transport_stats *stats, const char *addr,
    const char *auth, const char *cookie_path, uint32_t backoff_ms, uint32_t stall_ms)
{
    memset(c, 0, sizeof *c); nsync_mu_init(&c->mu); c->fd = -1;
    c->owner = owner; c->stats = stats;
    if (!addr || !*addr || strlen(addr) > 256) return -1;
    strcpy(c->addr, addr);
    const char *path = NULL;
    if (!auth || !strcmp(auth, "cookie")) { c->auth = TOR_CONTROL_COOKIE; path = cookie_path; }
    else if (!strncmp(auth, "cookie:", 7)) { c->auth = TOR_CONTROL_COOKIE; path = auth + 7; }
    else if (!strncmp(auth, "password:", 9)) {
        c->auth = TOR_CONTROL_PASSWORD;
        if (strlen(auth + 9) >= sizeof c->password) return -1;
        strcpy(c->password, auth + 9);
    } else return -1;
    if (c->auth == TOR_CONTROL_COOKIE) {
        if (!path || !*path || strlen(path) >= sizeof c->cookie_path) return -1;
        strcpy(c->cookie_path, path);
    }
    c->backoff_ms = backoff_ms ? backoff_ms : 1000; c->stall_ms = stall_ms ? stall_ms : 60000;
    c->stop = go_channel_create(1); c->done = go_channel_create(1); c->updates = go_channel_create(1);
    if (!c->stop || !c->done || !c->updates) { tor_control_destroy(c); return -1; }
    return 0;
}
/* The session's descriptor is published under mu so stop can latch its
 * cancellation; the fiber withdraws it under mu before closing. */
static int session_fd(tor_control *c) { nsync_mu_lock(&c->mu); int fd = c->fd; nsync_mu_unlock(&c->mu); return fd; }
static void close_session(tor_control *c)
{
    nsync_mu_lock(&c->mu); int fd = c->fd; c->fd = -1; nsync_mu_unlock(&c->mu);
    c->used = 0;
    if (fd >= 0) tcp_fd_close(fd);
}
static int connect_control(tor_control *c)
{
    struct sockaddr_storage ss; socklen_t slen; int family;
    memset(&ss, 0, sizeof ss);
    if (c->addr[0] == '/' || !strncmp(c->addr, "./", 2)) {
        struct sockaddr_un *un = (struct sockaddr_un *)&ss;
        if (strlen(c->addr) >= sizeof un->sun_path) return -1;
        un->sun_family = AF_UNIX; strcpy(un->sun_path, c->addr);
        slen = (socklen_t)sizeof *un; family = AF_UNIX;
    } else {
        fips_transport_addr a; tcp_endpoint *items = NULL; size_t n = 0;
        if (fips_transport_addr_from_string(c->addr, strlen(c->addr), &a) != FIPS_OK ||
            !tcp_resolve(&a, false, &items, &n)) return -1;
        memcpy(&ss, &items[0].address, sizeof ss); slen = items[0].len; family = ss.ss_family; free(items);
    }
    int fd = socket(family, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (fcntl(fd, F_SETFL, O_NONBLOCK) || fcntl(fd, F_SETFD, FD_CLOEXEC) || gof_io_reset_fd(fd)) { close(fd); return -1; }
#ifdef SO_NOSIGPIPE
    int one = 1; (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    nsync_mu_lock(&c->mu);
    if (stopped(c)) { nsync_mu_unlock(&c->mu); close(fd); return -1; }
    c->fd = fd;
    nsync_mu_unlock(&c->mu);
    if (gof_connect(fd, (const struct sockaddr *)&ss, slen, CONNECT_WAIT_MS) != 0) {
        int e = io_errno();
        if (!stopped(c)) FIPSD_DEBUG(MODULE, "%s: control port %s connect failed: %s", c->owner, c->addr, strerror(e));
        close_session(c); return -1;
    }
    return 0;
}
static bool send_command(int fd, const char *command)
{
    char line[1024];
    int n = snprintf(line, sizeof line, "%s\r\n", command);
    if (n <= 0 || (size_t)n >= sizeof line) return false;
    uint64_t deadline = fipsd_transport_now_ms() + REPLY_WAIT_MS;
    size_t sent = 0;
    while (sent < (size_t)n) {
        uint64_t now = fipsd_transport_now_ms();
        if (now >= deadline) return false;
        struct iovec v = {.iov_base = line + sent, .iov_len = (size_t)n - sent};
        struct msghdr m = {.msg_iov = &v, .msg_iovlen = 1};
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags = MSG_NOSIGNAL;
#endif
        ssize_t k = gof_sendmsg(fd, &m, flags, (int)(deadline - now));
        if (k < 0) { if (io_errno() == EINTR) continue; return false; }
        if (!k) return false;
        sent += (size_t)k;
    }
    return true;
}
/* 1 line, 0 EOF, -errno (ETIMEDOUT for the bounded wait, ECANCELED on stop,
 * EPROTO for an overlong line). CR LF and LF line ends both accepted. */
static int read_line(tor_control *c, int fd, char *line, size_t cap, int timeout_ms)
{
    for (;;) {
        char *nl = memchr(c->buf, '\n', c->used);
        if (nl) {
            size_t n = (size_t)(nl - c->buf);
            if (n && c->buf[n - 1] == '\r') --n;
            if (n >= cap) return -EPROTO;
            memcpy(line, c->buf, n); line[n] = 0;
            size_t drop = (size_t)(nl - c->buf) + 1;
            memmove(c->buf, c->buf + drop, c->used - drop); c->used -= drop;
            return 1;
        }
        if (c->used == sizeof c->buf) return -EPROTO;
        ssize_t k = gof_recvfrom(fd, c->buf + c->used, sizeof c->buf - c->used, 0, NULL, NULL, timeout_ms);
        if (k < 0) { int e = io_errno(); if (e == EINTR) continue; return -e; }
        if (!k) return 0;
        c->used += (size_t)k;
    }
}
static void publish(tor_control *c, const fipsd_tor_monitoring *m)
{
    nsync_mu_lock(&c->mu); c->info = *m; c->has_info = true; nsync_mu_unlock(&c->mu);
    (void)go_channel_try_send(c->updates, c);
}
/* Upstream's monitoring-task logging: bootstrap milestones, a stall warning,
 * liveness transitions and dormant entry. */
static void observe(tor_control *c, const fipsd_tor_monitoring *m)
{
    static const uint8_t milestones[] = {25, 50, 75, 100};
    for (size_t i = 0; i < sizeof milestones; ++i)
        if (m->bootstrap >= milestones[i] && c->last_bootstrap < milestones[i])
            FIPSD_INFO(MODULE, "%s: Tor bootstrap %u%%", c->owner, (unsigned)milestones[i]);
    if (m->bootstrap < 100 && !c->stall_warned && fipsd_transport_now_ms() - c->started_ms >= c->stall_ms) {
        FIPSD_WARN(MODULE, "%s: Tor bootstrap stalled - not at 100%% after %" PRIu32 " s (at %u%%)", c->owner,
            c->stall_ms / 1000, (unsigned)m->bootstrap);
        c->stall_warned = true;
    }
    if (m->bootstrap == 100) c->stall_warned = false;
    c->last_bootstrap = m->bootstrap;
    if (c->last_liveness[0] && strcmp(c->last_liveness, m->network_liveness))
        FIPSD_WARN(MODULE, "%s: Tor network liveness changed from %s to %s", c->owner, c->last_liveness, m->network_liveness);
    snprintf(c->last_liveness, sizeof c->last_liveness, "%s", m->network_liveness);
    if (m->dormant && !c->was_dormant) FIPSD_WARN(MODULE, "%s: Tor daemon entered dormant mode", c->owner);
    c->was_dormant = m->dormant;
}
static bool progress_of(const char *s, uint8_t *out)
{
    const char *p = strstr(s, "PROGRESS=");
    if (!p) return false;
    unsigned v = 0; p += 9;
    if (!isdigit((unsigned char)*p)) return false;
    while (isdigit((unsigned char)*p) && v < 1000) v = v * 10 + (unsigned)(*p++ - '0');
    *out = v > 100 ? 100 : (uint8_t)v;
    return true;
}
static void descriptor(tor_control *c, tor_descriptor_signal s)
{ if (c->descriptor) c->descriptor(c->descriptor_user, s); }
/* An asynchronous 650 line's content (after "650 "). Only the first line of
 * a multi-line event carries what the snapshot needs. */
static void handle_event(tor_control *c, const char *content)
{
    if (!strncmp(content, "HS_DESC ", 8)) {
        /* Action HSAddress AuthType HsDir ...: only our own successful upload
         * counts, never a fetch (REQUESTED/RECEIVED) of some other onion. */
        const char *s = content + 8; size_t n = strlen(c->service);
        if (n && !strncmp(s, "UPLOADED ", 9) && !strncasecmp(s + 9, c->service, n) && (s[9 + n] == ' ' || !s[9 + n]))
            descriptor(c, TOR_DESCRIPTOR_UPLOADED);
        return;
    }
    fipsd_tor_monitoring m;
    nsync_mu_lock(&c->mu);
    if (c->has_info) m = c->info;
    else { memset(&m, 0, sizeof m); strcpy(m.network_liveness, "unknown"); strcpy(m.version, "unknown"); }
    nsync_mu_unlock(&c->mu);
    if (!strncmp(content, "STATUS_CLIENT ", 14)) {
        const char *severity = content + 14, *action = strchr(severity, ' ');
        if (!action) return;
        ++action;
        if (!strncmp(action, "BOOTSTRAP", 9)) {
            if (!progress_of(action, &m.bootstrap)) return;
            if (strncmp(severity, "NOTICE", 6)) FIPSD_WARN(MODULE, "%s: Tor bootstrap problem: %s", c->owner, action);
        } else if (!strncmp(action, "CIRCUIT_ESTABLISHED", 19)) m.circuit_established = true;
        else if (!strncmp(action, "CIRCUIT_NOT_ESTABLISHED", 23)) m.circuit_established = false;
        else return;
        c->refresh = true;
    } else if (!strncmp(content, "NETWORK_LIVENESS ", 17)) {
        const char *state = content + 17;
        snprintf(m.network_liveness, sizeof m.network_liveness, "%s",
            !strncmp(state, "UP", 2) ? "up" : !strncmp(state, "DOWN", 4) ? "down" : "unknown");
        c->refresh = true;
    } else if (!strncmp(content, "BW ", 3)) {
        unsigned long long r = 0, w = 0;
        if (sscanf(content + 3, "%llu %llu", &r, &w) != 2) return;
        m.traffic_read += r; m.traffic_written += w;
    } else return;
    observe(c, &m); publish(c, &m);
}
/* One complete reply; 650 lines seen meanwhile are dispatched, never lost. */
static int read_reply(tor_control *c, int fd, reply *r)
{
    r->count = 0; r->code = 0; r->status[0] = 0;
    for (;;) {
        char line[1024];
        int rc = read_line(c, fd, line, sizeof line, REPLY_WAIT_MS);
        if (rc <= 0) return rc ? rc : -EPIPE;
        if (strlen(line) < 4 || !isdigit((unsigned char)line[0]) || !isdigit((unsigned char)line[1]) ||
            !isdigit((unsigned char)line[2])) return -EPROTO;
        int code = (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');
        char sep = line[3]; const char *content = line + 4;
        if (code == 650) { if (sep == ' ' || sep == '-' || sep == '+') handle_event(c, content); continue; }
        if (sep == ' ') { r->code = code; snprintf(r->status, sizeof r->status, "%s", content); return 0; }
        if (sep != '-' && sep != '+') return -EPROTO;
        if (r->count < 8) snprintf(r->data[r->count++], sizeof r->data[0], "%s", content);
        if (sep == '+') for (;;) { /* dot-terminated body; kept only if room */
            rc = read_line(c, fd, line, sizeof line, REPLY_WAIT_MS);
            if (rc <= 0) return rc ? rc : -EPIPE;
            if (!strcmp(line, ".")) break;
            if (r->count < 8) snprintf(r->data[r->count++], sizeof r->data[0], "%s", line[0] == '.' ? line + 1 : line);
        }
    }
}
typedef struct cookie_read { const char *path; uint8_t bytes[33]; size_t n; int error; } cookie_read;
static void *read_cookie(void *arg)
{
    cookie_read *r = arg;
    FILE *f = fopen(r->path, "rb");
    if (!f) { r->error = errno ? errno : EIO; return NULL; }
    r->n = fread(r->bytes, 1, sizeof r->bytes, f);
    if (ferror(f)) r->error = errno ? errno : EIO;
    fclose(f); return NULL;
}
static int authenticate(tor_control *c, int fd)
{
    char command[600];
    if (c->auth == TOR_CONTROL_COOKIE) {
        cookie_read r = {.path = c->cookie_path};
        go_blocking_submit(read_cookie, &r);
        if (r.error) { FIPSD_WARN(MODULE, "%s: failed to read cookie file '%s': %s", c->owner, c->cookie_path, strerror(r.error)); return -1; }
        if (r.n != 32) { FIPSD_WARN(MODULE, "%s: cookie file '%s' has %zu bytes, expected 32", c->owner, c->cookie_path, r.n); return -1; }
        static const char hex[] = "0123456789abcdef";
        char *p = command + snprintf(command, sizeof command, "AUTHENTICATE ");
        for (size_t i = 0; i < 32; ++i) { *p++ = hex[r.bytes[i] >> 4]; *p++ = hex[r.bytes[i] & 15]; }
        *p = 0;
    } else {
        char *p = command + snprintf(command, sizeof command, "AUTHENTICATE \"");
        for (const char *s = c->password; *s && p < command + sizeof command - 4; ++s) {
            if (*s == '\\' || *s == '"') *p++ = '\\';
            *p++ = *s;
        }
        *p++ = '"'; *p = 0;
    }
    reply r;
    if (!send_command(fd, command)) return -1;
    int rc = read_reply(c, fd, &r);
    if (rc < 0) return rc;
    if (r.code != 250) { FIPSD_WARN(MODULE, "%s: control port AUTHENTICATE failed: %d %s", c->owner, r.code, r.status); return -1; }
    return 0;
}
static void apply_value(fipsd_tor_monitoring *m, const char *line)
{
    const char *value = strchr(line, '=');
    if (!value) return;
    size_t klen = (size_t)(value - line); ++value;
    if (klen == 22 && !strncmp(line, "status/bootstrap-phase", 22)) (void)progress_of(value, &m->bootstrap);
    else if (klen == 26 && !strncmp(line, "status/circuit-established", 26)) m->circuit_established = !strcmp(value, "1");
    else if (klen == 12 && !strncmp(line, "traffic/read", 12)) m->traffic_read = strtoull(value, NULL, 10);
    else if (klen == 15 && !strncmp(line, "traffic/written", 15)) m->traffic_written = strtoull(value, NULL, 10);
    else if (klen == 16 && !strncmp(line, "network-liveness", 16)) snprintf(m->network_liveness, sizeof m->network_liveness, "%s", value);
    else if (klen == 7 && !strncmp(line, "version", 7)) snprintf(m->version, sizeof m->version, "%s", value);
    else if (klen == 7 && !strncmp(line, "dormant", 7)) m->dormant = !strcmp(value, "1");
}
/* The upstream monitoring_snapshot: one batched GETINFO, and when Tor refuses
 * the batch (a 552 for a key this daemon lacks) one GETINFO per key, where a
 * refused key keeps its default (0, false, "unknown") as unwrap_or did. Only a
 * lost connection fails the refresh. */
static int refresh_info(tor_control *c, int fd)
{
    char command[256] = "GETINFO";
    for (size_t i = 0; i < SNAPSHOT_KEYS; ++i) { strcat(command, " "); strcat(command, snapshot_keys[i]); }
    reply r;
    if (!send_command(fd, command)) return -1;
    int rc = read_reply(c, fd, &r);
    if (rc < 0) return rc;
    fipsd_tor_monitoring m; memset(&m, 0, sizeof m);
    strcpy(m.network_liveness, "unknown"); strcpy(m.version, "unknown");
    if (r.code == 250) for (size_t i = 0; i < r.count; ++i) apply_value(&m, r.data[i]);
    else for (size_t k = 0; k < SNAPSHOT_KEYS; ++k) {
        snprintf(command, sizeof command, "GETINFO %s", snapshot_keys[k]);
        if (!send_command(fd, command)) return -1;
        if ((rc = read_reply(c, fd, &r)) < 0) return rc;
        if (r.code != 250) { FIPSD_DEBUG(MODULE, "%s: GETINFO %s: %d %s", c->owner, snapshot_keys[k], r.code, r.status); continue; }
        for (size_t i = 0; i < r.count; ++i) apply_value(&m, r.data[i]);
    }
    observe(c, &m); publish(c, &m);
    return 0;
}
static int session_open(tor_control *c)
{
    c->used = 0;
    if (connect_control(c) != 0) return -1;
    int fd = session_fd(c);
    if (authenticate(c, fd) != 0 || refresh_info(c, fd) != 0) { close_session(c); return -1; }
    return 0;
}
/* Subscribe, then follow Tor's events until the session ends or stop. */
static void session_run(tor_control *c)
{
    int fd = session_fd(c); reply r = {0};
    if (fd < 0) return;
    if (!send_command(fd, c->service[0] ? EVENTS " HS_DESC" : EVENTS) || read_reply(c, fd, &r) < 0 || r.code != 250) {
        if (!stopped(c)) FIPSD_WARN(MODULE, "%s: control port refused event subscription (%d %s)", c->owner, r.code, r.status);
        return;
    }
    c->subscribed = true;
    FIPSD_INFO(MODULE, "%s: Tor control port connected (monitoring enabled, addr=%s)", c->owner, c->addr);
    if (c->service[0]) {
        /* Subscribed first, so an upload racing this question is still seen.
         * 250 means Tor holds a current descriptor for the service (one
         * published before this session); otherwise wait for HS_DESC. */
        char command[128]; snprintf(command, sizeof command, "GETINFO hs/service/desc/id/%s", c->service);
        if (!send_command(fd, command) || read_reply(c, fd, &r) < 0) return;
        if (r.code == 250) descriptor(c, TOR_DESCRIPTOR_CURRENT);
        else FIPSD_INFO(MODULE, "%s: waiting for Tor to publish the descriptor of %s.onion (HS_DESC UPLOADED)", c->owner, c->service);
    }
    for (;;) {
        int timeout = -1;
        if (!c->stall_warned && c->last_bootstrap < 100) {
            uint64_t now = fipsd_transport_now_ms(), until = c->started_ms + c->stall_ms;
            timeout = now >= until ? 0 : (int)(until - now > INT32_MAX ? INT32_MAX : until - now);
        }
        char line[1024];
        int rc = read_line(c, fd, line, sizeof line, timeout);
        if (rc == -ETIMEDOUT) { /* the one bounded wait: upstream's 60 s stall check */
            fipsd_tor_monitoring m; bool has;
            nsync_mu_lock(&c->mu); has = c->has_info; m = c->info; nsync_mu_unlock(&c->mu);
            if (has) observe(c, &m); else c->stall_warned = true;
            continue;
        }
        if (rc <= 0) return;
        if (strlen(line) >= 4 && !strncmp(line, "650", 3)) handle_event(c, line + 4);
        if (c->refresh) { c->refresh = false; if (refresh_info(c, fd) != 0) return; }
    }
}
static bool backoff(tor_control *c, uint32_t *attempt)
{
    uint64_t wait = (uint64_t)c->backoff_ms << (*attempt < 5 ? *attempt : 5);
    if (wait > BACKOFF_CAP_MS) wait = BACKOFF_CAP_MS;
    if (*attempt < UINT32_MAX) ++*attempt;
    void *v = NULL;
    GoSelectCase cases[] = {{GO_SELECT_RECEIVE, c->stop, NULL, &v},
        {GO_SELECT_RECEIVE, go_context_done(c->root), NULL, &v}};
    return go_select_timeout(cases, 2, wait).selected_case < 0;
}
static void monitor(void *arg)
{
    tor_control *c = arg; uint32_t attempt = 0;
    while (!stopped(c)) {
        if (session_fd(c) < 0 && session_open(c) != 0) {
            if (stopped(c)) break;
            control_error(c);
            if (!c->subscribed) descriptor(c, TOR_DESCRIPTOR_UNMONITORED);
            if (!c->warned) FIPSD_WARN(MODULE, "%s: Tor control port %s unavailable; monitoring retries with backoff", c->owner, c->addr);
            else FIPSD_DEBUG(MODULE, "%s: Tor control port %s still unavailable", c->owner, c->addr);
            c->warned = true;
            if (!backoff(c, &attempt)) break;
            continue;
        }
        attempt = 0; c->warned = false;
        session_run(c);
        close_session(c);
        if (stopped(c)) break;
        if (!c->subscribed) descriptor(c, TOR_DESCRIPTOR_UNMONITORED);
        control_error(c);
        FIPSD_WARN(MODULE, "%s: Tor control port connection lost; reconnecting with backoff", c->owner);
        if (!backoff(c, &attempt)) break;
    }
    close_session(c);
    go_channel_close(c->done);
}
int tor_control_connect_now(tor_control *c, fipsd_runtime *rt)
{
    c->rt = rt; c->root = fipsd_runtime_context(rt);
    if (!c->started_ms) c->started_ms = fipsd_transport_now_ms();
    if (session_open(c) != 0) { control_error(c); return -1; }
    return 0;
}
int tor_control_start(tor_control *c, fipsd_runtime *rt)
{
    if (c->started) return -1;
    c->rt = rt; c->root = fipsd_runtime_context(rt);
    if (!c->started_ms) c->started_ms = fipsd_transport_now_ms();
    if (fipsd_runtime_spawn(rt, monitor, c) != 0) return -1;
    c->started = true; return 0;
}
void tor_control_stop(tor_control *c)
{
    if (!c->stop) return;
    nsync_mu_lock(&c->mu);
    go_channel_close(c->stop);
    if (c->fd >= 0) (void)gof_io_cancel_fd(c->fd); /* never parks; the fiber withdraws fd under mu */
    nsync_mu_unlock(&c->mu);
}
void tor_control_close(tor_control *c)
{
    if (c->started) return;
    nsync_mu_lock(&c->mu); int fd = c->fd; c->fd = -1; nsync_mu_unlock(&c->mu);
    if (fd >= 0) { (void)gof_io_reset_fd(fd); close(fd); }
}
void tor_control_destroy(tor_control *c)
{
    tor_control_close(c);
    if (c->stop) go_channel_unref(c->stop);
    if (c->done) go_channel_unref(c->done);
    if (c->updates) go_channel_unref(c->updates);
    c->stop = c->done = c->updates = NULL;
}
bool tor_control_snapshot(tor_control *c, fipsd_tor_monitoring *out)
{
    nsync_mu_lock(&c->mu); bool has = c->has_info; if (has) *out = c->info; nsync_mu_unlock(&c->mu);
    return has;
}
