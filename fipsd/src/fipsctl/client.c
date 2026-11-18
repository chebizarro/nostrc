/* The control-socket client: default socket resolution, the IPv6 adapter's
 * sibling socket, one request/one response over a Unix socket (TCP on
 * 127.0.0.1 on Windows), and the show_status composition (upstream
 * fipsctl.rs send_request/query, config.rs default_control_path/
 * ipv6tun_socket/ipv6tun_client, control/compose.rs). */
#include "fipsctl.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#if defined(_WIN32)
/* Windows: the control socket's "path" is a TCP port on 127.0.0.1, 21210 by
 * default (upstream default_control_path). */
char *fipsctl_default_socket_path(void) { return strdup("21210"); }

/* Rust's u16::from_str: an optional '+', then decimal digits up to 65535. */
bool fipsctl_parse_port(const char *text, uint16_t *port)
{
    const char *p = text + (*text == '+');
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

/* The adapter's status socket is the control port plus 2 (21211, one above
 * the default, is the gateway's); a port that does not parse is read as
 * 21210, as the daemon reads it, port 0 stays 0, and NULL when the port is
 * too high to offset (upstream config.rs ipv6tun_socket). */
char *fipsctl_ipv6tun_socket(const char *control)
{
    uint16_t port;
    if (!fipsctl_parse_port(control, &port)) port = 21210;
    if (port == 0) return strdup("0");
    if (port > UINT16_MAX - 2) return NULL;
    return fipsctl_aprintf("%u", (unsigned)port + 2);
}

char *fipsctl_ipv6tun_client(const char *control) { return fipsctl_ipv6tun_socket(control); }
#else
static bool is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* `/run/fips/control.sock` -> `/var/run/fips/control.sock` (macOS/FreeBSD) ->
 * `$XDG_RUNTIME_DIR/fips/control.sock` -> `/tmp/fips-control.sock`, selecting
 * by existence as the daemon does (fipsd/src/config/config.c). */
char *fipsctl_default_socket_path(void)
{
    const char *filename = "control.sock";
    const char *dir = NULL;
    if (is_dir("/run/fips")) dir = "/run/fips";
#if defined(__APPLE__) || defined(__FreeBSD__)
    else if (is_dir("/var/run/fips")) dir = "/var/run/fips";
#ifdef __APPLE__
    else if (geteuid() == 0 && is_dir("/var/run")) dir = "/var/run/fips";
#endif
#endif
    if (dir) return fipsctl_aprintf("%s/%s", dir, filename);
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (xdg && xdg[0] && is_dir(xdg)) return fipsctl_aprintf("%s/fips/%s", xdg, filename);
    return fipsctl_aprintf("/tmp/fips-%s", filename);
}

/* The sibling named from the control socket's file name with `-ipv6tun`
 * before the extension: `control.sock` -> `control-ipv6tun.sock`, `node.ctl`
 * -> `node-ipv6tun.ctl`, `node` -> `node-ipv6tun`. NULL for a path with no
 * file name (`/`, `.`, `..`), as upstream's Path::file_stem has none. */
char *fipsctl_ipv6tun_socket(const char *control)
{
    size_t len = strlen(control);
    while (len > 1 && control[len - 1] == '/') --len; /* Rust ignores a trailing slash */
    size_t base = len;
    while (base > 0 && control[base - 1] != '/') --base;
    size_t name_len = len - base;
    const char *name = control + base;
    if (!name_len || (name_len == 1 && name[0] == '.') || (name_len == 2 && name[0] == '.' && name[1] == '.'))
        return NULL;
    /* Path::extension: after the last '.', unless that dot is the first char. */
    size_t dot = name_len;
    for (size_t i = name_len; i > 1; --i)
        if (name[i - 1] == '.') { dot = i - 1; break; }
    size_t stem_len = dot;
    char *out = malloc(base + stem_len + 8 + (name_len - dot) + 1);
    if (!out) return NULL;
    memcpy(out, control, base);
    memcpy(out + base, name, stem_len);
    memcpy(out + base + stem_len, "-ipv6tun", 8);
    memcpy(out + base + stem_len + 8, name + dot, name_len - dot);
    out[base + stem_len + 8 + (name_len - dot)] = 0;
    return out;
}

/* A symlink given with -s is followed to the real control socket first; a
 * path that cannot be resolved is used as given. */
char *fipsctl_ipv6tun_client(const char *control)
{
    char real[PATH_MAX];
    const char *base = realpath(control, real) ? real : control;
    return fipsctl_ipv6tun_socket(base);
}
#endif

static bool set_error(char **error, char *message)
{
    if (error) *error = message; else free(message);
    return false;
}

#if defined(_WIN32)
typedef SOCKET client_socket;
#define close_socket closesocket
#define IO_LEN(n) ((int)(n))
#define INTERRUPTED() (WSAGetLastError() == WSAEINTR)
/* As std::io::Error prints a Windows error: the system's message, then
 * "(os error <n>)". */
static char *socket_error_text(int code)
{
    char text[512] = "";
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, (DWORD)code,
                             MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), text, (DWORD)sizeof text, NULL);
    while (n && (text[n - 1] == '\r' || text[n - 1] == '\n' || text[n - 1] == ' ')) text[--n] = 0;
    return fipsctl_aprintf("%s (os error %d)", n ? text : "unknown error", code);
}
static int last_socket_error(void) { return WSAGetLastError(); }
/* One TCP connection to 127.0.0.1:<port> (upstream fipsctl.rs send_request,
 * windows): an unparsable port warns and uses 21210. */
