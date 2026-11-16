/* fipsctl unit tests: argument parsing, request building, the serde-exact
 * pretty printer against every control golden, show_status composition over
 * fake sockets (upstream src/bin/tests/fipsctl.rs), the path request fixture
 * (fipsctl_path.rs), npub/address/key-file helpers (fipsctl_npub.rs and the
 * inline tests), the probe ceiling (fipsctl_probe.rs) and the probe
 * rendering, including the full human report for probe_report.json. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "fipsctl.h"
#include "test_tmpdir.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sodium.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <fips/identity/address.h>
#include <fips/identity/encoding.h>
#include <fips/identity/local.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s (errno %d)\n", __FILE__, __LINE__, #x, errno); abort(); } } while (0)
#define STREQ(a, b) streq_at(__LINE__, (a), (b))
static void streq_at(int line, const char *a, const char *b)
{
    if (a && b && !strcmp(a, b)) return;
    fprintf(stderr, "%s:%d: strings differ\n--- actual ---\n%s\n--- expected ---\n%s\n", __FILE__, line, a ? a : "(null)", b ? b : "(null)");
    abort();
}

#define ARGV(...) ((char *[]){"fipsctl", __VA_ARGS__, NULL})
#define ARGC(...) ((int)(sizeof((char *[]){"fipsctl", __VA_ARGS__, NULL}) / sizeof(char *)) - 1)
#define PARSE(a, ...) parse_into(&(a), ARGC(__VA_ARGS__), ARGV(__VA_ARGS__), NULL)
#define PARSE_ERR(buf, ...) parse_into(&(fipsctl_args){0}, ARGC(__VA_ARGS__), ARGV(__VA_ARGS__), buf)

static int parse_into(fipsctl_args *a, int argc, char **argv, char *errbuf)
{
    char *data = NULL; size_t size = 0;
    FILE *err = open_memstream(&data, &size);
    CHECK(err);
    int rc = fipsctl_parse(argc, argv, a, err);
    fclose(err);
    if (errbuf) { strncpy(errbuf, data, 1023); errbuf[1023] = 0; }
    free(data);
    return rc;
}

static void test_parse(void)
{
    fipsctl_args a;
    const char *shows[][2] = {{"status", "show_status"}, {"peers", "show_peers"}, {"links", "show_links"},
        {"tree", "show_tree"}, {"sessions", "show_sessions"}, {"bloom", "show_bloom"}, {"mmp", "show_mmp"},
        {"cache", "show_cache"}, {"connections", "show_connections"}, {"transports", "show_transports"},
        {"routing", "show_routing"}, {"identity-cache", "show_identity_cache"}, {"native-flows", "show_native_flows"}};
    for (size_t i = 0; i < 13; ++i) {
        char *argv[] = {"fipsctl", "show", (char *)shows[i][0], NULL};
        CHECK(parse_into(&a, 3, argv, NULL) == 0);
        CHECK(a.command == FIPSCTL_CMD_QUERY); STREQ(a.query, shows[i][1]); CHECK(!a.socket);
    }
    CHECK(PARSE(a, "acl", "show") == 0); CHECK(a.command == FIPSCTL_CMD_QUERY); STREQ(a.query, "show_acl");
    CHECK(PARSE(a, "stats", "list") == 0); STREQ(a.query, "show_stats_list");
    CHECK(PARSE(a, "stats", "metrics") == 0); STREQ(a.query, "show_metrics");
    CHECK(PARSE(a, "stats", "peers") == 0); STREQ(a.query, "show_stats_peers");
    CHECK(PARSE(a, "-s", "/x.sock", "show", "status") == 0); STREQ(a.socket, "/x.sock");
    CHECK(PARSE(a, "--socket=/y.sock", "show", "status") == 0); STREQ(a.socket, "/y.sock");
    CHECK(PARSE(a, "-s/z.sock", "show", "status") == 0); STREQ(a.socket, "/z.sock");
    CHECK(PARSE(a, "--socket", "/w.sock", "show", "status") == 0); STREQ(a.socket, "/w.sock");
    CHECK(PARSE(a, "probe", "hydra", "--json") == 0);
    CHECK(a.command == FIPSCTL_CMD_PROBE); STREQ(a.peer, "hydra"); CHECK(a.json && !a.has_timeout);
    CHECK(PARSE(a, "probe", "--timeout", "2", "hydra") == 0); CHECK(a.has_timeout && a.timeout_secs == 2 && !a.json);
    CHECK(PARSE(a, "stats", "history", "srtt_ms", "--peer", "hydra", "--window", "1h", "--granularity", "1m", "--plot") == 0);
    CHECK(a.command == FIPSCTL_CMD_STATS_HISTORY); STREQ(a.metric, "srtt_ms"); STREQ(a.peer, "hydra");
    STREQ(a.window, "1h"); STREQ(a.granularity, "1m"); CHECK(a.plot);
    CHECK(PARSE(a, "stats", "history", "mesh_size") == 0);
    STREQ(a.window, "10m"); STREQ(a.granularity, "1s"); CHECK(!a.plot && !a.peer);
    CHECK(PARSE(a, "keygen", "-d", "/k", "-f", "-s") == 0);
    CHECK(a.command == FIPSCTL_CMD_KEYGEN); STREQ(a.dir, "/k"); CHECK(a.force && a.to_stdout && !a.socket);
    CHECK(PARSE(a, "keygen", "--dir=/d", "--force", "--stdout") == 0); STREQ(a.dir, "/d"); CHECK(a.force && a.to_stdout);
    CHECK(PARSE(a, "keygen") == 0); CHECK(!a.dir && !a.force && !a.to_stdout);
    CHECK(PARSE(a, "address") == 0); CHECK(a.command == FIPSCTL_CMD_ADDRESS && !a.identity && !a.key);
    CHECK(PARSE(a, "address", "hydra") == 0); STREQ(a.identity, "hydra");
    CHECK(PARSE(a, "address", "-k", "/p") == 0); STREQ(a.key, "/p"); CHECK(!a.identity);
    CHECK(PARSE(a, "address", "--key", "/p") == 0); STREQ(a.key, "/p");
    CHECK(PARSE(a, "connect", "hydra", "1.2.3.4:2121", "udp") == 0);
    CHECK(a.command == FIPSCTL_CMD_CONNECT); STREQ(a.peer, "hydra"); STREQ(a.address, "1.2.3.4:2121"); STREQ(a.transport, "udp");
    CHECK(PARSE(a, "disconnect", "hydra") == 0); CHECK(a.command == FIPSCTL_CMD_DISCONNECT); STREQ(a.peer, "hydra");
    CHECK(PARSE(a, "path", "show", "hydra") == 0); CHECK(a.command == FIPSCTL_CMD_PATH_SHOW); STREQ(a.peer, "hydra");
    CHECK(PARSE(a, "path", "pin", "hydra", "cable") == 0);
    CHECK(a.command == FIPSCTL_CMD_PATH_PIN); STREQ(a.transport, "cable"); CHECK(!a.address);
    CHECK(PARSE(a, "path", "pin", "hydra", "2", "10.0.1.2:2121") == 0); STREQ(a.address, "10.0.1.2:2121");
    CHECK(PARSE(a, "path", "unpin", "hydra") == 0); CHECK(a.command == FIPSCTL_CMD_PATH_UNPIN);
    CHECK(PARSE(a, "-V") == 0); CHECK(a.command == FIPSCTL_CMD_VERSION_SHORT);
    CHECK(PARSE(a, "--version") == 0); CHECK(a.command == FIPSCTL_CMD_VERSION_LONG);
    CHECK(PARSE(a, "-V", "--version") == 0); CHECK(a.command == FIPSCTL_CMD_VERSION_SHORT);
    CHECK(PARSE(a, "-h") == 0); CHECK(a.command == FIPSCTL_CMD_HELP && !a.help_scope);
    CHECK(PARSE(a, "-V", "--help") == 0); CHECK(a.command == FIPSCTL_CMD_HELP);
    CHECK(PARSE(a, "show", "--help") == 0); CHECK(a.command == FIPSCTL_CMD_HELP); STREQ(a.help_scope, "show");
    CHECK(PARSE(a, "stats", "history", "-h") == 0); CHECK(a.command == FIPSCTL_CMD_HELP); STREQ(a.help_scope, "stats history");
    CHECK(PARSE(a, "help", "path", "pin") == 0); CHECK(a.command == FIPSCTL_CMD_HELP); STREQ(a.help_scope, "path pin");
    CHECK(PARSE(a, "help") == 0); CHECK(a.command == FIPSCTL_CMD_HELP); STREQ(a.help_scope, "");
    char err[1024];
    char *argv0[] = {"fipsctl", NULL};
    CHECK(parse_into(&a, 1, argv0, NULL) == 1);
    CHECK(PARSE_ERR(err, "bogus") == 1); CHECK(strstr(err, "unrecognized subcommand 'bogus'") && strstr(err, "Usage: fipsctl [OPTIONS] <COMMAND>"));
    CHECK(PARSE_ERR(err, "show", "bogus") == 1); CHECK(strstr(err, "unrecognized subcommand 'bogus'") && strstr(err, "Usage: fipsctl show <COMMAND>"));
    CHECK(PARSE_ERR(err, "show") == 1); CHECK(strstr(err, "requires a subcommand"));
    CHECK(PARSE_ERR(err, "show", "status", "extra") == 1); CHECK(strstr(err, "unexpected argument 'extra' found"));
    CHECK(PARSE_ERR(err, "--bogus", "show", "status") == 1); CHECK(strstr(err, "unexpected argument '--bogus' found"));
    CHECK(PARSE_ERR(err, "-s") == 1); CHECK(strstr(err, "a value is required for '--socket <SOCKET>'"));
    CHECK(PARSE_ERR(err, "probe") == 1); CHECK(strstr(err, "required arguments were not provided") && strstr(err, "<TARGET>"));
    CHECK(PARSE_ERR(err, "connect", "a", "b") == 1); CHECK(strstr(err, "<TRANSPORT>"));
    CHECK(PARSE_ERR(err, "path", "pin", "hydra") == 1); CHECK(strstr(err, "<TRANSPORT>"));
    CHECK(PARSE_ERR(err, "address", "hydra", "-k", "/p") == 1); CHECK(strstr(err, "cannot be used with '--key <KEY>'"));
    CHECK(PARSE_ERR(err, "probe", "x", "--timeout", "abc") == 1); CHECK(strstr(err, "invalid value 'abc' for '--timeout <TIMEOUT>'"));
    CHECK(PARSE_ERR(err, "probe", "x", "--timeout") == 1); CHECK(strstr(err, "a value is required for '--timeout <TIMEOUT>'"));
    CHECK(PARSE_ERR(err, "keygen", "stray") == 1); CHECK(strstr(err, "unexpected argument 'stray' found"));
#ifndef FIPSCTL_PROFILING
    CHECK(PARSE_ERR(err, "profile", "tick", "on") == 1);
#endif
    /* Help and version render to the given stream. */
    char *data = NULL; size_t size = 0;
    FILE *out = open_memstream(&data, &size);
    fipsctl_print_help(out, NULL); fclose(out);
    CHECK(strstr(data, "Usage: fipsctl [OPTIONS] <COMMAND>") && strstr(data, "  probe       Diagnose") && strstr(data, "-s, --socket <SOCKET>"));
    free(data); data = NULL;
    out = open_memstream(&data, &size);
    fipsctl_print_version(out, true); fclose(out);
    CHECK(!strncmp(data, "fipsctl ", 8) && strstr(data, "\ntarget: "));
    CHECK(!strncmp(data + 8, fipsctl_short_version(), strlen(fipsctl_short_version())));
    free(data);
    puts("parse: ok");
}

