/* The control-socket client: default socket resolution, the IPv6 adapter's
 * sibling socket, one request/one response over a Unix socket, and the
 * show_status composition (upstream fipsctl.rs send_request/query, config.rs
 * default_control_path/ipv6tun_client, control/compose.rs). */
#include "fipsctl.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

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

static bool set_error(char **error, char *message)
{
    if (error) *error = message; else free(message);
    return false;
}

/* One request line out, one response line in, over a fresh connection. The
 * five-second read and write deadlines match the daemon's own. */
json_t *fipsctl_send_request(const char *socket_path, const char *request, char **error)
{
    if (error) *error = NULL;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(socket_path) >= sizeof(addr.sun_path)) {
        set_error(error, fipsctl_aprintf("cannot connect to %s: path must be shorter than SUN_LEN (os error %d)\n"
                                         "Is the FIPS daemon running?", socket_path, EINVAL));
        return NULL;
    }
    memcpy(addr.sun_path, socket_path, strlen(socket_path) + 1);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        char *e = fipsctl_strerror(errno);
        set_error(error, fipsctl_aprintf("cannot connect to %s: %s\nIs the FIPS daemon running?", socket_path, e ? e : "?"));
        free(e);
        return NULL;
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
        return NULL;
    }
    struct timeval timeout = {5, 0};
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#ifdef SO_NOSIGPIPE
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    size_t len = strlen(request), sent = 0;
    while (sent < len) {
#ifdef MSG_NOSIGNAL
        ssize_t n = send(fd, request + sent, len - sent, MSG_NOSIGNAL);
#else
        ssize_t n = send(fd, request + sent, len - sent, 0);
#endif
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            int e = errno;
            close(fd);
            char *text = fipsctl_strerror(e);
            set_error(error, fipsctl_aprintf("failed to send request: %s", text ? text : "?"));
            free(text);
            return NULL;
        }
        sent += (size_t)n;
    }
    (void)shutdown(fd, SHUT_WR);
    size_t cap = 4096, used = 0;
    char *line = malloc(cap);
    if (!line) { close(fd); set_error(error, strdup("out of memory")); return NULL; }
    bool complete = false;
    while (!complete) {
        if (used == cap) {
            if (cap >= (1u << 26)) { free(line); close(fd); set_error(error, strdup("failed to read response: line too long")); return NULL; }
            char *grown = realloc(line, cap * 2);
            if (!grown) { free(line); close(fd); set_error(error, strdup("out of memory")); return NULL; }
            line = grown; cap *= 2;
        }
        ssize_t n = read(fd, line + used, cap - used);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            int e = errno;
            free(line); close(fd);
            char *text = fipsctl_strerror(e);
            set_error(error, fipsctl_aprintf("failed to read response: %s", text ? text : "?"));
            free(text);
            return NULL;
        }
        if (n == 0) break;
        char *nl = memchr(line + used, '\n', (size_t)n);
        used += (size_t)n;
        if (nl) { used = (size_t)(nl - line); complete = true; }
    }
    close(fd);
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
