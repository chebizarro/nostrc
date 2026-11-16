/* Argument parsing, help, version, request building and the top-level driver
 * (upstream fipsctl.rs: Cli/Commands, build_query/build_command, main). The
 * parser follows clap's shape: global options before the subcommand, each
 * subcommand owning its options and positionals, `--help` at any level. A
 * usage error exits 1, as docs/reference/cli-fipsctl.md documents. */
#include "fipsctl.h"
#include "fipsctl-version.h"
#include "fipsd_build_info.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#ifndef FIPSD_TARGET_TRIPLE
#define FIPSD_TARGET_TRIPLE "unknown"
#endif

const char *fipsctl_short_version(void)
{
    return FIPSD_GIT_REV[0] ? FIPSCTL_VERSION " (rev " FIPSD_GIT_REV ")" : FIPSCTL_VERSION;
}

void fipsctl_print_version(FILE *out, bool long_form)
{
    fprintf(out, "fipsctl %s\n", fipsctl_short_version());
    if (long_form) fprintf(out, "target: %s\n", FIPSD_TARGET_TRIPLE);
}

/* ---- help -------------------------------------------------------------- */

typedef struct scope_help { const char *scope, *usage, *about, *body; } scope_help;

static const scope_help HELP[] = {
    {"", "fipsctl [OPTIONS] <COMMAND>", "Control a running FIPS daemon",
     "Commands:\n"
     "  show        Show node information\n"
     "  acl         Show peer ACL information\n"
     "  keygen      Generate a new FIPS identity keypair\n"
     "  address     Print a node's mesh address, without contacting the daemon\n"
     "  connect     Connect to a peer\n"
     "  disconnect  Disconnect a peer\n"
     "  path        Show a peer's paths, or pin its traffic to a transport or one path\n"
     "  probe       Diagnose reachability of a mesh endpoint\n"
     "  stats       Query historical node statistics\n"
#ifdef FIPSCTL_PROFILING
     "  profile     Control the built-in profiler (requires a profiling build)\n"
#endif
     "  help        Print this message or the help of the given subcommand(s)\n"
     "\n"
     "Options:\n"
     "  -s, --socket <SOCKET>  Control socket path override\n"
     "  -h, --help             Print help\n"
     "  -V                     Print the short version, <version> (rev <git-hash>)\n"
     "      --version          Print the long version: short version plus build target triple\n"},
    {"show", "fipsctl show <COMMAND>", "Show node information",
     "Commands:\n"
     "  status          Node status overview\n"
     "  peers           Authenticated peers\n"
     "  links           Active links\n"
     "  tree            Spanning tree state\n"
     "  sessions        End-to-end sessions\n"
     "  bloom           Bloom filter state\n"
     "  mmp             MMP metrics summary\n"
     "  cache           Coordinate cache stats\n"
     "  connections     Pending handshake connections\n"
     "  transports      Transport instances\n"
     "  routing         Routing table summary\n"
     "  identity-cache  Identity cache entries (known node pubkeys)\n"
     "  native-flows    Native datagram API flows and listeners\n"
     "  help            Print this message or the help of the given subcommand(s)\n"
     "\n"
     "Options:\n"
     "  -h, --help  Print help\n"},
    {"acl", "fipsctl acl <COMMAND>", "Show peer ACL information",
     "Commands:\n"
     "  show  Loaded peer ACL state\n"
     "  help  Print this message or the help of the given subcommand(s)\n"
     "\n"
     "Options:\n"
     "  -h, --help  Print help\n"},
    {"keygen", "fipsctl keygen [OPTIONS]", "Generate a new FIPS identity keypair",
     "Options:\n"
     "  -d, --dir <DIR>  Output directory for fips.key and fips.pub [default: <platform config dir>]\n"
     "  -f, --force      Overwrite existing key files\n"
     "  -s, --stdout     Print nsec and npub to stdout instead of writing files\n"
     "  -h, --help       Print help\n"},
    {"address", "fipsctl address [OPTIONS] [IDENTITY]", "Print a node's mesh address, without contacting the daemon",
     "Arguments:\n"
     "  [IDENTITY]  npub (bech32) or hostname from the hosts file. Defaults to this node's own identity, read from its key files\n"
     "\n"
     "Options:\n"
     "  -k, --key <KEY>  Derive from this key file (an nsec) or public key file (an npub)\n"
     "  -h, --help       Print help\n"},
    {"connect", "fipsctl connect <PEER> <ADDRESS> <TRANSPORT>", "Connect to a peer",
     "Arguments:\n"
     "  <PEER>       Peer identifier: npub (bech32) or hostname from the hosts file\n"
     "  <ADDRESS>    Transport address (e.g., \"192.168.1.1:2121\")\n"
     "  <TRANSPORT>  Transport type: udp, tcp, tor, ethernet\n"
     "\n"
     "Options:\n"
     "  -h, --help  Print help\n"},
    {"disconnect", "fipsctl disconnect <PEER>", "Disconnect a peer",
     "Arguments:\n"
     "  <PEER>  Peer identifier: npub (bech32) or hostname from the hosts file\n"
     "\n"
     "Options:\n"
     "  -h, --help  Print help\n"},
    {"path", "fipsctl path <COMMAND>", "Show a peer's paths, or pin its traffic to a transport or one path",
     "Commands:\n"
     "  show   Show every path to a peer: its state, role, pin and measurements\n"
     "  pin    Pin traffic to the peer to a transport, or with an address to one path on it\n"
     "  unpin  Clear the peer's pin\n"
     "  help   Print this message or the help of the given subcommand(s)\n"
     "\n"
     "Options:\n"
     "  -h, --help  Print help\n"},
    {"path show", "fipsctl path show <PEER>", "Show every path to a peer: its state, role, pin and measurements",
     "Arguments:\n"
     "  <PEER>  Peer identifier: npub (bech32) or hostname from the hosts file\n"
     "\n"
     "Options:\n"
     "  -h, --help  Print help\n"},
    {"path pin", "fipsctl path pin <PEER> <TRANSPORT> [ADDRESS]",
     "Pin traffic to the peer to a transport, or with an address to one path on it",
     "Arguments:\n"
     "  <PEER>       Peer identifier: npub (bech32) or hostname from the hosts file\n"
     "  <TRANSPORT>  Transport: instance name as configured, or the numeric id `path show` prints\n"
     "  [ADDRESS]    The peer's address on that transport, to pin one path\n"
     "\n"
     "Options:\n"
     "  -h, --help  Print help\n"},
    {"path unpin", "fipsctl path unpin <PEER>", "Clear the peer's pin",
     "Arguments:\n"
     "  <PEER>  Peer identifier: npub (bech32) or hostname from the hosts file\n"
     "\n"
     "Options:\n"
     "  -h, --help  Print help\n"},
    {"probe", "fipsctl probe [OPTIONS] <TARGET>", "Diagnose reachability of a mesh endpoint",
     "Arguments:\n"
     "  <TARGET>  Target identifier: npub (bech32) or hostname from the hosts file\n"
     "\n"
     "Options:\n"
     "      --json               Emit the report as JSON instead of human-readable text\n"
     "      --timeout <TIMEOUT>  Client-side ceiling in seconds. Defaults to the daemon's own computed budget\n"
     "  -h, --help               Print help\n"},
    {"stats", "fipsctl stats <COMMAND>", "Query historical node statistics",
     "Commands:\n"
     "  list     List available history metrics\n"
     "  metrics  Dump current counter values for every protocol metric family\n"
     "  peers    List peers tracked in the stats history\n"
     "  history  Fetch a time-series window for a metric\n"
     "  help     Print this message or the help of the given subcommand(s)\n"
     "\n"
     "Options:\n"
     "  -h, --help  Print help\n"},
    {"stats history", "fipsctl stats history [OPTIONS] <METRIC>", "Fetch a time-series window for a metric",
     "Arguments:\n"
     "  <METRIC>  Metric name (see `fipsctl stats list`). Node-level metrics need no `--peer`; per-peer metrics require it\n"
     "\n"
     "Options:\n"
     "      --peer <PEER>                Peer npub (bech32) or hostname from the hosts file for per-peer metrics\n"
     "      --window <WINDOW>            Window duration \xe2\x80\x94 `<N>s`, `<N>m`, `<N>h` [default: 10m]\n"
     "      --granularity <GRANULARITY>  Sample resolution \xe2\x80\x94 `1s` (fast ring) or `1m` (slow ring) [default: 1s]\n"
     "      --plot                       Render a Unicode block sparkline instead of JSON\n"
     "  -h, --help                       Print help\n"},
#ifdef FIPSCTL_PROFILING
    {"profile", "fipsctl profile <COMMAND>", "Control the built-in profiler (requires a profiling build)",
     "Commands:\n"
     "  tick  Profile the rx-loop tick body\n"
     "  help  Print this message or the help of the given subcommand(s)\n"
     "\n"
     "Options:\n"
     "  -h, --help  Print help\n"},
    {"profile tick", "fipsctl profile tick <COMMAND>", "Profile the rx-loop tick body",
     "Commands:\n"
     "  on      Start a capture\n"
     "  off     Stop the running capture\n"
     "  status  Report capture state\n"
     "  help    Print this message or the help of the given subcommand(s)\n"
     "\n"
     "Options:\n"
     "  -h, --help  Print help\n"},
    {"profile tick on", "fipsctl profile tick on [OPTIONS]", "Start a capture",
     "Options:\n"
     "      --dir <DIR>  Directory for the capture file (default /var/log/fips)\n"
     "  -h, --help       Print help\n"},
#endif
};

