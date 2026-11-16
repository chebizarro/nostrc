#ifndef FIPSD_GATEWAY_CONNTRACK_H
#define FIPSD_GATEWAY_CONNTRACK_H
/* Conntrack sessions, the port of upstream src/gateway/conntrack.rs and the
 * conntrack half of src/gateway/pool.rs (nostrc-iq1n9.3).
 *
 * A snapshot is taken once per tick, so the pool does one map lookup per
 * mapping instead of scanning the table under its lock. It is read from
 * /proc/net/nf_conntrack when the kernel provides it, or, on a kernel built
 * without CONFIG_NF_CONNTRACK_PROCFS, from a dump of the IPv6 table over
 * NETLINK_NETFILTER (the request `conntrack -L` makes). Both parsers are
 * pure and portable; only the netlink socket is Linux. */
#include "gateway/addrmap.h"
#include "gateway/types.h"

typedef struct fipsd_gw_ct_snapshot {
    fipsd_gw_map sessions; /* address -> entries naming it among their destinations */
    fipsd_gw_map bindings; /* original destination | reply source -> bit 0: a reply was seen */
    bool read;             /* the result of a successful read; the default is not */
} fipsd_gw_ct_snapshot;
void fipsd_gw_ct_snapshot_init(fipsd_gw_ct_snapshot *s);      /* the default: a failed read */
void fipsd_gw_ct_snapshot_init_read(fipsd_gw_ct_snapshot *s); /* a read snapshot with no entries */
void fipsd_gw_ct_snapshot_clear(fipsd_gw_ct_snapshot *s);
int fipsd_gw_ct_snapshot_copy(fipsd_gw_ct_snapshot *dst, const fipsd_gw_ct_snapshot *src);
/* Count one more entry naming addr among its destinations; set a count. */
int fipsd_gw_ct_snapshot_add_session(fipsd_gw_ct_snapshot *s, const struct in6_addr *addr);
int fipsd_gw_ct_snapshot_set_sessions(fipsd_gw_ct_snapshot *s, const struct in6_addr *addr, uint32_t count);
int fipsd_gw_ct_snapshot_record_binding(fipsd_gw_ct_snapshot *s, const struct in6_addr *orig_dst,
                                        const struct in6_addr *reply_src, bool replied);
bool fipsd_gw_ct_snapshot_bound_to(const fipsd_gw_ct_snapshot *s, const struct in6_addr *orig_dst, const struct in6_addr *reply_src);
/* A reply from mesh_addr, which is not the virtual IP itself, was seen on a flow to virtual_ip. */
bool fipsd_gw_ct_snapshot_replied_from(const fipsd_gw_ct_snapshot *s, const struct in6_addr *virtual_ip, const struct in6_addr *mesh_addr);
uint32_t fipsd_gw_ct_snapshot_sessions_for(const fipsd_gw_ct_snapshot *s, const struct in6_addr *virtual_ip);
size_t fipsd_gw_ct_snapshot_len(const fipsd_gw_ct_snapshot *s);
bool fipsd_gw_ct_snapshot_is_empty(const fipsd_gw_ct_snapshot *s);
bool fipsd_gw_ct_snapshot_next_binding(const fipsd_gw_ct_snapshot *s, size_t *cursor, struct in6_addr *dst,
                                       struct in6_addr *src, bool *replied);

/* /proc/net/nf_conntrack: a line counts once per distinct IPv6 address among
 * its dst= fields; the first src=/dst= pair is the original tuple and the
 * second the reply tuple, whose binding is recorded, replied unless the line
 * carries [UNREPLIED]. Values that are not IPv6 addresses are skipped. */
int fipsd_gw_ct_parse_proc(const char *content, size_t len, fipsd_gw_ct_snapshot *out);

/* The netlink dump, one received buffer at a time. */
typedef enum fipsd_gw_ct_dump_state { FIPSD_GW_CT_DUMP_MORE, FIPSD_GW_CT_DUMP_DONE } fipsd_gw_ct_dump_state;
/* 0 and *state; EINVAL for a buffer that does not parse; the kernel's errno
 * for an error message carrying one. A message with another sequence number
 * is skipped. */
int fipsd_gw_ct_count_dump(const uint8_t *buf, size_t len, uint32_t seq, fipsd_gw_ct_snapshot *snapshot,
                           fipsd_gw_ct_dump_state *state);
