/* Conntrack vectors (nostrc-iq1n9.3), from upstream src/gateway/pool.rs
 * (the proc parser, the read log, the reader and its probe) and
 * src/gateway/conntrack.rs (the netlink dump). The dump messages are built
 * here byte by byte in the kernel's ctnetlink layout. */
#include "test_gateway.h"
#include <errno.h>

#define SEQ 7u

/* A proc line in the kernel's format: each tuple "src=%pI6 dst=%pI6", the
 * full uncompressed form with leading zeros. */
static const char *KERNEL_LINE = "ipv6     10 tcp      6 431999 ESTABLISHED "
    "src=fd02:0000:0000:0000:0000:0000:0000:0020 "
    "dst=fd01:0000:0000:0000:0000:0000:0000:0001 sport=45678 dport=8000 "
    "src=fd01:0000:0000:0000:0000:0000:0000:0001 "
    "dst=fd02:0000:0000:0000:0000:0000:0000:0020 sport=8000 dport=45678 "
    "[ASSURED] mark=0 use=1";
static const char *DNAT_LINE = "ipv6     10 tcp      6 431999 ESTABLISHED "
    "src=fd02:0000:0000:0000:0000:0000:0000:0020 "
    "dst=fd01:0000:0000:0000:0000:0000:0000:0005 sport=45678 dport=8000 "
    "src=fd9a:0000:0000:0000:0000:0000:0000:0002 "
    "dst=fd9a:0000:0000:0000:0000:0000:0000:0001 sport=8000 dport=45678 "
    "[ASSURED] mark=0 use=1";
static const char *UNREPLIED_LINE = "ipv6     10 udp      17 29 "
    "src=fd02:0000:0000:0000:0000:0000:0000:0020 "
    "dst=fd01:0000:0000:0000:0000:0000:0000:0005 sport=45678 dport=9 "
    "[UNREPLIED] src=fd9a:0000:0000:0000:0000:0000:0000:0002 "
    "dst=fd9a:0000:0000:0000:0000:0000:0000:0001 sport=9 dport=45678 "
    "mark=0 use=1";

static void parse(const char *text, fipsd_gw_ct_snapshot *s) { CHECK(fipsd_gw_ct_parse_proc(text, strlen(text), s) == 0); }
static uint32_t sessions(const fipsd_gw_ct_snapshot *s, const char *addr) { struct in6_addr a = gw_v6(addr); return fipsd_gw_ct_snapshot_sessions_for(s, &a); }
static bool bound(const fipsd_gw_ct_snapshot *s, const char *d, const char *r) { struct in6_addr a = gw_v6(d), b = gw_v6(r); return fipsd_gw_ct_snapshot_bound_to(s, &a, &b); }
static bool replied(const fipsd_gw_ct_snapshot *s, const char *d, const char *r) { struct in6_addr a = gw_v6(d), b = gw_v6(r); return fipsd_gw_ct_snapshot_replied_from(s, &a, &b); }