static const scope_help *scope_lookup(const char *scope)
{
    if (!scope) scope = "";
    for (size_t i = 0; i < sizeof(HELP) / sizeof(HELP[0]); ++i)
        if (!strcmp(HELP[i].scope, scope)) return &HELP[i];
    return &HELP[0];
}

void fipsctl_print_help(FILE *out, const char *scope)
{
    const scope_help *h = scope_lookup(scope);
    fprintf(out, "%s\n\nUsage: %s\n\n%s", h->about, h->usage, h->body);
}

/* ---- parsing ------------------------------------------------------------ */

static int usage_error(FILE *err, const char *scope, const char *message)
{
    if (err) fprintf(err, "error: %s\n\nUsage: %s\n\nFor more information, try '--help'.\n", message,
                     scope_lookup(scope)->usage);
    return 1;
}

static int unexpected(FILE *err, const char *scope, const char *arg)
{
    char *m = fipsctl_aprintf("unexpected argument '%s' found", arg);
    int rc = usage_error(err, scope, m ? m : "unexpected argument");
    free(m);
    return rc;
}

static int missing_value(FILE *err, const char *scope, const char *option)
{
    char *m = fipsctl_aprintf("a value is required for '%s' but none was supplied", option);
    int rc = usage_error(err, scope, m ? m : "a value is required");
    free(m);
    return rc;
}

