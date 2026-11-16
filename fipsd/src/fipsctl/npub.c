/* Peer identifiers, mesh addresses and key files (upstream fipsctl.rs
 * resolve_peer/mesh_address/keygen, hosts.rs HostMap, config.rs key files).
 * Secrets live in buffers wiped before release. */
#include "fipsctl.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <sodium.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fips/identity/address.h>
#include <fips/identity/encoding.h>
#include <fips/identity/local.h>
#include <fips/identity/peer.h>

static void secure_fill(void *ctx, uint8_t *buf, size_t n)
{
    (void)ctx;
    if (!n) return;
    if (sodium_init() < 0) abort(); /* never hand predictable entropy to the core */
    randombytes_buf(buf, n);
}

static fips_entropy entropy(void) { return (fips_entropy){secure_fill, NULL}; }

fips_secp *fipsctl_secp(void)
{
    static fips_secp *secp;
    if (!secp && fips_identity_init_secp(entropy(), &secp) != FIPS_IDENTITY_OK) secp = NULL;
    return secp;
}

bool fipsctl_begins_as_npub(const char *text)
{
    return strlen(text) >= 5 && !strncasecmp(text, "npub1", 5);
}

const char *fipsctl_default_hosts_path(void)
{
#if defined(__APPLE__) || defined(__FreeBSD__)
    return "/usr/local/etc/fips/hosts";
#else
    return "/etc/fips/hosts";
#endif
}

const char *fipsctl_default_key_dir(void)
{
#if defined(__APPLE__) || defined(__FreeBSD__)
    return "/usr/local/etc/fips";
#else
    return "/etc/fips";
#endif
}

/* hosts.rs validate_hostname: 1-63 of [A-Za-z0-9-], no leading/trailing
 * hyphen, not beginning as an npub. */
static bool valid_hostname(const char *s)
{
    size_t n = strlen(s);
    if (!n || n > 63 || !strncasecmp(s, "npub1", 5) || s[0] == '-' || s[n - 1] == '-') return false;
    for (size_t i = 0; i < n; ++i)
        if (!isalnum((unsigned char)s[i]) && s[i] != '-') return false;
    return true;
}

static bool valid_npub(const char *npub)
{
    fips_peer_identity peer;
    fips_secp *secp = fipsctl_secp();
    return secp && fips_peer_identity_from_npub(secp, npub, strlen(npub), &peer) == FIPS_IDENTITY_OK;
}

/* The npub of `name` in the hosts file: `hostname npub` lines, `#` comments,
 * case-insensitive names, a later line replacing an earlier one, invalid
 * lines skipped. NULL when absent, unreadable or not listed. */
char *fipsctl_lookup_host(const char *hosts_path, const char *name)
{
    FILE *f = fopen(hosts_path, "r");
    if (!f) return NULL;
    char *found = NULL;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        if (!strchr(line, '\n') && !feof(f)) { /* overlong line: skip its remainder */
            int c;
            while ((c = fgetc(f)) != EOF && c != '\n') {}
            continue;
        }
        char *fields[3] = {0};
        size_t count = 0;
        for (char *p = line; *p;) {
            while (*p && isspace((unsigned char)*p)) ++p;
            if (!*p) break;
            if (count < 3) fields[count] = p;
            ++count;
            while (*p && !isspace((unsigned char)*p)) ++p;
            if (*p) *p++ = 0;
        }
        if (!count || fields[0][0] == '#' || count != 2) continue;
        if (!valid_hostname(fields[0]) || !valid_npub(fields[1])) continue;
        if (strcasecmp(fields[0], name)) continue;
        free(found);
        found = strdup(fields[1]);
    }
    fclose(f);
    return found;
}

char *fipsctl_resolve_peer_in(const char *hosts_path, const char *peer, FILE *err)
{
    if (fipsctl_begins_as_npub(peer)) return strdup(peer);
    char *npub = fipsctl_lookup_host(hosts_path, peer);
    if (npub) return npub;
    fprintf(err, "error: unknown host '%s'\n", peer);
    fprintf(err, "Not found in %s and not an npub.\n", hosts_path);
    return NULL;
}