static void test_requests(void)
{
    char *q = fipsctl_build_query("show_status");
    STREQ(q, "{\"command\":\"show_status\"}\n"); free(q);
    char *c = fipsctl_build_command("connect", json_pack("{s:s,s:s,s:s}", "npub", "npub1abc", "transport", "udp", "address", "1.2.3.4:2121"));
    STREQ(c, "{\"command\":\"connect\",\"params\":{\"address\":\"1.2.3.4:2121\",\"npub\":\"npub1abc\",\"transport\":\"udp\"}}\n"); free(c);
    /* The path fixture shared with the daemon's round-trip test. */
    char path[4096]; snprintf(path, sizeof(path), "%s/fipsctl_path_requests.json", FIPSD_CONTROL_GOLDEN);
    json_error_t e; json_t *cases = json_load_file(path, 0, &e);
    CHECK(cases && json_array_size(cases) == 3);
    const char *NPUB = "npub1pathtestpeer", *ADDR = "10.0.1.2:2121";
    size_t i; json_t *entry;
    json_array_foreach(cases, i, entry) {
        json_t *args = json_object_get(entry, "args");
        char *argv[8] = {"fipsctl"}; int argc = 1;
        size_t j; json_t *arg;
        json_array_foreach(args, j, arg) {
            const char *s = json_string_value(arg);
            argv[argc++] = (char *)(!strcmp(s, "<npub>") ? NPUB : !strcmp(s, "<addr>") ? ADDR : s);
        }
        argv[argc] = NULL;
        fipsctl_args a;
        CHECK(fipsctl_parse(argc, argv, &a, stderr) == 0);
        char *line = fipsctl_path_request(&a, "/nonexistent/hosts", stderr);
        CHECK(line && line[strlen(line) - 1] == '\n');
        json_t *sent = json_loads(line, 0, &e); CHECK(sent);
        json_t *expected = json_deep_copy(json_object_get(entry, "request"));
        json_t *params = json_object_get(expected, "params");
        json_object_set_new(params, "npub", json_string(NPUB));
        if (json_object_get(params, "address")) json_object_set_new(params, "address", json_string(ADDR));
        CHECK(json_equal(sent, expected));
        json_decref(sent); json_decref(expected); free(line);
    }
    json_decref(cases);
    fipsctl_args bare;
    CHECK(PARSE(bare, "path", "pin", "npub1pathtestpeer", "cable") == 0);
    char *line = fipsctl_path_request(&bare, "/nonexistent/hosts", stderr);
    json_t *sent = json_loads(line, 0, &e); CHECK(sent);
    STREQ(json_string_value(json_object_get(sent, "command")), "path_pin");
    STREQ(json_string_value(json_object_get(json_object_get(sent, "params"), "transport")), "cable");
    CHECK(!json_object_get(json_object_get(sent, "params"), "address"));
    json_decref(sent); free(line);
    /* The daemon-bound request for stats history carries the resolved peer. */
    fipsctl_args h;
    CHECK(PARSE(h, "stats", "history", "srtt_ms", "--peer", "NPUB1HYDRA") == 0);
    line = fipsctl_request_for(&h, stderr);
    STREQ(line, "{\"command\":\"show_stats_history\",\"params\":{\"granularity\":\"1s\",\"metric\":\"srtt_ms\",\"peer\":\"NPUB1HYDRA\",\"window\":\"10m\"}}\n");
    free(line);
    fipsctl_args bad;
    char *data = NULL; size_t size = 0; FILE *err = open_memstream(&data, &size);
    CHECK(PARSE(bad, "connect", "npub1x", "[fd00::1]:2121", "udp") == 0);
    CHECK(!fipsctl_request_for(&bad, err)); fclose(err);
    CHECK(strstr(data, "error: '[fd00::1]:2121' is a FIPS mesh address (fd00::/8), not a reachable udp endpoint."));
    free(data);
    puts("requests: ok");
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb"); CHECK(f);
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    char *s = malloc((size_t)n + 1); CHECK(s);
    CHECK(fread(s, 1, (size_t)n, f) == (size_t)n); s[n] = 0; fclose(f);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = 0;
    return s;
}