static int required(FILE *err, const char *scope, const char *const *names, size_t from, size_t count)
{
    char buf[512];
    size_t n = (size_t)snprintf(buf, sizeof(buf), "the following required arguments were not provided:");
    for (size_t i = from; i < count && n < sizeof(buf); ++i)
        n += (size_t)snprintf(buf + n, sizeof(buf) - n, "\n  <%s>", names[i]);
    return usage_error(err, scope, buf);
}

static int subcommand_required(FILE *err, const char *scope)
{
    char *m = fipsctl_aprintf("'fipsctl%s%s' requires a subcommand but one was not provided",
                              scope[0] ? " " : "", scope);
    int rc = usage_error(err, scope, m ? m : "a subcommand is required");
    free(m);
    return rc;
}

static int unrecognized(FILE *err, const char *scope, const char *name)
{
    char *m = fipsctl_aprintf("unrecognized subcommand '%s'", name);
    int rc = usage_error(err, scope, m ? m : "unrecognized subcommand");
    free(m);
    return rc;
}

static bool is_help(const char *arg) { return !strcmp(arg, "-h") || !strcmp(arg, "--help"); }

/* An option that takes a value, written `--name VALUE`, `--name=VALUE`,
 * `-n VALUE` or `-nVALUE`. On a match the value is stored and *i advanced. */
typedef struct option_spec {
    const char *long_name; /* "--peer" */
    char short_name;       /* 'k' or 0 */
    bool takes_value;
    const char **value;    /* for takes_value */
    bool *flag;            /* for a flag */
} option_spec;

