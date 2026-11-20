#define _DEFAULT_SOURCE 1
#include "ipv6/listening.h"
#include "ipv6/firewall_state.h"
#include <fips/identity/address.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
const char *fipsd_ipv6_proto_str(fipsd_ipv6_proto p) { return p == FIPSD_IPV6_PROTO_TCP ? "tcp" : "udp"; }
bool fipsd_ipv6_inode_map_get(const fipsd_ipv6_inode_map *map, uint64_t inode, uint32_t *pid, char comm[FIPSD_IPV6_PROCESS_NAME])
{
    if (!map) return false;
    for (size_t i = 0; i < map->count; ++i) if (map->entries[i].inode == inode) {
        if (pid) *pid = map->entries[i].pid;
        if (comm) memcpy(comm, map->entries[i].comm, FIPSD_IPV6_PROCESS_NAME);
        return true;
    }
    return false;
}
/* 32 hex digits: four 32-bit words each printed in host byte order. */
static bool parse_addr(const char *hex, uint8_t out[16])
{
    if (strlen(hex) != 32) return false;
    for (unsigned w = 0; w < 4; ++w) {
        char word[9]; memcpy(word, hex + 8 * w, 8); word[8] = 0;
        char *end; unsigned long v = strtoul(word, &end, 16);
        if (*end) return false;
        uint32_t native = (uint32_t)v; memcpy(out + 4 * w, &native, 4);
    }
    return true;
}
bool fipsd_ipv6_listening_parse_row(const char *row, uint8_t local[16], uint16_t *port, uint8_t remote[16],
                                    uint16_t *remote_port, unsigned *state, uint64_t *inode)
{
    char lhex[33], rhex[33]; unsigned lport, rport, st; unsigned long long ino;
    if (sscanf(row, "%*s %32[0-9A-Fa-f]:%x %32[0-9A-Fa-f]:%x %x %*s %*s %*s %*s %*s %llu",
               lhex, &lport, rhex, &rport, &st, &ino) != 6) return false;
    if (lport > 0xffff || rport > 0xffff || !parse_addr(lhex, local) || !parse_addr(rhex, remote)) return false;
    *port = (uint16_t)lport; *remote_port = (uint16_t)rport; *state = st; *inode = ino;
    return true;
}
static bool unspecified(const uint8_t a[16]) { for (unsigned i = 0; i < 16; ++i) if (a[i]) return false; return true; }
bool fipsd_ipv6_listening_build_entry(fipsd_ipv6_proto proto, bool is_v6, const uint8_t local[16], uint16_t port,
                                      uint64_t inode, const fipsd_ipv6_inode_map *map, const uint8_t fips0[16],
                                      fipsd_ipv6_listening_socket *out)
{
    if (!is_v6) return false; /* fips0 is IPv6-only */
    bool wildcard = unspecified(local), ours = !memcmp(local, fips0, 16);
    if (!wildcard && !ours) return false; /* ::1 or another specific address: not reachable over the mesh */
    memset(out, 0, sizeof *out);
    out->proto = proto; memcpy(out->local_addr, local, 16); out->port = port; out->wildcard_bind = wildcard;
    out->has_pid = fipsd_ipv6_inode_map_get(map, inode, &out->pid, out->process);
    return true;
}
static int compare(const void *a, const void *b)
{
    const fipsd_ipv6_listening_socket *x = a, *y = b;
    int c = strcmp(fipsd_ipv6_proto_str(x->proto), fipsd_ipv6_proto_str(y->proto));
    if (c) return c;
    if (x->port != y->port) return x->port < y->port ? -1 : 1;
    if (x->has_pid != y->has_pid) return x->has_pid ? 1 : -1;
    return x->pid < y->pid ? -1 : x->pid > y->pid;
}
void fipsd_ipv6_listening_sort(fipsd_ipv6_listening_socket *sockets, size_t count)
{ if (count > 1) qsort(sockets, count, sizeof *sockets, compare); }
static bool v4_mapped(const uint8_t a[16])
{
    static const uint8_t prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    return !memcmp(a, prefix, 12);
}
#ifdef __linux__
/* Socket inode -> (pid, comm) by walking /proc/<pid>/fd/. Best effort:
 * processes the daemon cannot read are skipped. */