static void test_pretty(void)
{
    static const char *const goldens[] = {"probe_report", "show_acl", "show_bloom", "show_cache", "show_connections",
        "show_identity_cache", "show_links", "show_mmp", "show_native_flows", "show_peers", "show_routing",
        "show_sessions", "show_stats_all_history", "show_stats_history_all_peers", "show_stats_history",
        "show_stats_list", "show_stats_peers", "show_status_composed", "show_status_fallback", "show_status",
        "show_transports_with_interface", "show_transports", "show_tree"};
    unsigned count = 0;
    for (size_t i = 0; i < sizeof(goldens) / sizeof(goldens[0]); ++i) {
        char path[4096]; snprintf(path, sizeof(path), "%s/%s.json", FIPSD_CONTROL_GOLDEN, goldens[i]);
        char *text = read_file(path);
        json_error_t e; json_t *v = fipsctl_json_load(text, strlen(text), &e); CHECK(v);
        char *pretty = fipsctl_json_pretty(v);
        if (strcmp(pretty, text)) fprintf(stderr, "golden %s\n", goldens[i]);
        STREQ(pretty, text);
        free(pretty); free(text); json_decref(v); ++count;
    }
    CHECK(count == 23);
    /* Expectations checked against serde_json::to_string on these values. */
    const struct { double v; const char *s; } reals[] = {{18.0, "18.0"}, {0.1, "0.1"}, {1e16, "1e+16"},
        {1e15, "1000000000000000.0"}, {0.0001, "0.0001"}, {0.00001, "0.00001"}, {0.000001, "1e-6"}, {1.5e-7, "1.5e-7"},
        {123456.789, "123456.789"}, {-2.5, "-2.5"}, {1e21, "1e+21"}, {1.005659, "1.005659"}, {0.0, "0.0"},
        {1.5e300, "1.5e+300"}, {12340000000.0, "12340000000.0"}, {0.001234, "0.001234"}, {1e17, "1e+17"},
        {9007199254740993.0, "9007199254740992.0"}};
    for (size_t i = 0; i < sizeof(reals) / sizeof(reals[0]); ++i) {
        char *s = fipsctl_format_f64(reals[i].v); STREQ(s, reals[i].s); free(s);
    }
    const char *big = "{\"a\":18446744073709551615,\"b\":9223372036854775807,\"c\":9223372036854775808,"
                      "\"s\":\"18446744073709551615\",\"n\":[0,-1,1.5e3],\"t\":\"x\\u0001u64:1\"}";
    json_error_t e; json_t *v = fipsctl_json_load(big, strlen(big), &e); CHECK(v);
    char *compact = fipsctl_json_compact(v);
    STREQ(compact, "{\"a\":18446744073709551615,\"b\":9223372036854775807,\"c\":9223372036854775808,"
                   "\"n\":[0,-1,1500.0],\"s\":\"18446744073709551615\",\"t\":\"x\\u0001u64:1\"}");
    free(compact); json_decref(v);
    json_t *s = json_pack("{s:s,s:s}", "e", "\x1f\n\"\\\xc3\xa9/", "d", "\x7f");
    char *esc = fipsctl_json_pretty(s);
    STREQ(esc, "{\n  \"d\": \"\x7f\",\n  \"e\": \"\\u001f\\n\\\"\\\\\xc3\xa9/\"\n}");
    free(esc); json_decref(s);
    json_t *empty = json_pack("{s:[],s:{},s:n}", "a", "o", "z");
    char *p = fipsctl_json_pretty(empty); STREQ(p, "{\n  \"a\": [],\n  \"o\": {},\n  \"z\": null\n}"); free(p); json_decref(empty);
    /* print_response: data when present, the whole value otherwise, errors to stderr with exit 1. */
    char *data = NULL, *errdata = NULL; size_t size = 0, esize = 0;
    FILE *out = open_memstream(&data, &size), *err = open_memstream(&errdata, &esize);
    json_t *ok = json_pack("{s:s,s:{s:i}}", "status", "ok", "data", "x", 1);
    CHECK(fipsctl_print_response(out, err, ok) == 0);
    json_t *bad = json_pack("{s:s,s:s}", "status", "error", "message", "unknown command: reload");
    CHECK(fipsctl_print_response(out, err, bad) == 1);
    json_t *odd = json_pack("{s:s}", "weird", "yes");
    CHECK(fipsctl_print_response(out, err, odd) == 0);
    fclose(out); fclose(err);
    STREQ(data, "{\n  \"x\": 1\n}\n{\n  \"weird\": \"yes\"\n}\n");
    STREQ(errdata, "error: unknown command: reload\n");
    free(data); free(errdata); json_decref(ok); json_decref(bad); json_decref(odd);
    puts("pretty: 23 goldens, reals, u64 passthrough, escapes: ok");
}

static void test_plot(void)
{
    char *data = NULL, *errdata = NULL; size_t size = 0, esize = 0;
    FILE *out = open_memstream(&data, &size), *err = open_memstream(&errdata, &esize);
    json_t *v = json_pack("{s:s,s:{s:[f,n,f,f],s:s,s:i}}", "status", "ok", "data", "values", 1.0, 3.0, 2.0,
                          "unit", "nodes", "granularity_seconds", 60);
    CHECK(fipsctl_print_plot(out, err, v, "mesh_size") == 0);
    json_t *empty = json_pack("{s:s,s:{s:[]}}", "status", "ok", "data", "values");
    CHECK(fipsctl_print_plot(out, err, empty, "peer_count") == 0);
    json_t *bad = json_pack("{s:s,s:s}", "status", "error", "message", "unknown metric: x");
    CHECK(fipsctl_print_plot(out, err, bad, "x") == 1);
    fclose(out); fclose(err);
    STREQ(data, "mesh_size (nodes) \xe2\x80\x94 4 samples @ 60s = 240s window (1 gaps)\n  min=1.000 max=3.000 last=2.000\n"
                "  \xe2\x96\x81 \xe2\x96\x88\xe2\x96\x85\npeer_count: no data yet\n"); /* 2 of 1..3 is 3.5/7, rounded half away from zero as f64::round does */
    STREQ(errdata, "error: unknown metric: x\n");
    free(data); free(errdata); json_decref(v); json_decref(empty); json_decref(bad);
    double flat[3] = {2, 2, 2};
    char *s = fipsctl_sparkline(flat, 3, 2, 2); STREQ(s, "\xe2\x96\x84\xe2\x96\x84\xe2\x96\x84"); free(s);
    s = fipsctl_sparkline(NULL, 0, 0, 0); STREQ(s, ""); free(s);
    puts("plot: ok");
}

/* ---- composition over fake sockets ------------------------------------------- */

typedef struct fake { int listen_fd, stop[2]; char *reply; pthread_t thread; _Atomic unsigned connections; } fake;

static void *fake_serve(void *arg)
{
    fake *f = arg;
    for (;;) {
        struct pollfd fds[2] = {{f->listen_fd, POLLIN, 0}, {f->stop[0], POLLIN, 0}};
        if (poll(fds, 2, -1) < 0) { if (errno == EINTR) continue; break; }
        if (fds[1].revents) break;
        int c = accept(f->listen_fd, NULL, NULL);
        if (c < 0) continue;
        atomic_fetch_add(&f->connections, 1);
        char buf[4096]; ssize_t n;
        while ((n = read(c, buf, sizeof(buf))) > 0 && !memchr(buf, '\n', (size_t)n)) {}
        (void)write(c, f->reply, strlen(f->reply));
        (void)write(c, "\n", 1);
        close(c);
    }
    return NULL;
}