static client_socket connect_control(const char *socket_path, char **error)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    static int started;
    BOOL pending = FALSE;
    if (InitOnceBeginInitialize(&once, 0, &pending, NULL) && pending) {
        WSADATA data;
        started = WSAStartup(MAKEWORD(2, 2), &data) == 0;
        InitOnceComplete(&once, 0, NULL);
    }
    uint16_t port;
    if (!fipsctl_parse_port(socket_path, &port)) {
        fprintf(stderr, "warning: invalid port '%s', using default 21210\n", socket_path);
        port = 21210;
    }
    char addr_text[32];
    snprintf(addr_text, sizeof addr_text, "127.0.0.1:%u", (unsigned)port);
    SOCKET fd = started ? socket(AF_INET, SOCK_STREAM, IPPROTO_TCP) : INVALID_SOCKET;
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    if (fd == INVALID_SOCKET || connect(fd, (struct sockaddr *)&addr, (int)sizeof addr) != 0) {
        char *text = socket_error_text(started ? WSAGetLastError() : WSASYSNOTREADY);
        if (fd != INVALID_SOCKET) closesocket(fd);
        set_error(error, fipsctl_aprintf("cannot connect to %s: %s\nIs the FIPS daemon running?", addr_text, text ? text : "?"));
        free(text);
        return INVALID_SOCKET;
    }
    DWORD timeout_ms = 5000;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms, (int)sizeof timeout_ms);
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout_ms, (int)sizeof timeout_ms);
    return fd;
}
#else
typedef int client_socket;
#define INVALID_SOCKET (-1)
#define close_socket close
#define IO_LEN(n) (n)
#define INTERRUPTED() (errno == EINTR)
static char *socket_error_text(int code) { return fipsctl_strerror(code); }
static int last_socket_error(void) { return errno; }
static client_socket connect_control(const char *socket_path, char **error)
{
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(socket_path) >= sizeof(addr.sun_path)) {
        set_error(error, fipsctl_aprintf("cannot connect to %s: path must be shorter than SUN_LEN (os error %d)\n"
                                         "Is the FIPS daemon running?", socket_path, EINVAL));
        return INVALID_SOCKET;
    }
    memcpy(addr.sun_path, socket_path, strlen(socket_path) + 1);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        char *e = fipsctl_strerror(errno);
        set_error(error, fipsctl_aprintf("cannot connect to %s: %s\nIs the FIPS daemon running?", socket_path, e ? e : "?"));
        free(e);
        return INVALID_SOCKET;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        int e = errno;
        close(fd);
        char *text = fipsctl_strerror(e);
        if (e == EACCES || e == EPERM)
            set_error(error, fipsctl_aprintf("cannot connect to %s: %s\n"
                                             "Hint: add your user to the 'fips' group: sudo usermod -aG fips $USER\n"
                                             "Then log out and back in for the change to take effect.",
                                             socket_path, text ? text : "?"));
        else
            set_error(error, fipsctl_aprintf("cannot connect to %s: %s\nIs the FIPS daemon running?",
                                             socket_path, text ? text : "?"));
        free(text);
        return INVALID_SOCKET;
    }
    struct timeval timeout = {5, 0};
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#ifdef SO_NOSIGPIPE
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    return fd;
}
#endif

/* One request line out, one response line in, over a fresh connection. The
 * five-second read and write deadlines match the daemon's own. */