/* Returns 1 matched, 0 not an option of this scope, -1 usage error (printed). */
static int match_option(int argc, char **argv, int *i, const option_spec *specs, size_t nspecs,
                        const char *scope, FILE *err)
{
    const char *arg = argv[*i];
    for (size_t s = 0; s < nspecs; ++s) {
        const option_spec *o = &specs[s];
        size_t ll = o->long_name ? strlen(o->long_name) : 0;
        bool long_match = o->long_name && !strncmp(arg, o->long_name, ll) && (arg[ll] == 0 || arg[ll] == '=');
        bool short_match = o->short_name && arg[0] == '-' && arg[1] == o->short_name && arg[1] != '-';
        if (!long_match && !short_match) continue;
        if (!o->takes_value) {
            if ((long_match && arg[ll] == '=') || (short_match && arg[2] != 0)) return 0;
            *o->flag = true;
            return 1;
        }
        const char *value = NULL;
        if (long_match && arg[ll] == '=') value = arg + ll + 1;
        else if (short_match && arg[2] != 0) value = arg + 2;
        else if (*i + 1 < argc) value = argv[++*i];
        else {
            char label[96];
            snprintf(label, sizeof(label), "%s <%s>", o->long_name ? o->long_name : "-?",
                     o->long_name ? o->long_name + 2 : "VALUE");
            for (char *p = label; *p && *p != ' '; ++p) {}
            for (char *p = strchr(label, '<') ? strchr(label, '<') + 1 : label; *p && *p != '>'; ++p)
                if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 'a' + 'A');
            missing_value(err, scope, label);
            return -1;
        }
        *o->value = value;
        return 1;
    }
    return 0;
}

/* Collect a scope's options and positionals from argv[start..]. */
static int parse_scope(int argc, char **argv, int start, const char *scope, const option_spec *specs,
                       size_t nspecs, const char *const *positional_names, size_t min_pos, size_t max_pos,
                       const char **positionals, FILE *err, bool *help)
{
    size_t npos = 0;
    bool only_positionals = false;
    for (int i = start; i < argc; ++i) {
        const char *arg = argv[i];
        if (!only_positionals) {
            if (!strcmp(arg, "--")) { only_positionals = true; continue; }
            if (is_help(arg)) { *help = true; return 0; }
            if (arg[0] == '-' && arg[1]) {
                int m = match_option(argc, argv, &i, specs, nspecs, scope, err);
                if (m < 0) return 1;
                if (m) continue;
                return unexpected(err, scope, arg);
            }
        }
        if (npos >= max_pos) return unexpected(err, scope, arg);
        positionals[npos++] = arg;
    }
    if (npos < min_pos) return required(err, scope, positional_names, npos, min_pos);
    return 0;
}

static const struct { const char *name, *command; } SHOW[] = {
    {"status", "show_status"}, {"peers", "show_peers"}, {"links", "show_links"}, {"tree", "show_tree"},
    {"sessions", "show_sessions"}, {"bloom", "show_bloom"}, {"mmp", "show_mmp"}, {"cache", "show_cache"},
    {"connections", "show_connections"}, {"transports", "show_transports"}, {"routing", "show_routing"},
    {"identity-cache", "show_identity_cache"}, {"native-flows", "show_native_flows"},
};

static int parse_query_group(int argc, char **argv, int start, const char *scope, fipsctl_args *a, FILE *err,
                             bool (*lookup)(const char *, const char **))
{
    if (start >= argc) return subcommand_required(err, scope);
    const char *name = argv[start];
    if (is_help(name) || !strcmp(name, "help")) { a->command = FIPSCTL_CMD_HELP; a->help_scope = scope; return 0; }
    if (name[0] == '-') return unexpected(err, scope, name);
    const char *command = NULL;
    if (!lookup(name, &command)) return unrecognized(err, scope, name);
    a->command = FIPSCTL_CMD_QUERY; a->query = command;
    const char *none[1];
    bool help = false;
    char sub_scope[64]; snprintf(sub_scope, sizeof(sub_scope), "%s %s", scope, name);
    int rc = parse_scope(argc, argv, start + 1, scope, NULL, 0, NULL, 0, 0, none, err, &help);
    if (rc) return rc;
    if (help) { a->command = FIPSCTL_CMD_HELP; a->help_scope = scope; }
    return 0;
}

static bool show_lookup(const char *name, const char **command)
{
    for (size_t i = 0; i < sizeof(SHOW) / sizeof(SHOW[0]); ++i)
        if (!strcmp(SHOW[i].name, name)) { *command = SHOW[i].command; return true; }
    return false;
}

static bool acl_lookup(const char *name, const char **command)
{
    if (strcmp(name, "show")) return false;
    *command = "show_acl"; return true;
}