static void fake_start(fake *f, const char *path, const char *reply)
{
    memset(f, 0, sizeof(*f));
    f->reply = strdup(reply);
    CHECK(pipe(f->stop) == 0);
    f->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0); CHECK(f->listen_fd >= 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    CHECK(strlen(path) < sizeof(addr.sun_path));
    strcpy(addr.sun_path, path);
    CHECK(bind(f->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    CHECK(listen(f->listen_fd, 8) == 0);
    CHECK(pthread_create(&f->thread, NULL, fake_serve, f) == 0);
}

static void fake_stop(fake *f)
{
    CHECK(write(f->stop[1], "x", 1) == 1);
    pthread_join(f->thread, NULL);
    close(f->listen_fd); close(f->stop[0]); close(f->stop[1]); free(f->reply);
}

static json_t *status_of(const char *control)
{
    char *request = fipsctl_build_query("show_status");
    char *error = NULL;
    json_t *reply = fipsctl_query(control, "show_status", request, &error);
    if (!reply) fprintf(stderr, "query %s: %s\n", control, error);
    CHECK(reply); free(request); free(error);
    return reply;
}

static void test_sockets(const char *tmp)
{
    char a_ctl[256], a_tun[256], b_ctl[256], b_tun[256], dir[256];
    snprintf(dir, sizeof(dir), "%s/a", tmp); CHECK(mkdir(dir, 0700) == 0);
    snprintf(dir, sizeof(dir), "%s/b", tmp); CHECK(mkdir(dir, 0700) == 0);
    snprintf(a_ctl, sizeof(a_ctl), "%s/a/control.sock", tmp); snprintf(a_tun, sizeof(a_tun), "%s/a/control-ipv6tun.sock", tmp);
    snprintf(b_ctl, sizeof(b_ctl), "%s/b/node.sock", tmp); snprintf(b_tun, sizeof(b_tun), "%s/b/node-ipv6tun.sock", tmp);
    /* Each node leaves the adapter's keys out, and each adapter names its node. */
    fake na, ta, nb, tb;
    fake_start(&na, a_ctl, "{\"status\":\"ok\",\"data\":{\"npub\":\"npub1a\"}}");
    fake_start(&ta, a_tun, "{\"status\":\"ok\",\"data\":{\"tun_state\":\"active\",\"tun_name\":\"fips-a\",\"effective_ipv6_mtu\":1323}}");
    fake_start(&nb, b_ctl, "{\"status\":\"ok\",\"data\":{\"npub\":\"npub1b\"}}");
    fake_start(&tb, b_tun, "{\"status\":\"ok\",\"data\":{\"tun_state\":\"active\",\"tun_name\":\"fips-b\",\"effective_ipv6_mtu\":1223}}");
    json_t *r = status_of(a_ctl); json_t *d = json_object_get(r, "data");
    STREQ(json_string_value(json_object_get(d, "npub")), "npub1a");
    STREQ(json_string_value(json_object_get(d, "tun_name")), "fips-a");
    CHECK(json_integer_value(json_object_get(d, "effective_ipv6_mtu")) == 1323);
    STREQ(json_string_value(json_object_get(d, "tun_state")), "active");
    json_decref(r);
    r = status_of(b_ctl); d = json_object_get(r, "data");
    STREQ(json_string_value(json_object_get(d, "npub")), "npub1b");
    STREQ(json_string_value(json_object_get(d, "tun_name")), "fips-b");
    CHECK(json_integer_value(json_object_get(d, "effective_ipv6_mtu")) == 1223);
    json_decref(r);
    /* A symlink given with -s reaches the adapter beside the real socket. */
    char link[256]; snprintf(link, sizeof(link), "%s/link.sock", tmp);
    CHECK(symlink(a_ctl, link) == 0);
    r = status_of(link); d = json_object_get(r, "data");
    STREQ(json_string_value(json_object_get(d, "tun_name")), "fips-a"); json_decref(r);
    CHECK(atomic_load(&ta.connections) == 2 && atomic_load(&tb.connections) == 1);
    fake_stop(&na); fake_stop(&ta); fake_stop(&nb); fake_stop(&tb);
    CHECK(unlink(a_ctl) == 0 && unlink(a_tun) == 0 && unlink(b_ctl) == 0 && unlink(b_tun) == 0 && unlink(link) == 0);
    /* An error from the node is returned as it came, without asking the adapter. */
    fake_start(&na, a_ctl, "{\"status\":\"error\",\"message\":\"unknown command: show_status\"}");
    fake_start(&ta, a_tun, "{\"status\":\"ok\",\"data\":{\"tun_state\":\"active\"}}");
    r = status_of(a_ctl);
    STREQ(json_string_value(json_object_get(r, "status")), "error");
    STREQ(json_string_value(json_object_get(r, "message")), "unknown command: show_status");
    CHECK(!json_object_get(r, "data")); json_decref(r);
    CHECK(atomic_load(&ta.connections) == 0);
    fake_stop(&na); fake_stop(&ta); CHECK(unlink(a_ctl) == 0 && unlink(a_tun) == 0);
    /* A node that answers the TUN keys itself is not followed by an adapter query. */
    fake_start(&na, a_ctl, "{\"status\":\"ok\",\"data\":{\"tun_state\":\"disabled\",\"tun_name\":\"-\",\"effective_ipv6_mtu\":1203}}");
    fake_start(&ta, a_tun, "{\"status\":\"ok\",\"data\":{\"tun_state\":\"active\",\"tun_name\":\"fips0\",\"effective_ipv6_mtu\":1323}}");
    r = status_of(a_ctl); d = json_object_get(r, "data");
    STREQ(json_string_value(json_object_get(d, "tun_state")), "disabled");
    CHECK(json_integer_value(json_object_get(d, "effective_ipv6_mtu")) == 1203);
    CHECK(atomic_load(&ta.connections) == 0); json_decref(r);
    fake_stop(&ta); CHECK(unlink(a_tun) == 0);
    /* With no adapter answering, the keys show disabled, - and 0. */
    fake_stop(&na); CHECK(unlink(a_ctl) == 0);
    fake_start(&na, a_ctl, "{\"status\":\"ok\",\"data\":{\"npub\":\"npub1a\",\"x\":18446744073709551615}}");
    r = status_of(a_ctl); d = json_object_get(r, "data");
    STREQ(json_string_value(json_object_get(d, "tun_state")), "disabled");
    STREQ(json_string_value(json_object_get(d, "tun_name")), "-");
    CHECK(json_integer_value(json_object_get(d, "effective_ipv6_mtu")) == 0);
    char *pretty = fipsctl_json_pretty(d);
    STREQ(pretty, "{\n  \"effective_ipv6_mtu\": 0,\n  \"npub\": \"npub1a\",\n  \"tun_name\": \"-\",\n  \"tun_state\": \"disabled\",\n  \"x\": 18446744073709551615\n}");
    free(pretty); json_decref(r);
    /* Other commands never compose, and a non-object reply is left alone. */
    char *request = fipsctl_build_query("show_peers"); char *error = NULL;
    r = fipsctl_query(a_ctl, "show_peers", request, &error); CHECK(r);
    CHECK(!json_object_get(json_object_get(r, "data"), "tun_state")); json_decref(r); free(request);
    fake_stop(&na); CHECK(unlink(a_ctl) == 0);
    /* Nothing listening: the documented hint, exit code 1 from the driver. */
    request = fipsctl_build_query("show_status");
    CHECK(!fipsctl_send_request(a_ctl, request, &error));
    CHECK(strstr(error, "cannot connect to ") && strstr(error, "No such file or directory (os error 2)\nIs the FIPS daemon running?"));
    free(error); free(request);
    /* A truncated reply. */
    fake_start(&na, a_ctl, "{\"status\":\"ok\",\"data\":"); error = NULL;
    request = fipsctl_build_query("show_status");
    CHECK(!fipsctl_send_request(a_ctl, request, &error)); CHECK(strstr(error, "invalid response JSON: "));
    free(error); free(request); fake_stop(&na); CHECK(unlink(a_ctl) == 0);
    /* The pure pieces. */
    const char *pairs[][2] = {{"/run/fips/control.sock", "/run/fips/control-ipv6tun.sock"}, {"/a/node.ctl", "/a/node-ipv6tun.ctl"},
        {"/a/node", "/a/node-ipv6tun"}, {"/a/.hidden", "/a/.hidden-ipv6tun"}, {"rel.sock", "rel-ipv6tun.sock"}, {"/a/b.c.d", "/a/b.c-ipv6tun.d"}};
    for (size_t i = 0; i < 6; ++i) { char *s = fipsctl_ipv6tun_socket(pairs[i][0]); STREQ(s, pairs[i][1]); free(s); }
    CHECK(!fipsctl_ipv6tun_socket("/") && !fipsctl_ipv6tun_socket("a/..") && !fipsctl_ipv6tun_socket("."));
    json_t *node = json_pack("{s:s}", "npub", "npub1a");
    CHECK(fipsctl_composes("show_status", node) && !fipsctl_composes("show_peers", node));
    fipsctl_compose("show_peers", node, NULL); CHECK(json_object_size(node) == 1);
    json_t *adapter = json_pack("{s:s,s:i}", "tun_name", "fips0", "host_drops", 3);
    fipsctl_compose("show_status", node, adapter);
    STREQ(json_string_value(json_object_get(node, "tun_name")), "fips0");
    STREQ(json_string_value(json_object_get(node, "tun_state")), "disabled");
    CHECK(!json_object_get(node, "host_drops") && !fipsctl_composes("show_status", node));
    json_decref(node); json_decref(adapter);
    char *def = fipsctl_default_socket_path(); CHECK(def && strstr(def, "control.sock")); free(def);
    puts("sockets: adapter composition, symlink, error passthrough, fallback: ok");
}

/* ---- npub, addresses, key files ---------------------------------------------------- */

static void fill(void *ctx, uint8_t *buf, size_t n) { (void)ctx; if (n) { CHECK(sodium_init() >= 0); randombytes_buf(buf, n); } }

static fips_identity *generate(char npub[FIPS_NIP19_CAPACITY], char nsec[FIPS_NIP19_CAPACITY], char address[FIPSCTL_ADDR_CAPACITY])
{
    fips_identity *id = NULL;
    CHECK(fips_identity_generate(fipsctl_secp(), (fips_entropy){fill, NULL}, &id) == FIPS_IDENTITY_OK);
    CHECK(fips_identity_npub(id, npub, FIPS_NIP19_CAPACITY) == FIPS_IDENTITY_OK);
    uint8_t secret[32]; fips_identity_export_secret(id, secret);
    CHECK(fips_encode_nsec(fipsctl_secp(), secret, nsec, FIPS_NIP19_CAPACITY) == FIPS_IDENTITY_OK);
    sodium_memzero(secret, sizeof(secret));
    fips_ipv6_address a = fips_identity_address(id);
    CHECK(fips_ipv6_address_format(&a, address, FIPSCTL_ADDR_CAPACITY) == FIPS_IDENTITY_OK);
    return id;
}

static void alternating(const char *in, char *out)
{
    size_t i = 0;
    for (; in[i]; ++i) out[i] = i % 2 == 0 ? (char)toupper((unsigned char)in[i]) : in[i];
    out[i] = 0;
}

static void write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "w"); CHECK(f); fputs(text, f); fclose(f);
}