static void conntrack_parse_counts_a_kernel_format_line_for_its_virtual_ip(void)
{
    fipsd_gw_ct_snapshot s; parse(KERNEL_LINE, &s);
    CHECKM(sessions(&s, "fd01::1") == 1, "the kernel writes the uncompressed form");
    CHECK(sessions(&s, "fd01::10") == 0);
    CHECK(s.read);
    fipsd_gw_ct_snapshot_clear(&s);
}
static void conntrack_parse_counts_a_line_once_however_many_tuples_name_the_address(void)
{
    const char *line = "ipv6     10 udp      17 29 "
        "src=fd01:0000:0000:0000:0000:0000:0000:0001 dst=fd01:0000:0000:0000:0000:0000:0000:0001 sport=1 dport=2 "
        "src=fd01:0000:0000:0000:0000:0000:0000:0001 dst=fd01:0000:0000:0000:0000:0000:0000:0001 sport=2 dport=1 mark=0 use=1";
    fipsd_gw_ct_snapshot s; parse(line, &s);
    CHECK(sessions(&s, "fd01::1") == 1);
    fipsd_gw_ct_snapshot_clear(&s);
}
static void conntrack_parse_counts_each_line_that_names_the_address(void)
{
    char content[2048]; snprintf(content, sizeof content, "%s\n%s\n", KERNEL_LINE, KERNEL_LINE);
    fipsd_gw_ct_snapshot s; parse(content, &s);
    CHECK(sessions(&s, "fd01::1") == 2);
    fipsd_gw_ct_snapshot_clear(&s);
}
static void conntrack_parse_skips_a_value_that_is_not_an_ipv6_address(void)
{
    fipsd_gw_ct_snapshot s;
    parse("ipv4     2 tcp      6 431999 ESTABLISHED src=192.0.2.1 dst=192.0.2.2 sport=1 dport=2 mark=0 use=1\n", &s);
    CHECK(fipsd_gw_ct_snapshot_is_empty(&s));
    fipsd_gw_ct_snapshot_clear(&s);
}
static void conntrack_snapshot_reads_zero_for_an_address_it_did_not_see(void)
{
    fipsd_gw_ct_snapshot s; parse(KERNEL_LINE, &s);
    CHECK(sessions(&s, "fd01::1") == 1); CHECK(sessions(&s, "fd01::99") == 0);
    fipsd_gw_ct_snapshot_clear(&s);
    fipsd_gw_ct_snapshot d; fipsd_gw_ct_snapshot_init(&d);
    CHECK(fipsd_gw_ct_snapshot_is_empty(&d)); CHECK(!d.read);
}
static void conntrack_parse_records_the_reply_source_of_a_replied_dnat_line(void)
{
    fipsd_gw_ct_snapshot s; parse(DNAT_LINE, &s);
    CHECK(bound(&s, "fd01::5", "fd9a::2")); CHECK(replied(&s, "fd01::5", "fd9a::2"));
    CHECKM(!bound(&s, "fd01::5", "fd01::5"), "the original destination is not its own reply source");
    CHECKM(sessions(&s, "fd01::5") == 1, "pinning is unchanged");
    fipsd_gw_ct_snapshot_clear(&s);
}
static void conntrack_parse_ignores_the_reply_of_an_unreplied_line(void)
{
    fipsd_gw_ct_snapshot s; parse(UNREPLIED_LINE, &s);
    CHECKM(bound(&s, "fd01::5", "fd9a::2"), "the binding is recorded");
    CHECKM(!replied(&s, "fd01::5", "fd9a::2"), "but not as a reply");
    CHECK(sessions(&s, "fd01::5") == 1);
    fipsd_gw_ct_snapshot_clear(&s);
}
static void conntrack_read_log_warns_on_a_new_outcome_and_not_on_a_repeat(void)
{
    fipsd_gw_ct_read_log log = {0};
    CHECK(fipsd_gw_ct_read_log_observe(&log, ENOENT) == FIPSD_GW_READ_CHANGED);
    CHECK(fipsd_gw_ct_read_log_observe(&log, ENOENT) == FIPSD_GW_READ_REPEATED);
    CHECK(fipsd_gw_ct_read_log_observe(&log, 0) == FIPSD_GW_READ_CHANGED);
    CHECK(fipsd_gw_ct_read_log_observe(&log, 0) == FIPSD_GW_READ_REPEATED);
    CHECK(fipsd_gw_ct_read_log_observe(&log, ENOENT) == FIPSD_GW_READ_CHANGED);
    CHECKM(fipsd_gw_ct_read_log_observe(&log, EACCES) == FIPSD_GW_READ_CHANGED, "a different failure is a different outcome");
}