static int parse_path(int argc, char **argv, int start, fipsctl_args *a, FILE *err)
{
    if (start >= argc) return subcommand_required(err, "path");
    const char *name = argv[start];
    if (is_help(name) || !strcmp(name, "help")) { a->command = FIPSCTL_CMD_HELP; a->help_scope = "path"; return 0; }
    if (name[0] == '-') return unexpected(err, "path", name);
    const char *positionals[3] = {0};
    bool help = false;
    int rc;
    if (!strcmp(name, "show") || !strcmp(name, "unpin")) {
        static const char *const names[] = {"PEER"};
        const char *scope = name[0] == 's' ? "path show" : "path unpin";
        rc = parse_scope(argc, argv, start + 1, scope, NULL, 0, names, 1, 1, positionals, err, &help);
        if (rc) return rc;
        if (help) { a->command = FIPSCTL_CMD_HELP; a->help_scope = scope; return 0; }
        a->command = name[0] == 's' ? FIPSCTL_CMD_PATH_SHOW : FIPSCTL_CMD_PATH_UNPIN;
        a->peer = positionals[0];
        return 0;
    }
    if (!strcmp(name, "pin")) {
        static const char *const names[] = {"PEER", "TRANSPORT", "ADDRESS"};
        rc = parse_scope(argc, argv, start + 1, "path pin", NULL, 0, names, 2, 3, positionals, err, &help);
        if (rc) return rc;
        if (help) { a->command = FIPSCTL_CMD_HELP; a->help_scope = "path pin"; return 0; }
        a->command = FIPSCTL_CMD_PATH_PIN;
        a->peer = positionals[0]; a->transport = positionals[1]; a->address = positionals[2];
        return 0;
    }
    return unrecognized(err, "path", name);
}

static int parse_stats(int argc, char **argv, int start, fipsctl_args *a, FILE *err)
{
    if (start >= argc) return subcommand_required(err, "stats");
    const char *name = argv[start];
    if (is_help(name) || !strcmp(name, "help")) { a->command = FIPSCTL_CMD_HELP; a->help_scope = "stats"; return 0; }
    if (name[0] == '-') return unexpected(err, "stats", name);
    const char *simple = !strcmp(name, "list") ? "show_stats_list" : !strcmp(name, "metrics") ? "show_metrics"
                       : !strcmp(name, "peers") ? "show_stats_peers" : NULL;
    const char *positionals[1] = {0};
    bool help = false;
    if (simple) {
        int rc = parse_scope(argc, argv, start + 1, "stats", NULL, 0, NULL, 0, 0, positionals, err, &help);
        if (rc) return rc;
        if (help) { a->command = FIPSCTL_CMD_HELP; a->help_scope = "stats"; return 0; }
        a->command = FIPSCTL_CMD_QUERY; a->query = simple;
        return 0;
    }
    if (strcmp(name, "history")) return unrecognized(err, "stats", name);
    static const char *const names[] = {"METRIC"};
    a->window = "10m"; a->granularity = "1s";
    option_spec specs[] = {
        {"--peer", 0, true, &a->peer, NULL}, {"--window", 0, true, &a->window, NULL},
        {"--granularity", 0, true, &a->granularity, NULL}, {"--plot", 0, false, NULL, &a->plot},
    };
    int rc = parse_scope(argc, argv, start + 1, "stats history", specs, 4, names, 1, 1, positionals, err, &help);
    if (rc) return rc;
    if (help) { a->command = FIPSCTL_CMD_HELP; a->help_scope = "stats history"; return 0; }
    a->command = FIPSCTL_CMD_STATS_HISTORY; a->metric = positionals[0];
    return 0;
}

#ifdef FIPSCTL_PROFILING
static int parse_profile(int argc, char **argv, int start, fipsctl_args *a, FILE *err)
{
    if (start >= argc) return subcommand_required(err, "profile");
    const char *name = argv[start];
    if (is_help(name) || !strcmp(name, "help")) { a->command = FIPSCTL_CMD_HELP; a->help_scope = "profile"; return 0; }
    if (name[0] == '-') return unexpected(err, "profile", name);
    if (strcmp(name, "tick")) return unrecognized(err, "profile", name);
    if (start + 1 >= argc) return subcommand_required(err, "profile tick");
    const char *action = argv[start + 1];
    if (is_help(action) || !strcmp(action, "help")) { a->command = FIPSCTL_CMD_HELP; a->help_scope = "profile tick"; return 0; }
    if (action[0] == '-') return unexpected(err, "profile tick", action);
    const char *positionals[1] = {0};
    bool help = false;
    if (!strcmp(action, "on")) {
        option_spec specs[] = {{"--dir", 0, true, &a->dir, NULL}};
        int rc = parse_scope(argc, argv, start + 2, "profile tick on", specs, 1, NULL, 0, 0, positionals, err, &help);
        if (rc) return rc;
        if (help) { a->command = FIPSCTL_CMD_HELP; a->help_scope = "profile tick on"; return 0; }
        a->command = FIPSCTL_CMD_PROFILE_TICK_ON;
        return 0;
    }
    if (strcmp(action, "off") && strcmp(action, "status")) return unrecognized(err, "profile tick", action);
    int rc = parse_scope(argc, argv, start + 2, "profile tick", NULL, 0, NULL, 0, 0, positionals, err, &help);
    if (rc) return rc;
    if (help) { a->command = FIPSCTL_CMD_HELP; a->help_scope = "profile tick"; return 0; }
    a->command = action[1] == 'f' ? FIPSCTL_CMD_PROFILE_TICK_OFF : FIPSCTL_CMD_PROFILE_TICK_STATUS;
    return 0;
}
#endif