char *fipsctl_resolve_peer(const char *peer, FILE *err)
{
    return fipsctl_resolve_peer_in(fipsctl_default_hosts_path(), peer, err);
}

static bool parse_v6(const char *text, size_t len, unsigned char out[16])
{
    if (len >= 64) return false;
    char buf[64];
    memcpy(buf, text, len);
    buf[len] = 0;
    return inet_pton(AF_INET6, buf, out) == 1;
}

static bool all_digits(const char *s, size_t len)
{
    if (!len) return false;
    for (size_t i = 0; i < len; ++i) if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

/* An IPv6 literal in fd00::/8 as a bare address, `[addr]:port` or
 * `addr:port` with a numeric tail. */
bool fipsctl_is_fips_mesh_address(const char *address)
{
    unsigned char v6[16];
    size_t len = strlen(address);
    if (parse_v6(address, len, v6)) return v6[0] == 0xfd;
    /* SocketAddrV6: `[host]:port`, the host possibly carrying a `%scope`. */
    if (address[0] == '[') {
        const char *close = strchr(address, ']');
        if (close && close[1] == ':' && all_digits(close + 2, strlen(close + 2))) {
            const char *scope = memchr(address + 1, '%', (size_t)(close - address - 1));
            size_t host_len = (size_t)((scope ? scope : close) - address - 1);
            if (parse_v6(address + 1, host_len, v6)) return v6[0] == 0xfd;
        }
    }
    const char *colon = strrchr(address, ':');
    if (colon && all_digits(colon + 1, strlen(colon + 1))) {
        const char *host = address;
        const char *end = colon;
        while (host < end && *host == '[') ++host;
        while (end > host && end[-1] == ']') --end;
        if (parse_v6(host, (size_t)(end - host), v6)) return v6[0] == 0xfd;
    }
    return false;
}

char *fipsctl_validate_connect_address(const char *address, const char *transport)
{
    bool checked = !strcmp(transport, "udp") || !strcmp(transport, "tcp") || !strcmp(transport, "ethernet");
    if (!checked || !fipsctl_is_fips_mesh_address(address)) return NULL;
    return fipsctl_aprintf("'%s' is a FIPS mesh address (fd00::/8), not a reachable %s endpoint.\n"
                           "Provide the peer's routable IP/hostname and port (e.g., '192.0.2.1:2121' or 'peer.example.com:2121').",
                           address, transport);
}

static bool format_address(const fips_ipv6_address *address, char out[FIPSCTL_ADDR_CAPACITY])
{
    return fips_ipv6_address_format(address, out, FIPSCTL_ADDR_CAPACITY) == FIPS_IDENTITY_OK;
}

static void fail(char **error, char *message)
{
    if (error) { free(*error); *error = message; } else free(message);
}

bool fipsctl_address_from_npub(const char *npub, char out[FIPSCTL_ADDR_CAPACITY], char **error)
{
    fips_peer_identity peer;
    fips_secp *secp = fipsctl_secp();
    fips_identity_status st = secp ? fips_peer_identity_from_npub(secp, npub, strlen(npub), &peer)
                                   : FIPS_IDENTITY_AllocationFailed;
    if (st != FIPS_IDENTITY_OK) { fail(error, fipsctl_aprintf("invalid npub: %s", fips_identity_error_name(st))); return false; }
    return format_address(&peer.address, out);
}

/* The file's contents are the private key in the nsec case, so the buffer is
 * wiped on every exit path. */
bool fipsctl_address_from_file(const char *path, char out[FIPSCTL_ADDR_CAPACITY], char **error)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        char *e = fipsctl_strerror(errno);
        fail(error, fipsctl_aprintf("cannot read %s: %s", path, e ? e : "?"));
        free(e);
        return false;
    }
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    int read_error = ferror(f);
    fclose(f);
    if (read_error) { sodium_memzero(buf, sizeof(buf)); fail(error, fipsctl_aprintf("cannot read %s: %s", path, "read error")); return false; }
    buf[n] = 0;
    /* trim() */
    char *start = buf;
    while (*start && isspace((unsigned char)*start)) ++start;
    char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) --end;
    *end = 0;
    bool ok = false;
    if (!*start) { fail(error, fipsctl_aprintf("key file is empty: %s", path)); goto done; }
    if (fipsctl_begins_as_npub(start)) { ok = fipsctl_address_from_npub(start, out, error); goto done; }
    fips_identity *identity = NULL;
    fips_secp *secp = fipsctl_secp();
    fips_identity_status st = secp ? fips_identity_from_secret_str(secp, entropy(), start, (size_t)(end - start), &identity)
                                   : FIPS_IDENTITY_AllocationFailed;
    if (st != FIPS_IDENTITY_OK) {
        fail(error, fipsctl_aprintf("%s does not hold a usable key: %s", path, fips_identity_error_name(st)));
        goto done;
    }
    fips_ipv6_address address = fips_identity_address(identity);
    fips_identity_destroy(identity);
    ok = format_address(&address, out);
