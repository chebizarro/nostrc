#ifdef __linux__
#define _GNU_SOURCE 1
#endif
#include "gateway/conntrack.h"
#include "log.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __linux__
#include <linux/netlink.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#define MODULE "gateway"

void fipsd_gw_ct_snapshot_init(fipsd_gw_ct_snapshot *s)
{
    fipsd_gw_map_init(&s->sessions, 16); fipsd_gw_map_init(&s->bindings, 32); s->read = false;
}
void fipsd_gw_ct_snapshot_init_read(fipsd_gw_ct_snapshot *s) { fipsd_gw_ct_snapshot_init(s); s->read = true; }
void fipsd_gw_ct_snapshot_clear(fipsd_gw_ct_snapshot *s) { fipsd_gw_map_clear(&s->sessions); fipsd_gw_map_clear(&s->bindings); }
int fipsd_gw_ct_snapshot_copy(fipsd_gw_ct_snapshot *dst, const fipsd_gw_ct_snapshot *src)
{
    fipsd_gw_ct_snapshot_init(dst);
    dst->read = src->read;
    return fipsd_gw_map_copy(&dst->sessions, &src->sessions) == 0 && fipsd_gw_map_copy(&dst->bindings, &src->bindings) == 0 ? 0 : -1;
}
int fipsd_gw_ct_snapshot_add_session(fipsd_gw_ct_snapshot *s, const struct in6_addr *addr)
{
    uint32_t *count = fipsd_gw_map_find(&s->sessions, addr);
    if (count) { if (*count != UINT32_MAX) ++*count; return 0; }
    return fipsd_gw_map_set(&s->sessions, addr, 1);
}
int fipsd_gw_ct_snapshot_set_sessions(fipsd_gw_ct_snapshot *s, const struct in6_addr *addr, uint32_t count)
{ return fipsd_gw_map_set(&s->sessions, addr, count); }
static void pair_key(uint8_t key[32], const struct in6_addr *a, const struct in6_addr *b) { memcpy(key, a, 16); memcpy(key + 16, b, 16); }
int fipsd_gw_ct_snapshot_record_binding(fipsd_gw_ct_snapshot *s, const struct in6_addr *orig_dst,
                                        const struct in6_addr *reply_src, bool replied)
{
    uint8_t key[32]; pair_key(key, orig_dst, reply_src);
    uint32_t *flags = fipsd_gw_map_find(&s->bindings, key);
    if (flags) { if (replied) *flags |= 1; return 0; }
    return fipsd_gw_map_set(&s->bindings, key, replied ? 1 : 0);
}
bool fipsd_gw_ct_snapshot_bound_to(const fipsd_gw_ct_snapshot *s, const struct in6_addr *orig_dst, const struct in6_addr *reply_src)
{
    uint8_t key[32]; pair_key(key, orig_dst, reply_src);
    return fipsd_gw_map_contains(&s->bindings, key);
}
bool fipsd_gw_ct_snapshot_replied_from(const fipsd_gw_ct_snapshot *s, const struct in6_addr *virtual_ip, const struct in6_addr *mesh_addr)
{
    if (fipsd_gw_ip6_eq(virtual_ip, mesh_addr)) return false;
    uint8_t key[32]; pair_key(key, virtual_ip, mesh_addr);
    const uint32_t *flags = fipsd_gw_map_find(&s->bindings, key);
    return flags && (*flags & 1);
}
uint32_t fipsd_gw_ct_snapshot_sessions_for(const fipsd_gw_ct_snapshot *s, const struct in6_addr *virtual_ip)
{
    uint32_t count = 0;
    return fipsd_gw_map_get(&s->sessions, virtual_ip, &count) ? count : 0;
}
size_t fipsd_gw_ct_snapshot_len(const fipsd_gw_ct_snapshot *s) { return fipsd_gw_map_len(&s->sessions); }
bool fipsd_gw_ct_snapshot_is_empty(const fipsd_gw_ct_snapshot *s) { return fipsd_gw_map_len(&s->sessions) == 0; }
bool fipsd_gw_ct_snapshot_next_binding(const fipsd_gw_ct_snapshot *s, size_t *cursor, struct in6_addr *dst,
                                       struct in6_addr *src, bool *replied)
{
    const uint8_t *key; uint32_t *flags;
    if (!fipsd_gw_map_next(&s->bindings, cursor, &key, &flags)) return false;
    memcpy(dst, key, 16); memcpy(src, key + 16, 16); *replied = (*flags & 1) != 0;
    return true;
}

