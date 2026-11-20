/* Port of upstream src/dnsmsg.rs: the screen and the reply builder. */
#include "dns/msg.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define FLAG_QR 0x8000u
#define FLAG_OPCODE 0x7800u
#define FLAG_RD 0x0100u
#define FLAG_CD 0x0010u
/* Flags a reply copies from its query: the opcode, RD and CD. */
#define COPIED_FLAGS (FLAG_OPCODE | FLAG_RD | FLAG_CD)
/* Flags a caller may set: AA and RA. Every other bit, Z and TC included, stays clear. */
#define CALLER_FLAGS (FIPSD_DNS_AA | FIPSD_DNS_RA)
/* Source ports never answered: no resolver sends from them, and they are the
 * UDP services that answer arbitrary input (echo, daytime, quote of the day,
 * chargen, time), so a reply to one could start an endless exchange. Port 0
 * is not a valid source port. */
static const uint16_t service_ports[] = {0, 7, 13, 17, 19, 37};

uint32_t fipsd_dns_clamp_ttl(uint32_t ttl) { return ttl > FIPSD_DNS_MAX_TTL ? FIPSD_DNS_MAX_TTL : ttl; }
uint8_t fipsd_dns_rcode_from_header(uint8_t header_byte3) { return header_byte3 & 0x0F; }
const char *fipsd_dns_rcode_name(uint8_t rcode, char *buf, size_t cap)
{
    static const char *const names[] = {"NOERROR", "FORMERR", "SERVFAIL", "NXDOMAIN", "NOTIMP", "REFUSED",
                                        "YXDOMAIN", "YXRRSET", "NXRRSET", "NOTAUTH", "NOTZONE"};
    if (rcode < sizeof names / sizeof names[0]) return names[rcode];
    snprintf(buf, cap, "RCODE%u", (unsigned)rcode);
    return buf;
}
const char *fipsd_dns_drop_reason_str(fipsd_dns_drop_reason reason)
{
    switch (reason) {
    case FIPSD_DNS_DROP_SHORT: return "short";
    case FIPSD_DNS_DROP_FILLS_BUFFER: return "fills-buffer";
    case FIPSD_DNS_DROP_RESPONSE: return "response";
    case FIPSD_DNS_DROP_SERVICE_PORT: return "service-port";
    case FIPSD_DNS_DROP_NOT_QUERY_SHAPED: return "not-query-shaped";
    }
    return "?";
}

static uint16_t word(const uint8_t *bytes, size_t offset) { return (uint16_t)((bytes[offset] << 8) | bytes[offset + 1]); }
static void put16(uint8_t *out, uint16_t v) { out[0] = (uint8_t)(v >> 8); out[1] = (uint8_t)v; }
static void put32(uint8_t *out, uint32_t v)
{ out[0] = (uint8_t)(v >> 24); out[1] = (uint8_t)(v >> 16); out[2] = (uint8_t)(v >> 8); out[3] = (uint8_t)v; }

/* A reply header: id, QR set, the opcode, RD and CD from query_flags, the AA
 * and RA bits of flags, rcode, and the question, answer and authority counts.
 * Every other bit, Z and TC included, is clear; the additional count is zero. */
static void header(uint8_t out[FIPSD_DNS_HEADER_LEN], uint16_t id, uint16_t query_flags, uint8_t rcode,
                   uint16_t flags, const uint16_t counts[3])
{
    uint16_t f = (uint16_t)(FLAG_QR | (query_flags & COPIED_FLAGS) | (flags & CALLER_FLAGS) | (rcode & 0x0F));
    put16(out, id); put16(out + 2, f);
    for (size_t i = 0; i < 3; ++i) put16(out + 4 + 2 * i, counts[i]);
    put16(out + 10, 0);
}
static void header_only(fipsd_dns_screen *s, uint16_t id, uint16_t flags, uint8_t rcode)
{
    static const uint16_t none[3] = {0, 0, 0};
    s->kind = FIPSD_DNS_SCREEN_REPLY; s->rcode = rcode;
    header(s->reply, id, flags, rcode, 0, none); s->reply_len = FIPSD_DNS_HEADER_LEN;
}

