#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "torctl_fixture.h"
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
static bool wait_readable(torctl_fixture *f, int fd)
{
    for (;;) {
        struct pollfd p[] = {{fd, POLLIN, 0}, {f->stop[0], POLLIN, 0}};
        int n = poll(p, 2, -1);
        if (n < 0 && errno == EINTR) continue;
        assert(n > 0);
        return !p[1].revents;
    }
}
static void reply(torctl_fixture *f, int fd, const char *text)
{
    /* mu held: an injected event never splits a reply */
    size_t n = strlen(text);
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags = MSG_NOSIGNAL; /* a client that left is EPIPE, not a signal (Apple: SO_NOSIGPIPE at accept) */
#endif
    for (size_t sent = 0; sent < n;) {
        ssize_t k = send(fd, text + sent, n - sent, flags);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return;
        sent += (size_t)k;
    }
    (void)f;
}
static void value(torctl_fixture *f, const char *key, char *out, size_t cap)
{
    out[0] = 0;
    if (!strcmp(key, "status/bootstrap-phase"))
        snprintf(out, cap, "NOTICE BOOTSTRAP PROGRESS=%u TAG=%s SUMMARY=\"%s\"", f->bootstrap,
            f->bootstrap == 100 ? "done" : "loading", f->bootstrap == 100 ? "Done" : "Loading");
    else if (!strcmp(key, "status/circuit-established")) snprintf(out, cap, "%d", f->circuit);
    else if (!strcmp(key, "traffic/read")) snprintf(out, cap, "%llu", (unsigned long long)f->traffic_read);
    else if (!strcmp(key, "traffic/written")) snprintf(out, cap, "%llu", (unsigned long long)f->traffic_written);
    else if (!strcmp(key, "network-liveness")) snprintf(out, cap, "%s", f->liveness);
    else if (!strcmp(key, "version")) snprintf(out, cap, "%s", f->version);
    else if (!strcmp(key, "dormant")) snprintf(out, cap, "%d", f->dormant);
}
static void getinfo(torctl_fixture *f, int fd, char *keys)
{
    char out[2048] = "", line[512], v[256];
    if (!strncmp(keys, "hs/service/desc/id/", 19)) { /* a dot-terminated multi-line value */
        if (f->descriptor) snprintf(out, sizeof out, "250+%s=\r\nhs-descriptor 3\r\ndescriptor-lifetime 180\r\n.\r\n250 OK\r\n", keys);
        else snprintf(out, sizeof out, "552 Unknown onion service\r\n");
        reply(f, fd, out); return;
    }
    size_t count = 0; bool dormant = false;
    for (char *save = NULL, *k = strtok_r(keys, " ", &save); k; k = strtok_r(NULL, " ", &save)) {
        ++count; dormant |= !strcmp(k, "dormant");
        value(f, k, v, sizeof v);
        if (!v[0]) { snprintf(out, sizeof out, "552 Unrecognized key \"%s\"\r\n", k); reply(f, fd, out); return; }
        snprintf(line, sizeof line, "250-%s=%s\r\n", k, v); strcat(out, line);
    }
    if (f->refuse_batch && (count > 1 || dormant)) { reply(f, fd, "552 Unrecognized key \"dormant\"\r\n"); return; }
    strcat(out, "250 OK\r\n"); reply(f, fd, out);
}
static void command(torctl_fixture *f, int fd, char *line)
{
    char *copy = strdup(line); assert(copy);
    if (go_channel_try_send(f->commands, copy)) free(copy);
    pthread_mutex_lock(&f->mu);
    if (!strncmp(line, "AUTHENTICATE", 12)) {
        const char *arg = line[12] == ' ' ? line + 13 : "";
        char expected[80];
        if (f->use_cookie) {
            for (size_t i = 0; i < 32; ++i) snprintf(expected + 2 * i, 3, "%02x", f->cookie[i]);
        } else { /* control-spec quoted string: backslash and quote escaped */
            char *e = expected; *e++ = '"';
            for (const char *s = f->password; *s; ++s) { if (*s == '\\' || *s == '"') *e++ = '\\'; *e++ = *s; }
            *e++ = '"'; *e = 0;
        }
        if (f->reject_auth || strcmp(arg, expected)) reply(f, fd, "515 Authentication failed: Password did not match HashedControlPassword *or* authentication cookie.\r\n");
        else { ++f->authenticated; reply(f, fd, "250 OK\r\n"); }
    } else if (!strncmp(line, "GETINFO ", 8)) getinfo(f, fd, line + 8);
    else if (!strncmp(line, "SETEVENTS", 9)) reply(f, fd, "250 OK\r\n");
    else reply(f, fd, "510 Unrecognized command\r\n");
    pthread_mutex_unlock(&f->mu);
}
static void session(torctl_fixture *f, int fd)
{
    char buf[4096]; size_t used = 0;
    while (wait_readable(f, fd)) {
        ssize_t k = read(fd, buf + used, sizeof buf - 1 - used);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return;
        used += (size_t)k; buf[used] = 0;
        char *nl;
        while ((nl = strstr(buf, "\r\n"))) {
            *nl = 0; command(f, fd, buf);
            size_t drop = (size_t)(nl - buf) + 2;
            memmove(buf, buf + drop, used - drop + 1); used -= drop;
        }
        assert(used < sizeof buf - 1);
    }
}
static void *serve(void *arg)
{
    torctl_fixture *f = arg;
    while (wait_readable(f, f->fd)) {
        int fd = accept(f->fd, NULL, NULL);
        if (fd < 0) continue;
#ifdef SO_NOSIGPIPE
        int one = 1; (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        pthread_mutex_lock(&f->mu); f->client = fd; ++f->connections; pthread_mutex_unlock(&f->mu);
        session(f, fd);
        pthread_mutex_lock(&f->mu); f->client = -1; close(fd); pthread_mutex_unlock(&f->mu);
    }
    return NULL;
}
void torctl_fixture_start(torctl_fixture *f, const char *unix_path)
{
    bool use_cookie = f->use_cookie, reject = f->reject_auth, refuse = f->refuse_batch, descriptor = f->descriptor;
    uint8_t cookie[32]; memcpy(cookie, f->cookie, 32);
    memset(f, 0, sizeof *f);
    f->use_cookie = use_cookie; f->reject_auth = reject; f->refuse_batch = refuse; f->descriptor = descriptor;
    memcpy(f->cookie, cookie, 32);
    f->client = -1; f->bootstrap = 10; f->traffic_read = 1048576; f->traffic_written = 524288;
    strcpy(f->liveness, "up"); strcpy(f->version, "0.4.8.10"); strcpy(f->password, "test\"pa\\ss");
    assert(!pthread_mutex_init(&f->mu, NULL));
    f->commands = go_channel_create(256); assert(f->commands && !pipe(f->stop));
    if (unix_path) {
        struct sockaddr_un a = {.sun_family = AF_UNIX};
        assert(strlen(unix_path) < sizeof a.sun_path); strcpy(a.sun_path, unix_path);
        (void)unlink(unix_path);
        f->fd = socket(AF_UNIX, SOCK_STREAM, 0); assert(f->fd >= 0);
        assert(!bind(f->fd, (struct sockaddr *)&a, sizeof a) && !listen(f->fd, 8));
        snprintf(f->addr, sizeof f->addr, "%s", unix_path); snprintf(f->path, sizeof f->path, "%s", unix_path);
    } else {
        f->fd = socket(AF_INET, SOCK_STREAM, 0); assert(f->fd >= 0);
        struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
        assert(!bind(f->fd, (struct sockaddr *)&a, sizeof a) && !listen(f->fd, 8));
        socklen_t len = sizeof a; assert(!getsockname(f->fd, (struct sockaddr *)&a, &len));
        snprintf(f->addr, sizeof f->addr, "127.0.0.1:%u", ntohs(a.sin_port));
    }
    assert(!pthread_create(&f->thread, NULL, serve, f));
}
void torctl_fixture_stop(torctl_fixture *f)
{
    assert(write(f->stop[1], "x", 1) == 1); assert(!pthread_join(f->thread, NULL));
    close(f->fd); close(f->stop[0]); close(f->stop[1]);
    if (f->path[0]) (void)unlink(f->path);
    void *p; while (!go_channel_try_receive(f->commands, &p)) free(p);
    go_channel_unref(f->commands); pthread_mutex_destroy(&f->mu);
}
void torctl_fixture_event(torctl_fixture *f, const char *event)
{
    pthread_mutex_lock(&f->mu);
    const char *p = strstr(event, "BOOTSTRAP PROGRESS=");
    if (p) f->bootstrap = (unsigned)atoi(p + 19);
    if (strstr(event, "CIRCUIT_ESTABLISHED") && !strstr(event, "NOT_ESTABLISHED")) f->circuit = true;
    if (strstr(event, "CIRCUIT_NOT_ESTABLISHED")) f->circuit = false;
    if (!strncmp(event, "NETWORK_LIVENESS ", 17)) strcpy(f->liveness, !strcmp(event + 17, "UP") ? "up" : "down");
    unsigned long long r, w;
    if (sscanf(event, "BW %llu %llu", &r, &w) == 2) { f->traffic_read += r; f->traffic_written += w; }
    char line[512]; snprintf(line, sizeof line, "650 %s\r\n", event);
    assert(f->client >= 0);
    reply(f, f->client, line);
    pthread_mutex_unlock(&f->mu);
}
void torctl_fixture_drop(torctl_fixture *f)
{
    pthread_mutex_lock(&f->mu);
    if (f->client >= 0) (void)shutdown(f->client, SHUT_RDWR);
    pthread_mutex_unlock(&f->mu);
}
