/* libfipsnative: the native datagram API's line protocol, as a client sees
 * it (upstream src/native/client/{mod,codec}.rs). Plain POSIX: no fiber
 * runtime, no JSON library, no libfips-core. See the public header for the
 * surface and docs/how-to/write-a-native-api-client.md for the rules this
 * file carries: every read on the setup connection is a recvmsg with an
 * ancillary buffer; a descriptor belongs to the last complete line of the
 * read that carried it; end of file is POLLHUP with an empty queue; sends
 * carry MSG_NOSIGNAL; the setup socket has a deadline; descriptors are
 * close-on-exec and the whole control buffer is walked.
 *
 * On macOS and FreeBSD connect and listen also ask for `"framing":"stream"`
 * (nostrc-6ljk8): fipsd answers with a SOCK_STREAM flow carrying
 * native/frame.h frames, whose writer the kernel wakes when this side reads,
 * so a slow reader is not dropped at a full buffer; upstream's daemon ignores
 * the field and answers with its datagram pair. Which one arrived is read off
 * the descriptor (SO_TYPE), never assumed. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "native/client_internal.h"
#include "native/frame.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#if defined(__APPLE__) || defined(__FreeBSD__)
const char *const fips_native_socket = "/var/run/fips/api.sock";
/* The datagram pair there drops at a full client buffer; a stream does not. */
#define FRAMING_PARAM "\"framing\":\"stream\","
#else
const char *const fips_native_socket = "/run/fips/api.sock";
#define FRAMING_PARAM "" /* SOCK_SEQPACKET already waits for a slow reader */
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
/* Bytes taken from the setup connection per recvmsg, and the largest partial
 * line held before giving up on the daemon. */
#define CHUNK 8192
#define MAX_LINE 65536
#define MAX_LINES 16
#define FDS_PER_MESSAGE 8

struct fips_native_stream {
    int fd;
    fips_native_addr peer, local;
    size_t max;
    /* A framed SOCK_STREAM flow (native/frame.h): each direction has a lock
     * so concurrent callers never interleave frames, and the receive side
     * keeps a frame read in pieces across calls that return -EAGAIN. */
    bool framed;
    pthread_mutex_t send_mu, recv_mu;
    uint8_t header[FIPSD_NATIVE_FRAME_HEADER];
    size_t header_have, body_have;
    uint32_t body_len; /* valid once the header is whole */
    uint8_t *body;
    size_t body_cap;
};
struct fips_native_listener { int fd; fips_native_addr local; };

/* ---- socket path ---------------------------------------------------------- */
static bool is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}
int fips_native_socket_path(char *out, size_t cap)
{
    int n;
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (is_dir("/run/fips")) n = snprintf(out, cap, "/run/fips/api.sock");
#if defined(__APPLE__) || defined(__FreeBSD__)
    else if (is_dir("/var/run/fips")) n = snprintf(out, cap, "/var/run/fips/api.sock");
#endif
    else if (xdg && *xdg && is_dir(xdg)) n = snprintf(out, cap, "%s/fips/api.sock", xdg);
    else n = snprintf(out, cap, "/tmp/fips-api.sock");
    return n < 0 || (size_t)n >= cap ? -ENAMETOOLONG : 0;
}