bool fipsd_dns_query_next_label(const fipsd_dns_query *q, size_t *pos, const uint8_t **label, size_t *len)
{
    size_t wire = q->question_len - 4;
    if (*pos >= wire) return false;
    size_t n = q->question[*pos];
    if (!n || *pos + 1 + n > wire) return false;
    *label = q->question + *pos + 1; *len = n; *pos += 1 + n;
    return true;
}
const uint8_t *fipsd_dns_query_qname(const fipsd_dns_query *q, size_t *len)
{ *len = q->question_len - 4; return q->question; }

/* The length of the UTF-8 sequence at s, valid or not: a valid sequence's
 * length with *valid true, or the maximal invalid prefix (at least one byte)
 * with *valid false, as String::from_utf8_lossy replaces it. */
static size_t utf8_sequence(const uint8_t *s, size_t n, bool *valid)
{
    uint8_t b = s[0]; size_t need; uint8_t lo = 0x80, hi = 0xBF;
    *valid = true;
    if (b < 0x80) return 1;
    if (b >= 0xC2 && b <= 0xDF) need = 1;
    else if (b == 0xE0) { need = 2; lo = 0xA0; }
    else if ((b >= 0xE1 && b <= 0xEC) || b == 0xEE || b == 0xEF) need = 2;
    else if (b == 0xED) { need = 2; hi = 0x9F; }
    else if (b == 0xF0) { need = 3; lo = 0x90; }
    else if (b >= 0xF1 && b <= 0xF3) need = 3;
    else if (b == 0xF4) { need = 3; hi = 0x8F; }
    else { *valid = false; return 1; }
    for (size_t i = 1; i <= need; ++i) {
        if (i >= n || s[i] < (i == 1 ? lo : 0x80) || s[i] > (i == 1 ? hi : 0xBF)) { *valid = false; return i; }
    }
    return need + 1;
}
/* Append a label to the text name as its lossy UTF-8. */
static void append_label(char *name, size_t cap, size_t *at, const uint8_t *label, size_t len)
{
    for (size_t i = 0; i < len;) {
        bool valid; size_t n = utf8_sequence(label + i, len - i, &valid);
        if (valid) { if (*at + n < cap) { memcpy(name + *at, label + i, n); *at += n; } }
        else if (*at + 3 < cap) { memcpy(name + *at, "\xEF\xBF\xBD", 3); *at += 3; }
        i += n;
    }
    name[*at] = 0;
}

bool fipsd_dns_parse_query(const uint8_t *datagram, size_t len, fipsd_dns_query *out, fipsd_dns_screen *error)
{
    uint16_t id = word(datagram, 0), flags = word(datagram, 2);
    size_t pos = FIPSD_DNS_HEADER_LEN, at = 0;
    out->name[0] = 0;
    for (;;) {
        if (pos >= len) goto formerr;
        size_t n = datagram[pos];
        if (!n) { ++pos; break; }
        /* A pointer (0xC0) or an extended label type (0x40, 0x80); this also
         * rejects any label longer than 63 bytes. */
        if (n & 0xC0) goto formerr;
        if (pos + 1 + n > len) goto formerr;
        if (at) out->name[at++] = '.';
        append_label(out->name, sizeof out->name, &at, datagram + pos + 1, n);
        pos += 1 + n;
        /* Length bytes and labels so far, plus the root still to come. */
        if (pos - FIPSD_DNS_HEADER_LEN + 1 > FIPSD_DNS_MAX_NAME) goto formerr;
    }
    if (pos + 4 > len) goto formerr;
    out->id = id; out->flags = flags;
    out->question = datagram + FIPSD_DNS_HEADER_LEN; out->question_len = pos + 4 - FIPSD_DNS_HEADER_LEN;
    out->qtype = word(datagram, pos);
    uint16_t qclass = word(datagram, pos + 2);
    if (qclass != FIPSD_DNS_CLASS_IN && qclass != FIPSD_DNS_CLASS_ANY) {
        error->kind = FIPSD_DNS_SCREEN_REPLY; error->rcode = FIPSD_DNS_RCODE_REFUSED;
        error->reply_len = fipsd_dns_reply(out, FIPSD_DNS_RCODE_REFUSED, 0, NULL, 0, NULL, 0, error->reply, sizeof error->reply);
        return false;
    }
    return true;
formerr:
    header_only(error, id, flags, FIPSD_DNS_RCODE_FORMERR);
    return false;
}

