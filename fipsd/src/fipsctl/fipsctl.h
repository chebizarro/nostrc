#ifndef FIPSCTL_H
#define FIPSCTL_H
/* fipsctl: the control client for fipsd (upstream src/bin/fipsctl.rs), in
 * plain POSIX C over jansson and libfips-core. No libgo, no fipsd_lib. Every
 * function here is linkable from the unit test; main() only calls
 * fipsctl_main(). Exit codes follow docs/reference/cli-fipsctl.md: 0 for an
 * ok reply, 1 for everything else. */
#include <jansson.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <fips/identity/mod.h>

typedef enum fipsctl_command {
    FIPSCTL_CMD_HELP = 0,
    FIPSCTL_CMD_VERSION_SHORT,
    FIPSCTL_CMD_VERSION_LONG,
    FIPSCTL_CMD_QUERY, /* show *, acl show, stats list/metrics/peers: args.query */
    FIPSCTL_CMD_KEYGEN,
    FIPSCTL_CMD_ADDRESS,
    FIPSCTL_CMD_CONNECT,
    FIPSCTL_CMD_DISCONNECT,
    FIPSCTL_CMD_PATH_SHOW,
    FIPSCTL_CMD_PATH_PIN,
    FIPSCTL_CMD_PATH_UNPIN,
    FIPSCTL_CMD_PROBE,
    FIPSCTL_CMD_STATS_HISTORY,
    FIPSCTL_CMD_PROFILE_TICK_ON,
    FIPSCTL_CMD_PROFILE_TICK_OFF,
    FIPSCTL_CMD_PROFILE_TICK_STATUS
} fipsctl_command;

/* Parsed argv. Strings point into argv or at string literals; nothing to free. */
typedef struct fipsctl_args {
    fipsctl_command command;
    const char *socket;          /* -s/--socket, NULL for the default */
    const char *help_scope;      /* subcommand whose help was asked for, NULL for the top level */
    const char *query;           /* control command for FIPSCTL_CMD_QUERY */
    const char *dir;             /* keygen -d, profile tick on --dir */
    bool force, to_stdout;       /* keygen */
    const char *identity, *key;  /* address */
    const char *peer;            /* connect, disconnect, path *, stats history --peer */
    const char *address;         /* connect, path pin */
    const char *transport;       /* connect, path pin */
    bool json;                   /* probe */
    bool has_timeout;            /* probe --timeout */
    uint64_t timeout_secs;
    const char *metric, *window, *granularity; /* stats history */
    bool plot;
} fipsctl_args;

/* commands.c */
int fipsctl_parse(int argc, char **argv, fipsctl_args *out, FILE *err); /* 0, or 1 after a usage error */
void fipsctl_print_help(FILE *out, const char *scope);
const char *fipsctl_short_version(void);
void fipsctl_print_version(FILE *out, bool long_form);
char *fipsctl_build_query(const char *command);
char *fipsctl_build_command(const char *command, json_t *params); /* consumes params */
char *fipsctl_request_for(const fipsctl_args *a, FILE *err); /* NULL after printing an error */
int fipsctl_run(const fipsctl_args *a, FILE *out, FILE *err);
int fipsctl_main(int argc, char **argv);

/* client.c */
char *fipsctl_default_socket_path(void);
#if defined(_WIN32)
/* Windows: the control "socket" is a TCP port on 127.0.0.1 (Rust u16 parse). */
bool fipsctl_parse_port(const char *text, uint16_t *port);
#endif
char *fipsctl_ipv6tun_socket(const char *control);
char *fipsctl_ipv6tun_client(const char *control);
json_t *fipsctl_send_request(const char *socket_path, const char *request, char **error);
bool fipsctl_is_ok(const json_t *reply);
bool fipsctl_composes(const char *command, const json_t *node_data);
void fipsctl_compose(const char *command, json_t *node_data, const json_t *adapter_data);
json_t *fipsctl_query(const char *socket_path, const char *command, const char *request, char **error);

/* render.c */
/* Jansson integers are int64 where upstream's serde_json carries u64: an
 * integer token above INT64_MAX in a reply (u64::MAX sentinels occur) is
 * carried as a string tagged FIPSCTL_U64_TAG and printed back as the number. */