/* ---- bech32 (BIP-173, NIP-19 npub) ---------------------------------------- */
static const char CHARSET[] = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
static uint32_t polymod_step(uint32_t chk, uint8_t v)
{
    static const uint32_t gen[5] = {0x3b6a57b2u, 0x26508e6du, 0x1ea119fau, 0x3d4233ddu, 0x2a1462b3u};
    uint32_t top = chk >> 25;
    chk = ((chk & 0x1ffffffu) << 5) ^ v;
    for (int i = 0; i < 5; ++i) if ((top >> i) & 1) chk ^= gen[i];
    return chk;
}
static uint32_t hrp_checksum_start(void)
{
    /* hrp "npub": high bits, a zero separator, then low bits. */
    static const char hrp[] = "npub";
    uint32_t chk = 1;
    for (size_t i = 0; i < 4; ++i) chk = polymod_step(chk, (uint8_t)(hrp[i] >> 5));
    chk = polymod_step(chk, 0);
    for (size_t i = 0; i < 4; ++i) chk = polymod_step(chk, (uint8_t)(hrp[i] & 31));
    return chk;
}
int fips_native_npub_encode(const uint8_t key[FIPS_NATIVE_KEY_SIZE], char *out, size_t cap)
{
    if (!key || !out) return -EINVAL;
    if (cap < FIPS_NATIVE_NPUB_CAPACITY) return -ENAMETOOLONG;
    uint8_t values[52];
    uint32_t acc = 0; int bits = 0; size_t n = 0;
    for (size_t i = 0; i < FIPS_NATIVE_KEY_SIZE; ++i) {
        acc = (acc << 8) | key[i]; bits += 8;
        while (bits >= 5) { bits -= 5; values[n++] = (acc >> bits) & 31; }
    }
    if (bits) values[n++] = (acc << (5 - bits)) & 31;
    uint32_t chk = hrp_checksum_start();
    for (size_t i = 0; i < n; ++i) chk = polymod_step(chk, values[i]);
    for (int i = 0; i < 6; ++i) chk = polymod_step(chk, 0);
    chk ^= 1;
    memcpy(out, "npub1", 5);
    for (size_t i = 0; i < n; ++i) out[5 + i] = CHARSET[values[i]];
    for (int i = 0; i < 6; ++i) out[5 + n + (size_t)i] = CHARSET[(chk >> (5 * (5 - i))) & 31];
    out[5 + n + 6] = 0;
    return 0;
}
int fips_native_npub_decode(const char *npub, uint8_t key[FIPS_NATIVE_KEY_SIZE])
{
    if (!npub || !key) return -EINVAL;
    size_t len = strlen(npub);
    if (len != 63) return -EINVAL;
    bool lower = false, upper = false;
    char text[64];
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)npub[i];
        if (c < 33 || c > 126) return -EINVAL;
        if (c >= 'a' && c <= 'z') lower = true;
        if (c >= 'A' && c <= 'Z') { upper = true; c = (unsigned char)(c - 'A' + 'a'); }
        text[i] = (char)c;
    }
    text[len] = 0;
    if (lower && upper) return -EINVAL;
    if (memcmp(text, "npub1", 5)) return -EINVAL;
    uint8_t values[58];
    for (size_t i = 0; i < 58; ++i) {
        const char *at = strchr(CHARSET, text[5 + i]);
        if (!at || !text[5 + i]) return -EINVAL;
        values[i] = (uint8_t)(at - CHARSET);
    }
    uint32_t chk = hrp_checksum_start();
    for (size_t i = 0; i < 58; ++i) chk = polymod_step(chk, values[i]);
    if (chk != 1) return -EINVAL;
    uint32_t acc = 0; int bits = 0; size_t n = 0;
    for (size_t i = 0; i < 52; ++i) {
        acc = (acc << 5) | values[i]; bits += 5;
        while (bits >= 8) { bits -= 8; if (n < FIPS_NATIVE_KEY_SIZE) key[n++] = (acc >> bits) & 0xff; else return -EINVAL; }
    }
    /* Canonical: the padding bits are zero and short of a byte. */
    if (n != FIPS_NATIVE_KEY_SIZE || bits >= 8 || (acc & ((1u << bits) - 1))) return -EINVAL;
    return 0;
}
int fips_native_addr_from_npub(const char *npub, uint16_t port, fips_native_addr *out)
{
    fips_native_addr addr = {.port = port};
    int rc = fips_native_npub_decode(npub, addr.key);
    if (rc != 0) return rc;
    *out = addr;
    return 0;
}
int fips_native_addr_parse(const char *text, fips_native_addr *out)
{
    if (!text || !out) return -EINVAL;
    const char *colon = strrchr(text, ':');
    if (!colon || colon == text || !colon[1]) return -EINVAL;
    char *end = NULL;
    unsigned long port = strtoul(colon + 1, &end, 10);
    if (*end || port > 65535 || colon[1] < '0' || colon[1] > '9') return -EINVAL;
    char npub[64];
    size_t n = (size_t)(colon - text);
    if (n >= sizeof npub) return -EINVAL;
    memcpy(npub, text, n); npub[n] = 0;
    return fips_native_addr_from_npub(npub, (uint16_t)port, out);
}
int fips_native_addr_format(const fips_native_addr *addr, char *out, size_t cap)
{
    char npub[FIPS_NATIVE_NPUB_CAPACITY];
    int rc = fips_native_npub_encode(addr->key, npub, sizeof npub);
    if (rc != 0) return rc;
    int n = snprintf(out, cap, "%s:%u", npub, (unsigned)addr->port);
    return n < 0 || (size_t)n >= cap ? -ENAMETOOLONG : 0;
}
bool fips_native_addr_eq(const fips_native_addr *a, const fips_native_addr *b)
{
    return a->port == b->port && !memcmp(a->key, b->key, FIPS_NATIVE_KEY_SIZE);
}