void fipsd_dns_screen_datagram(const uint8_t *datagram, size_t len, uint16_t src_port, fipsd_dns_screen *out)
{
    out->kind = FIPSD_DNS_SCREEN_DROP;
    if (len < FIPSD_DNS_HEADER_LEN) { out->reason = FIPSD_DNS_DROP_SHORT; return; }
    if (len >= FIPSD_DNS_MAX_DATAGRAM) { out->reason = FIPSD_DNS_DROP_FILLS_BUFFER; return; }
    uint16_t id = word(datagram, 0), flags = word(datagram, 2);
    if (flags & FLAG_QR) { out->reason = FIPSD_DNS_DROP_RESPONSE; return; }
    for (size_t i = 0; i < sizeof service_ports / sizeof service_ports[0]; ++i)
        if (src_port == service_ports[i]) { out->reason = FIPSD_DNS_DROP_SERVICE_PORT; return; }
    uint16_t qdcount = word(datagram, 4);
    if (qdcount > 1 || word(datagram, 6) || word(datagram, 8) || word(datagram, 10) > 1) {
        out->reason = FIPSD_DNS_DROP_NOT_QUERY_SHAPED; return;
    }
    if (flags & FLAG_OPCODE) { header_only(out, id, flags, FIPSD_DNS_RCODE_NOTIMP); return; }
    if (!qdcount) { header_only(out, id, flags, FIPSD_DNS_RCODE_FORMERR); return; }
    if (fipsd_dns_parse_query(datagram, len, &out->query, out)) out->kind = FIPSD_DNS_SCREEN_QUERY;
}

fipsd_dns_record fipsd_dns_aaaa(const uint8_t addr[16], uint32_t ttl)
{
    fipsd_dns_record r = {.owner = FIPSD_DNS_HEADER_LEN, .rtype = FIPSD_DNS_TYPE_AAAA, .ttl = ttl, .rdata_len = 16};
    memcpy(r.rdata, addr, 16);
    return r;
}

/* The labels of a dotted name: start offsets and lengths of its non-empty parts. */
static size_t split_dotted(const char *name, const char **starts, size_t *lens, size_t cap)
{
    size_t n = 0;
    for (const char *p = name; ; ) {
        const char *dot = strchr(p, '.');
        size_t len = dot ? (size_t)(dot - p) : strlen(p);
        if (len && n < cap) { starts[n] = p; lens[n] = len; ++n; }
        if (!dot) break;
        p = dot + 1;
    }
    return n;
}
/* The number of parts zone.split('.') yields, empty ones included. */
static size_t zone_parts(const char *zone)
{
    size_t n = 1;
    for (const char *p = zone; *p; ++p) if (*p == '.') ++n;
    return n;
}
static bool label_eq(const uint8_t *a, size_t alen, const char *b, size_t blen)
{ return alen == blen && !strncasecmp((const char *)a, b, alen); }
/* The offset in a wire name (no pointers) where its remaining labels are
 * zone's labels, ignoring ASCII case; false when none. */
static bool zone_offset(const uint8_t *qname, size_t qlen, const char *zone, size_t *offset)
{
    const char *zs[128]; size_t zl[128];
    size_t zn = 0;
    for (const char *p = zone; ; ) {           /* zone.split('.'), empty parts included */
        const char *dot = strchr(p, '.');
        size_t len = dot ? (size_t)(dot - p) : strlen(p);
        if (zn < 128) { zs[zn] = p; zl[zn] = len; ++zn; }
        if (!dot) break;
        p = dot + 1;
    }
    for (size_t start = 0; start < qlen && qname[start]; start += 1 + qname[start]) {
        size_t pos = start, count = 0; bool match = true;
        while (pos < qlen && qname[pos]) {
            size_t n = qname[pos];
            if (count < zn && !label_eq(qname + pos + 1, n, zs[count], zl[count])) match = false;
            ++count; pos += 1 + n;
        }
        if (match && count == zn) { *offset = start; return true; }
    }
    return false;
}
/* Write a dotted name: its labels before zone and then the pointer when it
 * ends in zone and a pointer is given, otherwise all its labels and the root. */