done:
    sodium_memzero(buf, sizeof(buf));
    return ok;
}

bool fipsctl_address_from_key_dir(const char *dir, char out[FIPSCTL_ADDR_CAPACITY], char **error)
{
    char *key_path = fipsctl_aprintf("%s/fips.key", dir), *pub_path = fipsctl_aprintf("%s/fips.pub", dir);
    char *key_err = NULL, *pub_err = NULL;
    bool ok = false;
    if (!key_path || !pub_path) goto done;
    if (fipsctl_address_from_file(key_path, out, &key_err)) { ok = true; goto done; }
    if (fipsctl_address_from_file(pub_path, out, &pub_err)) { ok = true; goto done; }
    fail(error, fipsctl_aprintf("%s\n%s", key_err ? key_err : "", pub_err ? pub_err : ""));
done:
    free(key_path); free(pub_path); free(key_err); free(pub_err);
    return ok;
}

/* Precedence is the documented order: an npub or hostname, then a key file,
 * then this node's own key files. A hostname that cannot be resolved has its
 * error printed to `err` and leaves `*error` unset. */
bool fipsctl_mesh_address(const char *identity, const char *key, const char *hosts_path,
                          char out[FIPSCTL_ADDR_CAPACITY], char **error, FILE *err)
{
    if (identity) {
        char *npub = fipsctl_resolve_peer_in(hosts_path, identity, err);
        if (!npub) return false;
        bool ok = fipsctl_address_from_npub(npub, out, error);
        free(npub);
        return ok;
    }
    if (key) return fipsctl_address_from_file(key, out, error);
    return fipsctl_address_from_key_dir(fipsctl_default_key_dir(), out, error);
}

/* ---- keygen ----------------------------------------------------------------- */

static bool is_symlink(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0 && S_ISLNK(st.st_mode);
}

/* Open without following a final-component symlink; `enforce` tightens an
 * existing file to `mode` before any secret reaches it. */
static char *write_file(const char *path, const char *line, mode_t mode, bool enforce)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, mode);
    if (fd < 0) {
        int e = errno;
        if (is_symlink(path)) return fipsctl_aprintf("refusing to write key file through a symlink: %s", path);
        char *text = fipsctl_strerror(e);
        char *m = fipsctl_aprintf("failed to write key file %s: %s", path, text ? text : "?");
        free(text);
        return m;
    }
    if (enforce && fchmod(fd, mode) != 0) {
        int e = errno; close(fd);
        char *text = fipsctl_strerror(e);
        char *m = fipsctl_aprintf("failed to write key file %s: %s", path, text ? text : "?");
        free(text);
        return m;
    }
    size_t len = strlen(line), done = 0;
    while (done < len) {
        ssize_t n = write(fd, line + done, len - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            int e = errno; close(fd);
            char *text = fipsctl_strerror(e);
            char *m = fipsctl_aprintf("failed to write key file %s: %s", path, text ? text : "?");
            free(text);
            return m;
        }
        done += (size_t)n;
    }
    if (write(fd, "\n", 1) != 1) {
        int e = errno; close(fd);
        char *text = fipsctl_strerror(e);
        char *m = fipsctl_aprintf("failed to write key file %s: %s", path, text ? text : "?");
        free(text);
        return m;
    }
    close(fd);
    return NULL;
}

