/* The Windows control socket (nostrc-iq1n9.6.3): the production listener on
 * TCP 127.0.0.1 (upstream control/mod.rs windows_impl) and the production
 * client (fipsctl_lib), on an ephemeral port so a running daemon's 21210 is
 * never touched. The protocol bounds are the AF_UNIX suite's
 * (test_control.c): one request, one response, 4096-byte limit, EOF rules,
 * the read and execution deadlines, no head-of-line blocking, the connection
 * cap, and a stop that cancels the pending accept. Clients are blocking
 * Winsock calls on the main thread; the daemon's fibers serve them. */
#include "control/control.h"
#include "fipsctl.h"
#include "test_tmpdir.h"

#include <libgo/fiber.h>
#include <go.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <aclapi.h>
#include <sddl.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s (WSA %d)\n", __FILE__, __LINE__, #x, \
    WSAGetLastError()); abort(); } } while (0)

static GoRefPtr acquire(void *user) { return go_swap_load(user); }
static void free_snapshot(void *p) { fipsd_control_snapshot *s = p; json_decref((json_t *)s->tun_status); free(s); }
static GoSwap *fixture(bool tun)
{
    fipsd_control_snapshot *s = calloc(1, sizeof *s); CHECK(s);
    s->npub = "npub1test"; s->node_addr = "00112233445566778899aabbccddeeff";
    s->ipv6_addr = "fd00::1"; s->state = "running"; s->version = "test";
    if (tun) s->tun_status = json_pack("{s:s,s:s,s:i,s:i}", "tun_state", "active", "tun_name", "fips0",
                                       "effective_ipv6_mtu", 1280, "host_drops", 0);
    GoSwap *swap = go_swap_create(s, free_snapshot); CHECK(swap); return swap;
}
static json_t *deadline_provider(void *user, const char *name, const json_t *params,
                                 const fipsd_control_request_context *context)
{
    (void)user; (void)params;
    if (!strcmp(name, "path_show")) {
        /* Wait for the request's own execution deadline (not a sleep). */
        struct timespec t; CHECK(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
        uint64_t now = (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
        uint64_t ms = context->deadline_ns > now ? (context->deadline_ns - now + 999999) / 1000000 : 0;
        GoChannel *never = go_channel_create(0); CHECK(never);
        void *token = NULL;
        GoSelectCase cases[] = {{.op = GO_SELECT_RECEIVE, .chan = never, .recv_buf = &token},
            {.op = GO_SELECT_RECEIVE, .chan = go_context_done(context->shutdown), .recv_buf = &token}};
        GoSelectResult r = go_select_timeout(cases, 2, ms);
        go_channel_free(never);
        return fipsd_control_error(r.selected_case == -1 ? "query timeout" : "node shutting down");
    }
    return fipsd_control_error("held");
}

static SOCKET dial(uint16_t port)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); CHECK(s != INVALID_SOCKET);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    CHECK(connect(s, (struct sockaddr *)&a, (int)sizeof a) == 0);
    DWORD ms = 15000; /* a stuck daemon fails the test instead of hanging it */
    CHECK(setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, (int)sizeof ms) == 0);
    return s;
}
static void send_all(SOCKET s, const char *data, size_t len)
{
    size_t sent = 0;
    while (sent < len) { int n = send(s, data + sent, (int)(len - sent), 0); CHECK(n > 0); sent += (size_t)n; }
}
/* Everything the daemon writes before its end of file. */
static void read_reply(SOCKET s, char *out, size_t cap)
{
    size_t used = 0;
    for (;;) {
        int n = recv(s, out + used, (int)(cap - 1 - used), 0);
        if (n == 0) break;
        CHECK(n > 0); used += (size_t)n; CHECK(used < cap - 1);
    }
    out[used] = 0;
}
static void exchange(uint16_t port, const char *line, size_t len, bool eof, const char *expected)
{
    SOCKET s = dial(port);
    send_all(s, line, len);
    if (eof) CHECK(shutdown(s, SD_SEND) == 0);
    char out[8192]; read_reply(s, out, sizeof out);
    if (strcmp(out, expected)) fprintf(stderr, "got: %s\nwant: %s\n", out, expected);
    CHECK(!strcmp(out, expected));
    CHECK(closesocket(s) == 0);
}
static fipsd_runtime *start_runtime(fipsd_control_service *service)
{
    fipsd_runtime_config cfg; fipsd_runtime_config_default(&cfg); cfg.handle_signals = false; cfg.workers = 2;
    fipsd_runtime *rt = fipsd_runtime_create(&cfg); CHECK(rt);
    CHECK(fipsd_runtime_register(rt, &service->subsystem) == 0 && fipsd_runtime_start(rt) == 0);
    return rt;
}
static GoSwap *config_for(const char *port)
{
    char yaml[256]; snprintf(yaml, sizeof yaml, "node: {control: {enabled: true, socket_path: '%s'}}", port);
    fipsd_config *config = fipsd_config_parse(yaml, strlen(yaml), NULL); CHECK(config);
    GoSwap *configs = go_swap_create(config, fipsd_config_free); CHECK(configs); return configs;
}

