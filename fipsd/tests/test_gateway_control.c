/* Gateway control protocol vectors (nostrc-iq1n9.3), from upstream
 * src/gateway/control.rs tests, plus the line protocol over a real socket:
 * request parsing errors, the snapshot's absence, the encoded shapes and the
 * bind policy (stale socket removed, live one and other files left). */
#include "test_gateway.h"
#include "gateway/control.h"
#include <errno.h>
#include <jansson.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <limits.h>

static fipsd_gw_snapshot *make_snapshot(void)
{
    fipsd_gw_pool_status st = {.total = 65535, .allocated = 1, .active = 0, .draining = 0, .free = 65534, .evicted = 4, .releasing = 2, .refused = 7};
    fipsd_gw_mapping_info *m = calloc(1, sizeof *m); CHECK(m);
    m->virtual_ip = gw_v6("fd01::1"); m->mesh_addr = gw_v6("fd97:467a::1");
    snprintf(m->dns_name, sizeof m->dns_name, "npub1test.fips");
    m->state = FIPSD_GW_MAPPING_ACTIVE; m->session_count = 3; m->age_secs = 120; m->last_ref_secs = 5; m->used = true;
    fipsd_gw_snapshot_config cfg = {"fd01::/112", "br-lan", "127.0.0.1:5354", "[fd02::10]:53", 60, 60};
    fipsd_gw_snapshot *s = fipsd_gw_snapshot_build(&st, m, 1, 1, 3600, &cfg); CHECK(s);
    return s;
}
static json_int_t get_int(json_t *o, const char *k) { json_t *v = json_object_get(o, k); CHECKM(json_is_integer(v), "%s", k); return json_integer_value(v); }
static const char *get_str(json_t *o, const char *k) { json_t *v = json_object_get(o, k); CHECKM(json_is_string(v), "%s", k); return json_string_value(v); }

