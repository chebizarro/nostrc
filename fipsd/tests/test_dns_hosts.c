/* Host map and reloader tests from upstream src/hosts.rs and the hosts-file
 * cases of src/ipv6tun/tests/dns.rs. File changes are made with explicit
 * modification times, never sleeps. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "dns/hosts.h"
#include "dns/resolver.h"
#include "host_os.h"
#include "log.h"
#include "test_tmpdir.h"
#include <fips/identity/local.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s (errno %d %s)\n", __FILE__, __LINE__, #x, errno, strerror(errno)); abort(); } } while (0)

static fips_secp *secp;
static const char *dir;
/* A fresh identity's npub. */
static void fresh_npub(char out[FIPS_NIP19_CAPACITY], fips_node_addr *addr)
{
    fips_identity *id;
    CHECK(fips_identity_generate(secp, fipsd_host_os_entropy(), &id) == FIPS_IDENTITY_OK);
    CHECK(fips_identity_npub(id, out, FIPS_NIP19_CAPACITY) == FIPS_IDENTITY_OK);
    if (addr) *addr = fips_identity_node_addr(id);
    fips_identity_destroy(id);
}
static void write_file(const char *path, const void *content, size_t len)
{
    FILE *f = fopen(path, "wb"); CHECK(f);
    CHECK(fwrite(content, 1, len, f) == len); CHECK(fclose(f) == 0);
}
static void write_text(const char *path, const char *text) { write_file(path, text, strlen(text)); }
/* Set a file's modification time to secs after the epoch. */
static void set_mtime(const char *path, time_t secs)
{
    struct timespec times[2] = {{0, UTIME_OMIT}, {secs, 0}};
    CHECK(utimensat(AT_FDCWD, path, times, 0) == 0);
}
static int insert(fipsd_hosts_map *m, const char *name, const char *npub)
{ char reason[160]; return fipsd_hosts_map_insert(m, name, npub, secp, reason, sizeof reason); }

/* ---- capture sink: count lines by substring ---------------------------- */
typedef struct capture { char lines[64][FIPSD_LOG_LINE_MAX]; size_t count; } capture;
static capture logs;
static int capture_write(void *ctx, const char *line, size_t len)
{ capture *c = ctx; if (c->count < 64) { snprintf(c->lines[c->count++], FIPSD_LOG_LINE_MAX, "%.*s", (int)len, line); } return 0; }
static size_t lines_with(const char *text, const char **first)
{
    size_t n = 0;
    for (size_t i = 0; i < logs.count; ++i) if (strstr(logs.lines[i], text)) { if (!n && first) *first = logs.lines[i]; ++n; }
    return n;
}
static bool level_is(const char *line, const char *level)
{ const char *sp = strchr(line, ' '); return sp && !strncmp(sp + 1, level, strlen(level)); }

/* Whether the reloader, with its map as check_reload leaves it, resolves <name>.fips. */
static bool resolves(fipsd_hosts_reloader *r, const char *name)
{
    (void)fipsd_hosts_reloader_check_reload(r);
    fips_peer_identity peer;
    return fipsd_dns_resolve_label(name, fipsd_hosts_reloader_hosts(r), secp, &peer);
}
/* A hosts file with gateway mapped and a modification time a minute ago, and
 * the reloader that read it. */
static time_t loaded_hosts(const char *path, fipsd_hosts_reloader *r)
{
    char npub[FIPS_NIP19_CAPACITY]; fresh_npub(npub, NULL);
    char line[128]; snprintf(line, sizeof line, "gateway %s\n", npub); write_text(path, line);
    time_t at = time(NULL) - 60; set_mtime(path, at);
    fipsd_hosts_map base; fipsd_hosts_map_init(&base);
    CHECK(fipsd_hosts_reloader_init(r, &base, path, secp));
    return at;
}