static size_t write_name(uint8_t *out, size_t cap, const char *name, const char *zone, bool has_pointer, uint16_t pointer)
{
    const char *ls[128]; size_t ll[128];
    size_t n = split_dotted(name, ls, ll, 128), zone_len = zone_parts(zone);
    const char *zs[128]; size_t zl[128];
    size_t zn = 0;
    for (const char *p = zone; ; ) {
        const char *dot = strchr(p, '.');
        size_t len = dot ? (size_t)(dot - p) : strlen(p);
        if (zn < 128) { zs[zn] = p; zl[zn] = len; ++zn; }
        if (!dot) break;
        p = dot + 1;
    }
    bool in_zone = n > zone_len;
    for (size_t i = 0; in_zone && i < zone_len; ++i)
        if (!label_eq((const uint8_t *)ls[n - zone_len + i], ll[n - zone_len + i], zs[i], zl[i])) in_zone = false;
    size_t written = has_pointer && in_zone ? n - zone_len : n, at = 0;
    for (size_t i = 0; i < written; ++i) {
        size_t len = ll[i] > 63 ? 63 : ll[i];
        if (at + 1 + len > cap) return at;
        out[at++] = (uint8_t)len; memcpy(out + at, ls[i], len); at += len;
    }
    if (has_pointer && in_zone) { if (at + 2 <= cap) { put16(out + at, (uint16_t)(0xC000 | pointer)); at += 2; } }
    else if (at < cap) out[at++] = 0;
    return at;
}
fipsd_dns_record fipsd_dns_soa(const fipsd_dns_query *query, const char *zone, const char *mname,
                               const char *rname, uint32_t serial, uint32_t ttl)
{
    size_t qlen, apex = 0;
    const uint8_t *qname = fipsd_dns_query_qname(query, &qlen);
    bool in_zone = zone_offset(qname, qlen, zone, &apex);
    fipsd_dns_record r = {.owner = (uint16_t)(FIPSD_DNS_HEADER_LEN + (in_zone ? apex : 0)), .rtype = FIPSD_DNS_TYPE_SOA, .ttl = ttl};
    size_t at = 0;
    at += write_name(r.rdata + at, sizeof r.rdata - at, mname, zone, in_zone, r.owner);
    at += write_name(r.rdata + at, sizeof r.rdata - at, rname, zone, in_zone, r.owner);
    put32(r.rdata + at, serial); at += 4;
    for (int i = 0; i < 4; ++i) { put32(r.rdata + at, ttl); at += 4; }
    r.rdata_len = at;
    return r;
}
size_t fipsd_dns_encode_query(uint16_t id, const uint8_t *qname_wire, size_t qname_len, uint16_t qtype,
                              uint16_t qclass, uint8_t *out, size_t cap)
{
    size_t total = FIPSD_DNS_HEADER_LEN + qname_len + 4;
    if (total > cap) return 0;
    put16(out, id);
    static const uint8_t fixed[10] = {0, 0, 0, 1, 0, 0, 0, 0, 0, 0};
    memcpy(out + 2, fixed, sizeof fixed);
    memcpy(out + FIPSD_DNS_HEADER_LEN, qname_wire, qname_len);
    put16(out + FIPSD_DNS_HEADER_LEN + qname_len, qtype); put16(out + FIPSD_DNS_HEADER_LEN + qname_len + 2, qclass);
    return total;
}

size_t fipsd_dns_reply(const fipsd_dns_query *query, uint8_t rcode, uint16_t flags,
                       const fipsd_dns_record *answers, size_t n_answers,
                       const fipsd_dns_record *authority, size_t n_authority, uint8_t *out, size_t cap)
{
    if (cap < FIPSD_DNS_HEADER_LEN + query->question_len) return 0;
    uint16_t counts[3] = {1, (uint16_t)n_answers, (uint16_t)n_authority};
    /* A query that reaches here has opcode QUERY (0); the reply says so
     * whatever the flags word holds. */
    header(out, query->id, (uint16_t)(query->flags & ~FLAG_OPCODE), rcode, flags, counts);
    size_t at = FIPSD_DNS_HEADER_LEN;
    memcpy(out + at, query->question, query->question_len); at += query->question_len;
    for (size_t i = 0; i < n_answers + n_authority; ++i) {
        const fipsd_dns_record *r = i < n_answers ? &answers[i] : &authority[i - n_answers];
        if (at + 12 + r->rdata_len > cap) return 0;
        put16(out + at, (uint16_t)(0xC000 | r->owner)); put16(out + at + 2, r->rtype);
        put16(out + at + 4, FIPSD_DNS_CLASS_IN); put32(out + at + 6, r->ttl);
        put16(out + at + 10, (uint16_t)r->rdata_len); at += 12;
        memcpy(out + at, r->rdata, r->rdata_len); at += r->rdata_len;
    }
    return at;
}