/* The netlink and nfnetlink ABI the parser and the dump request use. */
#define FIPSD_GW_NLMSG_HDRLEN 16u
#define FIPSD_GW_NLMSG_NOOP 1u
#define FIPSD_GW_NLMSG_ERROR 2u
#define FIPSD_GW_NLMSG_DONE 3u
#define FIPSD_GW_NLM_F_REQUEST 0x01u
#define FIPSD_GW_NLM_F_MULTI 0x02u
#define FIPSD_GW_NLM_F_DUMP 0x300u
#define FIPSD_GW_NFNL_SUBSYS_CTNETLINK 1u
#define FIPSD_GW_IPCTNL_MSG_CT_NEW 0u
#define FIPSD_GW_IPCTNL_MSG_CT_GET 1u
#define FIPSD_GW_CTA_TUPLE_ORIG 1u
#define FIPSD_GW_CTA_TUPLE_REPLY 2u
#define FIPSD_GW_CTA_STATUS 3u
#define FIPSD_GW_CTA_TUPLE_IP 1u
#define FIPSD_GW_CTA_IP_V4_SRC 1u
#define FIPSD_GW_CTA_IP_V4_DST 2u
#define FIPSD_GW_CTA_IP_V6_SRC 3u
#define FIPSD_GW_CTA_IP_V6_DST 4u
#define FIPSD_GW_IPS_SEEN_REPLY 2u
#define FIPSD_GW_NLA_F_NESTED 0x8000u

/* A conntrack querier: 0 and a read snapshot in *out, else an errno. */
typedef int (*fipsd_gw_ct_querier_fn)(void *user, fipsd_gw_ct_snapshot *out);
typedef struct fipsd_gw_ct_querier { fipsd_gw_ct_querier_fn snapshot; void *user; } fipsd_gw_ct_querier;
typedef enum fipsd_gw_ct_source { FIPSD_GW_CT_SOURCE_PROC, FIPSD_GW_CT_SOURCE_NETLINK } fipsd_gw_ct_source;
const char *fipsd_gw_ct_source_name(fipsd_gw_ct_source source);
/* Why no source could be read: the proc error, and the dump's when it was tried. */
typedef struct fipsd_gw_ct_unreadable { int proc_errno; bool netlink_tried; int netlink_errno; } fipsd_gw_ct_unreadable;
/* The error that stands for the whole failed read (the dump's when tried). */
int fipsd_gw_ct_unreadable_errno(const fipsd_gw_ct_unreadable *why);
void fipsd_gw_ct_unreadable_text(const fipsd_gw_ct_unreadable *why, char *buf, size_t cap);
/* The reader the gateway uses: the proc file first; the netlink dump only
 * when the file is absent, decided on every read. */
typedef struct fipsd_gw_ct_reader { fipsd_gw_ct_querier proc, netlink; } fipsd_gw_ct_reader;
void fipsd_gw_ct_reader_system(fipsd_gw_ct_reader *reader);
int fipsd_gw_ct_reader_read(const fipsd_gw_ct_reader *reader, fipsd_gw_ct_snapshot *out, fipsd_gw_ct_source *source,
                            fipsd_gw_ct_unreadable *why);
/* The reader as a querier: 0 or the deciding errno. */
int fipsd_gw_ct_reader_snapshot(const fipsd_gw_ct_reader *reader, fipsd_gw_ct_snapshot *out);
typedef struct fipsd_gw_ct_probe { bool found; fipsd_gw_ct_source source; fipsd_gw_ct_unreadable why; } fipsd_gw_ct_probe;
fipsd_gw_ct_probe fipsd_gw_ct_probe_reader(const fipsd_gw_ct_reader *reader);
/* Warn once per change of outcome: 0 is a successful read, else the errno. */
typedef enum fipsd_gw_read_report { FIPSD_GW_READ_CHANGED, FIPSD_GW_READ_REPEATED } fipsd_gw_read_report;
typedef struct fipsd_gw_ct_read_log { bool observed; int last; } fipsd_gw_ct_read_log;
fipsd_gw_read_report fipsd_gw_ct_read_log_observe(fipsd_gw_ct_read_log *log, int outcome);
/* The system queriers. */
#define FIPSD_GW_CT_PROC_PATH "/proc/net/nf_conntrack"
int fipsd_gw_ct_proc_snapshot(void *path, fipsd_gw_ct_snapshot *out);    /* path: const char *, NULL for the default */
int fipsd_gw_ct_netlink_snapshot(void *unused, fipsd_gw_ct_snapshot *out); /* Linux; ENOSYS elsewhere */
/* The read a tick takes, logged once per change of outcome: a failed read
 * yields the unread default, so every mapping reads zero sessions. */
void fipsd_gw_ct_read_logged(const fipsd_gw_ct_reader *reader, fipsd_gw_ct_read_log *log, fipsd_gw_ct_snapshot *out);
#endif