/* ---- a reader for the flat JSON the daemon writes ------------------------- */
typedef struct jv { const unsigned char *p; size_t n; } jv;
static size_t ws(const unsigned char *s, size_t n, size_t i)
{
    while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    return i;
}
static bool skip_string(const unsigned char *s, size_t n, size_t *i)
{
    if (*i >= n || s[*i] != '"') return false;
    for (++*i; *i < n; ++*i) {
        if (s[*i] == '\\') { if (++*i >= n) return false; continue; }
        if (s[*i] == '"') { ++*i; return true; }
    }
    return false;
}
static bool skip_value(const unsigned char *s, size_t n, size_t *i)
{
    *i = ws(s, n, *i);
    if (*i >= n) return false;
    unsigned char c = s[*i];
    if (c == '"') return skip_string(s, n, i);
    if (c == '{' || c == '[') {
        unsigned char close = c == '{' ? '}' : ']';
        ++*i;
        for (;;) {
            *i = ws(s, n, *i);
            if (*i >= n) return false;
            if (s[*i] == close) { ++*i; return true; }
            if (c == '{') {
                if (!skip_string(s, n, i)) return false;
                *i = ws(s, n, *i);
                if (*i >= n || s[*i] != ':') return false;
                ++*i;
            }
            if (!skip_value(s, n, i)) return false;
            *i = ws(s, n, *i);
            if (*i < n && s[*i] == ',') ++*i;
        }
    }
    size_t start = *i;
    while (*i < n && s[*i] != ',' && s[*i] != '}' && s[*i] != ']' && s[*i] != ' ' && s[*i] != '\n' && s[*i] != '\r' && s[*i] != '\t') ++*i;
    return *i > start;
}
/* The value of `key` in the object `o`, which must be a whole JSON object. */
static bool object_find(jv o, const char *key, jv *out)
{
    const unsigned char *s = o.p; size_t n = o.n, i = ws(s, n, 0), klen = strlen(key);
    if (i >= n || s[i] != '{') return false;
    ++i;
    for (;;) {
        i = ws(s, n, i);
        if (i >= n) return false;
        if (s[i] == '}') return false;
        size_t kstart = i;
        if (!skip_string(s, n, &i)) return false;
        bool match = i - kstart == klen + 2 && !memcmp(s + kstart + 1, key, klen);
        i = ws(s, n, i);
        if (i >= n || s[i] != ':') return false;
        ++i;
        i = ws(s, n, i);
        size_t vstart = i;
        if (!skip_value(s, n, &i)) return false;
        if (match) { out->p = s + vstart; out->n = i - vstart; return true; }
        i = ws(s, n, i);
        if (i < n && s[i] == ',') ++i;
    }
}
static int hexval(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static bool put_utf8(uint32_t cp, char *out, size_t cap, size_t *len)
{
    unsigned char buf[4]; size_t n;
    if (cp < 0x80) { buf[0] = (unsigned char)cp; n = 1; }
    else if (cp < 0x800) { buf[0] = (unsigned char)(0xC0 | (cp >> 6)); buf[1] = (unsigned char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) { buf[0] = (unsigned char)(0xE0 | (cp >> 12)); buf[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F)); buf[2] = (unsigned char)(0x80 | (cp & 0x3F)); n = 3; }
    else { buf[0] = (unsigned char)(0xF0 | (cp >> 18)); buf[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F)); buf[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F)); buf[3] = (unsigned char)(0x80 | (cp & 0x3F)); n = 4; }
    if (*len + n >= cap) return false;
    memcpy(out + *len, buf, n); *len += n;
    return true;
}
/* A JSON string value, unescaped; -EPROTO for anything that is not one. */
static int string_value(jv v, char *out, size_t cap)
{
    if (v.n < 2 || v.p[0] != '"' || v.p[v.n - 1] != '"') return -EPROTO;
    size_t len = 0;
    for (size_t i = 1; i + 1 < v.n; ++i) {
        unsigned char c = v.p[i];
        if (c != '\\') { if (len + 1 >= cap) return -EPROTO; out[len++] = (char)c; continue; }
        if (++i + 1 >= v.n) return -EPROTO;
        c = v.p[i];
        char plain = 0;
        switch (c) {
        case '"': plain = '"'; break; case '\\': plain = '\\'; break; case '/': plain = '/'; break;
        case 'b': plain = '\b'; break; case 'f': plain = '\f'; break; case 'n': plain = '\n'; break;
        case 'r': plain = '\r'; break; case 't': plain = '\t'; break;
        case 'u': {
            if (i + 4 >= v.n) return -EPROTO;
            uint32_t cp = 0;
            for (int k = 1; k <= 4; ++k) { int h = hexval(v.p[i + (size_t)k]); if (h < 0) return -EPROTO; cp = (cp << 4) | (uint32_t)h; }
            i += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                if (i + 6 >= v.n || v.p[i + 1] != '\\' || v.p[i + 2] != 'u') return -EPROTO;
                uint32_t low = 0;
                for (int k = 3; k <= 6; ++k) { int h = hexval(v.p[i + (size_t)k]); if (h < 0) return -EPROTO; low = (low << 4) | (uint32_t)h; }
                if (low < 0xDC00 || low > 0xDFFF) return -EPROTO;
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                i += 6;
            }
            if (!put_utf8(cp, out, cap, &len)) return -EPROTO;
            continue;
        }
        default: return -EPROTO;
        }
        if (len + 1 >= cap) return -EPROTO;
        out[len++] = plain;
    }
    out[len] = 0;
    return 0;
}
static int uint_value(jv v, uint64_t *out)
{
    if (!v.n || v.n > 19) return -EPROTO;
    uint64_t n = 0;
    for (size_t i = 0; i < v.n; ++i) {
        if (v.p[i] < '0' || v.p[i] > '9') return -EPROTO;
        n = n * 10 + (uint64_t)(v.p[i] - '0');
    }
    *out = n;
    return 0;
}