int main(void)
{
    fipsd_log_sink sink = {capture_write, NULL, &logs};
    fipsd_log *log = fipsd_log_create(FIPSD_LOG_DEBUG, &sink); CHECK(log);
    fipsd_log_set_default(log);
    dir = fipsd_test_tmpdir(NULL);
    CHECK(fips_identity_init_secp(fipsd_host_os_entropy(), &secp) == FIPS_IDENTITY_OK);
    char path[1024]; snprintf(path, sizeof path, "%s/hosts", dir);
    char n1[FIPS_NIP19_CAPACITY], n2[FIPS_NIP19_CAPACITY]; fips_node_addr a1;
    fresh_npub(n1, &a1); fresh_npub(n2, NULL);

    /* default path follows the packaging layout */
#if defined(__APPLE__) || defined(__FreeBSD__)
    CHECK(!strcmp(FIPSD_HOSTS_DEFAULT_PATH, "/usr/local/etc/fips/hosts"));
#else
    CHECK(!strcmp(FIPSD_HOSTS_DEFAULT_PATH, "/etc/fips/hosts"));
#endif
    /* validate_hostname */
    { char x63[64]; memset(x63, 'x', 63); x63[63] = 0; char x64[65]; memset(x64, 'x', 64); x64[64] = 0;
      const char *valid[] = {"gateway", "core-vm", "a", "node1", "my-peer-2", "A", "GATEWAY", "a1b2c3", x63};
      for (size_t i = 0; i < 9; ++i) CHECK(fipsd_hosts_validate_hostname(valid[i], NULL, 0));
      const char *invalid[] = {"", "-starts", "ends-", "has space", "has.dot", "has_underscore", x64, "npub1foo", "NPUB1bar"};
      for (size_t i = 0; i < 9; ++i) { char why[96]; CHECK(!fipsd_hosts_validate_hostname(invalid[i], why, sizeof why) && why[0]); } }
    /* insert and lookup */
    { fipsd_hosts_map m; fipsd_hosts_map_init(&m);
      CHECK(insert(&m, "gateway", n1) == 0);
      CHECK(!strcmp(fipsd_hosts_map_lookup_npub(&m, "gateway"), n1) && !strcmp(fipsd_hosts_map_lookup_npub(&m, "GATEWAY"), n1));
      CHECK(!strcmp(fipsd_hosts_map_lookup_npub(&m, "Gateway"), n1) && !strcmp(fipsd_hosts_map_lookup_hostname(&m, &a1), "gateway"));
      CHECK(fipsd_hosts_map_len(&m) == 1);
      CHECK(insert(&m, "", n1) != 0 && errno == EINVAL && fipsd_hosts_map_len(&m) == 1);
      CHECK(insert(&m, "gw2", "not-an-npub") != 0 && errno == EINVAL && fipsd_hosts_map_len(&m) == 1);
      CHECK(insert(&m, "gateway", n2) == 0 && !strcmp(fipsd_hosts_map_lookup_npub(&m, "gateway"), n2) && fipsd_hosts_map_len(&m) == 1);
      CHECK(!fipsd_hosts_map_lookup_npub(&m, "nonexistent"));
      fipsd_hosts_map_clear(&m); }
    /* from_config: with alias, without, invalid alias skipped */
    { char yaml[512]; fipsd_hosts_map m; fipsd_hosts_map_init(&m);
      snprintf(yaml, sizeof yaml, "peers: [{npub: '%s', alias: 'core', addresses: [{transport: udp, addr: '127.0.0.1:9999'}]}]", n1);
      fipsd_config *c = fipsd_config_parse(yaml, strlen(yaml), NULL); CHECK(c);
      CHECK(fipsd_hosts_map_from_config(&m, c, secp, false) && !strcmp(fipsd_hosts_map_lookup_npub(&m, "core"), n1)); fipsd_config_free(c);
      snprintf(yaml, sizeof yaml, "peers: [{npub: '%s', addresses: [{transport: udp, addr: '127.0.0.1:9999'}]}]", n1);
      c = fipsd_config_parse(yaml, strlen(yaml), NULL); CHECK(c);
      CHECK(fipsd_hosts_map_from_config(&m, c, secp, false) && fipsd_hosts_map_len(&m) == 0); fipsd_config_free(c);
      snprintf(yaml, sizeof yaml, "peers: [{npub: '%s', alias: 'has space', addresses: [{transport: udp, addr: '127.0.0.1:9999'}]}]", n1);
      c = fipsd_config_parse(yaml, strlen(yaml), NULL); CHECK(c);
      CHECK(fipsd_hosts_map_from_config(&m, c, secp, false) && fipsd_hosts_map_len(&m) == 0); fipsd_config_free(c);
      CHECK(lines_with("Skipping invalid peer alias for host map", NULL) == 1);
      fipsd_hosts_map_clear(&m); }
    /* load_file: not found, valid, bad lines skipped, whitespace */
    { fipsd_hosts_map m; fipsd_hosts_map_init(&m); char text[512];
      CHECK(fipsd_hosts_map_load_file(&m, "/nonexistent/path/hosts", secp) == 0 && fipsd_hosts_map_len(&m) == 0);
      snprintf(text, sizeof text, "# A comment\ngateway   %s\n\n# Another comment\ncore-vm   %s\n", n1, n2); write_text(path, text);
      CHECK(fipsd_hosts_map_load_file(&m, path, secp) == 0 && fipsd_hosts_map_len(&m) == 2);
      CHECK(!strcmp(fipsd_hosts_map_lookup_npub(&m, "gateway"), n1) && !strcmp(fipsd_hosts_map_lookup_npub(&m, "core-vm"), n2));
      snprintf(text, sizeof text, "gateway   %s\nbad_host   %s\ntoo many fields here\ngood-host   %s\n", n1, n1, n1); write_text(path, text);
      CHECK(fipsd_hosts_map_load_file(&m, path, secp) == 0 && fipsd_hosts_map_len(&m) == 2);
      CHECK(fipsd_hosts_map_lookup_npub(&m, "gateway") && fipsd_hosts_map_lookup_npub(&m, "good-host"));
      snprintf(text, sizeof text, "  # indented comment  \n\t gateway \t %s \t \n\n\t  \n", n1); write_text(path, text);
      CHECK(fipsd_hosts_map_load_file(&m, path, secp) == 0 && fipsd_hosts_map_len(&m) == 1 && fipsd_hosts_map_lookup_npub(&m, "gateway"));
      /* non-UTF-8 content is a read failure, as read_to_string's is */
      write_file(path, "# caf\xe9\n", 7);
      CHECK(fipsd_hosts_map_load_file(&m, path, secp) != 0 && errno == EILSEQ);
      fipsd_hosts_map_clear(&m); }
    /* merge */
    { fipsd_hosts_map m1, m2; fipsd_hosts_map_init(&m1); fipsd_hosts_map_init(&m2);
      CHECK(insert(&m1, "alpha", n1) == 0 && insert(&m2, "beta", n2) == 0 && fipsd_hosts_map_merge(&m1, &m2));
      CHECK(fipsd_hosts_map_len(&m1) == 2 && fipsd_hosts_map_lookup_npub(&m1, "alpha") && fipsd_hosts_map_lookup_npub(&m1, "beta"));
      fipsd_hosts_map_clear(&m1); fipsd_hosts_map_clear(&m2); fipsd_hosts_map_init(&m1); fipsd_hosts_map_init(&m2);
      CHECK(insert(&m1, "gateway", n1) == 0 && insert(&m2, "gateway", n2) == 0 && fipsd_hosts_map_merge(&m1, &m2));
      CHECK(fipsd_hosts_map_len(&m1) == 1 && !strcmp(fipsd_hosts_map_lookup_npub(&m1, "gateway"), n2));
      fipsd_hosts_map_clear(&m1); fipsd_hosts_map_clear(&m2); }
    /* reloader: initial load, no file, change, deletion, creation, base preserved */
    { fipsd_hosts_reloader r; fipsd_hosts_map base; char text[256];
      fipsd_hosts_map_init(&base); CHECK(insert(&base, "core", n1) == 0);
      snprintf(text, sizeof text, "gateway   %s\n", n2); write_text(path, text);
      CHECK(fipsd_hosts_reloader_init(&r, &base, path, secp));
      CHECK(fipsd_hosts_map_len(fipsd_hosts_reloader_hosts(&r)) == 2);
      CHECK(fipsd_hosts_map_lookup_npub(fipsd_hosts_reloader_hosts(&r), "core") && fipsd_hosts_map_lookup_npub(fipsd_hosts_reloader_hosts(&r), "gateway"));
      fipsd_hosts_reloader_clear(&r);
      fipsd_hosts_map_init(&base); CHECK(insert(&base, "core", n1) == 0);
      CHECK(fipsd_hosts_reloader_init(&r, &base, "/nonexistent/hosts", secp));
      CHECK(fipsd_hosts_map_len(fipsd_hosts_reloader_hosts(&r)) == 1 && fipsd_hosts_map_lookup_npub(fipsd_hosts_reloader_hosts(&r), "core"));
      fipsd_hosts_reloader_clear(&r);
      /* detects a change (explicit mtime, no sleep) */
      snprintf(text, sizeof text, "gateway   %s\n", n1); write_text(path, text); set_mtime(path, time(NULL) - 100);
      fipsd_hosts_map_init(&base); CHECK(fipsd_hosts_reloader_init(&r, &base, path, secp));
      CHECK(fipsd_hosts_map_len(fipsd_hosts_reloader_hosts(&r)) == 1 && !strcmp(fipsd_hosts_map_lookup_npub(fipsd_hosts_reloader_hosts(&r), "gateway"), n1));
      CHECK(!fipsd_hosts_reloader_check_reload(&r));
      snprintf(text, sizeof text, "gateway   %s\nnew-host   %s\n", n1, n2); write_text(path, text); set_mtime(path, time(NULL) - 50);
      CHECK(fipsd_hosts_reloader_check_reload(&r) && fipsd_hosts_map_len(fipsd_hosts_reloader_hosts(&r)) == 2);
      CHECK(fipsd_hosts_map_lookup_npub(fipsd_hosts_reloader_hosts(&r), "new-host"));
      /* detects deletion */
      CHECK(unlink(path) == 0);
      CHECK(fipsd_hosts_reloader_check_reload(&r) && fipsd_hosts_map_len(fipsd_hosts_reloader_hosts(&r)) == 0);
      /* detects creation */
      snprintf(text, sizeof text, "gateway   %s\n", n1); write_text(path, text);
      CHECK(fipsd_hosts_reloader_check_reload(&r) && fipsd_hosts_map_len(fipsd_hosts_reloader_hosts(&r)) == 1);
      fipsd_hosts_reloader_clear(&r);
      /* preserves the base on reload */
      fipsd_hosts_map_init(&base); CHECK(insert(&base, "core", n1) == 0);
      snprintf(text, sizeof text, "gateway   %s\n", n2); write_text(path, text);
      CHECK(fipsd_hosts_reloader_init(&r, &base, path, secp) && fipsd_hosts_map_len(fipsd_hosts_reloader_hosts(&r)) == 2);
      CHECK(unlink(path) == 0);
      CHECK(fipsd_hosts_reloader_check_reload(&r) && fipsd_hosts_map_len(fipsd_hosts_reloader_hosts(&r)) == 1);
      CHECK(fipsd_hosts_map_lookup_npub(fipsd_hosts_reloader_hosts(&r), "core") && !fipsd_hosts_map_lookup_npub(fipsd_hosts_reloader_hosts(&r), "gateway"));
      fipsd_hosts_reloader_clear(&r); }
    /* set_base replaces peer aliases and keeps the last reloaded hosts file on top */
    { char x[FIPS_NIP19_CAPACITY], y[FIPS_NIP19_CAPACITY], z[FIPS_NIP19_CAPACITY], v[FIPS_NIP19_CAPACITY], w[FIPS_NIP19_CAPACITY], u[FIPS_NIP19_CAPACITY], t[FIPS_NIP19_CAPACITY];
      fips_node_addr x_addr; fresh_npub(x, &x_addr); fresh_npub(y, NULL); fresh_npub(z, NULL); fresh_npub(v, NULL); fresh_npub(w, NULL); fresh_npub(u, NULL); fresh_npub(t, NULL);
      char text[256]; fipsd_hosts_reloader r; fipsd_hosts_map base;
      snprintf(text, sizeof text, "f %s\na %s\n", y, z); write_text(path, text); set_mtime(path, time(NULL) - 300);
      fipsd_hosts_map_init(&base); CHECK(insert(&base, "a", x) == 0);
      CHECK(fipsd_hosts_reloader_init(&r, &base, path, secp));
      const fipsd_hosts_map *h = fipsd_hosts_reloader_hosts(&r);
      CHECK(!strcmp(fipsd_hosts_map_lookup_npub(h, "f"), y));
      snprintf(text, sizeof text, "g %s\na %s\n", v, z); write_text(path, text); set_mtime(path, time(NULL) - 200);
      CHECK(fipsd_hosts_reloader_check_reload(&r));
      fipsd_hosts_map_init(&base); CHECK(insert(&base, "b", w) == 0); CHECK(fipsd_hosts_reloader_set_base(&r, &base));
      CHECK(!strcmp(fipsd_hosts_map_lookup_npub(h, "b"), w) && !strcmp(fipsd_hosts_map_lookup_npub(h, "g"), v));
      CHECK(!strcmp(fipsd_hosts_map_lookup_npub(h, "a"), z) && !fipsd_hosts_map_lookup_npub(h, "f"));
      CHECK(!fipsd_hosts_map_lookup_hostname(h, &x_addr));
      snprintf(text, sizeof text, "h %s\na %s\n", u, z); write_text(path, text); set_mtime(path, time(NULL) - 100);
      CHECK(fipsd_hosts_reloader_try_check_reload(&r) == 1);
      fipsd_hosts_map_init(&base); CHECK(insert(&base, "c", t) == 0); CHECK(fipsd_hosts_reloader_set_base(&r, &base));
      CHECK(!strcmp(fipsd_hosts_map_lookup_npub(h, "c"), t) && !strcmp(fipsd_hosts_map_lookup_npub(h, "h"), u) && !strcmp(fipsd_hosts_map_lookup_npub(h, "a"), z));
      CHECK(!fipsd_hosts_map_lookup_npub(h, "g") && !fipsd_hosts_map_lookup_npub(h, "f") && !fipsd_hosts_map_lookup_npub(h, "b"));
      fipsd_hosts_reloader_clear(&r); }
    /* a hosts file that becomes unreadable keeps its last names resolving */
    { fipsd_hosts_reloader r; time_t at = loaded_hosts(path, &r);
      CHECK(resolves(&r, "gateway"));
      write_file(path, "# caf\xe9\ngateway npub1notread\n", 28); set_mtime(path, at + 10);
      CHECK(resolves(&r, "gateway"));
      fipsd_hosts_reloader_clear(&r); }
    /* ... and keeps its override of a peer alias */
    { char alias[FIPS_NIP19_CAPACITY], file[FIPS_NIP19_CAPACITY]; fresh_npub(alias, NULL); fresh_npub(file, NULL);
      char text[128]; snprintf(text, sizeof text, "gateway %s\n", file); write_text(path, text);
      time_t at = time(NULL) - 60; set_mtime(path, at);
      fipsd_hosts_map base; fipsd_hosts_map_init(&base); CHECK(insert(&base, "gateway", alias) == 0);
      fipsd_hosts_reloader r; CHECK(fipsd_hosts_reloader_init(&r, &base, path, secp));
      CHECK(resolves(&r, "gateway") && !strcmp(fipsd_hosts_map_lookup_npub(fipsd_hosts_reloader_hosts(&r), "gateway"), file));
      write_file(path, "# caf\xe9\ngateway npub1notread\n", 28); set_mtime(path, at + 10);
      CHECK(resolves(&r, "gateway") && !strcmp(fipsd_hosts_map_lookup_npub(fipsd_hosts_reloader_hosts(&r), "gateway"), file));
      fipsd_hosts_reloader_clear(&r); }
    /* a failed read is retried though the mtime did not change */
    { fipsd_hosts_reloader r; time_t at = loaded_hosts(path, &r);
      CHECK(resolves(&r, "gateway"));
      time_t failed = at + 10;
      write_file(path, "\xff\xfe not text\n", 12); set_mtime(path, failed);
      (void)fipsd_hosts_reloader_check_reload(&r);
      char later[FIPS_NIP19_CAPACITY]; fresh_npub(later, NULL);
      char text[128]; snprintf(text, sizeof text, "later %s\n", later); write_text(path, text); set_mtime(path, failed);
      CHECK(resolves(&r, "later") && !resolves(&r, "gateway"));
      fipsd_hosts_reloader_clear(&r); }
    /* a hosts file removed after a good load drops its names */
    { fipsd_hosts_reloader r; (void)loaded_hosts(path, &r);
      CHECK(resolves(&r, "gateway")); CHECK(unlink(path) == 0); CHECK(!resolves(&r, "gateway"));
      fipsd_hosts_reloader_clear(&r); }
    /* a run of failed reads warns once and its end is logged */
    { logs.count = 0; fipsd_hosts_reloader r; time_t at = loaded_hosts(path, &r); time_t failed = at + 10;
      write_file(path, "\xff\xfe not text\n", 12); set_mtime(path, failed);
      for (int i = 0; i < 3; ++i) (void)fipsd_hosts_reloader_check_reload(&r);
      const char *first = NULL;
      CHECK(lines_with("Failed to read hosts file; keeping the names last read", &first) == 1 && level_is(first, "WARN"));
      CHECK(lines_with("Hosts file read again after a failed read", NULL) == 0);
      write_text(path, "# readable\n"); set_mtime(path, failed);
      (void)fipsd_hosts_reloader_check_reload(&r); (void)fipsd_hosts_reloader_check_reload(&r);
      CHECK(lines_with("Hosts file read again after a failed read", &first) == 1 && level_is(first, "INFO"));
      fipsd_hosts_reloader_clear(&r); }
    /* a failed run is not ended by the mtime returning to the last good read */
    { logs.count = 0; fipsd_hosts_reloader r; time_t at = loaded_hosts(path, &r);
      write_file(path, "\xff\xfe not text\n", 12); set_mtime(path, at + 10);
      CHECK(!fipsd_hosts_reloader_check_reload(&r));
      set_mtime(path, at); CHECK(!fipsd_hosts_reloader_check_reload(&r));
      CHECK(lines_with("Hosts file read again after a failed read", NULL) == 0);
      set_mtime(path, at + 20); CHECK(!fipsd_hosts_reloader_check_reload(&r));
      CHECK(lines_with("Failed to read hosts file; keeping the names last read", NULL) == 1);
      fipsd_hosts_reloader_clear(&r); }
    (void)unlink(path);
    fips_identity_destroy_secp(secp);
    fipsd_log_set_default(NULL); fipsd_log_destroy(log);
    puts("dns hosts: validation, map, config aliases, file loading, merge, reloader, failed-read runs: ok");
    return 0;
}
