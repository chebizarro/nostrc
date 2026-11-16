/* Port of upstream src/hosts.rs: the host map and its mtime-driven reloader. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "dns/hosts.h"
#include "log.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define MODULE "hosts"

void fipsd_hosts_map_init(fipsd_hosts_map *m) { *m = (fipsd_hosts_map){0}; }
void fipsd_hosts_map_clear(fipsd_hosts_map *m) { free(m->entries); *m = (fipsd_hosts_map){0}; }
bool fipsd_hosts_map_copy(fipsd_hosts_map *dst, const fipsd_hosts_map *src)
{
    fipsd_hosts_map out = {0};
    if (src->count) {
        out.entries = malloc(src->count * sizeof *out.entries);
        if (!out.entries) return false;
        memcpy(out.entries, src->entries, src->count * sizeof *out.entries);
        out.count = out.capacity = src->count;
    }
    fipsd_hosts_map_clear(dst); *dst = out;
    return true;
}
void fipsd_hosts_map_move(fipsd_hosts_map *dst, fipsd_hosts_map *src)
{ fipsd_hosts_map_clear(dst); *dst = *src; *src = (fipsd_hosts_map){0}; }
size_t fipsd_hosts_map_len(const fipsd_hosts_map *m) { return m->count; }

bool fipsd_hosts_validate_hostname(const char *hostname, char *reason, size_t cap)
{
    const char *why = NULL; char invalid[32];
    size_t n = strlen(hostname);
    if (!n) why = "empty hostname";
    else if (n > 63) why = "exceeds 63 characters";
    else if (!strncasecmp(hostname, "npub1", 5)) why = "must not start with 'npub1' (ambiguous with npub resolution)";
    else if (hostname[0] == '-') why = "must not start with a hyphen";
    else if (hostname[n - 1] == '-') why = "must not end with a hyphen";
    else for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)hostname[i];
        if (!isalnum(c) && c != '-') {
            if (c < 0x80) snprintf(invalid, sizeof invalid, "invalid character '%c'", c);
            else snprintf(invalid, sizeof invalid, "invalid character '\\x%02x'", c);
            why = invalid; break;
        }
    }
    if (why && reason && cap) snprintf(reason, cap, "%s", why);
    return !why;
}
static void remove_at(fipsd_hosts_map *m, size_t i)
{ memmove(m->entries + i, m->entries + i + 1, (m->count - i - 1) * sizeof *m->entries); --m->count; }
static bool reserve(fipsd_hosts_map *m, size_t want)
{
    if (want <= m->capacity) return true;
    size_t cap = m->capacity ? m->capacity * 2 : 8;
    if (cap < want) cap = want;
    fipsd_hosts_entry *e = realloc(m->entries, cap * sizeof *e);
    if (!e) return false;
    m->entries = e; m->capacity = cap;
    return true;
}
static bool append(fipsd_hosts_map *m, const fipsd_hosts_entry *e)
{
    for (size_t i = 0; i < m->count; ++i) if (!strcmp(m->entries[i].name, e->name)) { remove_at(m, i); break; }
    if (!reserve(m, m->count + 1)) return false;
    m->entries[m->count++] = *e;
    return true;
}
int fipsd_hosts_map_insert(fipsd_hosts_map *m, const char *hostname, const char *npub, const fips_secp *secp,
                           char *reason, size_t reason_cap)
{
    char why[96];
    if (!fipsd_hosts_validate_hostname(hostname, why, sizeof why)) {
        if (reason && reason_cap) snprintf(reason, reason_cap, "invalid hostname '%s': %s", hostname, why);
        errno = EINVAL; return -1;
    }
    fips_peer_identity peer; fips_identity_status st = FIPS_IDENTITY_OK;
    if (strlen(npub) >= FIPS_NIP19_CAPACITY ||
        (st = fips_peer_identity_from_npub(secp, npub, strlen(npub), &peer)) != FIPS_IDENTITY_OK) {
        if (reason && reason_cap) snprintf(reason, reason_cap, "invalid npub '%s': %s", npub,
            strlen(npub) >= FIPS_NIP19_CAPACITY ? "InvalidNpubLength" : fips_identity_error_name(st));
        errno = EINVAL; return -1;
    }
    fipsd_hosts_entry e = {.node_addr = peer.node_addr};
    for (size_t i = 0; hostname[i]; ++i) e.name[i] = (char)tolower((unsigned char)hostname[i]);
    memcpy(e.npub, npub, strlen(npub) + 1);
    if (!append(m, &e)) { if (reason && reason_cap) snprintf(reason, reason_cap, "out of memory"); errno = ENOMEM; return -1; }
    return 0;
}
const char *fipsd_hosts_map_lookup_npub(const fipsd_hosts_map *m, const char *hostname)
{
    for (size_t i = 0; i < m->count; ++i) if (!strcasecmp(m->entries[i].name, hostname)) return m->entries[i].npub;
    return NULL;
}
const char *fipsd_hosts_map_lookup_hostname(const fipsd_hosts_map *m, const fips_node_addr *node_addr)
{
    for (size_t i = m->count; i-- > 0;)
        if (!memcmp(m->entries[i].node_addr.bytes, node_addr->bytes, sizeof node_addr->bytes)) return m->entries[i].name;
    return NULL;
}
bool fipsd_hosts_map_merge(fipsd_hosts_map *m, const fipsd_hosts_map *other)
{
    if (!reserve(m, m->count + other->count)) return false;
    for (size_t i = 0; i < other->count; ++i) if (!append(m, &other->entries[i])) return false;
    return true;
}
bool fipsd_hosts_map_from_config(fipsd_hosts_map *out, const fipsd_config *config, const fips_secp *secp, bool quiet)
{
    fipsd_hosts_map m = {0};
    const fipsd_config_value *peers = fipsd_config_get(config, "peers");
    for (size_t i = 0; i < fipsd_config_count(peers); ++i) {
        const fipsd_config_value *v = fipsd_config_at(peers, i);
        const char *alias = fipsd_config_string(fipsd_config_member(v, "alias"));
        const char *npub = fipsd_config_string(fipsd_config_member(v, "npub"));
        if (!alias || !npub) continue;
        char reason[160];
        if (fipsd_hosts_map_insert(&m, alias, npub, secp, reason, sizeof reason) != 0) {
            if (errno == ENOMEM) { fipsd_hosts_map_clear(&m); return false; }
            if (!quiet) FIPSD_WARN(MODULE, "Skipping invalid peer alias for host map: alias=%s npub=%s error=%s", alias, npub, reason);
        }
    }
    if (m.count && !quiet) FIPSD_DEBUG(MODULE, "Host map entries from peer config: count=%zu", m.count);
    fipsd_hosts_map_move(out, &m);
    return true;
}

fipsd_hosts_stamp fipsd_hosts_file_mtime(const char *path)
{
    struct stat s;
    if (stat(path, &s) != 0) return (fipsd_hosts_stamp){0};
#ifdef __APPLE__
    return (fipsd_hosts_stamp){true, s.st_mtimespec.tv_sec, s.st_mtimespec.tv_nsec};
#else
    return (fipsd_hosts_stamp){true, s.st_mtim.tv_sec, s.st_mtim.tv_nsec};
#endif
}
bool fipsd_hosts_stamp_equal(fipsd_hosts_stamp a, fipsd_hosts_stamp b)
{ return a.present == b.present && (!a.present || (a.secs == b.secs && a.nanos == b.nanos)); }

/* Whether s is valid UTF-8, as std::fs::read_to_string requires. */
static bool utf8_valid(const uint8_t *s, size_t n)
{
    for (size_t i = 0; i < n;) {
        uint8_t b = s[i]; size_t need; uint8_t lo = 0x80, hi = 0xBF;
        if (b < 0x80) { ++i; continue; }
        if (b >= 0xC2 && b <= 0xDF) need = 1;
        else if (b == 0xE0) { need = 2; lo = 0xA0; }
        else if ((b >= 0xE1 && b <= 0xEC) || b == 0xEE || b == 0xEF) need = 2;
        else if (b == 0xED) { need = 2; hi = 0x9F; }
        else if (b == 0xF0) { need = 3; lo = 0x90; }
        else if (b >= 0xF1 && b <= 0xF3) need = 3;
        else if (b == 0xF4) { need = 3; hi = 0x8F; }
        else return false;
        for (size_t k = 1; k <= need; ++k)
            if (i + k >= n || s[i + k] < (k == 1 ? lo : 0x80) || s[i + k] > (k == 1 ? hi : 0xBF)) return false;
        i += need + 1;
    }
    return true;
}
/* Read the whole file; NULL with errno on failure. */
static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 4096, n = 0; uint8_t *s = malloc(cap);
    if (!s) { fclose(f); errno = ENOMEM; return NULL; }
    for (;;) {
        if (n == cap) { uint8_t *g = realloc(s, cap * 2); if (!g) { free(s); fclose(f); errno = ENOMEM; return NULL; } s = g; cap *= 2; }
        size_t got = fread(s + n, 1, cap - n, f);
        n += got;
        if (got == 0) break;
    }
    bool err = ferror(f);
    fclose(f);
    if (err) { free(s); errno = EIO; return NULL; }
    *len = n;
    return s;
}
static bool is_space(uint8_t c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }
int fipsd_hosts_map_load_file(fipsd_hosts_map *out, const char *path, const fips_secp *secp)
{
    size_t len = 0;
    uint8_t *text = read_file(path, &len);
    if (!text) {
        int e = errno;
        if (e == ENOENT && !fipsd_hosts_file_mtime(path).present) {
            FIPSD_DEBUG(MODULE, "No hosts file found, skipping: path=%s", path);
            fipsd_hosts_map_clear(out); return 0;
        }
        errno = e; return -1;
    }
    if (!utf8_valid(text, len)) { free(text); errno = EILSEQ; return -1; }
    fipsd_hosts_map m = {0};
    size_t line_num = 0;
    for (size_t at = 0; at < len; ++line_num) {
        size_t end = at;
        while (end < len && text[end] != '\n') ++end;
        size_t next = end < len ? end + 1 : end;
        if (end > at && text[end - 1] == '\r') --end;
        /* Trim, then skip empty lines and comments. */
        size_t b = at, e = end;
        while (b < e && is_space(text[b])) ++b;
        while (e > b && is_space(text[e - 1])) --e;
        at = next;
        if (b == e || text[b] == '#') continue;
        /* Fields split on whitespace; the two expected are copied out. */
        char fields[2][256]; size_t nfields = 0;
        for (size_t p = b; p < e;) {
            while (p < e && is_space(text[p])) ++p;
            size_t q = p;
            while (q < e && !is_space(text[q])) ++q;
            if (q > p) {
                if (nfields < 2) {
                    size_t n = q - p; if (n > 255) n = 255;
                    memcpy(fields[nfields], text + p, n); fields[nfields][n] = 0;
                }
                ++nfields;
            }
            p = q;
        }
        if (nfields != 2) {
            FIPSD_WARN(MODULE, "Expected 'hostname npub', skipping: path=%s line=%zu content=%.*s", path, line_num + 1, (int)(e - b), (const char *)text + b);
            continue;
        }
        char reason[160];
        if (fipsd_hosts_map_insert(&m, fields[0], fields[1], secp, reason, sizeof reason) != 0) {
            if (errno == ENOMEM) { fipsd_hosts_map_clear(&m); free(text); errno = ENOMEM; return -1; }
            FIPSD_WARN(MODULE, "Skipping invalid hosts file entry: path=%s line=%zu error=%s", path, line_num + 1, reason);
        }
    }
    free(text);
    if (m.count) FIPSD_INFO(MODULE, "Loaded hosts file: path=%s count=%zu", path, m.count);
    fipsd_hosts_map_move(out, &m);
    return 0;
}
void fipsd_hosts_map_load(fipsd_hosts_map *out, const char *path, const fips_secp *secp)
{
    if (fipsd_hosts_map_load_file(out, path, secp) != 0) {
        FIPSD_WARN(MODULE, "Failed to read hosts file: path=%s error=%s", path, strerror(errno));
        fipsd_hosts_map_clear(out);
    }
}