/* ---- the reader over fake queriers --------------------------------------- */
typedef struct fake { int error; bool one; const char *addr; int calls; } fake;
static int fake_snapshot(void *user, fipsd_gw_ct_snapshot *out)
{
    fake *f = user;
    f->calls++;
    if (f->error) return f->error;
    fipsd_gw_ct_snapshot_init_read(out);
    if (f->one) { struct in6_addr a = gw_v6(f->addr); CHECK(fipsd_gw_ct_snapshot_set_sessions(out, &a, 1) == 0); }
    return 0;
}
static fipsd_gw_ct_reader reader(fake *proc, fake *netlink)
{ fipsd_gw_ct_reader r = {{fake_snapshot, proc}, {fake_snapshot, netlink}}; return r; }
/* A netlink stand-in that must not be reached: an error no test expects. */
#define NOT_CALLED {ENOTSUP, false, NULL, 0}
static void conntrack_probe_names_the_proc_source_when_the_proc_read_succeeds(void)
{
    fake proc = {0, false, NULL, 0}, net = NOT_CALLED;
    fipsd_gw_ct_reader r = reader(&proc, &net);
    fipsd_gw_ct_probe p = fipsd_gw_ct_probe_reader(&r);
    CHECK(p.found && p.source == FIPSD_GW_CT_SOURCE_PROC && !strcmp(fipsd_gw_ct_source_name(p.source), "proc"));
    CHECK(net.calls == 0);
}
static void conntrack_probe_reports_missing_with_the_error_when_the_proc_read_fails(void)
{
    fake proc = {EACCES, false, NULL, 0}, net = NOT_CALLED;
    fipsd_gw_ct_reader r = reader(&proc, &net);
    fipsd_gw_ct_probe p = fipsd_gw_ct_probe_reader(&r);
    CHECK(!p.found && p.why.proc_errno == EACCES && !p.why.netlink_tried);
}
static void system_conntrack_falls_back_to_netlink_when_the_proc_file_is_absent(void)
{
    fake proc = {ENOENT, false, NULL, 0}, net = {0, true, "fd01::1", 0};
    fipsd_gw_ct_reader r = reader(&proc, &net);
    fipsd_gw_ct_snapshot s; fipsd_gw_ct_source source; fipsd_gw_ct_unreadable why;
    CHECKM(fipsd_gw_ct_reader_read(&r, &s, &source, &why) == 0, "the netlink dump answered");
    CHECK(source == FIPSD_GW_CT_SOURCE_NETLINK && !strcmp(fipsd_gw_ct_source_name(source), "netlink"));
    CHECK(sessions(&s, "fd01::1") == 1);
    fipsd_gw_ct_snapshot_clear(&s);
}
static void system_conntrack_does_not_fall_back_on_a_proc_error_other_than_not_found(void)
{
    fake proc = {EACCES, false, NULL, 0}, net = {0, true, "fd01::1", 0};
    fipsd_gw_ct_reader r = reader(&proc, &net);
    fipsd_gw_ct_snapshot s; fipsd_gw_ct_source source; fipsd_gw_ct_unreadable why;
    CHECKM(fipsd_gw_ct_reader_read(&r, &s, &source, &why) != 0, "a denied proc read is not a missing file");
    CHECK(why.proc_errno == EACCES && !why.netlink_tried && net.calls == 0);
    CHECK(fipsd_gw_ct_reader_snapshot(&r, &s) == EACCES);
}
static void system_conntrack_prefers_proc_when_it_reads(void)
{
    fake proc = {0, true, "fd01::1", 0}, net = {0, true, "fd01::2", 0};
    fipsd_gw_ct_reader r = reader(&proc, &net);
    fipsd_gw_ct_snapshot s; fipsd_gw_ct_source source; fipsd_gw_ct_unreadable why;
    CHECK(fipsd_gw_ct_reader_read(&r, &s, &source, &why) == 0);
    CHECK(source == FIPSD_GW_CT_SOURCE_PROC && sessions(&s, "fd01::1") == 1 && sessions(&s, "fd01::2") == 0);
    fipsd_gw_ct_snapshot_clear(&s);
}
static void conntrack_probe_reports_both_errors_when_neither_source_reads(void)
{
    fake proc = {ENOENT, false, NULL, 0}, net = {EACCES, false, NULL, 0};
    fipsd_gw_ct_reader r = reader(&proc, &net);
    fipsd_gw_ct_probe p = fipsd_gw_ct_probe_reader(&r);
    CHECK(!p.found && p.why.proc_errno == ENOENT && p.why.netlink_tried && p.why.netlink_errno == EACCES);
    fipsd_gw_ct_snapshot s;
    CHECKM(fipsd_gw_ct_reader_snapshot(&r, &s) == EACCES, "the per-tick read reports the error that decided it: the dump's");
    char text[256]; fipsd_gw_ct_unreadable_text(&p.why, text, sizeof text);
    CHECK(strstr(text, "proc: ") && strstr(text, "; netlink: "));
}
static void a_failed_logged_read_yields_the_unread_default(void)
{
    fake proc = {EACCES, false, NULL, 0}, net = NOT_CALLED;
    fipsd_gw_ct_reader r = reader(&proc, &net);
    fipsd_gw_ct_read_log log = {0};
    fipsd_gw_ct_snapshot s;
    fipsd_gw_ct_read_logged(&r, &log, &s);
    CHECK(!s.read && fipsd_gw_ct_snapshot_is_empty(&s));
    CHECK(log.observed && log.last == EACCES);
}
static void the_proc_querier_reads_a_file_and_reports_an_absent_one(void)
{
    char path[] = "/tmp/fipsgw-ct-XXXXXX";
    int fd = mkstemp(path); CHECK(fd >= 0);
    CHECK(write(fd, DNAT_LINE, strlen(DNAT_LINE)) == (ssize_t)strlen(DNAT_LINE));
    close(fd);
    fipsd_gw_ct_snapshot s;
    CHECK(fipsd_gw_ct_proc_snapshot(path, &s) == 0);
    CHECK(s.read && sessions(&s, "fd01::5") == 1);
    fipsd_gw_ct_snapshot_clear(&s);
    unlink(path);
    CHECK(fipsd_gw_ct_proc_snapshot(path, &s) == ENOENT);
}