json_t *fipsctl_send_request(const char *socket_path, const char *request, char **error)
{
    if (error) *error = NULL;
    client_socket fd = connect_control(socket_path, error);
    if (fd == INVALID_SOCKET) return NULL;
    size_t len = strlen(request), sent = 0;
    while (sent < len) {
#if defined(MSG_NOSIGNAL) && !defined(_WIN32)
        ssize_t n = send(fd, request + sent, len - sent, MSG_NOSIGNAL);
#else
        ssize_t n = send(fd, request + sent, IO_LEN(len - sent), 0);
#endif
        if (n < 0 && INTERRUPTED()) continue;
        if (n <= 0) {
            int e = last_socket_error();
            close_socket(fd);
            char *text = socket_error_text(e);
            set_error(error, fipsctl_aprintf("failed to send request: %s", text ? text : "?"));
            free(text);
            return NULL;
        }
        sent += (size_t)n;
    }
#if defined(_WIN32)
    (void)shutdown(fd, SD_SEND);
#else
    (void)shutdown(fd, SHUT_WR);
#endif
    size_t cap = 4096, used = 0;
    char *line = malloc(cap);
    if (!line) { close_socket(fd); set_error(error, strdup("out of memory")); return NULL; }
    bool complete = false;
    while (!complete) {
        if (used == cap) {
            if (cap >= (1u << 26)) { free(line); close_socket(fd); set_error(error, strdup("failed to read response: line too long")); return NULL; }
            char *grown = realloc(line, cap * 2);
            if (!grown) { free(line); close_socket(fd); set_error(error, strdup("out of memory")); return NULL; }
            line = grown; cap *= 2;
        }
        ssize_t n = recv(fd, line + used, IO_LEN(cap - used), 0);
        if (n < 0 && INTERRUPTED()) continue;
        if (n < 0) {
            int e = last_socket_error();
            free(line); close_socket(fd);
            char *text = socket_error_text(e);
            set_error(error, fipsctl_aprintf("failed to read response: %s", text ? text : "?"));
            free(text);
            return NULL;
        }
        if (n == 0) break;
        char *nl = memchr(line + used, '\n', (size_t)n);
        used += (size_t)n;
        if (nl) { used = (size_t)(nl - line); complete = true; }
    }
    close_socket(fd);
    if (!used && !complete) { free(line); set_error(error, strdup("no response from daemon")); return NULL; }
    if (used && line[used - 1] == '\r') --used; /* BufRead::lines strips \r\n too */
    json_error_t jerr;
    json_t *value = fipsctl_json_load(line, used, &jerr);
    free(line);
    if (!value) {
        set_error(error, fipsctl_aprintf("invalid response JSON: %s at line %d column %d", jerr.text, jerr.line, jerr.column));
        return NULL;
    }
    return value;
}

bool fipsctl_is_ok(const json_t *reply)
{
    const char *status = json_string_value(json_object_get(reply, "status"));
    return status && !strcmp(status, "ok");
}

static const struct { const char *key; const char *text; int number; } ADAPTER_KEYS[] = {
    {"tun_state", "disabled", 0}, {"tun_name", "-", 0}, {"effective_ipv6_mtu", NULL, 0},
};

bool fipsctl_composes(const char *command, const json_t *node_data)
{
    if (strcmp(command, "show_status") || !json_is_object(node_data)) return false;
    for (size_t i = 0; i < 3; ++i)
        if (!json_object_get(node_data, ADAPTER_KEYS[i].key)) return true;
    return false;
}

void fipsctl_compose(const char *command, json_t *node_data, const json_t *adapter_data)
{
    if (strcmp(command, "show_status") || !json_is_object(node_data)) return;
    for (size_t i = 0; i < 3; ++i) {
        const char *key = ADAPTER_KEYS[i].key;
        if (json_object_get(node_data, key)) continue;
        json_t *answered = adapter_data ? json_object_get(adapter_data, key) : NULL;
        json_t *value = answered ? json_deep_copy(answered)
                      : ADAPTER_KEYS[i].text ? json_string(ADAPTER_KEYS[i].text) : json_integer(ADAPTER_KEYS[i].number);
        json_object_set_new(node_data, key, value);
    }
}

json_t *fipsctl_query(const char *socket_path, const char *command, const char *request, char **error)
{
    json_t *value = fipsctl_send_request(socket_path, request, error);
    if (!value || !fipsctl_is_ok(value)) return value;
    json_t *data = json_object_get(value, "data");
    if (!data || !fipsctl_composes(command, data)) return value;
    json_t *adapter = NULL;
    char *adapter_path = fipsctl_ipv6tun_client(socket_path);
    if (adapter_path) {
        json_t *reply = fipsctl_send_request(adapter_path, request, NULL);
        if (reply && fipsctl_is_ok(reply)) adapter = json_incref(json_object_get(reply, "data"));
        json_decref(reply);
        free(adapter_path);
    }
    fipsctl_compose(command, data, adapter);
    json_decref(adapter);
    return value;
}