static void build_inode_map(fipsd_ipv6_inode_map *map)
{
    DIR *proc = opendir("/proc");
    if (!proc) return;
    struct dirent *d;
    while ((d = readdir(proc))) {
        char *end; unsigned long pid = strtoul(d->d_name, &end, 10);
        if (*end || !pid) continue;
        char path[64], comm[FIPSD_IPV6_PROCESS_NAME] = {0};
        snprintf(path, sizeof path, "/proc/%lu/comm", pid);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (fgets(comm, sizeof comm, f)) comm[strcspn(comm, "\n")] = 0;
        fclose(f);
        snprintf(path, sizeof path, "/proc/%lu/fd", pid);
        DIR *fds = opendir(path);
        if (!fds) continue;
        struct dirent *fd;
        while ((fd = readdir(fds)) && map->count < map->capacity) {
            if (fd->d_name[0] == '.') continue;
            char target[64];
            ssize_t n = readlinkat(dirfd(fds), fd->d_name, target, sizeof target - 1);
            if (n <= 0) continue;
            target[n] = 0;
            unsigned long long inode;
            if (sscanf(target, "socket:[%llu]", &inode) != 1) continue;
            fipsd_ipv6_inode_owner *o = &map->entries[map->count++];
            o->inode = inode; o->pid = (uint32_t)pid; memcpy(o->comm, comm, sizeof o->comm);
        }
        closedir(fds);
    }
    closedir(proc);
}
static size_t scan(const char *file, fipsd_ipv6_proto proto, const fipsd_ipv6_inode_map *map, const uint8_t fips0[16],
                   fipsd_ipv6_listening_socket *out, size_t at, size_t cap)
{
    FILE *f = fopen(file, "r");
    if (!f) return at;
    char row[512];
    while (at < cap && fgets(row, sizeof row, f)) {
        uint8_t local[16], remote[16]; uint16_t port, rport; unsigned state; uint64_t inode;
        if (!fipsd_ipv6_listening_parse_row(row, local, &port, remote, &rport, &state, &inode)) continue;
        /* TCP: listeners only. UDP has no LISTEN state: a wildcard remote is a
         * listener; a connected socket carries a remote and is excluded. */
        if (proto == FIPSD_IPV6_PROTO_TCP ? state != FIPSD_IPV6_TCP_LISTEN : !unspecified(remote)) continue;
        if (fipsd_ipv6_listening_build_entry(proto, !v4_mapped(local), local, port, inode, map, fips0, &out[at])) ++at;
    }
    fclose(f);
    return at;
}
size_t fipsd_ipv6_listening_enumerate(const uint8_t fips0[16], fipsd_ipv6_listening_socket *out, size_t cap)
{
    fipsd_ipv6_inode_map map = {.capacity = 4096};
    map.entries = calloc(map.capacity, sizeof *map.entries);
    if (map.entries) build_inode_map(&map);
    size_t n = scan("/proc/net/tcp6", FIPSD_IPV6_PROTO_TCP, &map, fips0, out, 0, cap);
    n = scan("/proc/net/udp6", FIPSD_IPV6_PROTO_UDP, &map, fips0, out, n, cap);
    free(map.entries);
    fipsd_ipv6_listening_sort(out, n);
    return n;
}
#else
size_t fipsd_ipv6_listening_enumerate(const uint8_t fips0[16], fipsd_ipv6_listening_socket *out, size_t cap)
{ (void)fips0; (void)out; (void)cap; (void)v4_mapped; return 0; }
#endif
json_t *fipsd_ipv6_listening_render(const struct fipsd_ipv6_filter_classifier *classifier, const char *tun,
                                    const uint8_t fips0[16], const fipsd_ipv6_listening_socket *sockets, size_t count)
{
    /* A loaded table that does not police tun protects nothing on it: the
     * reply reports it exactly as if no table were loaded. */
    fipsd_ipv6_filter_classifier *unpoliced = fipsd_ipv6_filter_no_firewall();
    const fipsd_ipv6_filter_classifier *c = classifier && fipsd_ipv6_filter_polices(classifier, tun) ? classifier : unpoliced;
    json_t *rows = json_array();
    for (size_t i = 0; i < count && rows; ++i) {
        const fipsd_ipv6_listening_socket *s = &sockets[i];
        fips_ipv6_address a; memcpy(a.bytes, s->local_addr, 16);
        char text[46] = "::";
        if (fips_ipv6_address_format(&a, text, sizeof text) != FIPS_IDENTITY_OK) snprintf(text, sizeof text, "::");
        json_t *row = json_pack("{s:s,s:s,s:i,s:o,s:o,s:s,s:b}", "proto", fipsd_ipv6_proto_str(s->proto), "local_addr", text,
            "port", (int)s->port, "pid", s->has_pid ? json_integer((json_int_t)s->pid) : json_null(),
            "process", s->has_pid ? json_string(s->process) : json_null(),
            "filter", fipsd_ipv6_filter_state_str(fipsd_ipv6_filter_classify(c, s->proto, s->port)), "wildcard_bind", (int)s->wildcard_bind);
        if (!row || json_array_append_new(rows, row) != 0) { json_decref(row); json_decref(rows); rows = NULL; }
    }
    fips_ipv6_address me; memcpy(me.bytes, fips0, 16);
    char mine[46]; if (fips_ipv6_address_format(&me, mine, sizeof mine) != FIPS_IDENTITY_OK) snprintf(mine, sizeof mine, "::");
    json_t *reply = rows ? json_pack("{s:s,s:b,s:o}", "fips0_addr", mine, "firewall_active", (int)fipsd_ipv6_filter_is_active(c), "sockets", rows) : NULL;
    if (!reply) json_decref(rows);
    fipsd_ipv6_filter_destroy(unpoliced);
    return reply;
}