/* ---- the netlink dump ----------------------------------------------------- */
typedef struct buf { uint8_t b[4096]; size_t len; } buf;
static void put16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
/* Open an attribute; returns its offset for close_attr. */
static size_t open_attr(buf *m, uint16_t type) { size_t at = m->len; put16(m->b + at + 2, type); m->len += 4; return at; }
static void close_attr(buf *m, size_t at) { put16(m->b + at, (uint16_t)(m->len - at)); while (m->len & 3) m->b[m->len++] = 0; }
static void attr_bytes(buf *m, uint16_t type, const void *data, size_t n)
{ size_t at = open_attr(m, type); memcpy(m->b + m->len, data, n); m->len += n; close_attr(m, at); }
static void tuple(buf *m, uint16_t which, const char *src, const char *dst, bool v4)
{
    size_t t = open_attr(m, which | FIPSD_GW_NLA_F_NESTED);
    size_t ip = open_attr(m, FIPSD_GW_CTA_TUPLE_IP | FIPSD_GW_NLA_F_NESTED);
    if (v4) {
        uint8_t a[4] = {192, 0, 2, 1}, b[4] = {192, 0, 2, 2};
        attr_bytes(m, FIPSD_GW_CTA_IP_V4_SRC, a, 4); attr_bytes(m, FIPSD_GW_CTA_IP_V4_DST, b, 4);
    } else {
        struct in6_addr s = gw_v6(src), d = gw_v6(dst);
        attr_bytes(m, FIPSD_GW_CTA_IP_V6_SRC, &s, 16); attr_bytes(m, FIPSD_GW_CTA_IP_V6_DST, &d, 16);
    }
    close_attr(m, ip); close_attr(m, t);
}
/* Start a netlink message of type with seq; returns its offset. */
static size_t msg_open(buf *m, uint16_t type, uint32_t seq)
{
    size_t at = m->len;
    memset(m->b + at, 0, FIPSD_GW_NLMSG_HDRLEN);
    put16(m->b + at + 4, type); put16(m->b + at + 6, FIPSD_GW_NLM_F_MULTI); put32(m->b + at + 8, seq);
    m->len += FIPSD_GW_NLMSG_HDRLEN;
    return at;
}
static void msg_close(buf *m, size_t at) { put32(m->b + at, (uint32_t)(m->len - at)); while (m->len & 3) m->b[m->len++] = 0; }
#define CT_NEW ((FIPSD_GW_NFNL_SUBSYS_CTNETLINK << 8) | FIPSD_GW_IPCTNL_MSG_CT_NEW)
/* A conntrack entry: orig src->dst, reply src->dst, an optional status. */
static void entry(buf *m, uint32_t seq, const char *os, const char *od, const char *rs, const char *rd, int64_t status, bool v4)
{
    size_t at = msg_open(m, CT_NEW, seq);
    m->b[m->len] = v4 ? 2 : 10; m->b[m->len + 1] = 0; m->b[m->len + 2] = 0; m->b[m->len + 3] = 0; m->len += 4; /* nfgenmsg */
    tuple(m, FIPSD_GW_CTA_TUPLE_ORIG, os, od, v4);
    if (rs) tuple(m, FIPSD_GW_CTA_TUPLE_REPLY, rs, rd, v4);
    if (status >= 0) {
        uint8_t be[4] = {(uint8_t)(status >> 24), (uint8_t)(status >> 16), (uint8_t)(status >> 8), (uint8_t)status};
        attr_bytes(m, FIPSD_GW_CTA_STATUS, be, 4);
    }
    msg_close(m, at);
}
/* The flow the gateway sees for a LAN client using a virtual IP. */
static void client_flow(buf *m, uint32_t seq, const char *vip) { entry(m, seq, "fd02::20", vip, "fd9a::1", "fd9a::2", -1, false); }
static void done(buf *m, uint32_t seq) { size_t at = msg_open(m, FIPSD_GW_NLMSG_DONE, seq); put32(m->b + m->len, 0); m->len += 4; msg_close(m, at); }
static void error_msg(buf *m, uint32_t seq, int32_t code)
{ size_t at = msg_open(m, FIPSD_GW_NLMSG_ERROR, seq); memcpy(m->b + m->len, &code, 4); m->len += 4; memset(m->b + m->len, 0, 16); m->len += 16; msg_close(m, at); }
static int count(const buf *m, fipsd_gw_ct_snapshot *s, fipsd_gw_ct_dump_state *state)
{ fipsd_gw_ct_snapshot_init_read(s); return fipsd_gw_ct_count_dump(m->b, m->len, SEQ, s, state); }