static bool rebuild(fipsd_hosts_reloader *r)
{
    fipsd_hosts_map e = {0};
    if (!fipsd_hosts_map_copy(&e, &r->base) || !fipsd_hosts_map_merge(&e, &r->file)) { fipsd_hosts_map_clear(&e); return false; }
    fipsd_hosts_map_move(&r->effective, &e);
    return true;
}
bool fipsd_hosts_reloader_init(fipsd_hosts_reloader *r, fipsd_hosts_map *base, const char *path, const fips_secp *secp)
{
    *r = (fipsd_hosts_reloader){.secp = secp};
    fipsd_hosts_map_move(&r->base, base);
    r->path = strdup(path);
    if (!r->path) { fipsd_hosts_reloader_clear(r); return false; }
    /* A failed initial read records no mtime, so the next check sees a
     * change and retries rather than treating the unread file as empty for
     * the lifetime of the process. */
    if (fipsd_hosts_map_load_file(&r->file, path, secp) == 0) { r->last_mtime = fipsd_hosts_file_mtime(path); r->failing = false; }
    else {
        if (errno == ENOMEM) { fipsd_hosts_reloader_clear(r); return false; }
        FIPSD_WARN(MODULE, "Failed to read hosts file: path=%s error=%s", path, strerror(errno));
        fipsd_hosts_map_clear(&r->file); r->last_mtime = (fipsd_hosts_stamp){0}; r->failing = true;
    }
    if (!rebuild(r)) { fipsd_hosts_reloader_clear(r); return false; }
    return true;
}
void fipsd_hosts_reloader_clear(fipsd_hosts_reloader *r)
{
    fipsd_hosts_map_clear(&r->base); fipsd_hosts_map_clear(&r->file); fipsd_hosts_map_clear(&r->effective);
    free(r->path); r->path = NULL;
}
const fipsd_hosts_map *fipsd_hosts_reloader_hosts(const fipsd_hosts_reloader *r) { return &r->effective; }
const char *fipsd_hosts_reloader_path(const fipsd_hosts_reloader *r) { return r->path; }
int fipsd_hosts_reloader_try_check_reload(fipsd_hosts_reloader *r)
{
    fipsd_hosts_stamp current = fipsd_hosts_file_mtime(r->path);
    if (fipsd_hosts_stamp_equal(current, r->last_mtime)) return 0;
    fipsd_hosts_map loaded = {0};
    if (fipsd_hosts_map_load_file(&loaded, r->path, r->secp) != 0) return -1;
    r->last_mtime = current;
    fipsd_hosts_map_move(&r->file, &loaded);
    if (!rebuild(r)) { errno = ENOMEM; return -1; }
    FIPSD_INFO(MODULE, "Reloaded hosts file: path=%s entries=%zu", r->path, r->effective.count);
    return 1;
}
bool fipsd_hosts_reloader_check_reload(fipsd_hosts_reloader *r)
{
    int rc = fipsd_hosts_reloader_try_check_reload(r);
    if (rc >= 0) {
        if (rc == 1 && r->failing) { r->failing = false; FIPSD_INFO(MODULE, "Hosts file read again after a failed read: path=%s", r->path); }
        return rc == 1;
    }
    if (r->failing) FIPSD_DEBUG(MODULE, "Hosts file still unreadable: path=%s error=%s", r->path, strerror(errno));
    else FIPSD_WARN(MODULE, "Failed to read hosts file; keeping the names last read: path=%s error=%s", r->path, strerror(errno));
    r->failing = true;
    return false;
}
bool fipsd_hosts_reloader_set_base(fipsd_hosts_reloader *r, fipsd_hosts_map *base)
{
    fipsd_hosts_map_move(&r->base, base);
    return rebuild(r);
}