/* ---- the line protocol ---------------------------------------------------- */
int fips_native__errno_of(const char *name, size_t len)
{
    static const struct { const char *name; int code; } table[] = {
        {"EADDRINUSE", EADDRINUSE}, {"EADDRNOTAVAIL", EADDRNOTAVAIL}, {"EMFILE", EMFILE}, {"EINVAL", EINVAL},
        {"EMSGSIZE", EMSGSIZE}, {"EPIPE", EPIPE}, {"ETIMEDOUT", ETIMEDOUT}
    };
    for (size_t i = 0; i < sizeof table / sizeof table[0]; ++i)
        if (strlen(table[i].name) == len && !memcmp(table[i].name, name, len)) return -table[i].code;
    /* An unknown name is the daemon refusing for a reason this client has no row for. */
    return -ECONNREFUSED;
}
int fips_native__reply(const unsigned char *line, size_t len, const unsigned char **data, size_t *data_len)
{
    jv o = {line, len}, status, v;
    char text[32];
    if (!object_find(o, "status", &status) || string_value(status, text, sizeof text) != 0) return -EPROTO;
    if (!strcmp(text, "ok")) {
        if (object_find(o, "data", &v)) { *data = v.p; *data_len = v.n; }
        else { *data = (const unsigned char *)"null"; *data_len = 4; }
        return 0;
    }
    if (strcmp(text, "error")) return -EPROTO;
    jv d, e; char name[32];
    if (object_find(o, "data", &d) && object_find(d, "errno", &e) && string_value(e, name, sizeof name) == 0)
        return fips_native__errno_of(name, strlen(name));
    return -ECONNREFUSED;
}
static int key_field(jv data, const char *field, uint8_t key[FIPS_NATIVE_KEY_SIZE])
{
    jv v; char npub[80];
    if (!object_find(data, field, &v) || string_value(v, npub, sizeof npub) != 0) return -EPROTO;
    return fips_native_npub_decode(npub, key) == 0 ? 0 : -EPROTO;
}
static int port_field(jv data, const char *field, uint16_t *port)
{
    jv v; uint64_t n;
    if (!object_find(data, field, &v) || uint_value(v, &n) != 0 || n > 65535) return -EPROTO;
    *port = (uint16_t)n;
    return 0;
}
/* One shape for two producers: a connect reply's data and an arrival message. */
static int opened(jv data, fips_native_addr *peer, fips_native_addr *local, size_t *max)
{
    jv v; uint64_t n;
    if (!object_find(data, "max_payload", &v) || uint_value(v, &n) != 0 || n > SIZE_MAX) return -EPROTO;
    int rc;
    if ((rc = key_field(data, "peer", peer->key)) != 0 || (rc = key_field(data, "node", local->key)) != 0) return rc;
    if ((rc = port_field(data, "local_port", &local->port)) != 0 || (rc = port_field(data, "remote_port", &peer->port)) != 0) return rc;
    *max = (size_t)n;
    return 0;
}

/* The setup connection: a line buffer over recvmsg, and the write side. */
typedef struct line { unsigned char *bytes; size_t len; int fd; } line;
typedef struct wire {
    int fd;
    unsigned char *partial;
    size_t partial_len;
    line lines[MAX_LINES];
    size_t count, head;
} wire;
static void wire_close(wire *w)
{
    for (size_t i = 0; i < w->count; ++i) {
        line *l = &w->lines[(w->head + i) % MAX_LINES];
        free(l->bytes);
        if (l->fd >= 0) close(l->fd);
    }
    free(w->partial);
    if (w->fd >= 0) close(w->fd);
}
static int wire_open(const char *path, wire *w)
{
    memset(w, 0, sizeof *w); w->fd = -1;
    struct sockaddr_un addr;
    if (!path) path = fips_native_socket;
    if (!*path || strlen(path) >= sizeof addr.sun_path) return -ENAMETOOLONG;
    memset(&addr, 0, sizeof addr); addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, strlen(path) + 1);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -errno;
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) { int e = errno; close(fd); return -e; }
#ifdef SO_NOSIGPIPE
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    int rc;
    do rc = connect(fd, (struct sockaddr *)&addr, sizeof addr); while (rc < 0 && errno == EINTR);
    if (rc < 0) { int e = errno; close(fd); return -e; }
    struct timeval deadline = {FIPS_NATIVE_SETUP_TIMEOUT_MS / 1000, (FIPS_NATIVE_SETUP_TIMEOUT_MS % 1000) * 1000};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &deadline, sizeof deadline) < 0) { int e = errno; close(fd); return -e; }
    w->fd = fd;
    return 0;
}
static int wire_send(wire *w, const char *text)
{
    size_t rest = strlen(text), at = 0;
    while (rest) {
        ssize_t n = send(w->fd, text + at, rest, MSG_NOSIGNAL);
        if (n < 0) { if (errno == EINTR) continue; return errno == ECONNRESET ? -EPIPE : -errno; }
        if (n == 0) return -EPROTO;
        at += (size_t)n; rest -= (size_t)n;
    }
    return 0;
}
/* One recvmsg, split into lines, with any descriptor placed by the
 * association rule: it belongs to the last complete line of the read that
 * carried it. A read that carries one and completes no line is reported. */