static bool create_dir_all(const char *dir)
{
    char *copy = strdup(dir);
    if (!copy) { errno = ENOMEM; return false; }
    size_t len = strlen(copy);
    for (size_t i = 1; i <= len; ++i) {
        if (copy[i] != '/' && copy[i] != 0) continue;
        char saved = copy[i];
        copy[i] = 0;
        if (mkdir(copy, 0777) != 0 && errno != EEXIST) { int e = errno; free(copy); errno = e; return false; }
        copy[i] = saved;
    }
    struct stat st;
    bool ok = stat(dir, &st) == 0 && S_ISDIR(st.st_mode);
    if (!ok) errno = ENOTDIR;
    free(copy);
    return ok;
}

int fipsctl_keygen(const char *dir, bool force, bool to_stdout, FILE *out, FILE *err)
{
    fips_secp *secp = fipsctl_secp();
    fips_identity *identity = NULL;
    if (!secp || fips_identity_generate(secp, entropy(), &identity) != FIPS_IDENTITY_OK) {
        fprintf(err, "error: cannot generate an identity\n");
        return 1;
    }
    uint8_t secret[32];
    char nsec[FIPS_NIP19_CAPACITY], npub[FIPS_NIP19_CAPACITY];
    fips_identity_export_secret(identity, secret);
    bool encoded = fips_encode_nsec(secp, secret, nsec, sizeof(nsec)) == FIPS_IDENTITY_OK
                && fips_identity_npub(identity, npub, sizeof(npub)) == FIPS_IDENTITY_OK;
    sodium_memzero(secret, sizeof(secret));
    fips_identity_destroy(identity);
    if (!encoded) { sodium_memzero(nsec, sizeof(nsec)); fprintf(err, "error: cannot encode the identity\n"); return 1; }
    int rc = 1;
    if (to_stdout) {
        fprintf(out, "%s\n%s\n", nsec, npub);
        rc = 0;
        goto done;
    }
    char *key_path = fipsctl_aprintf("%s/fips.key", dir), *pub_path = fipsctl_aprintf("%s/fips.pub", dir);
    if (!key_path || !pub_path) { free(key_path); free(pub_path); fprintf(err, "error: out of memory\n"); goto done; }
    struct stat st;
#if defined(__APPLE__) || defined(__FreeBSD__)
    /* The default key directory moved from /etc/fips; point at keys stranded there. */
    if (stat("/etc/fips/fips.key", &st) == 0 && stat(key_path, &st) != 0) {
        fprintf(err, "note: /etc/fips/fips.key exists but the default key directory\n");
        fprintf(err, "      is now %s; that key is no longer used by default.\n", dir);
    }
#endif
    /* lstat rather than stat: a dangling symlink at the key path must not slip past. */
    if (lstat(key_path, &st) == 0 && !force) {
        fprintf(err, "error: key file already exists: %s\nUse --force to overwrite.\n", key_path);
        goto paths;
    }
    if (!create_dir_all(dir)) {
        char *text = fipsctl_strerror(errno);
        fprintf(err, "error: cannot create directory %s: %s\n", dir, text ? text : "?");
        free(text);
        goto paths;
    }
    char *problem = write_file(key_path, nsec, 0600, true);
    if (problem) { fprintf(err, "error: failed to write key file: %s\n", problem); free(problem); goto paths; }
    /* Non-fatal: the private key is already on disk. */
    problem = write_file(pub_path, npub, 0644, false);
    if (problem) { fprintf(err, "warning: failed to write pub file: %s\n", problem); free(problem); }
    fprintf(err, "%s\n", npub);
    fprintf(err, "Key files written to: %s/\n\n", dir);
    fprintf(err, "NOTE: Set 'node.identity.persistent: true' in fips.yaml before the next daemon start.\n");
    fprintf(err, "      Without it the daemon does not use this key: it moves fips.key aside to fips.key.unused.\n");
    rc = 0;
paths:
    free(key_path); free(pub_path);
done:
    sodium_memzero(nsec, sizeof(nsec));
    return rc;
}