static void test_ports(void)
{
    uint16_t port = 1;
    CHECK(fipsd_control_resolve_port(NULL, &port) && port == FIPSD_CONTROL_DEFAULT_PORT);
    CHECK(fipsd_control_resolve_port("0", &port) && port == 0);
    CHECK(!fipsd_control_resolve_port("not-a-port", &port) && port == 21210);
    /* The client's resolution: default, the adapter's port + 2, Rust's u16 parse. */
    char *p = fipsctl_default_socket_path(); CHECK(p && !strcmp(p, "21210")); free(p);
    p = fipsctl_ipv6tun_socket("21210"); CHECK(p && !strcmp(p, "21212")); free(p);
    p = fipsctl_ipv6tun_client("31000"); CHECK(p && !strcmp(p, "31002")); free(p);
    p = fipsctl_ipv6tun_socket("0"); CHECK(p && !strcmp(p, "0")); free(p);
    p = fipsctl_ipv6tun_socket("bogus"); CHECK(p && !strcmp(p, "21212")); free(p);
    CHECK(fipsctl_ipv6tun_socket("65534") == NULL);
    CHECK(fipsctl_parse_port("+80", &port) && port == 80 && !fipsctl_parse_port("65536", &port));
    CHECK(!strcmp(fipsctl_default_key_dir(), "C:\\ProgramData\\fips"));
    CHECK(!strcmp(fipsctl_default_hosts_path(), "C:\\ProgramData\\fips\\hosts"));
    /* The daemon's default socket_path on Windows is the port, not a path. */
    const char *yaml = "node: {control: {enabled: true}}";
    fipsd_config *config = fipsd_config_parse(yaml, strlen(yaml), NULL); CHECK(config);
    CHECK(!strcmp(fipsd_config_string(fipsd_config_get(config, "node.control.socket_path")), "21210"));
    fipsd_config_free(config);
}