static int wire_fill(wire *w)
{
    unsigned char buf[CHUNK];
    struct iovec iov = {.iov_base = buf, .iov_len = sizeof buf};
    union { struct cmsghdr header; unsigned char bytes[CMSG_SPACE(sizeof(int) * FDS_PER_MESSAGE)]; } control;
    memset(&control, 0, sizeof control);
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control.bytes, .msg_controllen = sizeof control.bytes};
    int flags = 0;
#ifdef MSG_CMSG_CLOEXEC
    flags = MSG_CMSG_CLOEXEC;
#endif
    ssize_t n;
    do n = recvmsg(w->fd, &msg, flags); while (n < 0 && errno == EINTR);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return -ETIMEDOUT;
        if (errno == ECONNRESET) return -ECONNREFUSED;
        return -errno;
    }
    int fd = -1;
    for (struct cmsghdr *h = CMSG_FIRSTHDR(&msg); h; h = CMSG_NXTHDR(&msg, h)) {
        if (h->cmsg_level != SOL_SOCKET || h->cmsg_type != SCM_RIGHTS) continue;
        size_t payload = h->cmsg_len - CMSG_LEN(0);
        for (size_t i = 0; i + sizeof(int) <= payload; i += sizeof(int)) {
            int got; memcpy(&got, CMSG_DATA(h) + i, sizeof got);
#ifndef MSG_CMSG_CLOEXEC
            int fl = fcntl(got, F_GETFD); if (fl >= 0) (void)fcntl(got, F_SETFD, fl | FD_CLOEXEC);
#endif
            if (fd < 0) fd = got; else close(got);
        }
    }
    if (msg.msg_flags & MSG_CTRUNC) { if (fd >= 0) close(fd); return -EPROTO; }
    if (n == 0) { if (fd >= 0) close(fd); return -ECONNREFUSED; }
    unsigned char *grown = realloc(w->partial, w->partial_len + (size_t)n);
    if (!grown) { if (fd >= 0) close(fd); return -ENOMEM; }
    w->partial = grown;
    memcpy(w->partial + w->partial_len, buf, (size_t)n);
    w->partial_len += (size_t)n;
    size_t produced = 0;
    for (;;) {
        unsigned char *nl = memchr(w->partial, '\n', w->partial_len);
        if (!nl) break;
        size_t len = (size_t)(nl - w->partial);
        if (w->count == MAX_LINES) { if (fd >= 0) close(fd); return -EPROTO; }
        line *l = &w->lines[(w->head + w->count) % MAX_LINES];
        l->bytes = malloc(len + 1);
        if (!l->bytes) { if (fd >= 0) close(fd); return -ENOMEM; }
        memcpy(l->bytes, w->partial, len); l->bytes[len] = 0; l->len = len; l->fd = -1;
        ++w->count; ++produced;
        memmove(w->partial, nl + 1, w->partial_len - len - 1);
        w->partial_len -= len + 1;
    }
    if (w->partial_len > MAX_LINE) { if (fd >= 0) close(fd); return -EPROTO; }
    if (fd >= 0) {
        if (!produced) { close(fd); return -EPROTO; }
        w->lines[(w->head + w->count - 1) % MAX_LINES].fd = fd;
    }
    return 0;
}
static int wire_line(wire *w, line *out)
{
    while (!w->count) {
        int rc = wire_fill(w);
        if (rc != 0) return rc;
    }
    *out = w->lines[w->head];
    w->lines[w->head] = (line){0};
    w->head = (w->head + 1) % MAX_LINES; --w->count;
    return 0;
}
/* Send one command and read its answer: replies only, in command order. */
static int wire_call(wire *w, const char *request, line *reply, const unsigned char **data, size_t *data_len)
{
    int rc = wire_send(w, request);
    if (rc != 0) return rc;
    if ((rc = wire_line(w, reply)) != 0) return rc;
    rc = fips_native__reply(reply->bytes, reply->len, data, data_len);
    if (rc != 0) { if (reply->fd >= 0) close(reply->fd); free(reply->bytes); reply->bytes = NULL; reply->fd = -1; }
    return rc;
}
static void prepare_fd(int fd)
{
#ifdef SO_NOSIGPIPE
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#else
    (void)fd;
#endif
}
int fips_native__stream_wrap(int fd, const fips_native_addr *peer, const fips_native_addr *local, size_t max_payload,
                             fips_native_stream **out)
{
    fips_native_stream *s = calloc(1, sizeof *s);
    if (!s) { close(fd); return -ENOMEM; }
    int type = 0; socklen_t type_len = sizeof type;
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &type_len) != 0) { int e = errno; close(fd); free(s); return -e; }
    if (type == SOCK_STREAM) {
        if (pthread_mutex_init(&s->send_mu, NULL) != 0) { close(fd); free(s); return -ENOMEM; }
        if (pthread_mutex_init(&s->recv_mu, NULL) != 0) { pthread_mutex_destroy(&s->send_mu); close(fd); free(s); return -ENOMEM; }
        s->framed = true;
    }
    prepare_fd(fd);
    s->fd = fd; s->peer = *peer; s->local = *local; s->max = max_payload;
    *out = s;
    return 0;
}
int fips_native__listener_wrap(int fd, const fips_native_addr *local, fips_native_listener **out)
{
    fips_native_listener *l = calloc(1, sizeof *l);
    if (!l) { close(fd); return -ENOMEM; }
    prepare_fd(fd);
    l->fd = fd; l->local = *local;
    *out = l;
    return 0;
}
static int connect_exchange(wire *w, uint16_t local_port, const fips_native_addr *peer, fips_native_stream **out)
{
    char npub[FIPS_NATIVE_NPUB_CAPACITY], request[256];
    int rc = fips_native_npub_encode(peer->key, npub, sizeof npub);
    if (rc != 0) return rc;
    /* local_port 0 is spelled out rather than omitted: one spelling of
     * "any port" for connect and listen both. */
    snprintf(request, sizeof request, "{\"command\":\"connect\",\"params\":{" FRAMING_PARAM "\"local_port\":%u,\"peer\":\"%s\",\"remote_port\":%u}}\n",
        (unsigned)local_port, npub, (unsigned)peer->port);
    line reply; const unsigned char *data; size_t data_len;
    if ((rc = wire_call(w, request, &reply, &data, &data_len)) != 0) return rc;
    fips_native_addr far, near; size_t max;
    rc = reply.fd < 0 ? -EPROTO : opened((jv){data, data_len}, &far, &near, &max);
    free(reply.bytes);
    if (rc != 0) { if (reply.fd >= 0) close(reply.fd); return rc; }
    return fips_native__stream_wrap(reply.fd, &far, &near, max, out);
}
static int listen_exchange(wire *w, uint16_t local_port, fips_native_listener **out)
{
    char request[128];
    snprintf(request, sizeof request, "{\"command\":\"listen\",\"params\":{" FRAMING_PARAM "\"local_port\":%u}}\n", (unsigned)local_port);
    line reply; const unsigned char *data; size_t data_len;
    int rc = wire_call(w, request, &reply, &data, &data_len);
    if (rc != 0) return rc;
    fips_native_addr local = {0};
    rc = reply.fd < 0 ? -EPROTO : key_field((jv){data, data_len}, "node", local.key);
    if (rc == 0) rc = port_field((jv){data, data_len}, "local_port", &local.port);
    free(reply.bytes);
    if (rc != 0) { if (reply.fd >= 0) close(reply.fd); return rc; }
    return fips_native__listener_wrap(reply.fd, &local, out);
}
int fips_native__connect_on(int fd, uint16_t local_port, const fips_native_addr *peer, fips_native_stream **out)
{
    wire w; memset(&w, 0, sizeof w); w.fd = fd;
    int rc = connect_exchange(&w, local_port, peer, out);
    wire_close(&w);
    return rc;
}
int fips_native__listen_on(int fd, uint16_t local_port, fips_native_listener **out)
{
    wire w; memset(&w, 0, sizeof w); w.fd = fd;
    int rc = listen_exchange(&w, local_port, out);
    wire_close(&w);
    return rc;
}
int fips_native_connect(const char *socket_path, uint16_t local_port, const fips_native_addr *peer, fips_native_stream **out)
{
    if (!peer || !out) return -EINVAL;
    *out = NULL;
    wire w;
    int rc = wire_open(socket_path, &w);
    if (rc != 0) return rc;
    rc = connect_exchange(&w, local_port, peer, out);
    /* The connection owns nothing and is closed before the caller holds the stream. */
    wire_close(&w);
    return rc;
}
int fips_native_listen(const char *socket_path, uint16_t local_port, fips_native_listener **out)
{
    if (!out) return -EINVAL;
    *out = NULL;
    wire w;
    int rc = wire_open(socket_path, &w);
    if (rc != 0) return rc;
    rc = listen_exchange(&w, local_port, out);
    wire_close(&w);
    return rc;
}
int fips_native_accept(fips_native_listener *listener, fips_native_stream **out, fips_native_addr *peer)
{
    if (!listener || !out) return -EINVAL;
    *out = NULL;
    unsigned char buf[CHUNK];
    struct iovec iov = {.iov_base = buf, .iov_len = sizeof buf};
    union { struct cmsghdr header; unsigned char bytes[CMSG_SPACE(sizeof(int) * FDS_PER_MESSAGE)]; } control;
    memset(&control, 0, sizeof control);
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control.bytes, .msg_controllen = sizeof control.bytes};
    int flags = 0;