static bool parse_u64(const char *text, uint64_t *out)
{
    if (!text[0]) return false;
    uint64_t v = 0;
    for (const char *p = text; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        unsigned d = (unsigned)(*p - '0');
        if (v > (UINT64_MAX - d) / 10) return false;
        v = v * 10 + d;
    }
    *out = v;
    return true;
}

int fipsctl_parse(int argc, char **argv, fipsctl_args *out, FILE *err)
{
    fipsctl_args a = {0};
    a.command = FIPSCTL_CMD_HELP;
    bool help = false;
    fipsctl_command version = FIPSCTL_CMD_HELP; /* HELP doubles as "no version flag yet" */
    int i = 1;
    for (; i < argc; ++i) {
        const char *arg = argv[i];
        if (!strcmp(arg, "--")) { ++i; break; }
        if (arg[0] != '-' || !arg[1]) break;
        if (is_help(arg)) { help = true; continue; }
        if (!strcmp(arg, "-V")) { if (version == FIPSCTL_CMD_HELP) version = FIPSCTL_CMD_VERSION_SHORT; continue; }
        if (!strcmp(arg, "--version")) { if (version == FIPSCTL_CMD_HELP) version = FIPSCTL_CMD_VERSION_LONG; continue; }
        option_spec specs[] = {{"--socket", 's', true, &a.socket, NULL}};
        int m = match_option(argc, argv, &i, specs, 1, "", err);
        if (m < 0) return 1;
        if (m) continue;
        return unexpected(err, "", arg);
    }
    if (help) { a.command = FIPSCTL_CMD_HELP; a.help_scope = NULL; *out = a; return 0; }
    if (version != FIPSCTL_CMD_HELP) { a.command = version; *out = a; return 0; }
    if (i >= argc) return subcommand_required(err, "");
    const char *name = argv[i];
    int rc = 0;
    if (!strcmp(name, "help")) {
        /* `fipsctl help [show [status]]`: the help of the named scope, or the
         * top level for a scope that has none. */
        static char scope[64];
        scope[0] = 0;
        for (int j = i + 1; j < argc && j < i + 3; ++j) {
            if (strlen(scope) + strlen(argv[j]) + 2 > sizeof(scope)) break;
            if (scope[0]) strcat(scope, " ");
            strcat(scope, argv[j]);
        }
        a.command = FIPSCTL_CMD_HELP;
        a.help_scope = scope_lookup(scope)->scope;
    } else if (!strcmp(name, "show")) {
        rc = parse_query_group(argc, argv, i + 1, "show", &a, err, show_lookup);
    } else if (!strcmp(name, "acl")) {
        rc = parse_query_group(argc, argv, i + 1, "acl", &a, err, acl_lookup);
    } else if (!strcmp(name, "keygen")) {
        option_spec specs[] = {{"--dir", 'd', true, &a.dir, NULL}, {"--force", 'f', false, NULL, &a.force},
                               {"--stdout", 's', false, NULL, &a.to_stdout}};
        const char *none[1];
        rc = parse_scope(argc, argv, i + 1, "keygen", specs, 3, NULL, 0, 0, none, err, &help);
        if (!rc) { a.command = help ? FIPSCTL_CMD_HELP : FIPSCTL_CMD_KEYGEN; a.help_scope = "keygen"; }
    } else if (!strcmp(name, "address")) {
        option_spec specs[] = {{"--key", 'k', true, &a.key, NULL}};
        const char *positionals[1] = {0};
        rc = parse_scope(argc, argv, i + 1, "address", specs, 1, NULL, 0, 1, positionals, err, &help);
        if (!rc) {
            a.identity = positionals[0];
            if (help) { a.command = FIPSCTL_CMD_HELP; a.help_scope = "address"; }
            else if (a.identity && a.key)
                rc = usage_error(err, "address", "the argument '[IDENTITY]' cannot be used with '--key <KEY>'");
            else a.command = FIPSCTL_CMD_ADDRESS;
        }
    } else if (!strcmp(name, "connect")) {
        static const char *const names[] = {"PEER", "ADDRESS", "TRANSPORT"};
        const char *positionals[3] = {0};
        rc = parse_scope(argc, argv, i + 1, "connect", NULL, 0, names, 3, 3, positionals, err, &help);
        if (!rc) {
            a.command = help ? FIPSCTL_CMD_HELP : FIPSCTL_CMD_CONNECT; a.help_scope = "connect";
            a.peer = positionals[0]; a.address = positionals[1]; a.transport = positionals[2];
        }
    } else if (!strcmp(name, "disconnect")) {
        static const char *const names[] = {"PEER"};
        const char *positionals[1] = {0};
        rc = parse_scope(argc, argv, i + 1, "disconnect", NULL, 0, names, 1, 1, positionals, err, &help);
        if (!rc) {
            a.command = help ? FIPSCTL_CMD_HELP : FIPSCTL_CMD_DISCONNECT; a.help_scope = "disconnect";
            a.peer = positionals[0];
        }
    } else if (!strcmp(name, "path")) {
        rc = parse_path(argc, argv, i + 1, &a, err);
    } else if (!strcmp(name, "probe")) {
        static const char *const names[] = {"TARGET"};
        const char *positionals[1] = {0};
        const char *timeout = NULL;
        option_spec specs[] = {{"--json", 0, false, NULL, &a.json}, {"--timeout", 0, true, &timeout, NULL}};
        rc = parse_scope(argc, argv, i + 1, "probe", specs, 2, names, 1, 1, positionals, err, &help);
        if (!rc) {
            a.command = help ? FIPSCTL_CMD_HELP : FIPSCTL_CMD_PROBE; a.help_scope = "probe";
            a.peer = positionals[0];
            if (!help && timeout) {
                a.has_timeout = true;
                if (!parse_u64(timeout, &a.timeout_secs)) {
                    char *m = fipsctl_aprintf("invalid value '%s' for '--timeout <TIMEOUT>': invalid digit found in string", timeout);
                    rc = usage_error(err, "probe", m ? m : "invalid value for '--timeout <TIMEOUT>'");
                    free(m);
                }
            }
        }
    } else if (!strcmp(name, "stats")) {
        rc = parse_stats(argc, argv, i + 1, &a, err);
#ifdef FIPSCTL_PROFILING
    } else if (!strcmp(name, "profile")) {
        rc = parse_profile(argc, argv, i + 1, &a, err);
#endif
    } else {
        rc = unrecognized(err, "", name);
    }
    if (rc) return rc;
    *out = a;
    return 0;
}