static void netlink_dump_counts_an_entry_whose_original_destination_is_the_virtual_ip(void)
{
    buf m = {.len = 0}; client_flow(&m, SEQ, "fd01::1");
    fipsd_gw_ct_snapshot s; fipsd_gw_ct_dump_state st;
    CHECK(count(&m, &s, &st) == 0 && st == FIPSD_GW_CT_DUMP_MORE);
    CHECK(sessions(&s, "fd01::1") == 1);
    CHECKM(sessions(&s, "fd9a::2") == 1, "the reply tuple's destination is counted too");
    fipsd_gw_ct_snapshot_clear(&s);
}
static void netlink_dump_counts_an_entry_once_when_both_tuples_name_the_address(void)
{
    buf m = {.len = 0}; entry(&m, SEQ, "fd01::1", "fd01::1", "fd01::1", "fd01::1", -1, false);
    fipsd_gw_ct_snapshot s; fipsd_gw_ct_dump_state st;
    CHECK(count(&m, &s, &st) == 0 && sessions(&s, "fd01::1") == 1);
    fipsd_gw_ct_snapshot_clear(&s);
}
static void netlink_dump_counts_each_entry_across_several_messages_in_one_buffer(void)
{
    buf m = {.len = 0}; client_flow(&m, SEQ, "fd01::1"); client_flow(&m, SEQ, "fd01::1"); client_flow(&m, SEQ, "fd01::2");
    fipsd_gw_ct_snapshot s; fipsd_gw_ct_dump_state st;
    CHECK(count(&m, &s, &st) == 0 && st == FIPSD_GW_CT_DUMP_MORE);
    CHECK(sessions(&s, "fd01::1") == 2 && sessions(&s, "fd01::2") == 1);
    fipsd_gw_ct_snapshot_clear(&s);
}
static void netlink_dump_ignores_an_ipv4_entry(void)
{
    buf m = {.len = 0};
    entry(&m, SEQ, NULL, NULL, NULL, NULL, -1, true);
    client_flow(&m, SEQ, "fd01::1");
    fipsd_gw_ct_snapshot s; fipsd_gw_ct_dump_state st;
    CHECK(count(&m, &s, &st) == 0 && st == FIPSD_GW_CT_DUMP_MORE);
    CHECKM(fipsd_gw_ct_snapshot_len(&s) == 2, "only the IPv6 entry's two destinations");
    fipsd_gw_ct_snapshot_clear(&s);
}
static void netlink_dump_reports_done_on_the_done_message(void)
{
    buf m = {.len = 0}; client_flow(&m, SEQ, "fd01::1"); done(&m, SEQ);
    fipsd_gw_ct_snapshot s; fipsd_gw_ct_dump_state st;
    CHECK(count(&m, &s, &st) == 0 && st == FIPSD_GW_CT_DUMP_DONE && sessions(&s, "fd01::1") == 1);
    fipsd_gw_ct_snapshot_clear(&s);
}
static void netlink_dump_turns_an_eperm_error_message_into_permission_denied(void)
{
    buf m = {.len = 0}; error_msg(&m, SEQ, -EPERM);
    fipsd_gw_ct_snapshot s; fipsd_gw_ct_dump_state st;
    CHECKM(count(&m, &s, &st) == EPERM, "an error reply must fail the read");
    fipsd_gw_ct_snapshot_clear(&s);
    buf ack = {.len = 0}; error_msg(&ack, SEQ, 0);
    CHECKM(count(&ack, &s, &st) == 0, "an acknowledgement is not an error");
    fipsd_gw_ct_snapshot_clear(&s);
}
static void netlink_dump_skips_a_message_with_another_sequence_number(void)
{
    buf m = {.len = 0}; client_flow(&m, SEQ + 1, "fd01::1"); done(&m, SEQ + 1); client_flow(&m, SEQ, "fd01::1");
    fipsd_gw_ct_snapshot s; fipsd_gw_ct_dump_state st;
    CHECK(count(&m, &s, &st) == 0);
    CHECKM(st == FIPSD_GW_CT_DUMP_MORE, "another request's end of dump does not end this one");
    CHECK(sessions(&s, "fd01::1") == 1);
    fipsd_gw_ct_snapshot_clear(&s);
}
static void netlink_dump_records_the_reply_source_only_with_seen_reply(void)
{
    buf m = {.len = 0};
    entry(&m, SEQ, "fd02::20", "fd01::5", "fd9a::2", "fd9a::1", FIPSD_GW_IPS_SEEN_REPLY | 8, false);
    entry(&m, SEQ, "fd02::20", "fd01::6", "fd9a::3", "fd9a::1", 8, false);
    fipsd_gw_ct_snapshot s; fipsd_gw_ct_dump_state st;
    CHECK(count(&m, &s, &st) == 0);
    CHECK(bound(&s, "fd01::5", "fd9a::2") && replied(&s, "fd01::5", "fd9a::2"));
    CHECKM(!bound(&s, "fd01::5", "fd01::5"), "the original destination is not its own reply source");
    CHECKM(bound(&s, "fd01::6", "fd9a::3"), "the binding is recorded");
    CHECKM(!replied(&s, "fd01::6", "fd9a::3"), "an entry without seen-reply is not a reply");
    CHECK(sessions(&s, "fd01::5") == 1 && sessions(&s, "fd01::6") == 1);
    fipsd_gw_ct_snapshot_clear(&s);
}
static void netlink_dump_rejects_a_buffer_that_does_not_parse(void)
{
    buf m = {.len = 0}; client_flow(&m, SEQ, "fd01::1"); m.len -= 4;
    fipsd_gw_ct_snapshot s; fipsd_gw_ct_dump_state st;
    CHECKM(count(&m, &s, &st) == EINVAL, "a short buffer must fail the read");
    fipsd_gw_ct_snapshot_clear(&s);
}