#ifdef MSG_CMSG_CLOEXEC
    flags = MSG_CMSG_CLOEXEC;
#endif
    ssize_t n;
    do n = recvmsg(listener->fd, &msg, flags); while (n < 0 && errno == EINTR);
    if (n < 0) return errno == ECONNRESET ? -EPIPE : -errno;
    int fd = -1;
    for (struct cmsghdr *h = CMSG_FIRSTHDR(&msg); h; h = CMSG_NXTHDR(&msg, h)) {
        if (h->cmsg_level != SOL_SOCKET || h->cmsg_type != SCM_RIGHTS) continue;
        size_t payload = h->cmsg_len - CMSG_LEN(0);
        for (size_t i = 0; i + sizeof(int) <= payload; i += sizeof(int)) {
            int got; memcpy(&got, CMSG_DATA(h) + i, sizeof got);
#ifndef MSG_CMSG_CLOEXEC
            int fl = fcntl(got, F_GETFD); if (fl >= 0) (void)fcntl(got, F_SETFD, fl | FD_CLOEXEC);
#endif
            if (fd < 0) fd = got; else close(got);
        }
    }
    /* Taken before the truncation check, so nothing leaks on that path. */
    if (msg.msg_flags & MSG_CTRUNC) { if (fd >= 0) close(fd); return -EPROTO; }
    /* No descriptor: the daemon closed its half, or wrote an arrival with no flow in it. */
    if (fd < 0) return -EPIPE;
    fips_native_addr far, near; size_t max;
    int rc = opened((jv){buf, (size_t)n}, &far, &near, &max);
    /* An unparseable arrival refuses the flow rather than leaking it. */
    if (rc != 0) { close(fd); return rc; }
    rc = fips_native__stream_wrap(fd, &far, &near, max, out);
    if (rc == 0 && peer) *peer = far;
    return rc;
}
static int peer_gone(int error) { return error == ECONNRESET || error == EDESTADDRREQ ? -EPIPE : -error; }
/* One frame. Nothing written yet: a deadline or non-blocking mode reports
 * -EAGAIN as a datagram socket would. Part written: the rest must follow or
 * the stream is corrupt, so the call waits for writability (the daemon's
 * reader drains continuously) whatever the deadline. */