/* ---- requests ----------------------------------------------------------- */

char *fipsctl_build_query(const char *command)
{
    return fipsctl_aprintf("{\"command\":\"%s\"}\n", command);
}

char *fipsctl_build_command(const char *command, json_t *params)
{
    json_t *request = json_pack("{s:s,s:o}", "command", command, "params", params ? params : json_null());
    if (!request) return NULL;
    char *body = fipsctl_json_compact(request);
    json_decref(request);
    if (!body) return NULL;
    char *line = fipsctl_aprintf("%s\n", body);
    free(body);
    return line;
}

char *fipsctl_request_for(const fipsctl_args *a, FILE *err)
{
    switch (a->command) {
    case FIPSCTL_CMD_QUERY:
        return fipsctl_build_query(a->query);
    case FIPSCTL_CMD_CONNECT: {
        char *problem = fipsctl_validate_connect_address(a->address, a->transport);
        if (problem) { fprintf(err, "error: %s\n", problem); free(problem); return NULL; }
        char *npub = fipsctl_resolve_peer(a->peer, err);
        if (!npub) return NULL;
        json_t *params = json_pack("{s:s,s:s,s:s}", "npub", npub, "address", a->address, "transport", a->transport);
        free(npub);
        return fipsctl_build_command("connect", params);
    }
    case FIPSCTL_CMD_DISCONNECT: {
        char *npub = fipsctl_resolve_peer(a->peer, err);
        if (!npub) return NULL;
        json_t *params = json_pack("{s:s}", "npub", npub);
        free(npub);
        return fipsctl_build_command("disconnect", params);
    }
    case FIPSCTL_CMD_PATH_SHOW:
    case FIPSCTL_CMD_PATH_PIN:
    case FIPSCTL_CMD_PATH_UNPIN:
        return fipsctl_path_request(a, fipsctl_default_hosts_path(), err);
    case FIPSCTL_CMD_STATS_HISTORY: {
        json_t *params = json_pack("{s:s,s:s,s:s}", "metric", a->metric, "window", a->window,
                                   "granularity", a->granularity);
        if (a->peer) {
            char *npub = fipsctl_resolve_peer(a->peer, err);
            if (!npub) { json_decref(params); return NULL; }
            json_object_set_new(params, "peer", json_string(npub));
            free(npub);
        }
        return fipsctl_build_command("show_stats_history", params);
    }
    case FIPSCTL_CMD_PROFILE_TICK_ON:
        return a->dir ? fipsctl_build_command("profile_tick_on", json_pack("{s:s}", "dir", a->dir))
                      : fipsctl_build_query("profile_tick_on");
    case FIPSCTL_CMD_PROFILE_TICK_OFF:
        return fipsctl_build_query("profile_tick_off");
    case FIPSCTL_CMD_PROFILE_TICK_STATUS:
        return fipsctl_build_query("profile_tick_status");
    default:
        return NULL;
    }
}