static void test_socket(void)
{
    GoSwap *configs = config_for("0"), *state = fixture(false);
    fipsd_control_provider provider = {.user = state, .acquire = acquire, .command = deadline_provider};
    fipsd_control_service service; fipsd_control_service_init(&service, configs, &provider);
    fipsd_runtime *rt = start_runtime(&service);
    uint16_t port = service.port;
    char text[16]; snprintf(text, sizeof text, "%u", (unsigned)port);
    char where[32]; snprintf(where, sizeof where, "127.0.0.1:%u", (unsigned)port);
    CHECK(port != 0 && !strcmp(service.path, where));
    /* Another listener cannot take the port (SO_EXCLUSIVEADDRUSE). */
    GoSwap *same = config_for(text);
    fipsd_control_service collision; fipsd_control_service_init(&collision, same, NULL);
    CHECK(collision.subsystem.start(&collision.subsystem, rt) < 0);
    fipsd_control_service_clear(&collision);

    const char *ok = "{\"status\":\"ok\",\"data\":{\"peers\":[]}}\n";
    const char *req = "{\"command\":\"show_peers\"}\n";
    exchange(port, req, strlen(req), false, ok);
    exchange(port, "{\"command\":\"bad\"}\n", 18, false, "{\"status\":\"error\",\"message\":\"unknown command: bad\"}\n");
    exchange(port, "", 0, true, "{\"status\":\"error\",\"message\":\"empty request\"}\n");
    exchange(port, req, strlen(req) - 1, true, ok); /* EOF without the delimiter */
    json_t *malformed = fipsd_control_parse(NULL, "{\n", 2); char *bad = fipsd_control_encode(malformed); CHECK(bad);
    char bad_line[1024]; snprintf(bad_line, sizeof bad_line, "%s\n", bad); free(bad); json_decref(malformed);
    exchange(port, "{\n", 2, false, bad_line);
    char boundary[4097]; memset(boundary, ' ', sizeof boundary); memcpy(boundary, req, strlen(req) - 1);
    boundary[4095] = '\n'; exchange(port, boundary, 4096, false, ok);
    boundary[4095] = ' '; boundary[4096] = '\n';
    exchange(port, boundary, 4097, true, "{\"status\":\"error\",\"message\":\"read error: request too large\"}\n");

    /* The production client: one request, and show_status composed with the
     * adapter's defaults (nothing listens on port + 2). */
    char *error = NULL;
    json_t *reply = fipsctl_send_request(text, req, &error);
    CHECK(reply && !error && fipsctl_is_ok(reply)); json_decref(reply);
    reply = fipsctl_query(text, "show_status", "{\"command\":\"show_status\"}\n", &error);
    CHECK(reply && !error && fipsctl_is_ok(reply));
    json_t *data = json_object_get(reply, "data");
    CHECK(!strcmp(json_string_value(json_object_get(data, "npub")), "npub1test"));
    CHECK(!strcmp(json_string_value(json_object_get(data, "tun_state")), "disabled"));
    CHECK(!strcmp(json_string_value(json_object_get(data, "tun_name")), "-"));
    json_decref(reply);

    /* A partly written request does not hold up independent connections. */
    SOCKET slow = dial(port); send_all(slow, "{", 1);
    for (int i = 0; i < 24; ++i) exchange(port, req, strlen(req), false, ok);
    send_all(slow, req + 1, strlen(req) - 1);
    char out[8192]; read_reply(slow, out, sizeof out); CHECK(!strcmp(out, ok)); CHECK(closesocket(slow) == 0);

    /* The two deadlines, at their real 5 s budget: a provider that waits
     * yields "query timeout", a client that never writes "read timeout". */
    const char *execute = "{\"command\":\"path_show\",\"params\":{\"npub\":\"test\"}}\n";
    exchange(port, execute, strlen(execute), false, "{\"status\":\"error\",\"message\":\"query timeout\"}\n");
    exchange(port, "", 0, false, "{\"status\":\"error\",\"message\":\"read timeout\"}\n");

    /* Stop cancels the pending accept; the port is free again afterwards. */
    fipsd_runtime_shutdown(rt); CHECK(fipsd_runtime_run(rt) == FIPSD_RUN_OK); fipsd_runtime_destroy(rt);
    fipsd_control_service_clear(&service);
    SOCKET again = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); CHECK(again != INVALID_SOCKET);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    CHECK(bind(again, (struct sockaddr *)&a, (int)sizeof a) == 0); CHECK(closesocket(again) == 0);
    /* With the daemon gone the client reports what upstream's does. */
    reply = fipsctl_send_request(text, req, &error);
    CHECK(!reply && error && strstr(error, "cannot connect to 127.0.0.1:") && strstr(error, "Is the FIPS daemon running?"));
    free(error);
    go_swap_free(same); go_swap_free(state); go_swap_free(configs);
}

/* The status a node with a TUN reports reaches fipsctl uncomposed. */
static void test_tun_status(void)
{
    GoSwap *configs = config_for("0"), *state = fixture(true);
    fipsd_control_provider provider = {.user = state, .acquire = acquire};
    fipsd_control_service service; fipsd_control_service_init(&service, configs, &provider);
    fipsd_runtime *rt = start_runtime(&service);
    char text[16]; snprintf(text, sizeof text, "%u", (unsigned)service.port);
    char *error = NULL;
    json_t *reply = fipsctl_query(text, "show_status", "{\"command\":\"show_status\"}\n", &error);
    CHECK(reply && !error && fipsctl_is_ok(reply));
    json_t *data = json_object_get(reply, "data");
    CHECK(!strcmp(json_string_value(json_object_get(data, "tun_state")), "active"));
    CHECK(!strcmp(json_string_value(json_object_get(data, "tun_name")), "fips0"));
    CHECK(json_integer_value(json_object_get(data, "effective_ipv6_mtu")) == 1280);
    json_decref(reply);
    fipsd_runtime_shutdown(rt); CHECK(fipsd_runtime_run(rt) == FIPSD_RUN_OK); fipsd_runtime_destroy(rt);
    fipsd_control_service_clear(&service); go_swap_free(state); go_swap_free(configs);
}

