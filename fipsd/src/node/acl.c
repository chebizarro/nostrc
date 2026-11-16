#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "node/acl.h"
#include "host_os.h"
#include "log.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct stamp { bool present; int64_t secs, nanos; } stamp;
struct fipsd_acl {
    char *paths[3];
    stamp mtimes[3];
    json_t *status;
    fips_node_addr *allow, *deny;
    size_t nallow, ndeny;
    bool stale;
    unsigned empty_holds;
};
fipsd_acl_paths fipsd_acl_default_paths(void)
{
#if defined(__APPLE__) || defined(__FreeBSD__)
    return (fipsd_acl_paths){"/usr/local/etc/fips/peers.allow", "/usr/local/etc/fips/peers.deny", "/usr/local/etc/fips/hosts"};
#else
    return (fipsd_acl_paths){"/etc/fips/peers.allow", "/etc/fips/peers.deny", "/etc/fips/hosts"};
#endif
}
static stamp mtime(const char *path)
{
    struct stat s;
    if (stat(path, &s)) return (stamp){0};
#ifdef __APPLE__
    return (stamp){true, s.st_mtimespec.tv_sec, s.st_mtimespec.tv_nsec};
#else
    return (stamp){true, s.st_mtim.tv_sec, s.st_mtim.tv_nsec};
#endif
}
static bool same(stamp a, stamp b) { return a.present == b.present && a.secs == b.secs && a.nanos == b.nanos; }
bool fipsd_acl_changed(const fipsd_acl *a)
{
    if (!a) return false;
    if (a->stale) return true;
    for (size_t i = 0; i < 3; ++i) if (!same(mtime(a->paths[i]), a->mtimes[i])) return true;
    return false;
}
bool fipsd_acl_stale(const fipsd_acl *a) { return a && a->stale; }
static json_t *empty_status(const char *allow, const char *deny)
{
    return json_pack("{s:s,s:s,s:b,s:s,s:s,s:b,s:b,s:[],s:[],s:[],s:[]}",
        "allow_file", allow, "deny_file", deny, "enforcement_active", false,
        "effective_mode", "default_open", "default_decision", "allow", "allow_all", false, "deny_all", false,
        "allow_file_entries", "deny_file_entries", "allow_entries", "deny_entries");
}
void fipsd_acl_free(fipsd_acl *a)
{
    if (!a) return;
    for (size_t i = 0; i < 3; ++i) free(a->paths[i]);
    free(a->allow); free(a->deny); json_decref(a->status); free(a);
}
fipsd_acl *fipsd_acl_clone(const fipsd_acl *old)
{
    if (!old) return NULL;
    fipsd_acl *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    a->stale = old->stale; a->empty_holds = old->empty_holds;
    for (size_t i = 0; i < 3; ++i) {
        a->paths[i] = strdup(old->paths[i]); a->mtimes[i] = old->mtimes[i];
        if (!a->paths[i]) { fipsd_acl_free(a); return NULL; }
    }
    a->nallow = old->nallow; a->ndeny = old->ndeny;
    a->allow = malloc((a->nallow + 1) * sizeof *a->allow);
    a->deny = malloc((a->ndeny + 1) * sizeof *a->deny);
    if (!a->allow || !a->deny) { fipsd_acl_free(a); return NULL; }
    if (a->nallow) memcpy(a->allow, old->allow, a->nallow * sizeof *a->allow);
    if (a->ndeny) memcpy(a->deny, old->deny, a->ndeny * sizeof *a->deny);
    a->status = json_incref(old->status);
    return a;
}
/* Missing is policy; unreadable/non-UTF8/in-flight rewrite is a fault. */
static char *read_input(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (errno == ENOENT && !mtime(path).present) return strdup("");
        return NULL;
    }
    struct stat st;
    if (fstat(fileno(f), &st) || !S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > 8 * 1024 * 1024) { fclose(f); return NULL; }
    size_t n = (size_t)st.st_size;
    char *s = malloc(n + 1);
    if (!s) { fclose(f); return NULL; }
    size_t got = fread(s, 1, n, f);
    int extra = fgetc(f);
    bool ok = got == n && extra == EOF && !ferror(f) && !memchr(s, 0, n);
    fclose(f); s[n] = 0;
    json_t *utf8 = ok ? json_stringn(s, n) : NULL;
    if (!utf8) { free(s); return NULL; }
    json_decref(utf8); return s;
}
static bool hostname(const char *s)
{
    size_t n = strlen(s);
    if (!n || n > 63 || !strncasecmp(s, "npub1", 5) || s[0] == '-' || s[n - 1] == '-') return false;
    for (size_t i = 0; i < n; ++i) if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '-')) return false;
    return true;
}
static void lower(char *s) { for (; *s; ++s) *s = (char)tolower((unsigned char)*s); }
static bool alias_put(json_t *hosts, const char *alias, const char *npub, fips_secp *secp)
{
    fips_peer_identity p;
    if (!alias || !npub || !hostname(alias) || fips_peer_identity_from_npub(secp, npub, strlen(npub), &p) != FIPS_IDENTITY_OK) return true;
    char name[64]; memcpy(name, alias, strlen(alias) + 1); lower(name);
    return json_object_set_new(hosts, name, json_string(npub)) == 0;
}
static size_t fields(char *s, char **out, size_t cap)
{
    char *save = NULL; size_t n = 0;
    for (char *v = strtok_r(s, " \t\r", &save); v; v = strtok_r(NULL, " \t\r", &save)) {
        if (n < cap) out[n] = v;
        ++n;
    }
    return n;
}
static bool sorted_add(json_t *array, const char *s)
{
    size_t n = json_array_size(array), at = 0;
    for (; at < n; ++at) {
        int c = strcmp(s, json_string_value(json_array_get(array, at)));
        if (!c) return true;
        if (c < 0) break;
    }
    return json_array_insert_new(array, at, json_string(s)) == 0;
}
static bool rules(fipsd_acl *a, char *text, bool allow, json_t *hosts, fips_secp *secp)
{
    const char *entries = allow ? "allow_entries" : "deny_entries";
    const char *tokens = allow ? "allow_file_entries" : "deny_file_entries";
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *hash = strchr(line, '#'); if (hash) *hash = 0;
        char *f[1]; size_t n = fields(line, f, 1);
        if (!n) continue;
        if (n != 1) { FIPSD_WARN("acl", "expected one ACL entry per line; skipping"); continue; }
        if (!strcasecmp(f[0], "ALL")) { if (json_object_set_new(a->status, allow ? "allow_all" : "deny_all", json_true())) return false; continue; }
        const char *npub = f[0]; fips_peer_identity peer;
        if (fips_peer_identity_from_npub(secp, npub, strlen(npub), &peer) != FIPS_IDENTITY_OK) {
            char *name = strdup(f[0]); if (!name) return false; lower(name);
            npub = json_string_value(json_object_get(hosts, name)); free(name);
            if (!npub || fips_peer_identity_from_npub(secp, npub, strlen(npub), &peer) != FIPS_IDENTITY_OK) {
                FIPSD_WARN("acl", "unknown alias or invalid npub: %s", f[0]); continue;
            }
        }
        fips_node_addr **list = allow ? &a->allow : &a->deny;
        size_t *count = allow ? &a->nallow : &a->ndeny;
        bool found = false;
        for (size_t i = 0; i < *count; ++i) if (!memcmp((*list)[i].bytes, peer.node_addr.bytes, 16)) found = true;
        if (!found) {
            fips_node_addr *p = realloc(*list, (*count + 1) * sizeof *p);
            if (!p) return false;
            *list = p; p[(*count)++] = peer.node_addr;
        }
        if (!sorted_add(json_object_get(a->status, entries), npub) || !sorted_add(json_object_get(a->status, tokens), f[0])) return false;
    }
    return true;
}
static bool active(const fipsd_acl *a) { return a && json_is_true(json_object_get(a->status, "enforcement_active")); }
fipsd_acl *fipsd_acl_load(const fipsd_config *cfg, const fipsd_acl_paths *paths, const fipsd_acl *old)
{
    fipsd_acl_paths defaults = fipsd_acl_default_paths();
    const char *p[3] = {paths ? paths->allow : old ? old->paths[0] : defaults.allow,
        paths ? paths->deny : old ? old->paths[1] : defaults.deny,
        paths ? paths->hosts : old ? old->paths[2] : defaults.hosts};
    fipsd_acl *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    for (size_t i = 0; i < 3; ++i) {
        if (!p[i] || !(a->paths[i] = strdup(p[i]))) { fipsd_acl_free(a); return NULL; }
        a->mtimes[i] = mtime(p[i]);
    }
    a->status = empty_status(p[0], p[1]);
    if (!a->status) { fipsd_acl_free(a); return NULL; }
    char *text[3] = {NULL, NULL, NULL}; bool ok = true;
    for (size_t i = 0; i < 3; ++i) {
        text[i] = read_input(p[i]);
        if (!text[i]) { if (!old || !old->stale) FIPSD_ERROR("acl", "cannot read %s; %s", p[i], old ? "holding last loaded ACL" : "starting with no ACL entries"); ok = false; }
    }
    fips_secp *secp = NULL; json_t *hosts = json_object();
    if (!hosts || fips_identity_init_secp(fipsd_host_os_entropy(), &secp) != FIPS_IDENTITY_OK) ok = false;
    const fipsd_config_value *peers = fipsd_config_get(cfg, "peers");
    for (size_t i = 0; ok && i < fipsd_config_count(peers); ++i) {
        const fipsd_config_value *v = fipsd_config_at(peers, i);
        ok = alias_put(hosts, fipsd_config_string(fipsd_config_member(v, "alias")), fipsd_config_string(fipsd_config_member(v, "npub")), secp);
    }
    if (ok) {
        char *save = NULL;
        for (char *line = strtok_r(text[2], "\n", &save); ok && line; line = strtok_r(NULL, "\n", &save)) {
            char *f[2]; size_t n = fields(line, f, 2);
            if (!n || f[0][0] == '#') continue;
            if (n != 2) { FIPSD_WARN("acl", "expected hostname npub in hosts file; skipping"); continue; }
            ok = alias_put(hosts, f[0], f[1], secp);
        }
        if (ok) ok = rules(a, text[0], true, hosts, secp) && rules(a, text[1], false, hosts, secp);
    }
    json_decref(hosts); fips_identity_destroy_secp(secp);
    for (size_t i = 0; i < 3; ++i) free(text[i]);
    if (!ok) {
        fipsd_acl_free(a);
        if (old) a = fipsd_acl_clone(old);
        else {
            a = calloc(1, sizeof *a);
            if (!a) return NULL;
            for (size_t i = 0; i < 3; ++i) if (!(a->paths[i] = strdup(p[i]))) { fipsd_acl_free(a); return NULL; }
            a->status = empty_status(p[0], p[1]);
            if (!a->status) { fipsd_acl_free(a); return NULL; }
        }
        if (a) a->stale = true;
        return a;
    }
    bool aa = json_is_true(json_object_get(a->status, "allow_all")), da = json_is_true(json_object_get(a->status, "deny_all"));
    const char *mode = aa ? "allow_all" : a->nallow && da ? "allow_then_deny_all" : a->nallow && a->ndeny ? "allow_then_deny" :
        a->nallow ? "allowlist" : da ? "deny_all" : a->ndeny ? "denylist" : "default_open";
    json_object_set_new(a->status, "effective_mode", json_string(mode));
    json_object_set_new(a->status, "default_decision", json_string(!aa && da ? "deny" : "allow"));
    json_object_set_new(a->status, "enforcement_active", json_boolean(aa || da || a->nallow || a->ndeny));
    bool acl_changed = old && (!same(a->mtimes[0], old->mtimes[0]) || !same(a->mtimes[1], old->mtimes[1]));
    if (old && !active(a) && active(old) && acl_changed && (a->mtimes[0].present || a->mtimes[1].present) && !old->empty_holds) {
        fipsd_acl_free(a); a = fipsd_acl_clone(old);
        if (a) { a->empty_holds = 1; a->stale = true; }
    }
    return a;
}
fipsd_acl_decision fipsd_acl_check(const fipsd_acl *a, const fips_peer_identity *p)
{
    if (!a) return FIPSD_ACL_DEFAULT_ALLOW;
    if (json_is_true(json_object_get(a->status, "allow_all"))) return FIPSD_ACL_ALLOW_LIST;
    for (size_t i = 0; i < a->nallow; ++i) if (!memcmp(a->allow[i].bytes, p->node_addr.bytes, 16)) return FIPSD_ACL_ALLOW_LIST;
    if (json_is_true(json_object_get(a->status, "deny_all"))) return FIPSD_ACL_DENY_LIST;
    for (size_t i = 0; i < a->ndeny; ++i) if (!memcmp(a->deny[i].bytes, p->node_addr.bytes, 16)) return FIPSD_ACL_DENY_LIST;
    return FIPSD_ACL_DEFAULT_ALLOW;
}
json_t *fipsd_acl_status(const fipsd_acl *a)
{
    fipsd_acl_paths p = fipsd_acl_default_paths();
    return a ? json_deep_copy(a->status) : empty_status(p.allow, p.deny);
}

/* Dispatcher admission seam. Policy snapshots outlive each evaluation; no
 * pointer from a config generation escapes its retained reference. */
#include "node/handlers/internal.h"
bool hs_acl_authorize(handlers *h, const fips_peer_identity *peer, const char *context)
{
    GoRefPtr cfg = fipsd_node_config(h->ctx);
    fipsd_acl_decision d = fipsd_acl_check(fipsd_config_acl(cfg.ptr), peer);
    go_refptr_release(&cfg);
    if (d != FIPSD_ACL_DENY_LIST) return true;
    ++h->counters.hs_rejected;
    METRIC(h, handshake, bad_state);
    char npub[FIPS_NIP19_CAPACITY], addr[33];
    (void)fips_peer_identity_npub(peer, npub, sizeof npub);
    fipsd_diag_hex(peer->node_addr.bytes, 16, addr);
    FIPSD_WARN("acl", "Rejected peer by ACL peer=%s npub=%s context=%s decision=denylist_match", addr, npub, context);
    hs_mark_dirty(h); return false;
}