static int send_frame(fips_native_stream *s, const void *buf, size_t len)
{
    uint8_t header[FIPSD_NATIVE_FRAME_HEADER];
    fipsd_native_frame_header(header, (uint32_t)len);
    struct iovec iov[2] = {{.iov_base = header, .iov_len = sizeof header}, {.iov_base = (void *)buf, .iov_len = len}};
    size_t first = 0, left = sizeof header + len;
    bool started = false;
    while (left) {
        struct msghdr msg = {.msg_iov = iov + first};
        msg.msg_iovlen = first ? 1 : 2;
        ssize_t n = sendmsg(s->fd, &msg, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            if ((errno == EAGAIN || errno == EWOULDBLOCK) && started) {
                struct pollfd p = {.fd = s->fd, .events = POLLOUT};
                if (poll(&p, 1, -1) < 0 && errno != EINTR) return -errno;
                continue;
            }
            return peer_gone(errno);
        }
        started = true;
        left -= (size_t)n;
        for (size_t sent = (size_t)n; first < 2 && sent;) {
            size_t step = sent < iov[first].iov_len ? sent : iov[first].iov_len;
            iov[first].iov_base = (uint8_t *)iov[first].iov_base + step; iov[first].iov_len -= step; sent -= step;
            if (!iov[first].iov_len) ++first;
        }
        if (first < 2 && !iov[first].iov_len) ++first;
    }
    return 0;
}
/* One frame, read only up to its end so whatever follows stays queued in the
 * kernel (and the descriptor stays readable for it). */