static void test_show_gateway_response(void)
{
    fipsd_gw_snapshot *s = make_snapshot();
    json_t *r = fipsd_gw_control_show_gateway(s), *d = json_object_get(r, "data");
    CHECK(!strcmp(get_str(r, "status"), "ok"));
    CHECK(get_int(d, "pool_total") == 65535 && get_int(d, "pool_free") == 65534 && get_int(d, "pool_evicted") == 4);
    CHECK(get_int(d, "pool_releasing") == 2 && get_int(d, "pool_refused") == 7 && get_int(d, "nat_mappings") == 1);
    CHECK(!strcmp(get_str(d, "dns_listen"), "[fd02::10]:53") && get_int(d, "uptime_secs") == 3600);
    CHECK(!strcmp(get_str(d, "pool_cidr"), "fd01::/112") && !strcmp(get_str(d, "lan_interface"), "br-lan"));
    CHECK(!strcmp(get_str(d, "dns_upstream"), "127.0.0.1:5354") && get_int(d, "dns_ttl") == 60 && get_int(d, "pool_grace_period") == 60);
    CHECKM(json_object_size(d) == 16, "upstream's sixteen fields (%zu)", json_object_size(d));
    json_decref(r); fipsd_gw_snapshot_release(s);
}
static void test_show_mappings_response(void)
{
    fipsd_gw_snapshot *s = make_snapshot();
    json_t *r = fipsd_gw_control_show_mappings(s);
    CHECK(!strcmp(get_str(r, "status"), "ok"));
    json_t *list = json_object_get(json_object_get(r, "data"), "mappings");
    CHECK(json_array_size(list) == 1);
    json_t *m = json_array_get(list, 0);
    CHECK(!strcmp(get_str(m, "state"), "Active") && get_int(m, "sessions") == 3 && !strcmp(get_str(m, "virtual_ip"), "fd01::1"));
    CHECK(json_is_true(json_object_get(m, "used")) && !strcmp(get_str(m, "mesh_addr"), "fd97:467a::1"));
    CHECKM(!strcmp(get_str(m, "node_addr"), "00000000000000000000000000000000"), "the node address as lowercase hex");
    CHECK(!strcmp(get_str(m, "dns_name"), "npub1test.fips") && get_int(m, "age_secs") == 120 && get_int(m, "last_ref_secs") == 5);
    CHECK(json_object_size(m) == 9);
    json_decref(r); fipsd_gw_snapshot_release(s);
}
static void test_unknown_command(void)
{
    fipsd_gw_snapshot *s = make_snapshot();
    json_t *r = fipsd_gw_control_dispatch("bogus", s);
    CHECK(!strcmp(get_str(r, "status"), "error") && strstr(get_str(r, "message"), "unknown command: bogus"));
    json_decref(r); fipsd_gw_snapshot_release(s);
}
static void test_not_initialized(void)
{
    json_t *r = fipsd_gw_control_dispatch("show_gateway", NULL);
    CHECK(!strcmp(get_str(r, "status"), "error") && strstr(get_str(r, "message"), "not yet initialized"));
    json_decref(r);
}
static void test_mapping_state_str(void)
{
    CHECK(!strcmp(fipsd_gw_mapping_state_str(FIPSD_GW_MAPPING_ALLOCATED), "Allocated"));
    CHECK(!strcmp(fipsd_gw_mapping_state_str(FIPSD_GW_MAPPING_ACTIVE), "Active"));
    CHECK(!strcmp(fipsd_gw_mapping_state_str(FIPSD_GW_MAPPING_DRAINING), "Draining"));
}
static void test_empty_mappings(void)
{
    fipsd_gw_pool_status st = {.total = 255, .free = 255};
    fipsd_gw_snapshot_config cfg = {"fd01::/112", "br-lan", "127.0.0.1:5354", "[::1]:53", 60, 60};
    fipsd_gw_snapshot *s = fipsd_gw_snapshot_build(&st, NULL, 0, 0, 0, &cfg);
    json_t *r = fipsd_gw_control_show_mappings(s);
    CHECK(!strcmp(get_str(r, "status"), "ok") && json_array_size(json_object_get(json_object_get(r, "data"), "mappings")) == 0);
    json_decref(r); fipsd_gw_snapshot_release(s);
}
/* The encoded lines the protocol answers with, request by request. */
static void the_line_protocol_answers_each_request_shape(void)
{
    fipsd_gw_snapshot_slot slot; fipsd_gw_snapshot_slot_init(&slot);
    char *out = fipsd_gw_control_answer("{\"command\":\"show_gateway\"}\n", 27, &slot);
    CHECK(!strcmp(out, "{\"status\":\"error\",\"message\":\"gateway not yet initialized\"}")); free(out);
    fipsd_gw_snapshot_publish(&slot, make_snapshot());
    static const struct { const char *in, *out; } vectors[] = {
        {"", "{\"status\":\"error\",\"message\":\"empty request\"}"},
        {"   \n", "{\"status\":\"error\",\"message\":\"empty request\"}"},
        {"{\"params\":{}}", "{\"status\":\"error\",\"message\":\"invalid request: missing or non-string 'command'\"}"},
        {"[1,2]", "{\"status\":\"error\",\"message\":\"invalid request: missing or non-string 'command'\"}"},
        {"{\"command\":\"nope\"}", "{\"status\":\"error\",\"message\":\"unknown command: nope\"}"},
        {"{\"command\":\"show_mappings\",\"params\":null}",
         "{\"status\":\"ok\",\"data\":{\"mappings\":[{\"age_secs\":120,\"dns_name\":\"npub1test.fips\",\"last_ref_secs\":5,"
         "\"mesh_addr\":\"fd97:467a::1\",\"node_addr\":\"00000000000000000000000000000000\",\"sessions\":3,\"state\":\"Active\","
         "\"used\":true,\"virtual_ip\":\"fd01::1\"}]}}"},
        {"{\"command\":\"show_gateway\"}",
         "{\"status\":\"ok\",\"data\":{\"dns_listen\":\"[fd02::10]:53\",\"dns_ttl\":60,\"dns_upstream\":\"127.0.0.1:5354\","
         "\"lan_interface\":\"br-lan\",\"nat_mappings\":1,\"pool_active\":0,\"pool_allocated\":1,\"pool_cidr\":\"fd01::/112\","
         "\"pool_draining\":0,\"pool_evicted\":4,\"pool_free\":65534,\"pool_grace_period\":60,\"pool_refused\":7,"
         "\"pool_releasing\":2,\"pool_total\":65535,\"uptime_secs\":3600}}"},
    };
    for (size_t i = 0; i < sizeof vectors / sizeof vectors[0]; ++i) {
        out = fipsd_gw_control_answer(vectors[i].in, strlen(vectors[i].in), &slot);
        CHECKM(out && !strcmp(out, vectors[i].out), "request %zu: got %s", i, out ? out : "(null)");
        free(out);
    }
    out = fipsd_gw_control_answer("{\"command\":", 11, &slot);
    CHECKM(strstr(out, "\"message\":\"invalid request: "), "%s", out); free(out);
    fipsd_gw_snapshot_slot_clear(&slot);
}