#define FIPSCTL_U64_TAG "\x01u64:"
json_t *fipsctl_json_load(const char *text, size_t len, json_error_t *error);
char *fipsctl_json_pretty(const json_t *value);  /* serde_json::to_string_pretty, lexical keys */
char *fipsctl_json_compact(const json_t *value); /* serde_json::to_string, lexical keys */
int fipsctl_print_response(FILE *out, FILE *err, const json_t *value);
int fipsctl_print_plot(FILE *out, FILE *err, const json_t *value, const char *metric);
char *fipsctl_sparkline(const double *values, size_t count, double min, double max);
char *fipsctl_format_f64(double value); /* ryu's pretty shortest representation */
char *fipsctl_aprintf(const char *fmt, ...);
char *fipsctl_strerror(int err); /* "<strerror> (os error <n>)", as std::io::Error prints */
size_t fipsctl_utf8_chars(const char *text);
char *fipsctl_take_chars(const char *text, size_t count);

/* npub.c */
fips_secp *fipsctl_secp(void);
bool fipsctl_begins_as_npub(const char *text);
const char *fipsctl_default_hosts_path(void);
const char *fipsctl_default_key_dir(void);
char *fipsctl_lookup_host(const char *hosts_path, const char *name);
char *fipsctl_resolve_peer_in(const char *hosts_path, const char *peer, FILE *err);
char *fipsctl_resolve_peer(const char *peer, FILE *err);
bool fipsctl_is_fips_mesh_address(const char *address);
char *fipsctl_validate_connect_address(const char *address, const char *transport); /* NULL when fine */
#define FIPSCTL_ADDR_CAPACITY 48
bool fipsctl_address_from_npub(const char *npub, char out[FIPSCTL_ADDR_CAPACITY], char **error);
bool fipsctl_address_from_file(const char *path, char out[FIPSCTL_ADDR_CAPACITY], char **error);
bool fipsctl_address_from_key_dir(const char *dir, char out[FIPSCTL_ADDR_CAPACITY], char **error);
bool fipsctl_mesh_address(const char *identity, const char *key, const char *hosts_path,
                          char out[FIPSCTL_ADDR_CAPACITY], char **error, FILE *err);
int fipsctl_keygen(const char *dir, bool force, bool to_stdout, FILE *out, FILE *err);

/* path.c */
char *fipsctl_path_request(const fipsctl_args *a, const char *hosts_path, FILE *err);

/* probe.c */
#define FIPSCTL_SPIN_HERE "\x01"
#define FIPSCTL_LIVE_WIDTH 78
typedef struct fipsctl_stage_row {
    const char *name;
    char *verdict, *reason, *text;
    bool has_elapsed;
    uint64_t elapsed_ms;
    bool settled, hidden;
    char **subs;
    size_t sub_count;
} fipsctl_stage_row;
typedef struct fipsctl_probe_progress {
    bool tty;
    size_t drawn, printed, frame;
    bool header;
    FILE *out;
} fipsctl_probe_progress;
uint64_t fipsctl_probe_ceiling_ms(uint64_t budget_ms, bool has_timeout, uint64_t timeout_secs);
size_t fipsctl_stage_rows(const json_t *report, fipsctl_stage_row **rows);
void fipsctl_stage_rows_free(fipsctl_stage_row *rows, size_t count);
char *fipsctl_stage_line(const fipsctl_stage_row *row, const char *spin);
size_t fipsctl_block_lines(const fipsctl_stage_row *rows, size_t count, const char *spin, char ***lines);
void fipsctl_lines_free(char **lines, size_t count);
char *fipsctl_render_coords(const json_t *coords, const char *marker);
char *fipsctl_walk_line(const json_t *path, const char *lca, bool tty);
char *fipsctl_clip(const char *line, size_t width);
const char *fipsctl_overall_note(const json_t *report);
void fipsctl_probe_header(FILE *out, const json_t *report);
void fipsctl_probe_stages(FILE *out, const json_t *report);
void fipsctl_probe_detail(FILE *out, const json_t *report, bool tty);
size_t fipsctl_settled_since(fipsctl_probe_progress *p, const fipsctl_stage_row *rows, size_t count, char ***lines);
void fipsctl_probe_update(fipsctl_probe_progress *p, const json_t *report);
void fipsctl_probe_finish(fipsctl_probe_progress *p, const json_t *report);
int fipsctl_run_probe(const char *socket_path, const char *npub, bool json, bool has_timeout,
                      uint64_t timeout_secs, FILE *out, FILE *err);
#endif