int gw_suite_conntrack(void)
{
    RUN(conntrack_parse_counts_a_kernel_format_line_for_its_virtual_ip);
    RUN(conntrack_parse_counts_a_line_once_however_many_tuples_name_the_address);
    RUN(conntrack_parse_counts_each_line_that_names_the_address);
    RUN(conntrack_parse_skips_a_value_that_is_not_an_ipv6_address);
    RUN(conntrack_snapshot_reads_zero_for_an_address_it_did_not_see);
    RUN(conntrack_parse_records_the_reply_source_of_a_replied_dnat_line);
    RUN(conntrack_parse_ignores_the_reply_of_an_unreplied_line);
    RUN(conntrack_read_log_warns_on_a_new_outcome_and_not_on_a_repeat);
    RUN(conntrack_probe_names_the_proc_source_when_the_proc_read_succeeds);
    RUN(conntrack_probe_reports_missing_with_the_error_when_the_proc_read_fails);
    RUN(system_conntrack_falls_back_to_netlink_when_the_proc_file_is_absent);
    RUN(system_conntrack_does_not_fall_back_on_a_proc_error_other_than_not_found);
    RUN(system_conntrack_prefers_proc_when_it_reads);
    RUN(conntrack_probe_reports_both_errors_when_neither_source_reads);
    RUN(a_failed_logged_read_yields_the_unread_default);
    RUN(the_proc_querier_reads_a_file_and_reports_an_absent_one);
    RUN(netlink_dump_counts_an_entry_whose_original_destination_is_the_virtual_ip);
    RUN(netlink_dump_counts_an_entry_once_when_both_tuples_name_the_address);
    RUN(netlink_dump_counts_each_entry_across_several_messages_in_one_buffer);
    RUN(netlink_dump_ignores_an_ipv4_entry);
    RUN(netlink_dump_reports_done_on_the_done_message);
    RUN(netlink_dump_turns_an_eperm_error_message_into_permission_denied);
    RUN(netlink_dump_skips_a_message_with_another_sequence_number);
    RUN(netlink_dump_records_the_reply_source_only_with_seen_reply);
    RUN(netlink_dump_rejects_a_buffer_that_does_not_parse);
    return 0;
}