static ssize_t recv_frame(fips_native_stream *s, void *buf, size_t cap)
{
    for (;;) {
        void *at; size_t want;
        bool header = s->header_have < sizeof s->header;
        if (header) {
            at = s->header + s->header_have; want = sizeof s->header - s->header_have;
        } else {
            s->body_len = fipsd_native_frame_length(s->header);
            if (s->body_len > FIPSD_NATIVE_FRAME_MAX) return -EPROTO; /* not this protocol; every later call too */
            if (s->body_have == s->body_len) {
                size_t n = s->body_len < cap ? s->body_len : cap; /* longer than cap: the rest is discarded */
                if (n) memcpy(buf, s->body, n);
                s->header_have = s->body_have = 0;
                return (ssize_t)n;
            }
            if (s->body_len > s->body_cap) {
                uint8_t *grown = realloc(s->body, s->body_len);
                if (!grown) return -ENOMEM;
                s->body = grown; s->body_cap = s->body_len;
            }
            at = s->body + s->body_have; want = s->body_len - s->body_have;
        }
        ssize_t n = recv(s->fd, at, want, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return peer_gone(errno); /* -EAGAIN keeps what was read for the next call */
        }
        if (n == 0) return -EPIPE; /* the daemon closed its half, mid-frame or not */
        if (header) s->header_have += (size_t)n; else s->body_have += (size_t)n;
    }
}
int fips_native_send(fips_native_stream *s, const void *buf, size_t len)
{
    if (!s) return -EINVAL;
    if (len > s->max) return -EMSGSIZE;
    if (s->framed) {
        if (len > FIPSD_NATIVE_FRAME_MAX) return -EMSGSIZE;
        pthread_mutex_lock(&s->send_mu);
        int rc = send_frame(s, buf, len);
        pthread_mutex_unlock(&s->send_mu);
        return rc;
    }
    for (;;) {
        ssize_t n = send(s->fd, buf, len, MSG_NOSIGNAL);
        if (n >= 0) return 0;
        if (errno == EINTR) continue;
        return errno == ECONNRESET || errno == EDESTADDRREQ ? -EPIPE : -errno;
    }
}
static bool hung_up(int fd)
{
    struct pollfd p = {.fd = fd, .events = POLLIN};
    int rc = poll(&p, 1, 0);
    return rc > 0 && (p.revents & POLLHUP);
}
static size_t queued(int fd)
{
    int n = 0;
    if (ioctl(fd, FIONREAD, &n) < 0 || n < 0) return 0;
    return (size_t)n;
}
ssize_t fips_native_recv(fips_native_stream *s, void *buf, size_t cap)
{
    if (!s) return -EINVAL;
    if (s->framed) {
        pthread_mutex_lock(&s->recv_mu);
        ssize_t n = recv_frame(s, buf, cap);
        pthread_mutex_unlock(&s->recv_mu);
        return n;
    }
    for (;;) {
        ssize_t n = recv(s->fd, buf, cap, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return errno == ECONNRESET || errno == EDESTADDRREQ ? -EPIPE : -errno;
        }
        /* POLLHUP alone is not the rule: it latches while messages are queued. */
        if (n == 0 && hung_up(s->fd) && queued(s->fd) == 0) return -EPIPE;
        return n;
    }
}
void fips_native_stream_close(fips_native_stream *s)
{
    if (!s) return;
    close(s->fd);
    if (s->framed) { pthread_mutex_destroy(&s->send_mu); pthread_mutex_destroy(&s->recv_mu); }
    free(s->body);
    free(s);
}
void fips_native_listener_close(fips_native_listener *l)
{
    if (!l) return;
    close(l->fd);
    free(l);
}
fips_native_addr fips_native_stream_peer_addr(const fips_native_stream *s) { return s->peer; }
fips_native_addr fips_native_stream_local_addr(const fips_native_stream *s) { return s->local; }
size_t fips_native_stream_max_payload(const fips_native_stream *s) { return s->max; }
int fips_native_stream_fd(const fips_native_stream *s) { return s->fd; }
static int set_timeout(int fd, int option, int64_t ms)
{
    if (ms == 0) return -EINVAL;
    struct timeval tv = {0, 0};
    if (ms > 0) {
        int64_t secs = ms / 1000;
        tv.tv_sec = secs > (int64_t)INT32_MAX ? (time_t)INT32_MAX : (time_t)secs;
        tv.tv_usec = (suseconds_t)((ms % 1000) * 1000);
    }
    return setsockopt(fd, SOL_SOCKET, option, &tv, sizeof tv) < 0 ? -errno : 0;
}
static int get_timeout(int fd, int option, int64_t *ms)
{
    struct timeval tv = {0, 0};
    socklen_t len = sizeof tv;
    if (getsockopt(fd, SOL_SOCKET, option, &tv, &len) < 0) return -errno;
    *ms = tv.tv_sec == 0 && tv.tv_usec == 0 ? -1 : (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    return 0;
}
int fips_native_stream_set_read_timeout(fips_native_stream *s, int64_t ms) { return set_timeout(s->fd, SO_RCVTIMEO, ms); }
int fips_native_stream_set_write_timeout(fips_native_stream *s, int64_t ms) { return set_timeout(s->fd, SO_SNDTIMEO, ms); }
int fips_native_stream_read_timeout(const fips_native_stream *s, int64_t *ms) { return get_timeout(s->fd, SO_RCVTIMEO, ms); }
int fips_native_stream_write_timeout(const fips_native_stream *s, int64_t *ms) { return get_timeout(s->fd, SO_SNDTIMEO, ms); }
static int set_nonblocking(int fd, bool nonblocking)
{
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0) return -errno;
    int wanted = nonblocking ? flags | O_NONBLOCK : flags & ~O_NONBLOCK;
    return fcntl(fd, F_SETFL, wanted) < 0 ? -errno : 0;
}
int fips_native_stream_set_nonblocking(fips_native_stream *s, bool nonblocking) { return set_nonblocking(s->fd, nonblocking); }
fips_native_addr fips_native_listener_local_addr(const fips_native_listener *l) { return l->local; }
int fips_native_listener_fd(const fips_native_listener *l) { return l->fd; }
int fips_native_listener_set_nonblocking(fips_native_listener *l, bool nonblocking) { return set_nonblocking(l->fd, nonblocking); }
const char *fips_native_strerror(int error)
{
    if (error >= 0) return "success";
    if (error == -EPROTO) return "the daemon is not speaking the native API protocol";
    return strerror(-error);
}