/* ---- driver ------------------------------------------------------------- */

int fipsctl_run(const fipsctl_args *a, FILE *out, FILE *err)
{
    switch (a->command) {
    case FIPSCTL_CMD_HELP:
        fipsctl_print_help(out, a->help_scope);
        return 0;
    case FIPSCTL_CMD_VERSION_SHORT:
    case FIPSCTL_CMD_VERSION_LONG:
        fipsctl_print_version(out, a->command == FIPSCTL_CMD_VERSION_LONG);
        return 0;
    case FIPSCTL_CMD_KEYGEN:
        return fipsctl_keygen(a->dir ? a->dir : fipsctl_default_key_dir(), a->force, a->to_stdout, out, err);
    case FIPSCTL_CMD_ADDRESS: {
        char address[FIPSCTL_ADDR_CAPACITY];
        char *error = NULL;
        if (!fipsctl_mesh_address(a->identity, a->key, fipsctl_default_hosts_path(), address, &error, err)) {
            if (error) { fprintf(err, "error: %s\n", error); free(error); }
            return 1;
        }
        fprintf(out, "%s\n", address);
        return 0;
    }
    default:
        break;
    }
    char *socket_path = a->socket ? strdup(a->socket) : fipsctl_default_socket_path();
    if (!socket_path) { fprintf(err, "error: out of memory\n"); return 1; }
    int rc = 1;
    if (a->command == FIPSCTL_CMD_PROBE) {
        char *npub = fipsctl_resolve_peer(a->peer, err);
        if (npub) {
            rc = fipsctl_run_probe(socket_path, npub, a->json, a->has_timeout, a->timeout_secs, out, err);
            free(npub);
        }
        free(socket_path);
        return rc;
    }
    char *request = fipsctl_request_for(a, err);
    if (!request) { free(socket_path); return 1; }
    char *error = NULL;
    if (a->command == FIPSCTL_CMD_STATS_HISTORY && a->plot) {
        json_t *value = fipsctl_send_request(socket_path, request, &error);
        if (value) { rc = fipsctl_print_plot(out, err, value, a->metric); json_decref(value); }
        else { fprintf(err, "error: %s\n", error ? error : "request failed"); rc = 1; }
    } else {
        /* Only `show status` composes with the IPv6 adapter's socket. */
        const char *command = a->command == FIPSCTL_CMD_QUERY && !strcmp(a->query, "show_status") ? a->query : "";
        json_t *value = fipsctl_query(socket_path, command, request, &error);
        if (value) { rc = fipsctl_print_response(out, err, value); json_decref(value); }
        else { fprintf(err, "error: %s\n", error ? error : "request failed"); rc = 1; }
    }
    free(error);
    free(request);
    free(socket_path);
    return rc;
}

int fipsctl_main(int argc, char **argv)
{
    fipsctl_args args;
    if (fipsctl_parse(argc, argv, &args, stderr)) return 1;
    int rc = fipsctl_run(&args, stdout, stderr);
    fflush(stdout);
    return rc;
}