static void test_npub(const char *tmp)
{
    CHECK(fipsctl_begins_as_npub("npub1abc") && fipsctl_begins_as_npub("NPUB1ABC") && fipsctl_begins_as_npub("nPuB1"));
    CHECK(!fipsctl_begins_as_npub("npub") && !fipsctl_begins_as_npub("") && !fipsctl_begins_as_npub("xnpub1"));
#if defined(__APPLE__) || defined(__FreeBSD__)
    STREQ(fipsctl_default_key_dir(), "/usr/local/etc/fips"); STREQ(fipsctl_default_hosts_path(), "/usr/local/etc/fips/hosts");
#else
    STREQ(fipsctl_default_key_dir(), "/etc/fips"); STREQ(fipsctl_default_hosts_path(), "/etc/fips/hosts");
#endif
    CHECK(fipsctl_is_fips_mesh_address("fd9d:abcd::1") && fipsctl_is_fips_mesh_address("fd00::") &&
          fipsctl_is_fips_mesh_address("fdff:ffff:ffff:ffff:ffff:ffff:ffff:ffff"));
    CHECK(fipsctl_is_fips_mesh_address("[fd9d:abcd::1]:2121") && fipsctl_is_fips_mesh_address("[fd00::1]:8443"));
    CHECK(fipsctl_is_fips_mesh_address("fd9d:abcd::1:2121"));
    CHECK(!fipsctl_is_fips_mesh_address("fc00::1") && !fipsctl_is_fips_mesh_address("::1") &&
          !fipsctl_is_fips_mesh_address("2001:db8::1") && !fipsctl_is_fips_mesh_address("[2001:db8::1]:2121"));
    CHECK(!fipsctl_is_fips_mesh_address("192.0.2.1:2121") && !fipsctl_is_fips_mesh_address("peer.example.com:2121") &&
          !fipsctl_is_fips_mesh_address("coinos.pro:2121") && !fipsctl_is_fips_mesh_address(""));
    const char *t[] = {"udp", "tcp", "ethernet"};
    for (size_t i = 0; i < 3; ++i) { char *e = fipsctl_validate_connect_address("fd9d::1:2121", t[i]); CHECK(e); free(e); }
    CHECK(!fipsctl_validate_connect_address("fd9d::1:2121", "tor"));
    CHECK(!fipsctl_validate_connect_address("192.0.2.1:2121", "udp") && !fipsctl_validate_connect_address("peer.example.com:2121", "tcp")
          && !fipsctl_validate_connect_address("[2001:db8::1]:2121", "udp"));
    char npub[FIPS_NIP19_CAPACITY], nsec[FIPS_NIP19_CAPACITY], expected[FIPSCTL_ADDR_CAPACITY], got[FIPSCTL_ADDR_CAPACITY], spelling[FIPS_NIP19_CAPACITY];
    fips_identity *id = generate(npub, nsec, expected);
    fips_identity_destroy(id);
    CHECK(!strncmp(expected, "fd", 2));
    char *error = NULL;
    CHECK(fipsctl_address_from_npub(npub, got, &error)); STREQ(got, expected);
    for (size_t i = 0; npub[i]; ++i) spelling[i] = (char)toupper((unsigned char)npub[i]); spelling[strlen(npub)] = 0;
    CHECK(fipsctl_address_from_npub(spelling, got, &error)); STREQ(got, expected);
    alternating(npub, spelling);
    CHECK(fipsctl_address_from_npub(spelling, got, &error)); STREQ(got, expected);
    CHECK(!fipsctl_address_from_npub("npub1notarealkey", got, &error)); CHECK(strstr(error, "invalid npub: ")); free(error); error = NULL;
    CHECK(!fipsctl_address_from_npub("", got, &error)); free(error); error = NULL;
    /* resolve_peer passes an npub through in any case and looks hostnames up. */
    char hosts[256]; snprintf(hosts, sizeof(hosts), "%s/hosts", tmp);
    char text[2048];
    snprintf(text, sizeof(text), "# comment\n\n   \nHydra %s\nthree fields %s\n-bad %s\nbadnpub npub1notarealkey\nnpub1host %s\n"
             "ALIAS %s\nalias npub1notarealkey\nalias %s\n", npub, npub, npub, npub, "npub1qq8s4f7x", npub);
    write_text(hosts, text);
    char *r = fipsctl_lookup_host(hosts, "hydra"); STREQ(r, npub); free(r);
    r = fipsctl_lookup_host(hosts, "HYDRA"); STREQ(r, npub); free(r);
    r = fipsctl_lookup_host(hosts, "alias"); STREQ(r, npub); free(r); /* the last valid line wins */
    CHECK(!fipsctl_lookup_host(hosts, "three") && !fipsctl_lookup_host(hosts, "-bad") && !fipsctl_lookup_host(hosts, "badnpub")
          && !fipsctl_lookup_host(hosts, "npub1host") && !fipsctl_lookup_host(hosts, "nosuch") && !fipsctl_lookup_host("/nonexistent", "hydra"));
    char *data = NULL; size_t size = 0; FILE *err = open_memstream(&data, &size);
    r = fipsctl_resolve_peer_in(hosts, "hydra", err); STREQ(r, npub); free(r);
    r = fipsctl_resolve_peer_in(hosts, spelling, err); STREQ(r, spelling); free(r);
    r = fipsctl_resolve_peer_in(hosts, "NPUB1ANYTHING", err); STREQ(r, "NPUB1ANYTHING"); free(r);
    CHECK(!fipsctl_resolve_peer_in(hosts, "nosuch", err)); fclose(err);
    char want[512]; snprintf(want, sizeof(want), "error: unknown host 'nosuch'\nNot found in %s and not an npub.\n", hosts);
    STREQ(data, want); free(data);
    /* Key files: nsec in fips.key, npub in fips.pub, the public one answering when the private is absent. */
    char dir[256], key[256], pub[256]; snprintf(dir, sizeof(dir), "%s/keys", tmp); CHECK(mkdir(dir, 0700) == 0);
    snprintf(key, sizeof(key), "%s/fips.key", dir); snprintf(pub, sizeof(pub), "%s/fips.pub", dir);
    CHECK(!fipsctl_address_from_key_dir(dir, got, &error));
    CHECK(strstr(error, "fips.key") && strstr(error, "fips.pub") && strstr(error, "cannot read ")); free(error); error = NULL;
    snprintf(text, sizeof(text), "%s\n", npub); write_text(pub, text);
    CHECK(fipsctl_address_from_key_dir(dir, got, &error)); STREQ(got, expected);
    alternating(npub, spelling); snprintf(text, sizeof(text), "  %s\n\n", spelling); write_text(pub, text);
    CHECK(fipsctl_address_from_file(pub, got, &error)); STREQ(got, expected);
    snprintf(text, sizeof(text), "%s\n", nsec); write_text(key, text);
    CHECK(fipsctl_address_from_file(key, got, &error)); STREQ(got, expected);
    CHECK(fipsctl_address_from_key_dir(dir, got, &error)); STREQ(got, expected);
    CHECK(fipsctl_mesh_address(NULL, key, hosts, got, &error, stderr)); STREQ(got, expected);
    CHECK(fipsctl_mesh_address("hydra", NULL, hosts, got, &error, stderr)); STREQ(got, expected);
    CHECK(fipsctl_mesh_address(npub, NULL, hosts, got, &error, stderr)); STREQ(got, expected);
    err = open_memstream(&data, &size);
    CHECK(!fipsctl_mesh_address("nosuch", NULL, hosts, got, &error, err) && !error); fclose(err);
    CHECK(strstr(data, "unknown host 'nosuch'")); free(data);
    char junk[256]; snprintf(junk, sizeof(junk), "%s/junk.key", dir);
    write_text(junk, "\n");
    CHECK(!fipsctl_address_from_file(junk, got, &error)); CHECK(strstr(error, "key file is empty: ")); free(error); error = NULL;
    write_text(junk, "not-a-key\n");
    CHECK(!fipsctl_address_from_file(junk, got, &error)); CHECK(strstr(error, "does not hold a usable key")); free(error); error = NULL;
    /* keygen: stdout mode prints nsec then npub; file mode writes 0600/0644 and refuses to overwrite. */
    char *out_data = NULL, *err_data = NULL; size_t out_size = 0, err_size = 0;
    FILE *out = open_memstream(&out_data, &out_size); err = open_memstream(&err_data, &err_size);
    CHECK(fipsctl_keygen(dir, false, true, out, err) == 0); fclose(out); fclose(err);
    CHECK(!strncmp(out_data, "nsec1", 5) && strchr(out_data, '\n') && !strncmp(strchr(out_data, '\n') + 1, "npub1", 5));
    STREQ(err_data, "");
    {
        char *nl = strchr(out_data, '\n'); *nl = 0;
        fips_identity *from = NULL;
        CHECK(fips_identity_from_secret_str(fipsctl_secp(), (fips_entropy){fill, NULL}, out_data, strlen(out_data), &from) == FIPS_IDENTITY_OK);
        char derived[FIPS_NIP19_CAPACITY]; CHECK(fips_identity_npub(from, derived, sizeof(derived)) == FIPS_IDENTITY_OK);
        char *second = nl + 1; second[strcspn(second, "\n")] = 0;
        STREQ(second, derived);
        fips_identity_destroy(from);
    }
    free(out_data); free(err_data); out_data = err_data = NULL;
    char fresh[256]; snprintf(fresh, sizeof(fresh), "%s/fresh/sub", tmp);
    out = open_memstream(&out_data, &out_size); err = open_memstream(&err_data, &err_size);
    CHECK(fipsctl_keygen(fresh, false, false, out, err) == 0); fclose(out); fclose(err);
    STREQ(out_data, "");
    CHECK(!strncmp(err_data, "npub1", 5) && strstr(err_data, "Key files written to: ") && strstr(err_data, "/fresh/sub/\n\nNOTE: Set 'node.identity.persistent: true'"));
    free(out_data); free(err_data); out_data = err_data = NULL;
    struct stat st; char fkey[256], fpub[256];
    snprintf(fkey, sizeof(fkey), "%s/fips.key", fresh); snprintf(fpub, sizeof(fpub), "%s/fips.pub", fresh);
    CHECK(stat(fkey, &st) == 0 && (st.st_mode & 0777) == 0600);
    CHECK(stat(fpub, &st) == 0 && (st.st_mode & 0777) == 0644);
    char *key_text = read_file(fkey), *pub_text = read_file(fpub);
    CHECK(!strncmp(key_text, "nsec1", 5) && !strncmp(pub_text, "npub1", 5));
    char addr_key[FIPSCTL_ADDR_CAPACITY], addr_pub[FIPSCTL_ADDR_CAPACITY];
    CHECK(fipsctl_address_from_file(fkey, addr_key, &error) && fipsctl_address_from_file(fpub, addr_pub, &error));
    STREQ(addr_key, addr_pub);
    free(key_text); free(pub_text);
    out = open_memstream(&out_data, &out_size); err = open_memstream(&err_data, &err_size);
    CHECK(fipsctl_keygen(fresh, false, false, out, err) == 1); fclose(out); fclose(err);
    snprintf(want, sizeof(want), "error: key file already exists: %s\nUse --force to overwrite.\n", fkey);
    STREQ(err_data, want); free(out_data); free(err_data); out_data = err_data = NULL;
    out = open_memstream(&out_data, &out_size); err = open_memstream(&err_data, &err_size);
    CHECK(fipsctl_keygen(fresh, true, false, out, err) == 0); fclose(out); fclose(err);
    free(out_data); free(err_data); out_data = err_data = NULL;
    char *replaced = read_file(fkey); CHECK(fipsctl_address_from_file(fkey, got, &error)); CHECK(strcmp(got, addr_key)); free(replaced);
    /* A symlink at the key path is refused rather than followed, with --force too. */
    char linked[256], target[256]; snprintf(linked, sizeof(linked), "%s/linked", tmp); CHECK(mkdir(linked, 0700) == 0);
    snprintf(target, sizeof(target), "%s/target.txt", tmp); write_text(target, "keep\n");
    snprintf(fkey, sizeof(fkey), "%s/fips.key", linked); CHECK(symlink(target, fkey) == 0);
    out = open_memstream(&out_data, &out_size); err = open_memstream(&err_data, &err_size);
    CHECK(fipsctl_keygen(linked, true, false, out, err) == 1); fclose(out); fclose(err);
    CHECK(strstr(err_data, "error: failed to write key file: refusing to write key file through a symlink: "));
    free(out_data); free(err_data); out_data = err_data = NULL;
    char *kept = read_file(target); STREQ(kept, "keep"); free(kept);
    /* Dangling symlink: exists for the overwrite guard. */
    CHECK(unlink(fkey) == 0 && symlink("/nonexistent/x", fkey) == 0);
    out = open_memstream(&out_data, &out_size); err = open_memstream(&err_data, &err_size);
    CHECK(fipsctl_keygen(linked, false, false, out, err) == 1); fclose(out); fclose(err);
    CHECK(strstr(err_data, "key file already exists")); free(out_data); free(err_data);
    puts("npub: identifiers, mesh addresses, hosts, key files, keygen: ok");
}

