#define _POSIX_C_SOURCE 200809L
#include "transport/socks5/socks5.h"
#include "transport/presence.h"
#include <libgo/fiber.h>
#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>

size_t fipsd_socks5_greeting(uint8_t *out, size_t cap, bool userpass)
{
    size_t n = userpass ? 4 : 3;
    if (!out || cap < n) return 0;
    out[0] = FIPSD_SOCKS5_VERSION; out[1] = userpass ? 2 : 1; out[2] = FIPSD_SOCKS5_METHOD_NONE;
    if (userpass) out[3] = FIPSD_SOCKS5_METHOD_USERPASS;
    return n;
}
size_t fipsd_socks5_userpass(uint8_t *out, size_t cap, const char *username, const char *password)
{
    if (!out || !username || !password) return 0;
    size_t u = strlen(username), p = strlen(password), n = 3 + u + p;
    if (u > 255 || p > 255 || cap < n) return 0;
    out[0] = FIPSD_SOCKS5_USERPASS_VERSION; out[1] = (uint8_t)u; memcpy(out + 2, username, u);
    out[2 + u] = (uint8_t)p; memcpy(out + 3 + u, password, p);
    return n;
}
size_t fipsd_socks5_request(uint8_t *out, size_t cap, const fipsd_socks5_target *t)
{
    if (!out || !t) return 0;
    size_t n;
    switch (t->kind) {
    case FIPSD_SOCKS5_TARGET_IPV4: n = 10; break;
    case FIPSD_SOCKS5_TARGET_IPV6: n = 22; break;
    case FIPSD_SOCKS5_TARGET_DOMAIN: if (!t->domain_len) return 0; n = 7 + t->domain_len; break;
    default: return 0;
    }
    if (cap < n) return 0;
    out[0] = FIPSD_SOCKS5_VERSION; out[1] = FIPSD_SOCKS5_CMD_CONNECT; out[2] = 0;
    size_t at = 4;
    if (t->kind == FIPSD_SOCKS5_TARGET_IPV4) { out[3] = FIPSD_SOCKS5_ATYP_IPV4; memcpy(out + 4, t->ip, 4); at = 8; }
    else if (t->kind == FIPSD_SOCKS5_TARGET_IPV6) { out[3] = FIPSD_SOCKS5_ATYP_IPV6; memcpy(out + 4, t->ip, 16); at = 20; }
    else { out[3] = FIPSD_SOCKS5_ATYP_DOMAIN; out[4] = t->domain_len; memcpy(out + 5, t->domain, t->domain_len); at = 5 + t->domain_len; }
    out[at] = (uint8_t)(t->port >> 8); out[at + 1] = (uint8_t)t->port;
    return n;
}
int fipsd_socks5_reply_length(const uint8_t *in, size_t len)
{
    if (!in || len < 4) return 0;
    if (in[0] != FIPSD_SOCKS5_VERSION || in[2] != 0) return -1;
    switch (in[3]) {
    case FIPSD_SOCKS5_ATYP_IPV4: return 10;
    case FIPSD_SOCKS5_ATYP_IPV6: return 22;
    case FIPSD_SOCKS5_ATYP_DOMAIN: return len < 5 ? 0 : 7 + in[4];
    default: return -1;
    }
}
static bool parse_port(const char *s, size_t n, uint16_t *port)
{
    if (!n || n > 5) return false;
    unsigned v = 0;
    for (size_t i = 0; i < n; ++i) { if (s[i] < '0' || s[i] > '9') return false; v = v * 10 + (unsigned)(s[i] - '0'); }
    if (v > 65535) return false;
    *port = (uint16_t)v; return true;
}
bool fipsd_socks5_target_parse(const char *text, size_t len, bool require_dot, fipsd_socks5_target *out)
{
    if (!text || !out || !len || len > 300 || memchr(text, 0, len)) return false;
    const char *colon = NULL;
    for (size_t i = len; i-- > 0;) if (text[i] == ':') { colon = text + i; break; }
    if (!colon) return false;
    size_t host_len = (size_t)(colon - text), port_len = len - host_len - 1;
    fipsd_socks5_target t = {0};
    if (!parse_port(colon + 1, port_len, &t.port) || !host_len) return false;
    char host[301]; memcpy(host, text, host_len); host[host_len] = 0;
    if (host[0] == '[' && host[host_len - 1] == ']') {
        host[host_len - 1] = 0;
        if (inet_pton(AF_INET6, host + 1, t.ip) != 1) return false;
        t.kind = FIPSD_SOCKS5_TARGET_IPV6; *out = t; return true;
    }
    if (inet_pton(AF_INET, host, t.ip) == 1) { t.kind = FIPSD_SOCKS5_TARGET_IPV4; *out = t; return true; }
    if (host_len > 255 || strchr(host, ':') || (require_dot && !strchr(host, '.'))) return false;
    for (size_t i = 0; i < host_len; ++i) if ((unsigned char)host[i] <= ' ' || host[i] == 0x7f) return false;
    t.kind = FIPSD_SOCKS5_TARGET_DOMAIN; memcpy(t.domain, host, host_len + 1); t.domain_len = (uint8_t)host_len;
    *out = t; return true;
}
__attribute__((noinline)) static int io_errno(void) { return errno; } /* TLS after a fiber migration */
static int remaining(uint64_t deadline, fipsd_socks5_result *r)
{
    uint64_t now = fipsd_transport_now_ms();
    if (now >= deadline) { *r = FIPSD_SOCKS5_TIMEOUT; return -1; }
    return deadline - now > INT_MAX ? INT_MAX : (int)(deadline - now);
}
static fipsd_socks5_result classify(int e)
{
    return e == ETIMEDOUT ? FIPSD_SOCKS5_TIMEOUT : e == ECANCELED ? FIPSD_SOCKS5_CANCELLED : FIPSD_SOCKS5_IO;
}
static bool send_all(int fd, const uint8_t *p, size_t n, uint64_t deadline, fipsd_socks5_result *r)
{
    size_t sent = 0;
    while (sent < n) {
        int wait = remaining(deadline, r);
        if (wait < 0) return false;
        struct iovec v = {.iov_base = (void *)(p + sent), .iov_len = n - sent};
        struct msghdr m = {.msg_iov = &v, .msg_iovlen = 1};
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags = MSG_NOSIGNAL;
#endif
        ssize_t k = gof_sendmsg(fd, &m, flags, wait);
        if (k < 0) { int e = io_errno(); if (e == EINTR) continue; *r = classify(e); return false; }
        if (!k) { *r = FIPSD_SOCKS5_IO; return false; }
        sent += (size_t)k;
    }
    return true;
}
static bool read_exact(int fd, uint8_t *p, size_t n, uint64_t deadline, fipsd_socks5_result *r)
{
    size_t got = 0;
    while (got < n) {
        int wait = remaining(deadline, r);
        if (wait < 0) return false;
        ssize_t k = gof_recvfrom(fd, p + got, n - got, 0, NULL, NULL, wait);
        if (k < 0) { int e = io_errno(); if (e == EINTR) continue; *r = classify(e); return false; }
        if (!k) { *r = FIPSD_SOCKS5_PROTOCOL; return false; } /* the proxy closed mid-handshake */
        got += (size_t)k;
    }
    return true;
}
fipsd_socks5_result fipsd_socks5_handshake(int fd, const fipsd_socks5_auth *auth, const fipsd_socks5_target *target,
    uint64_t deadline, uint8_t *rep_out)
{
    uint8_t buf[FIPSD_SOCKS5_MAX_REQUEST + 256];
    fipsd_socks5_result r = FIPSD_SOCKS5_PROTOCOL;
    if (rep_out) *rep_out = 0;
    if (fd < 0 || !auth || !target) return FIPSD_SOCKS5_PROTOCOL;
    bool userpass = auth->username != NULL;
    size_t n = fipsd_socks5_greeting(buf, sizeof buf, userpass);
    if (!n || !send_all(fd, buf, n, deadline, &r) || !read_exact(fd, buf, 2, deadline, &r)) return r;
    if (buf[0] != FIPSD_SOCKS5_VERSION) return FIPSD_SOCKS5_PROTOCOL;
    if (buf[1] == FIPSD_SOCKS5_METHOD_USERPASS) {
        if (!userpass) return FIPSD_SOCKS5_PROTOCOL;
        n = fipsd_socks5_userpass(buf, sizeof buf, auth->username, auth->password ? auth->password : "");
        if (!n) return FIPSD_SOCKS5_PROTOCOL;
        if (!send_all(fd, buf, n, deadline, &r) || !read_exact(fd, buf, 2, deadline, &r)) return r;
        if (buf[0] != FIPSD_SOCKS5_USERPASS_VERSION) return FIPSD_SOCKS5_PROTOCOL;
        if (buf[1] != 0) return FIPSD_SOCKS5_AUTH_FAILED;
    } else if (buf[1] == FIPSD_SOCKS5_METHOD_REJECTED) return FIPSD_SOCKS5_NO_METHOD;
    else if (buf[1] != FIPSD_SOCKS5_METHOD_NONE) return FIPSD_SOCKS5_PROTOCOL; /* a method we did not offer */
    n = fipsd_socks5_request(buf, sizeof buf, target);
    if (!n || !send_all(fd, buf, n, deadline, &r) || !read_exact(fd, buf, 4, deadline, &r)) return r;
    if (buf[0] != FIPSD_SOCKS5_VERSION || buf[2] != 0) return FIPSD_SOCKS5_PROTOCOL;
    if (rep_out) *rep_out = buf[1];
    if (buf[1] != FIPSD_SOCKS5_REP_SUCCESS) return buf[1] == FIPSD_SOCKS5_REP_REFUSED ? FIPSD_SOCKS5_REFUSED : FIPSD_SOCKS5_REJECTED;
    int total = fipsd_socks5_reply_length(buf, 4);
    if (total < 0) return FIPSD_SOCKS5_PROTOCOL;
    if (!total) { if (!read_exact(fd, buf + 4, 1, deadline, &r)) return r; total = fipsd_socks5_reply_length(buf, 5); }
    if (total < 4 || (size_t)total > sizeof buf) return FIPSD_SOCKS5_PROTOCOL;
    size_t have = buf[3] == FIPSD_SOCKS5_ATYP_DOMAIN ? 5 : 4;
    if ((size_t)total > have && !read_exact(fd, buf + have, (size_t)total - have, deadline, &r)) return r;
    return FIPSD_SOCKS5_OK;
}
int fipsd_socks5_errno(fipsd_socks5_result r)
{
    switch (r) {
    case FIPSD_SOCKS5_OK: return 0;
    case FIPSD_SOCKS5_TIMEOUT: return ETIMEDOUT;
    case FIPSD_SOCKS5_REFUSED: return ECONNREFUSED;
    case FIPSD_SOCKS5_REJECTED: return ECONNABORTED;
    case FIPSD_SOCKS5_NO_METHOD: return EACCES;
    case FIPSD_SOCKS5_AUTH_FAILED: return EACCES;
    case FIPSD_SOCKS5_PROTOCOL: return EPROTO;
    case FIPSD_SOCKS5_IO: return EIO;
    case FIPSD_SOCKS5_CANCELLED: return ECANCELED;
    }
    return EPROTO;
}
const char *fipsd_socks5_result_name(fipsd_socks5_result r)
{
    static const char *const names[] = {"ok", "timeout", "refused", "rejected", "no_method", "auth_failed",
        "protocol", "io", "cancelled"};
    return (size_t)r < sizeof names / sizeof names[0] ? names[r] : "?";
}