/* ---- the proc file --------------------------------------------------- */
static bool token_addr(const char *tok, size_t len, const char *prefix, struct in6_addr *out, bool *is_v6)
{
    size_t plen = strlen(prefix);
    if (len < plen || memcmp(tok, prefix, plen)) return false;
    char text[64];
    size_t vlen = len - plen;
    if (vlen >= sizeof text) { *is_v6 = false; return true; }
    memcpy(text, tok + plen, vlen); text[vlen] = 0;
    *is_v6 = fipsd_gw_ip6_parse(text, out);
    return true;
}
int fipsd_gw_ct_parse_proc(const char *content, size_t len, fipsd_gw_ct_snapshot *out)
{
    fipsd_gw_ct_snapshot_init_read(out);
    size_t at = 0;
    while (at < len) {
        size_t end = at;
        while (end < len && content[end] != '\n') ++end;
        const char *line = content + at; size_t line_len = end - at;
        at = end + 1;
        struct in6_addr seen[4]; size_t nseen = 0;
        struct in6_addr dst_first; bool has_dst_first = false; size_t ndst = 0;
        struct in6_addr src_second; bool has_src_second = false; size_t nsrc = 0;
        bool unreplied = false;
        size_t i = 0;
        while (i < line_len) {
            while (i < line_len && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) ++i;
            size_t start = i;
            while (i < line_len && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') ++i;
            if (i == start) continue;
            const char *tok = line + start; size_t tlen = i - start;
            struct in6_addr addr; bool v6;
            if (token_addr(tok, tlen, "dst=", &addr, &v6)) {
                if (v6) {
                    bool dup = false;
                    for (size_t k = 0; k < nseen; ++k) if (fipsd_gw_ip6_eq(&seen[k], &addr)) dup = true;
                    if (!dup && nseen < 4) seen[nseen++] = addr;
                    if (ndst == 0) { dst_first = addr; has_dst_first = true; }
                }
                ++ndst;
            } else if (token_addr(tok, tlen, "src=", &addr, &v6)) {
                if (nsrc == 1 && v6) { src_second = addr; has_src_second = true; }
                ++nsrc;
            } else if (tlen == 11 && !memcmp(tok, "[UNREPLIED]", 11)) unreplied = true;
        }
        for (size_t k = 0; k < nseen; ++k) if (fipsd_gw_ct_snapshot_add_session(out, &seen[k]) != 0) return -1;
        if (has_dst_first && has_src_second &&
            fipsd_gw_ct_snapshot_record_binding(out, &dst_first, &src_second, !unreplied) != 0) return -1;
    }
    return 0;
}

/* ---- the netlink dump ------------------------------------------------ */
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
typedef struct entry { struct in6_addr seen[4]; size_t nseen; bool has_orig_dst, has_reply_src, replied; struct in6_addr orig_dst, reply_src; } entry;
static void see(entry *e, const struct in6_addr *addr)
{
    for (size_t k = 0; k < e->nseen; ++k) if (fipsd_gw_ip6_eq(&e->seen[k], addr)) return;
    if (e->nseen < 4) e->seen[e->nseen++] = *addr;
}
static void walk_ip(const uint8_t *p, size_t len, bool original, entry *e)
{
    size_t at = 0;
    while (at + 4 <= len) {
        uint16_t alen = rd16(p + at), type = rd16(p + at + 2) & 0x3fff;
        if (alen < 4 || at + alen > len) return;
        if (alen == 20) {
            struct in6_addr addr; memcpy(&addr, p + at + 4, 16);
            if (type == FIPSD_GW_CTA_IP_V6_DST) { see(e, &addr); if (original) { e->orig_dst = addr; e->has_orig_dst = true; } }
            else if (type == FIPSD_GW_CTA_IP_V6_SRC && !original) { e->reply_src = addr; e->has_reply_src = true; }
        }
        at += ((size_t)alen + 3) & ~(size_t)3;
    }
}
static void walk_tuple(const uint8_t *p, size_t len, bool original, entry *e)
{
    size_t at = 0;
    while (at + 4 <= len) {
        uint16_t alen = rd16(p + at), type = rd16(p + at + 2) & 0x3fff;
        if (alen < 4 || at + alen > len) return;
        if (type == FIPSD_GW_CTA_TUPLE_IP) walk_ip(p + at + 4, alen - 4, original, e);
        at += ((size_t)alen + 3) & ~(size_t)3;
    }
}
static int count_entry(const uint8_t *attrs, size_t len, fipsd_gw_ct_snapshot *snapshot)
{
    entry e; memset(&e, 0, sizeof e);
    size_t at = 0;
    while (at + 4 <= len) {
        uint16_t alen = rd16(attrs + at), type = rd16(attrs + at + 2) & 0x3fff;
        if (alen < 4 || at + alen > len) break;
        const uint8_t *payload = attrs + at + 4; size_t plen = alen - 4;
        if (type == FIPSD_GW_CTA_TUPLE_ORIG) walk_tuple(payload, plen, true, &e);
        else if (type == FIPSD_GW_CTA_TUPLE_REPLY) walk_tuple(payload, plen, false, &e);
        else if (type == FIPSD_GW_CTA_STATUS && plen >= 4) {
            uint32_t status = ((uint32_t)payload[0] << 24) | ((uint32_t)payload[1] << 16) | ((uint32_t)payload[2] << 8) | payload[3];
            e.replied = (status & FIPSD_GW_IPS_SEEN_REPLY) != 0;
        }
        at += ((size_t)alen + 3) & ~(size_t)3;
    }
    for (size_t k = 0; k < e.nseen; ++k) if (fipsd_gw_ct_snapshot_add_session(snapshot, &e.seen[k]) != 0) return -1;
    if (e.has_orig_dst && e.has_reply_src && fipsd_gw_ct_snapshot_record_binding(snapshot, &e.orig_dst, &e.reply_src, e.replied) != 0) return -1;
    return 0;
}
int fipsd_gw_ct_count_dump(const uint8_t *buf, size_t len, uint32_t seq, fipsd_gw_ct_snapshot *snapshot,
                           fipsd_gw_ct_dump_state *state)
{
    size_t offset = 0;
    *state = FIPSD_GW_CT_DUMP_MORE;
    while (offset < len) {
        if (len - offset < FIPSD_GW_NLMSG_HDRLEN) return EINVAL;
        uint32_t mlen = rd32(buf + offset); uint16_t type = rd16(buf + offset + 4);
        uint32_t mseq = rd32(buf + offset + 8);
        if (mlen < FIPSD_GW_NLMSG_HDRLEN || mlen > len - offset) return EINVAL;
        const uint8_t *payload = buf + offset + FIPSD_GW_NLMSG_HDRLEN; size_t plen = mlen - FIPSD_GW_NLMSG_HDRLEN;
        /* Messages are padded to four bytes; the length is at least a header, so the walk advances. */
        size_t aligned = ((size_t)mlen + 3) & ~(size_t)3;
        offset += aligned > len - offset ? len - offset : aligned;
        if (mseq != seq) continue;
        if (type == FIPSD_GW_NLMSG_DONE) { *state = FIPSD_GW_CT_DUMP_DONE; return 0; }
        if (type == FIPSD_GW_NLMSG_ERROR) {
            if (plen < 4) return EINVAL;
            int32_t code; memcpy(&code, payload, 4);
            if (code != 0) return code < 0 ? -code : code;
            continue;
        }
        if (type == ((FIPSD_GW_NFNL_SUBSYS_CTNETLINK << 8) | FIPSD_GW_IPCTNL_MSG_CT_NEW)) {
            if (plen < 4) return EINVAL; /* nfgenmsg */
            if (count_entry(payload + 4, plen - 4, snapshot) != 0) return ENOMEM;
        }
    }
    return 0;
}

/* ---- the readers ----------------------------------------------------- */
const char *fipsd_gw_ct_source_name(fipsd_gw_ct_source source) { return source == FIPSD_GW_CT_SOURCE_PROC ? "proc" : "netlink"; }
int fipsd_gw_ct_unreadable_errno(const fipsd_gw_ct_unreadable *why) { return why->netlink_tried ? why->netlink_errno : why->proc_errno; }
void fipsd_gw_ct_unreadable_text(const fipsd_gw_ct_unreadable *why, char *buf, size_t cap)
{
    if (why->netlink_tried) snprintf(buf, cap, "proc: %s; netlink: %s", strerror(why->proc_errno), strerror(why->netlink_errno));
    else snprintf(buf, cap, "%s", strerror(why->proc_errno));
}
void fipsd_gw_ct_reader_system(fipsd_gw_ct_reader *reader)
{
    reader->proc = (fipsd_gw_ct_querier){fipsd_gw_ct_proc_snapshot, NULL};
    reader->netlink = (fipsd_gw_ct_querier){fipsd_gw_ct_netlink_snapshot, NULL};
}
int fipsd_gw_ct_reader_read(const fipsd_gw_ct_reader *reader, fipsd_gw_ct_snapshot *out, fipsd_gw_ct_source *source,
                            fipsd_gw_ct_unreadable *why)
{
    memset(why, 0, sizeof *why);
    int rc = reader->proc.snapshot(reader->proc.user, out);
    if (rc == 0) { *source = FIPSD_GW_CT_SOURCE_PROC; return 0; }
    why->proc_errno = rc;
    if (rc != ENOENT) return -1;
    why->netlink_tried = true;
    rc = reader->netlink.snapshot(reader->netlink.user, out);
    if (rc == 0) { *source = FIPSD_GW_CT_SOURCE_NETLINK; return 0; }
    why->netlink_errno = rc;
    return -1;
}
int fipsd_gw_ct_reader_snapshot(const fipsd_gw_ct_reader *reader, fipsd_gw_ct_snapshot *out)
{
    fipsd_gw_ct_source source; fipsd_gw_ct_unreadable why;
    if (fipsd_gw_ct_reader_read(reader, out, &source, &why) == 0) return 0;
    return fipsd_gw_ct_unreadable_errno(&why);
}
fipsd_gw_ct_probe fipsd_gw_ct_probe_reader(const fipsd_gw_ct_reader *reader)
{
    fipsd_gw_ct_probe probe; memset(&probe, 0, sizeof probe);
    fipsd_gw_ct_snapshot snapshot; fipsd_gw_ct_snapshot_init(&snapshot);
    probe.found = fipsd_gw_ct_reader_read(reader, &snapshot, &probe.source, &probe.why) == 0;
    fipsd_gw_ct_snapshot_clear(&snapshot);
    return probe;
}
fipsd_gw_read_report fipsd_gw_ct_read_log_observe(fipsd_gw_ct_read_log *log, int outcome)
{
    fipsd_gw_read_report report = log->observed && log->last == outcome ? FIPSD_GW_READ_REPEATED : FIPSD_GW_READ_CHANGED;
    log->observed = true; log->last = outcome;
    return report;
}
int fipsd_gw_ct_proc_snapshot(void *path, fipsd_gw_ct_snapshot *out)
{
    const char *file = path ? (const char *)path : FIPSD_GW_CT_PROC_PATH;
    FILE *f = fopen(file, "r");
    if (!f) return errno ? errno : EIO;
    size_t cap = 65536, len = 0; char *content = malloc(cap);
    if (!content) { fclose(f); return ENOMEM; }
    for (;;) {
        if (len == cap) { char *grown = realloc(content, cap * 2); if (!grown) { free(content); fclose(f); return ENOMEM; } content = grown; cap *= 2; }
        size_t n = fread(content + len, 1, cap - len, f);
        len += n;
        if (n == 0) break;
    }
    int err = ferror(f) ? EIO : 0;
    fclose(f);
    if (!err && fipsd_gw_ct_parse_proc(content, len, out) != 0) err = ENOMEM;
    free(content);
    if (err) fipsd_gw_ct_snapshot_clear(out);
    return err;
}
#ifdef __linux__
static _Atomic uint32_t next_seq = 1;
/* Longest wait for each part of the kernel's reply: the read runs on a
 * blocking thread once per tick, so a silent kernel must not hold it longer. */
#define READ_TIMEOUT_SECS 2
int fipsd_gw_ct_netlink_snapshot(void *unused, fipsd_gw_ct_snapshot *out)
{
    (void)unused;
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_NETFILTER);
    if (fd < 0) return errno;
    struct sockaddr_nl local; memset(&local, 0, sizeof local); local.nl_family = AF_NETLINK;
    struct timeval tv = {READ_TIMEOUT_SECS, 0};
    if (bind(fd, (struct sockaddr *)&local, sizeof local) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) != 0) { int e = errno; close(fd); return e; }
    uint32_t seq = atomic_fetch_add(&next_seq, 1);
    uint8_t request[20]; memset(request, 0, sizeof request);
    uint32_t rlen = sizeof request; uint16_t type = (FIPSD_GW_NFNL_SUBSYS_CTNETLINK << 8) | FIPSD_GW_IPCTNL_MSG_CT_GET;
    uint16_t flags = FIPSD_GW_NLM_F_REQUEST | FIPSD_GW_NLM_F_DUMP;
    memcpy(request, &rlen, 4); memcpy(request + 4, &type, 2); memcpy(request + 6, &flags, 2); memcpy(request + 8, &seq, 4);
    request[16] = AF_INET6; /* nfgenmsg: family, version 0, res_id 0 */
    struct sockaddr_nl kernel; memset(&kernel, 0, sizeof kernel); kernel.nl_family = AF_NETLINK;
    if (sendto(fd, request, sizeof request, 0, (struct sockaddr *)&kernel, sizeof kernel) != (ssize_t)sizeof request) { int e = errno; close(fd); return e; }
    fipsd_gw_ct_snapshot_init_read(out);
    size_t cap = 65536; uint8_t *buf = malloc(cap);
    if (!buf) { close(fd); fipsd_gw_ct_snapshot_clear(out); return ENOMEM; }
    int err = 0;
    for (;;) {
        /* Sized by peeking first, so a large batch is not truncated. */
        ssize_t need = recv(fd, buf, cap, MSG_PEEK | MSG_TRUNC);
        if (need < 0) { err = errno == EAGAIN || errno == EWOULDBLOCK ? ETIMEDOUT : errno; break; }
        if ((size_t)need > cap) {
            uint8_t *grown = realloc(buf, (size_t)need);
            if (!grown) { err = ENOMEM; break; }
            buf = grown; cap = (size_t)need;
        }
        ssize_t got = recv(fd, buf, cap, 0);
        if (got < 0) { err = errno == EAGAIN || errno == EWOULDBLOCK ? ETIMEDOUT : errno; break; }
        fipsd_gw_ct_dump_state state;
        err = fipsd_gw_ct_count_dump(buf, (size_t)got, seq, out, &state);
        if (err || state == FIPSD_GW_CT_DUMP_DONE) break;
    }
    free(buf); close(fd);
    if (err) fipsd_gw_ct_snapshot_clear(out);
    return err;
}
#else
int fipsd_gw_ct_netlink_snapshot(void *unused, fipsd_gw_ct_snapshot *out) { (void)unused; (void)out; return ENOSYS; }
#endif
void fipsd_gw_ct_read_logged(const fipsd_gw_ct_reader *reader, fipsd_gw_ct_read_log *log, fipsd_gw_ct_snapshot *out)
{
    int rc = fipsd_gw_ct_reader_snapshot(reader, out);
    if (rc == 0) { (void)fipsd_gw_ct_read_log_observe(log, 0); return; }
    fipsd_gw_ct_snapshot_init(out);
    if (fipsd_gw_ct_read_log_observe(log, rc) == FIPSD_GW_READ_CHANGED)
        FIPSD_WARN(MODULE, "Conntrack unreadable; every mapping reads zero sessions: error=%s", strerror(rc));
    else FIPSD_DEBUG(MODULE, "Conntrack still unreadable; every mapping reads zero sessions: error=%s", strerror(rc));
}