/* ---- probe ----------------------------------------------------------------------- */

static json_t *J(const char *text)
{
    json_error_t e; json_t *v = json_loads(text, 0, &e);
    if (!v) fprintf(stderr, "%s: %s\n", text, e.text);
    CHECK(v); return v;
}

static json_t *running_report(void)
{
    return J("{\"elapsed_ms\":3400,"
             "\"bloom\":{\"verdict\":\"ok\",\"reason\":null,\"elapsed_ms\":0,\"fanout\":3},"
             "\"discovery\":{\"verdict\":\"ok\",\"reason\":null,\"elapsed_ms\":2000,\"attempts\":2,\"attempt_timeouts_secs\":[1,2,4,8]},"
             "\"path\":{\"verdict\":\"ok\",\"reason\":null,\"elapsed_ms\":0},"
             "\"session\":{\"verdict\":\"running\",\"reason\":null,\"elapsed_ms\":null},"
             "\"rtt\":{\"verdict\":\"pending\",\"reason\":null,\"elapsed_ms\":null,\"reports_seen\":0}}");
}

static json_t *absent_key_report(bool claimed)
{
    return claimed ? J("{\"overall\":\"failed\",\"elapsed_ms\":17000,"
                       "\"bloom\":{\"verdict\":\"ok\",\"reason\":null,\"elapsed_ms\":1000,\"fanout\":1},"
                       "\"discovery\":{\"verdict\":\"failed\",\"reason\":\"bloom_unconfirmed\",\"elapsed_ms\":16000,\"attempts\":4,\"attempt_timeouts_secs\":[1,2,4,8]},"
                       "\"path\":{\"verdict\":\"skipped\",\"reason\":\"not_reached\"},"
                       "\"session\":{\"verdict\":\"skipped\",\"reason\":\"not_reached\"},"
                       "\"rtt\":{\"verdict\":\"skipped\",\"reason\":\"not_reached\"}}")
                   : J("{\"overall\":\"failed\",\"elapsed_ms\":1900,"
                       "\"bloom\":{\"verdict\":\"failed\",\"reason\":\"bloom_miss\",\"elapsed_ms\":1900,\"fanout\":null},"
                       "\"discovery\":{\"verdict\":\"skipped\",\"reason\":\"not_reached\"},"
                       "\"path\":{\"verdict\":\"skipped\",\"reason\":\"not_reached\"},"
                       "\"session\":{\"verdict\":\"skipped\",\"reason\":\"not_reached\"},"
                       "\"rtt\":{\"verdict\":\"skipped\",\"reason\":\"not_reached\"}}");
}

static json_t *finished_report(void)
{
    return J("{\"overall\":\"ok\",\"elapsed_ms\":4000,"
             "\"bloom\":{\"verdict\":\"ok\",\"reason\":null,\"elapsed_ms\":1000,\"fanout\":1},"
             "\"discovery\":{\"verdict\":\"ok\",\"reason\":null,\"elapsed_ms\":0,\"attempts\":1,\"attempt_timeouts_secs\":[1,2,4,8]},"
             "\"path\":{\"verdict\":\"ok\",\"reason\":null,\"elapsed_ms\":0},"
             "\"session\":{\"verdict\":\"ok\",\"reason\":null,\"elapsed_ms\":1000},"
             "\"rtt\":{\"verdict\":\"ok\",\"reason\":null,\"elapsed_ms\":2000,\"rtt_ms\":83,\"reports_seen\":1}}");
}

static size_t row_at(const fipsctl_stage_row *rows, size_t count, const char *name)
{
    for (size_t i = 0; i < count; ++i) if (!strcmp(rows[i].name, name)) return i;
    fprintf(stderr, "no %s row\n", name); abort();
}

static bool starts_with(const char *s, const char *prefix)
{
    while (*s == ' ') ++s;
    return !strncmp(s, prefix, strlen(prefix));
}