/* ---- the socket ------------------------------------------------------------ */
typedef struct server { fipsd_gw_control c; pthread_t t; } server;
static void *serve_thread(void *arg) { server *s = arg; fipsd_gw_control_accept_loop(&s->c, NULL, NULL); return NULL; }
static int client(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0); CHECK(fd >= 0);
    struct sockaddr_un a; memset(&a, 0, sizeof a); a.sun_family = AF_UNIX; snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
    CHECK(connect(fd, (struct sockaddr *)&a, sizeof a) == 0);
    return fd;
}
static char *exchange(const char *path, const char *request)
{
    int fd = client(path);
    CHECK(write(fd, request, strlen(request)) == (ssize_t)strlen(request));
    static char buf[8192]; size_t at = 0;
    for (;;) { ssize_t n = read(fd, buf + at, sizeof buf - 1 - at); if (n <= 0) break; at += (size_t)n; }
    buf[at] = 0; close(fd);
    return buf;
}
static void the_socket_serves_one_line_per_connection_and_keeps_the_policy(void)
{
    /* The real path: the policy refuses to create through a symlink, and
     * macOS's /tmp is one. */
    char tmpl[] = "/tmp/fipsgw-ctl-XXXXXX"; CHECK(mkdtemp(tmpl));
    char dir[PATH_MAX]; CHECK(realpath(tmpl, dir));
    char path[128]; snprintf(path, sizeof path, "%s/sub/gateway.sock", dir);
    fipsd_gw_snapshot_slot slot; fipsd_gw_snapshot_slot_init(&slot);
    fipsd_gw_snapshot_publish(&slot, make_snapshot());
    server s; char err[256];
    CHECKM(fipsd_gw_control_bind(&s.c, path, &slot, err, sizeof err) == 0, "%s", err);
    struct stat st;
    CHECK(stat(path, &st) == 0 && S_ISSOCK(st.st_mode) && (st.st_mode & 0777) == 0770);
    char sub[128]; snprintf(sub, sizeof sub, "%s/sub", dir);
    CHECKM(stat(sub, &st) == 0 && (st.st_mode & 0777) == 0750, "a parent the service created is 0750");
    /* A second bind on a live socket is refused and leaves it alone. */
    fipsd_gw_control other;
    CHECK(fipsd_gw_control_bind(&other, path, &slot, err, sizeof err) != 0 && errno == EADDRINUSE);
    CHECK(pthread_create(&s.t, NULL, serve_thread, &s) == 0);
    char *line = exchange(path, "{\"command\":\"show_gateway\"}\n");
    const char *head = "{\"status\":\"ok\",\"data\":{\"dns_listen\":\"[fd02::10]:53\"";
    CHECKM(!strncmp(line, head, strlen(head)) && line[strlen(line) - 1] == '\n', "%s", line);
    line = exchange(path, "{\"command\":\"bogus\"}\n");
    CHECK(!strcmp(line, "{\"status\":\"error\",\"message\":\"unknown command: bogus\"}\n"));
    /* Too large: more than 4096 bytes without a newline. */
    char *big = malloc(5000); memset(big, 'x', 4999); big[4999] = 0;
    line = exchange(path, big); free(big);
    CHECK(!strcmp(line, "{\"status\":\"error\",\"message\":\"read error: request too large\"}\n"));
    /* A peer that closes without a newline still gets an answer to what it sent. */
    int fd = client(path);
    CHECK(write(fd, "{\"command\":\"show_mappings\"}", 27) == 27);
    CHECK(shutdown(fd, SHUT_WR) == 0);
    char buf[2048]; ssize_t n = read(fd, buf, sizeof buf - 1); CHECK(n > 0); buf[n] = 0; close(fd);
    CHECK(strstr(buf, "\"mappings\":[{"));
    fipsd_gw_control_stop(&s.c);
    /* Linux wakes an accept on a shut-down listener (and refuses new
     * connections); elsewhere a connection wakes it to see the stop. */
    fd = socket(AF_UNIX, SOCK_STREAM, 0); CHECK(fd >= 0);
    {
        struct sockaddr_un w; memset(&w, 0, sizeof w); w.sun_family = AF_UNIX; snprintf(w.sun_path, sizeof w.sun_path, "%s", path);
        (void)connect(fd, (struct sockaddr *)&w, sizeof w);
    }
    close(fd);
    CHECK(pthread_join(s.t, NULL) == 0);
    fipsd_gw_control_clear(&s.c);
    CHECKM(stat(path, &st) != 0, "the service removed the socket it bound");
    /* A stale socket is replaced; a regular file is not. */
    int stale = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a; memset(&a, 0, sizeof a); a.sun_family = AF_UNIX; snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
    CHECK(bind(stale, (struct sockaddr *)&a, sizeof a) == 0); close(stale);
    CHECKM(fipsd_gw_control_bind(&s.c, path, &slot, err, sizeof err) == 0, "a stale socket is replaced: %s", err);
    fipsd_gw_control_clear(&s.c);
    FILE *f = fopen(path, "w"); CHECK(f); fclose(f);
    CHECKM(fipsd_gw_control_bind(&s.c, path, &slot, err, sizeof err) != 0, "a regular file is never removed");
    CHECK(stat(path, &st) == 0 && S_ISREG(st.st_mode));
    unlink(path); rmdir(sub); rmdir(dir);
    fipsd_gw_snapshot_slot_clear(&slot);
}

int gw_suite_control(void)
{
    RUN(test_show_gateway_response);
    RUN(test_show_mappings_response);
    RUN(test_unknown_command);
    RUN(test_not_initialized);
    RUN(test_mapping_state_str);
    RUN(test_empty_mappings);
    RUN(the_line_protocol_answers_each_request_shape);
    RUN(the_socket_serves_one_line_per_connection_and_keeps_the_policy);
    return 0;
}