/* FIPSD_CONTROL_MAX_CONNECTIONS held requests; one more is closed unread. */
static GoChannel *held, *release;
static json_t *hold_provider(void *user, const char *name, const json_t *params,
                             const fipsd_control_request_context *context)
{
    (void)user; (void)name; (void)params; (void)context;
    CHECK(go_channel_send(held, held) == 0);
    void *token; CHECK(go_channel_receive(release, &token) == 0);
    return fipsd_control_error("held");
}
static void test_capacity(void)
{
    GoSwap *configs = config_for("0");
    fipsd_control_provider provider = {.command = hold_provider};
    fipsd_control_service service; fipsd_control_service_init(&service, configs, &provider);
    held = go_channel_create(FIPSD_CONTROL_MAX_CONNECTIONS); release = go_channel_create(FIPSD_CONTROL_MAX_CONNECTIONS);
    CHECK(held && release);
    fipsd_runtime *rt = start_runtime(&service);
    const char *req = "{\"command\":\"path_show\",\"params\":{\"npub\":\"hold\"}}\n";
    SOCKET clients[FIPSD_CONTROL_MAX_CONNECTIONS];
    void *token;
    for (size_t i = 0; i < FIPSD_CONTROL_MAX_CONNECTIONS; ++i) {
        clients[i] = dial(service.port); send_all(clients[i], req, strlen(req));
        CHECK(go_channel_receive(held, &token) == 0); /* the provider holds it */
    }
    CHECK(atomic_load(&service.connections) == FIPSD_CONTROL_MAX_CONNECTIONS);
    SOCKET excess = dial(service.port); /* accepted by the kernel, closed by the daemon */
    char out[256]; int n = recv(excess, out, (int)sizeof out, 0); CHECK(n <= 0); closesocket(excess);
    CHECK(atomic_load(&service.connections) == FIPSD_CONTROL_MAX_CONNECTIONS);
    for (size_t i = 0; i < FIPSD_CONTROL_MAX_CONNECTIONS; ++i) CHECK(go_channel_send(release, release) == 0);
    for (size_t i = 0; i < FIPSD_CONTROL_MAX_CONNECTIONS; ++i) {
        read_reply(clients[i], out, sizeof out);
        CHECK(!strcmp(out, "{\"status\":\"error\",\"message\":\"held\"}\n")); closesocket(clients[i]);
    }
    fipsd_runtime_shutdown(rt); CHECK(fipsd_runtime_run(rt) == FIPSD_RUN_OK);
    CHECK(atomic_load(&service.connections) == 0);
    fipsd_runtime_destroy(rt); fipsd_control_service_clear(&service); go_swap_free(configs);
    go_channel_free(held); go_channel_free(release);
}

/* fipsctl keygen's private key on Windows: a protected DACL of SYSTEM,
 * Administrators and the owner (the 0600 counterpart), also when --force
 * rewrites a key whose DACL had been opened up. */
static void expect_private(const char *path)
{
    PACL dacl = NULL; PSECURITY_DESCRIPTOR sd = NULL;
    CHECK(GetNamedSecurityInfoA(path, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &dacl, NULL, &sd) == ERROR_SUCCESS);
    SECURITY_DESCRIPTOR_CONTROL control; DWORD revision;
    CHECK(GetSecurityDescriptorControl(sd, &control, &revision) && (control & SE_DACL_PROTECTED));
    CHECK(dacl && dacl->AceCount == 3);
    LocalFree(sd);
}
static void test_keygen(void)
{
    const char *dir = fipsd_test_tmpdir(NULL);
    char key[1024]; snprintf(key, sizeof key, "%s\\fips.key", dir);
    FILE *null = fopen("NUL", "w"); CHECK(null);
    CHECK(fipsctl_keygen(dir, false, false, null, null) == 0);
    expect_private(key);
    CHECK(fipsctl_keygen(dir, false, false, null, null) == 1); /* exists, no --force */
    /* Open the key to Everyone, then --force: the DACL is private again. */
    PSECURITY_DESCRIPTOR open = NULL;
    CHECK(ConvertStringSecurityDescriptorToSecurityDescriptorA("D:(A;;FA;;;WD)", SDDL_REVISION_1, &open, NULL));
    BOOL present, defaulted; PACL everyone = NULL;
    CHECK(GetSecurityDescriptorDacl(open, &present, &everyone, &defaulted) && present);
    CHECK(SetNamedSecurityInfoA(key, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
                                NULL, NULL, everyone, NULL) == ERROR_SUCCESS);
    LocalFree(open);
    CHECK(fipsctl_keygen(dir, true, false, null, null) == 0);
    expect_private(key);
    char address[FIPSCTL_ADDR_CAPACITY]; char *error = NULL;
    CHECK(fipsctl_address_from_key_dir(dir, address, &error) && !error && address[0] == 'f' && address[1] == 'd');
    fclose(null);
}

int main(int argc, char **argv)
{
    WSADATA wsa; CHECK(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    if (argc == 2 && !strcmp(argv[1], "capacity")) { test_capacity(); puts("control tcp: connection cap"); return 0; }
    CHECK(argc == 1);
    test_ports();
    test_keygen();
    test_socket();
    test_tun_status();
    puts("control tcp: 127.0.0.1 listener, protocol bounds, EOF, deadlines, concurrency, exclusive port, "
         "stop, fipsctl client, show_status composition and the private key DACL passed");
    return 0;
}