static void test_probe(void)
{
    CHECK(fipsctl_probe_ceiling_ms(30000, false, 0) == 35000);
    CHECK(fipsctl_probe_ceiling_ms(30000, true, 2) == 2000);
    CHECK(fipsctl_probe_ceiling_ms(UINT64_MAX, false, 0) == UINT64_MAX);
    CHECK(fipsctl_probe_ceiling_ms(30000, true, UINT64_MAX) == UINT64_MAX);
    fipsctl_stage_row *rows; size_t n;
    json_t *running = running_report();
    n = fipsctl_stage_rows(running, &rows);
    size_t session = row_at(rows, n, "session");
    STREQ(rows[session].verdict, "running"); CHECK(!rows[session].settled);
    STREQ(rows[session].text, "XK handshake in flight"); CHECK(!strstr(rows[session].text, "established"));
    size_t rtt = row_at(rows, n, "rtt");
    STREQ(rows[rtt].verdict, "pending"); STREQ(rows[rtt].text, ""); CHECK(!rows[rtt].has_elapsed);
    CHECK(rows[row_at(rows, n, "discovery")].has_elapsed && rows[row_at(rows, n, "discovery")].elapsed_ms == 2000);
    CHECK(rows[row_at(rows, n, "path")].elapsed_ms == 0 && rows[session].has_elapsed && rows[session].elapsed_ms == 1400);
    const fipsctl_stage_row *d = &rows[row_at(rows, n, "discovery")];
    CHECK(d->sub_count == 2 && strstr(d->subs[0], "no reply") && strstr(d->subs[1], "replied"));
    STREQ(d->subs[0], "attempt 1    no reply  (1s)"); STREQ(d->subs[1], "attempt 2    replied   (2s)");
    char *l0 = fipsctl_stage_line(&rows[0], "\xe2\xa0\xb9"), *ls = fipsctl_stage_line(&rows[session], "\xe2\xa0\xb9"), *lq = fipsctl_stage_line(&rows[session], NULL);
    CHECK(!strstr(l0, "\xe2\xa0\xb9") && strstr(ls, "\xe2\xa0\xb9") && !strstr(lq, "\xe2\xa0\xb9"));
    STREQ(ls, "  session   \xe2\xa0\xb9 running   1.4s  XK handshake in flight");
    free(l0); free(ls); free(lq);
    /* Off a terminal each row prints once as it settles. */
    fipsctl_probe_progress p = {.tty = false, .header = true, .out = stdout};
    char **first; size_t nf = fipsctl_settled_since(&p, rows, n, &first);
    CHECK(nf == 4);
    CHECK(starts_with(first[0], "bloom") && starts_with(first[1], "discovery") && starts_with(first[2], "attempt 1") && starts_with(first[3], "attempt 2"));
    fipsctl_lines_free(first, nf);
    char **again; CHECK(fipsctl_settled_since(&p, rows, n, &again) == 0); fipsctl_lines_free(again, 0);
    fipsctl_stage_rows_free(rows, n);
    json_t *done = running_report();
    json_object_set_new(done, "session", J("{\"verdict\":\"ok\",\"reason\":null,\"elapsed_ms\":1000}"));
    json_object_set_new(done, "rtt", J("{\"verdict\":\"ok\",\"reason\":null,\"elapsed_ms\":1000,\"rtt_ms\":18,\"reports_seen\":1}"));
    n = fipsctl_stage_rows(done, &rows);
    char **rest; size_t nr = fipsctl_settled_since(&p, rows, n, &rest);
    CHECK(nr == 3); STREQ(rest[0], ""); CHECK(starts_with(rest[1], "session") && starts_with(rest[2], "rtt")); CHECK(p.printed == 7);
    fipsctl_lines_free(rest, nr); fipsctl_stage_rows_free(rows, n); json_decref(done); json_decref(running);
    /* The block ends at the first failure. */
    json_t *clean = absent_key_report(false), *claimed = absent_key_report(true);
    n = fipsctl_stage_rows(clean, &rows);
    CHECK(n == 1); STREQ(rows[0].name, "bloom"); STREQ(rows[0].text, "no peer filter holds this address");
    char **single; CHECK(fipsctl_block_lines(rows, n, NULL, &single) == 1);
    STREQ(single[0], "  bloom       failed    1.9s  no peer filter holds this address");
    fipsctl_lines_free(single, 1); fipsctl_stage_rows_free(rows, n);
    n = fipsctl_stage_rows(claimed, &rows);
    const char *claimed_text = rows[row_at(rows, n, "discovery")].text;
    STREQ(claimed_text, "a peer filter claimed this address; 4 requests went unanswered");
    const char *note = fipsctl_overall_note(claimed);
    CHECK(note && strstr(note, "false-positive") && strstr(note, "bloom_miss") && !fipsctl_overall_note(clean));
    fipsctl_stage_rows_free(rows, n);
    json_object_set_new(json_object_get(claimed, "discovery"), "reason", json_string("no_response"));
    n = fipsctl_stage_rows(claimed, &rows);
    STREQ(rows[row_at(rows, n, "discovery")].text, "no reply to 4 requests"); CHECK(!fipsctl_overall_note(claimed));
    fipsctl_stage_rows_free(rows, n); json_decref(clean); json_decref(claimed);
    /* A failed path keeps the stages that ran after it; a skip keeps the rows behind it. */
    json_t *failed_path = J("{\"elapsed_ms\":3000,\"bloom\":{\"verdict\":\"ok\",\"reason\":null,\"elapsed_ms\":0,\"fanout\":1},"
                            "\"discovery\":{\"verdict\":\"ok\",\"reason\":null,\"elapsed_ms\":1000,\"attempts\":1,\"attempt_timeouts_secs\":[1,2,4,8]},"
                            "\"path\":{\"verdict\":\"failed\",\"reason\":\"no_next_hop\",\"elapsed_ms\":0},"
                            "\"session\":{\"verdict\":\"ok\",\"reason\":null,\"elapsed_ms\":1000},"
                            "\"rtt\":{\"verdict\":\"ok\",\"reason\":null,\"elapsed_ms\":1000,\"rtt_ms\":22,\"reports_seen\":1}}");
    n = fipsctl_stage_rows(failed_path, &rows);
    CHECK(n == 5); STREQ(rows[row_at(rows, n, "path")].verdict, "failed"); STREQ(rows[row_at(rows, n, "session")].verdict, "ok");
    CHECK(!rows[row_at(rows, n, "path")].hidden); STREQ(rows[row_at(rows, n, "path")].text, "no next hop toward this target");
    fipsctl_stage_rows_free(rows, n); json_decref(failed_path);
    json_t *skip = J("{\"elapsed_ms\":2000,\"bloom\":{\"verdict\":\"skipped\",\"reason\":\"direct_peer\",\"elapsed_ms\":0},"
                     "\"discovery\":{\"verdict\":\"skipped\",\"reason\":\"direct_peer\",\"elapsed_ms\":0},"
                     "\"path\":{\"verdict\":\"ok\",\"reason\":null,\"elapsed_ms\":0},"
                     "\"session\":{\"verdict\":\"ok\",\"reason\":null,\"elapsed_ms\":1000},"
                     "\"rtt\":{\"verdict\":\"ok\",\"reason\":null,\"elapsed_ms\":1000,\"rtt_ms\":17,\"reports_seen\":1}}");
    n = fipsctl_stage_rows(skip, &rows); CHECK(n == 5);
    STREQ(rows[0].text, "target is a directly connected peer"); STREQ(rows[1].text, "no lookup needed");
    fipsctl_stage_rows_free(rows, n); json_decref(skip);
    /* Every request the discovery stage sent gets its own line; only the one in flight animates. */
    json_t *attempts = J("{\"elapsed_ms\":7100,\"bloom\":{\"verdict\":\"ok\",\"reason\":null,\"elapsed_ms\":0,\"fanout\":1},"
                         "\"discovery\":{\"verdict\":\"running\",\"reason\":null,\"elapsed_ms\":null,\"attempts\":3,\"attempt_timeouts_secs\":[1,2,4,8]},"
                         "\"path\":{\"verdict\":\"pending\"},\"session\":{\"verdict\":\"pending\"},\"rtt\":{\"verdict\":\"pending\"}}");
    n = fipsctl_stage_rows(attempts, &rows);
    const fipsctl_stage_row *disc = &rows[row_at(rows, n, "discovery")];
    CHECK(disc->sub_count == 3);
    CHECK(strstr(disc->subs[0], "no reply") && strstr(disc->subs[0], "(1s)") && !strstr(disc->subs[0], FIPSCTL_SPIN_HERE));
    CHECK(strstr(disc->subs[1], "no reply") && strstr(disc->subs[1], "(2s)"));
    CHECK(strstr(disc->subs[2], "waiting") && strstr(disc->subs[2], "(4s)") && strstr(disc->subs[2], FIPSCTL_SPIN_HERE));
    STREQ(disc->text, "waiting for a reply"); CHECK(disc->has_elapsed && disc->elapsed_ms == 7100);
    char **live; size_t nl = fipsctl_block_lines(rows, n, "\xe2\xa0\x8b", &live);
    CHECK(nl == 8); /* bloom, discovery, three attempts, the separator, session, rtt */
    STREQ(live[1], "  discovery \xe2\xa0\x8b running   7.1s  waiting for a reply");
    STREQ(live[5], ""); STREQ(live[6], "  session     pending"); STREQ(live[7], "  rtt         pending");
    STREQ(live[4], "                              attempt 3  \xe2\xa0\x8b waiting   (4s)");
    STREQ(live[2], "                              attempt 1    no reply  (1s)");
    fipsctl_lines_free(live, nl); fipsctl_stage_rows_free(rows, n); json_decref(attempts);
    /* A successful path stage takes no row; a failed one keeps it. One separator, above session. */
    json_t *finished = finished_report();
    n = fipsctl_stage_rows(finished, &rows);
    char **ok_lines; size_t no = fipsctl_block_lines(rows, n, NULL, &ok_lines);
    size_t blanks = 0, blank_at = 0;
    for (size_t i = 0; i < no; ++i) { CHECK(!starts_with(ok_lines[i], "path")); if (!ok_lines[i][0]) { ++blanks; blank_at = i; } }
    CHECK(blanks == 1 && starts_with(ok_lines[blank_at + 1], "session") && ok_lines[no - 1][0]);
    STREQ(rows[row_at(rows, n, "rtt")].text, "83 ms round trip");
    fipsctl_lines_free(ok_lines, no); fipsctl_stage_rows_free(rows, n);
    json_object_set_new(json_object_get(finished, "rtt"), "reports_seen", json_integer(3));
    n = fipsctl_stage_rows(finished, &rows); STREQ(rows[row_at(rows, n, "rtt")].text, "83 ms round trip (3 report exchanges)"); fipsctl_stage_rows_free(rows, n);
    json_object_set_new(finished, "path", J("{\"verdict\":\"failed\",\"reason\":\"disjoint_trees\",\"elapsed_ms\":0}"));
    n = fipsctl_stage_rows(finished, &rows);
    char **broken; size_t nb = fipsctl_block_lines(rows, n, NULL, &broken);
    bool found = false;
    for (size_t i = 0; i < nb; ++i) if (starts_with(broken[i], "path")) { found = true; CHECK(strstr(broken[i], "different spanning-tree roots")); }
    CHECK(found); fipsctl_lines_free(broken, nb); fipsctl_stage_rows_free(rows, n); json_decref(finished);
    /* Coordinates and the walk. */
    json_t *coords = J("[\"4d2201ff00000000000000000000dead\",\"91b40c6e00000000000000000000beef\"]");
    char *rc = fipsctl_render_coords(coords, "self"); STREQ(rc, "91b40c6e > 4d2201ff [self]"); free(rc); json_decref(coords);
    coords = J("[\"91b40c6e00000000000000000000beef\"]");
    rc = fipsctl_render_coords(coords, "self"); STREQ(rc, "91b40c6e [root]"); free(rc); json_decref(coords);
    coords = J("[]"); rc = fipsctl_render_coords(coords, "self"); STREQ(rc, "-"); free(rc); json_decref(coords);
    json_t *crossing = J("{\"our_coords\":[\"423877f900000000000000000000aaaa\",\"a23598d900000000000000000000bbbb\",\"00001a8c00000000000000000000cccc\"],"
                         "\"their_coords\":[\"4f9b4a8400000000000000000000dddd\",\"4d8009df00000000000000000000eeee\",\"00001a8c00000000000000000000cccc\"],"
                         "\"lca\":\"00001a8c00000000000000000000cccc\"}");
    char *plain = fipsctl_walk_line(crossing, "00001a8c00000000000000000000cccc", false);
    STREQ(plain, "423877f9 [self] > a23598d9 > 00001a8c > 4d8009df > 4f9b4a84 [target]");
    char *styled = fipsctl_walk_line(crossing, "00001a8c00000000000000000000cccc", true);
    STREQ(styled, "423877f9 [self] > a23598d9 > \x1b[1m00001a8c\x1b[0m > 4d8009df > 4f9b4a84 [target]");
    free(plain); free(styled); json_decref(crossing);
    json_t *parent = J("{\"our_coords\":[\"423877f900000000000000000000aaaa\",\"a23598d900000000000000000000bbbb\",\"00001a8c00000000000000000000cccc\"],"
                       "\"their_coords\":[\"a23598d900000000000000000000bbbb\",\"00001a8c00000000000000000000cccc\"],\"lca\":\"a23598d900000000000000000000bbbb\"}");
    plain = fipsctl_walk_line(parent, "a23598d900000000000000000000bbbb", false);
    STREQ(plain, "423877f9 [self] > a23598d9 [target]"); free(plain); json_decref(parent);
    /* Clipping counts characters, not bytes. */
    char line[1024] = "session   ok       ";
    for (int i = 0; i < 200; ++i) strcat(line, "\xc3\xa9");
    char *clipped = fipsctl_clip(line, FIPSCTL_LIVE_WIDTH);
    CHECK(fipsctl_utf8_chars(clipped) == FIPSCTL_LIVE_WIDTH && !strcmp(clipped + strlen(clipped) - 3, "\xe2\x80\xa6"));
    free(clipped); clipped = fipsctl_clip("short", FIPSCTL_LIVE_WIDTH); STREQ(clipped, "short"); free(clipped);
    /* Partial notes. */
    json_t *partial = J("{\"overall\":\"partial\",\"rtt\":{\"verdict\":\"failed\",\"reason\":\"no_report\"}}");
    CHECK(strstr(fipsctl_overall_note(partial), "reachable in one direction only"));
    json_object_set_new(json_object_get(partial, "rtt"), "reason", json_string("sub_millisecond"));
    CHECK(strstr(fipsctl_overall_note(partial), "The endpoint IS reachable"));
    json_object_set_new(json_object_get(partial, "rtt"), "reason", json_string("other"));
    CHECK(!fipsctl_overall_note(partial)); json_decref(partial);
    /* The whole human report for the upstream probe golden, off a terminal. */
    char path[4096]; snprintf(path, sizeof(path), "%s/probe_report.json", FIPSD_CONTROL_GOLDEN);
    json_error_t e; json_t *report = json_load_file(path, 0, &e); CHECK(report);
    char *data = NULL; size_t size = 0; FILE *out = open_memstream(&data, &size);
    fipsctl_probe_progress off = {.tty = false, .out = out};
    fipsctl_probe_update(&off, report);
    fipsctl_probe_finish(&off, report);
    fclose(out);
    STREQ(data,
        "probe hydra\n"
        "  npub       npub1qq8s4f7x\n"
        "  node_addr  4a1f9c22e08b5d3714aa06fb92c1de55\n"
        "  ipv6       fd00:4a1f:9c22:e08b:5d37:14aa:06fb:92c1\n"
        "\n"
        "  bloom       ok        0.0s  claimed by a peer filter, 3 peers asked\n"
        "  discovery   ok        2.0s  coordinates from mesh lookup\n"
        "                              attempt 1    no reply  (1s)\n"
        "                              attempt 2    replied   (2s)\n"
        "\n"
        "  session     ok        1.0s  established (XK handshake)\n"
        "  rtt         ok        1.0s  18 ms round trip\n"
        "\n"
        "path:\n"
        "  ours       depth 1    91b40c6e > 4d2201ff [self]\n"
        "  theirs     depth 1    91b40c6e > 4a1f9c22 [target]\n"
        "\n"
        "  route      4d2201ff [self] > 91b40c6e > 4a1f9c22 [target]\n"
        "\n"
        "  walk       1 up, 1 down   tree distance 2 (upper bound; crosslinks may shorten it)\n"
        "  next hop   91b40c6e (relay-a)   class tree up\n"
        "  path mtu   1420 bytes (observed)\n"
        "\n"
        "  cleanup    session was opened by this probe and has been torn down\n"
        "  overall    ok, 4000 ms   (stage timings quantized to the 1000 ms tick)\n");
    free(data);
    /* On a terminal the live block is erased and the settled one printed in full; the ancestor is emphasised. */
    data = NULL; out = open_memstream(&data, &size);
    fipsctl_probe_progress tty = {.tty = true, .out = out};
    fipsctl_probe_update(&tty, report);
    CHECK(tty.drawn == 7 && tty.frame == 1);
    fipsctl_probe_finish(&tty, report);
    fclose(out);
    CHECK(strstr(data, "\x1b[7A") && strstr(data, "\x1b[2K") && tty.drawn == 0);
    CHECK(strstr(data, "  route      4d2201ff [self] > \x1b[1m91b40c6e\x1b[0m > 4a1f9c22 [target]\n"));
    free(data);
    /* A failure before the path stage closes without a path section; the unknown-npub shape fipsd produces. */
    json_t *miss = absent_key_report(false);
    json_object_set_new(miss, "target", J("{\"display_name\":\"4a1f9c22...\",\"npub\":\"npub1qq8s4f7x\",\"node_addr\":\"4a1f\",\"ipv6_addr\":\"fd00::1\"}"));
    json_object_set_new(miss, "cleanup", J("{\"session_created_and_torn_down\":false,\"left_intact_reason\":null,\"warmups_sent\":0}"));
    json_object_set_new(miss, "tick_ms", json_integer(1000));
    data = NULL; out = open_memstream(&data, &size);
    fipsctl_probe_progress off2 = {.tty = false, .out = out};
    fipsctl_probe_finish(&off2, miss); fclose(out);
    STREQ(data,
        "probe 4a1f9c22...\n  npub       npub1qq8s4f7x\n  node_addr  4a1f\n  ipv6       fd00::1\n\n"
        "  bloom       failed    1.9s  no peer filter holds this address\n\n"
        "  cleanup    nothing to clean up\n"
        "  overall    failed, 1900 ms   (stage timings quantized to the 1000 ms tick)\n");
    free(data); json_decref(miss);
    /* No coordinates: the coords line replaces the tree lines; a warmup on a borrowed session is named. */
    json_object_set_new(report, "path", J("{\"verdict\":\"ok\",\"reason\":null,\"elapsed_ms\":0,\"coords_known\":false,\"next_hop\":null,\"no_hop_reason\":\"no_coords\"}"));
    json_object_set_new(report, "cleanup", J("{\"session_created_and_torn_down\":false,\"left_intact_reason\":\"preexisting\",\"warmups_sent\":2}"));
    data = NULL; out = open_memstream(&data, &size);
    fipsctl_probe_detail(out, report, false); fclose(out);
    STREQ(data, "\npath:\n  coords     none cached for this target, so no tree walk was computed\n  next hop   none (no coords)\n"
                "  path mtu   1420 bytes (observed)\n\n  cleanup    session left in place (preexisting)\n"
                "             2 coords-warmup message(s) were sent on a session this probe does not own\n"
                "  overall    ok, 4000 ms   (stage timings quantized to the 1000 ms tick)\n");
    free(data); json_decref(report);
    puts("probe: ceiling, rows, block, walk, clip, notes, full golden rendering: ok");
}

int main(void)
{
    const char *tmp = fipsd_test_tmpdir("/a/control-ipv6tun.sock");
    test_parse();
    test_requests();
    test_pretty();
    test_plot();
    test_sockets(tmp);
    test_npub(tmp);
    test_probe();
    puts("fipsctl unit tests: all ok");
    return 0;
}
