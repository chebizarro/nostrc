/* Codec vectors from upstream src/dnsmsg.rs tests: the screen rule by rule,
 * replies, SOA negative answers, encode_query, and the reflection bounds. */
#include "dns/msg.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)

/* An ordinary source port, as a resolver would send from. */
#define PORT 53000

static uint16_t word(const uint8_t *b, size_t off) { return (uint16_t)((b[off] << 8) | b[off + 1]); }
/* name in wire form, with a label per dot-separated part. */
static size_t wire_name(const char *name, uint8_t *out)
{
    size_t at = 0;
    for (const char *p = name; *p;) {
        const char *dot = strchr(p, '.');
        size_t len = dot ? (size_t)(dot - p) : strlen(p);
        if (len) { out[at++] = (uint8_t)len; memcpy(out + at, p, len); at += len; }
        if (!dot) break;
        p = dot + 1;
    }
    out[at++] = 0;
    return at;
}
/* A wire name of labels of the given lengths, every label byte fill. */
static size_t labelled(const size_t *lens, size_t n, uint8_t fill, uint8_t *out)
{
    size_t at = 0;
    for (size_t i = 0; i < n; ++i) { out[at++] = (uint8_t)lens[i]; memset(out + at, fill, lens[i]); at += lens[i]; }
    out[at++] = 0;
    return at;
}
/* A message with this header and body after it. */
static size_t message(uint16_t id, uint16_t flags, const uint16_t counts[4], const uint8_t *body, size_t body_len, uint8_t *out)
{
    out[0] = (uint8_t)(id >> 8); out[1] = (uint8_t)id; out[2] = (uint8_t)(flags >> 8); out[3] = (uint8_t)flags;
    for (size_t i = 0; i < 4; ++i) { out[4 + 2 * i] = (uint8_t)(counts[i] >> 8); out[5 + 2 * i] = (uint8_t)counts[i]; }
    memcpy(out + 12, body, body_len);
    return 12 + body_len;
}
/* A question section entry for the wire name. */
static size_t question(const uint8_t *name, size_t name_len, uint16_t qtype, uint16_t qclass, uint8_t *out)
{
    memcpy(out, name, name_len);
    out[name_len] = (uint8_t)(qtype >> 8); out[name_len + 1] = (uint8_t)qtype;
    out[name_len + 2] = (uint8_t)(qclass >> 8); out[name_len + 3] = (uint8_t)qclass;
    return name_len + 4;
}
/* A one-question message asking for name. */
static size_t datagram(uint16_t id, uint16_t flags, const char *name, uint16_t qtype, uint16_t qclass, uint8_t *out)
{
    uint8_t wire[300], body[320];
    size_t n = wire_name(name, wire);
    size_t body_len = question(wire, n, qtype, qclass, body);
    static const uint16_t one[4] = {1, 0, 0, 0};
    return message(id, flags, one, body, body_len, out);
}
static size_t aaaa_query(const char *name, uint8_t *out) { return datagram(0x1234, 0, name, FIPSD_DNS_TYPE_AAAA, FIPSD_DNS_CLASS_IN, out); }
/* One OPT record with pad bytes of padding. */
static size_t opt(size_t pad, uint8_t *out)
{
    size_t at = 0;
    out[at++] = 0; out[at++] = 0; out[at++] = 41; out[at++] = 1232 >> 8; out[at++] = 1232 & 0xff;
    memset(out + at, 0, 4); at += 4;
    out[at++] = (uint8_t)((4 + pad) >> 8); out[at++] = (uint8_t)(4 + pad);
    out[at++] = 0; out[at++] = 12; out[at++] = (uint8_t)(pad >> 8); out[at++] = (uint8_t)pad;
    memset(out + at, 0, pad); at += pad;
    return at;
}
static fipsd_dns_screen screen; /* large: one static copy */
static fipsd_dns_drop_reason dropped(const uint8_t *d, size_t len, uint16_t port)
{
    fipsd_dns_screen_datagram(d, len, port, &screen);
    CHECK(screen.kind == FIPSD_DNS_SCREEN_DROP);
    return screen.reason;
}
static uint8_t refused(const uint8_t *d, size_t len, uint16_t port)
{
    fipsd_dns_screen_datagram(d, len, port, &screen);
    CHECK(screen.kind == FIPSD_DNS_SCREEN_REPLY);
    return screen.rcode;
}
static fipsd_dns_query *accepted(const uint8_t *d, size_t len, uint16_t port)
{
    fipsd_dns_screen_datagram(d, len, port, &screen);
    CHECK(screen.kind == FIPSD_DNS_SCREEN_QUERY);
    return &screen.query;
}
static uint8_t rcode_of(const uint8_t *reply) { return reply[3] & 0x0F; }

/* A name at off, following compression pointers; *next is the offset after
 * the name's first representation. */
static void read_name(const uint8_t *msg, size_t len, size_t off, char *out, size_t cap, size_t *next)
{
    size_t at = 0; bool jumped = false; int hops = 0;
    out[0] = 0;
    for (;;) {
        CHECK(off < len);
        uint8_t n = msg[off];
        if (!n) { if (!jumped) *next = off + 1; break; }
        if ((n & 0xC0) == 0xC0) {
            CHECK(off + 1 < len && ++hops < 16);
            size_t target = ((n & 0x3F) << 8) | msg[off + 1];
            if (!jumped) *next = off + 2;
            jumped = true; off = target; continue;
        }
        CHECK(off + 1 + n <= len && at + n + 1 < cap);
        if (at) out[at++] = '.';
        memcpy(out + at, msg + off + 1, n); at += n; out[at] = 0;
        off += 1 + n;
    }
}
typedef struct record { char owner[300]; uint16_t type; uint32_t ttl; const uint8_t *rdata; size_t rdlen; size_t rdata_off; } record;
/* The first record after the question of a one-question reply, in section
 * index (0 answers, 1 authority). */
static record parse_record(const uint8_t *msg, size_t len, size_t index)
{
    size_t off = 12; char scratch[300];
    read_name(msg, len, off, scratch, sizeof scratch, &off); off += 4;
    record r;
    for (size_t i = 0; ; ++i) {
        read_name(msg, len, off, r.owner, sizeof r.owner, &off);
        r.type = word(msg, off); r.ttl = ((uint32_t)word(msg, off + 4) << 16) | word(msg, off + 6);
        r.rdlen = word(msg, off + 8); r.rdata_off = off + 10; r.rdata = msg + off + 10; off += 10 + r.rdlen;
        if (i == index) return r;
    }
}

int main(void)
{
    uint8_t d[4200], body[4200], name[300], out[1100], wire[300];
    size_t n, bn, nn, on;
    /* --- screen, rule by rule --- */
    memset(d, 0, 11);
    CHECK(dropped(d, 11, PORT) == FIPSD_DNS_DROP_SHORT);
    CHECK(dropped(d, 0, PORT) == FIPSD_DNS_DROP_SHORT);
    {   /* 4096 bytes dropped, 4095 screened */
        nn = wire_name("a.fips", name);
        bn = question(name, nn, FIPSD_DNS_TYPE_AAAA, FIPSD_DNS_CLASS_IN, body);
        size_t base = 12 + bn + 15;
        static const uint16_t counts[4] = {1, 0, 0, 1};
        size_t full_body = bn + opt(FIPSD_DNS_MAX_DATAGRAM - base, body + bn);
        n = message(1, 0, counts, body, full_body, d);
        CHECK(n == FIPSD_DNS_MAX_DATAGRAM);
        CHECK(dropped(d, n, PORT) == FIPSD_DNS_DROP_FILLS_BUFFER);
        bn = question(name, nn, FIPSD_DNS_TYPE_AAAA, FIPSD_DNS_CLASS_IN, body);
        size_t short_body = bn + opt(FIPSD_DNS_MAX_DATAGRAM - 1 - base, body + bn);
        n = message(1, 0, counts, body, short_body, d);
        CHECK(n == FIPSD_DNS_MAX_DATAGRAM - 1);
        CHECK(!strcmp(accepted(d, n, PORT)->name, "a.fips"));
    }
    n = datagram(1, 0x8000, "a.fips", FIPSD_DNS_TYPE_AAAA, FIPSD_DNS_CLASS_IN, d);
    CHECK(dropped(d, n, PORT) == FIPSD_DNS_DROP_RESPONSE);
    n = aaaa_query("a.fips", d);
    { static const uint16_t ports[] = {0, 7, 13, 17, 19, 37};
      for (size_t i = 0; i < 6; ++i) CHECK(dropped(d, n, ports[i]) == FIPSD_DNS_DROP_SERVICE_PORT); }
    CHECK(!strcmp(accepted(d, n, PORT)->name, "a.fips"));
    {   /* an answer or authority record, two additional records, two questions */
        nn = wire_name("a.fips", name); bn = question(name, nn, FIPSD_DNS_TYPE_AAAA, FIPSD_DNS_CLASS_IN, body);
        static const uint16_t shapes[4][4] = {{1, 1, 0, 0}, {1, 0, 1, 0}, {1, 0, 0, 2}, {2, 0, 0, 0}};
        for (size_t i = 0; i < 4; ++i) { n = message(1, 0, shapes[i], body, bn, d); CHECK(dropped(d, n, PORT) == FIPSD_DNS_DROP_NOT_QUERY_SHAPED); }
        const char *text = "Thu Oct  9 12:00:00 2026\r\n";
        CHECK(dropped((const uint8_t *)text, strlen(text), PORT) == FIPSD_DNS_DROP_NOT_QUERY_SHAPED);
    }
    {   /* a non-query opcode: header-only NOTIMP with the opcode and RD copied */
        n = datagram(0x4321, (4 << 11) | 0x0100, "a.fips", FIPSD_DNS_TYPE_AAAA, FIPSD_DNS_CLASS_IN, d);
        CHECK(refused(d, n, PORT) == FIPSD_DNS_RCODE_NOTIMP);
        CHECK(rcode_of(screen.reply) == FIPSD_DNS_RCODE_NOTIMP && screen.reply_len == 12);
        CHECK(word(screen.reply, 0) == 0x4321 && word(screen.reply, 2) == (0x8000 | (4 << 11) | 0x0100 | 4));
        static const uint8_t zero8[8] = {0}; CHECK(!memcmp(screen.reply + 4, zero8, 8));
    }
    {   static const uint16_t none[4] = {0, 0, 0, 0};
        n = message(7, 0, none, NULL, 0, d);
        CHECK(refused(d, n, PORT) == FIPSD_DNS_RCODE_FORMERR && rcode_of(screen.reply) == FIPSD_DNS_RCODE_FORMERR);
        CHECK(screen.reply_len == 12 && word(screen.reply, 0) == 7); }
    {   /* compression pointer or extended label type: FORMERR */
        static const uint8_t names[3][4] = {{0xC0, 0x0C}, {1, 'a', 0xC0, 0x00}, {0x40, 0}};
        static const size_t lens[3] = {2, 4, 2};
        static const uint16_t one[4] = {1, 0, 0, 0};
        for (size_t i = 0; i < 3; ++i) {
            bn = question(names[i], lens[i], FIPSD_DNS_TYPE_AAAA, FIPSD_DNS_CLASS_IN, body);
            n = message(1, 0, one, body, bn, d);
            CHECK(refused(d, n, PORT) == FIPSD_DNS_RCODE_FORMERR && screen.reply_len == 12);
        }
        size_t l64 = 64, l63 = 63;
        nn = labelled(&l64, 1, 'a', name); bn = question(name, nn, 28, 1, body); n = message(1, 0, one, body, bn, d);
        CHECK(refused(d, n, PORT) == FIPSD_DNS_RCODE_FORMERR);
        nn = labelled(&l63, 1, 'a', name); bn = question(name, nn, 28, 1, body); n = message(1, 0, one, body, bn, d);
        char sixty_three[64]; memset(sixty_three, 'a', 63); sixty_three[63] = 0;
        CHECK(!strcmp(accepted(d, n, PORT)->name, sixty_three));
        static const size_t long_lens[4] = {63, 63, 63, 62}, ok_lens[4] = {63, 63, 63, 61};
        nn = labelled(long_lens, 4, 'a', name); CHECK(nn == 256);
        bn = question(name, nn, 28, 1, body); n = message(1, 0, one, body, bn, d);
        CHECK(refused(d, n, PORT) == FIPSD_DNS_RCODE_FORMERR);
        nn = labelled(ok_lens, 4, 'a', name); CHECK(nn == 255);
        bn = question(name, nn, 28, 1, body); n = message(1, 0, one, body, bn, d);
        CHECK(accepted(d, n, PORT)->question_len == 259);
    }
    {   /* a question cut short */
        size_t whole = aaaa_query("a.fips", d);
        for (size_t cut = 12; cut < whole; ++cut) { CHECK(refused(d, cut, PORT) == FIPSD_DNS_RCODE_FORMERR); CHECK(screen.reply_len == 12); }
    }
    {   /* CH class refused with the question; class ANY accepted */
        static const uint16_t classes[3] = {3, 0x8001, 254};
        for (size_t i = 0; i < 3; ++i) {
            n = datagram(9, 0, "a.fips", FIPSD_DNS_TYPE_AAAA, classes[i], d);
            CHECK(refused(d, n, PORT) == FIPSD_DNS_RCODE_REFUSED && rcode_of(screen.reply) == FIPSD_DNS_RCODE_REFUSED);
            CHECK(word(screen.reply, 4) == 1 && screen.reply_len == n && !memcmp(screen.reply + 12, d + 12, n - 12));
        }
        n = datagram(9, 0, "a.fips", FIPSD_DNS_TYPE_AAAA, 255, d);
        CHECK(!strcmp(accepted(d, n, PORT)->name, "a.fips"));
    }
    {   /* a Z-bit query is screened as a query and Z is not echoed */
        n = datagram(9, 0x0040, "a.fips", FIPSD_DNS_TYPE_AAAA, FIPSD_DNS_CLASS_IN, d);
        fipsd_dns_query *q = accepted(d, n, PORT);
        on = fipsd_dns_reply(q, FIPSD_DNS_RCODE_NOERROR, 0, NULL, 0, NULL, 0, out, sizeof out);
        CHECK(on && !(word(out, 2) & 0x0040));
    }
    {   static const uint16_t types[4] = {44, 65, 0xFF00, 0};
        for (size_t i = 0; i < 4; ++i) { n = datagram(9, 0, "a.fips", types[i], FIPSD_DNS_CLASS_IN, d); CHECK(accepted(d, n, PORT)->qtype == types[i]); } }
    {   /* one OPT additional record is accepted and not echoed */
        nn = wire_name("a.fips", name); bn = question(name, nn, FIPSD_DNS_TYPE_AAAA, FIPSD_DNS_CLASS_IN, body);
        bn += opt(400, body + bn);
        static const uint16_t counts[4] = {1, 0, 0, 1};
        n = message(9, 0, counts, body, bn, d);
        fipsd_dns_query *q = accepted(d, n, PORT);
        static const uint8_t localhost[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
        fipsd_dns_record r = fipsd_dns_aaaa(localhost, 60);
        on = fipsd_dns_reply(q, FIPSD_DNS_RCODE_NOERROR, FIPSD_DNS_AA, &r, 1, NULL, 0, out, sizeof out);
        CHECK(word(out, 10) == 0 && on == 12 + 12 + 28);
    }
    {   /* the text form of a question name matches simple-dns's display */
        static const uint16_t one[4] = {1, 0, 0, 0};
        typedef struct vector { const uint8_t *wire; size_t len; const char *text; } vector;
        vector corpus[8];
        uint8_t w0[64], w1[64], w2[80], w7[300];
        size_t c0 = wire_name("MiXeD.Case.FIPS", w0), c1 = wire_name("host-1.a2b-c.fips", w1);
        static const size_t l2[2] = {63, 1}; size_t c2 = labelled(l2, 2, 'a', w2);
        static const uint8_t w3[] = {5, '.', 'f', 'i', 'p', 's', 0};
        static const uint8_t w4[] = {3, 'a', '.', 'b', 4, 'f', 'i', 'p', 's', 0};
        static const uint8_t w5[] = {2, 0xFF, 'a', 4, 'f', 'i', 'p', 's', 0};
        static const uint8_t w6[] = {0};
        static const size_t l7[4] = {63, 63, 63, 61}; size_t c7 = labelled(l7, 4, 'a', w7);
        char t2[66]; memset(t2, 'a', 63); t2[63] = '.'; t2[64] = 'a'; t2[65] = 0;
        char t7[256]; { size_t at = 0; for (size_t i = 0; i < 4; ++i) { if (i) t7[at++] = '.'; memset(t7 + at, 'a', l7[i]); at += l7[i]; } t7[at] = 0; }
        corpus[0] = (vector){w0, c0, "MiXeD.Case.FIPS"}; corpus[1] = (vector){w1, c1, "host-1.a2b-c.fips"};
        corpus[2] = (vector){w2, c2, t2}; corpus[3] = (vector){w3, sizeof w3, ".fips"};
        corpus[4] = (vector){w4, sizeof w4, "a.b.fips"}; corpus[5] = (vector){w5, sizeof w5, "\xEF\xBF\xBD" "a.fips"};
        corpus[6] = (vector){w6, sizeof w6, ""}; corpus[7] = (vector){w7, c7, t7};
        for (size_t i = 0; i < 8; ++i) {
            bn = question(corpus[i].wire, corpus[i].len, FIPSD_DNS_TYPE_AAAA, FIPSD_DNS_CLASS_IN, body);
            n = message(9, 0, one, body, bn, d);
            const char *ours = accepted(d, n, PORT)->name;
            if (strcmp(ours, corpus[i].text)) { fprintf(stderr, "name %zu: got '%s' want '%s'\n", i, ours, corpus[i].text); abort(); }
        }
    }
    CHECK(fipsd_dns_clamp_ttl(0) == 0 && fipsd_dns_clamp_ttl(300) == 300 && fipsd_dns_clamp_ttl(2147483647u) == 2147483647u);
    CHECK(fipsd_dns_clamp_ttl(2147483648u) == 2147483647u && fipsd_dns_clamp_ttl(UINT32_MAX) == FIPSD_DNS_MAX_TTL);
    { char b[8]; CHECK(!strcmp(fipsd_dns_rcode_name(FIPSD_DNS_RCODE_NOTIMP, b, sizeof b), "NOTIMP"));
      CHECK(!strcmp(fipsd_dns_rcode_name(10, b, sizeof b), "NOTZONE")); CHECK(!strcmp(fipsd_dns_rcode_name(13, b, sizeof b), "RCODE13")); }
    CHECK(!strcmp(fipsd_dns_drop_reason_str(FIPSD_DNS_DROP_FILLS_BUFFER), "fills-buffer"));
    CHECK(!strcmp(fipsd_dns_drop_reason_str(FIPSD_DNS_DROP_NOT_QUERY_SHAPED), "not-query-shaped"));
    CHECK(fipsd_dns_rcode_from_header(0x82) == FIPSD_DNS_RCODE_SERVFAIL && fipsd_dns_rcode_from_header(0x83) == FIPSD_DNS_RCODE_NXDOMAIN);
    CHECK(fipsd_dns_rcode_from_header(0x8F) == 15 && fipsd_dns_rcode_from_header(0x80) == FIPSD_DNS_RCODE_NOERROR);
    {   /* encode_query matches a hand-built query for the same name */
        n = aaaa_query("GateWay.FIPS", d);
        fipsd_dns_query *q = accepted(d, n, PORT);
        size_t qn; const uint8_t *qname = fipsd_dns_query_qname(q, &qn);
        on = fipsd_dns_encode_query(0xBEEF, qname, qn, FIPSD_DNS_TYPE_AAAA, FIPSD_DNS_CLASS_IN, out, sizeof out);
        size_t en = datagram(0xBEEF, 0, "GateWay.FIPS", FIPSD_DNS_TYPE_AAAA, FIPSD_DNS_CLASS_IN, wire);
        CHECK(on == en && !memcmp(out, wire, en));
    }
    {   /* an SOA for a .fips question is owned by the fips label: query + 51 */
        n = aaaa_query("a.fips", d); CHECK(n == 24);
        fipsd_dns_query *q = accepted(d, n, PORT);
        fipsd_dns_record soa = fipsd_dns_soa(q, "fips", "gateway.fips", "nobody.fips", 1, 60);
        on = fipsd_dns_reply(q, FIPSD_DNS_RCODE_NXDOMAIN, FIPSD_DNS_RA, NULL, 0, &soa, 1, out, sizeof out);
        CHECK(on == 75 && out[24] == 0xC0 && out[25] == 14);
        record r = parse_record(out, on, 0); char mname[300], rname[300]; size_t next;
        CHECK(r.type == 6 && !strcmp(r.owner, "fips"));
        read_name(out, on, r.rdata_off, mname, sizeof mname, &next); read_name(out, on, next, rname, sizeof rname, &next);
        CHECK(!strcmp(mname, "gateway.fips") && !strcmp(rname, "nobody.fips"));
        CHECK(word(out, r.rdata_off + r.rdlen - 2) == 60);
        /* upper case still points and parses */
        n = aaaa_query("Host.FIPS", d); q = accepted(d, n, PORT);
        soa = fipsd_dns_soa(q, "fips", "gateway.fips", "nobody.fips", 1, 60);
        on = fipsd_dns_reply(q, FIPSD_DNS_RCODE_NOERROR, FIPSD_DNS_RA, NULL, 0, &soa, 1, out, sizeof out);
        CHECK(on == n + 51);
        r = parse_record(out, on, 0); read_name(out, on, r.rdata_off, mname, sizeof mname, &next); read_name(out, on, next, rname, sizeof rname, &next);
        CHECK(!strcmp(r.owner, "FIPS") && !strcmp(mname, "gateway.FIPS") && !strcmp(rname, "nobody.FIPS"));
        /* not in the zone: owned by the question name, query + 59 */
        static const uint8_t dotfips[] = {5, '.', 'f', 'i', 'p', 's', 0};
        static const uint16_t one[4] = {1, 0, 0, 0};
        for (int which = 0; which < 2; ++which) {
            if (which == 0) nn = wire_name("example.com", name); else { memcpy(name, dotfips, sizeof dotfips); nn = sizeof dotfips; }
            bn = question(name, nn, FIPSD_DNS_TYPE_AAAA, FIPSD_DNS_CLASS_IN, body); n = message(9, 0, one, body, bn, d);
            q = accepted(d, n, PORT);
            soa = fipsd_dns_soa(q, "fips", "gateway.fips", "nobody.fips", 1, 60);
            on = fipsd_dns_reply(q, FIPSD_DNS_RCODE_NXDOMAIN, FIPSD_DNS_RA, NULL, 0, &soa, 1, out, sizeof out);
            CHECK(on == n + 59 && out[n] == 0xC0 && out[n + 1] == 0x0C);
            r = parse_record(out, on, 0); read_name(out, on, r.rdata_off, mname, sizeof mname, &next); read_name(out, on, next, rname, sizeof rname, &next);
            CHECK(!strcmp(r.owner, q->name) && !strcmp(mname, "gateway.fips") && !strcmp(rname, "nobody.fips"));
        }
    }
    {   /* every reply and every error reply is dropped when fed back */
        n = aaaa_query("a.fips", d); fipsd_dns_query *q = accepted(d, n, PORT);
        static const uint8_t localhost[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
        fipsd_dns_record r = fipsd_dns_aaaa(localhost, 60);
        uint8_t replies[7][1100]; size_t lens[7]; size_t count = 0;
        lens[count] = fipsd_dns_reply(q, FIPSD_DNS_RCODE_NOERROR, FIPSD_DNS_AA, &r, 1, NULL, 0, replies[count], 1100); ++count;
        lens[count] = fipsd_dns_reply(q, FIPSD_DNS_RCODE_NOERROR, FIPSD_DNS_AA, NULL, 0, NULL, 0, replies[count], 1100); ++count;
        lens[count] = fipsd_dns_reply(q, FIPSD_DNS_RCODE_NXDOMAIN, FIPSD_DNS_AA, NULL, 0, NULL, 0, replies[count], 1100); ++count;
        uint8_t bad[4][64]; size_t badn[4];
        badn[0] = datagram(1, 5 << 11, "a.fips", FIPSD_DNS_TYPE_AAAA, FIPSD_DNS_CLASS_IN, bad[0]);
        static const uint16_t none[4] = {0, 0, 0, 0}; badn[1] = message(1, 0, none, NULL, 0, bad[1]);
        static const uint8_t ptrq[] = {0xC0, 0x0C, 0, 28, 0, 1}; static const uint16_t one[4] = {1, 0, 0, 0};
        badn[2] = message(1, 0, one, ptrq, sizeof ptrq, bad[2]);
        badn[3] = datagram(1, 0, "a.fips", FIPSD_DNS_TYPE_AAAA, 3, bad[3]);
        for (size_t i = 0; i < 4; ++i) { refused(bad[i], badn[i], PORT); memcpy(replies[count], screen.reply, screen.reply_len); lens[count] = screen.reply_len; ++count; }
        for (size_t i = 0; i < count; ++i) CHECK(dropped(replies[i], lens[i], PORT) == FIPSD_DNS_DROP_RESPONSE);
        /* every error reply copies RD and CD and the ID and sets neither AA nor RA */
        badn[0] = datagram(0x5151, 0x0110 | (5 << 11), "a.fips", FIPSD_DNS_TYPE_AAAA, FIPSD_DNS_CLASS_IN, bad[0]);
        badn[1] = message(0x5151, 0x0110, none, NULL, 0, bad[1]);
        badn[2] = message(0x5151, 0x0110, one, ptrq, sizeof ptrq, bad[2]);
        badn[3] = datagram(0x5151, 0x0110, "a.fips", FIPSD_DNS_TYPE_AAAA, 3, bad[3]);
        for (size_t i = 0; i < 4; ++i) {
            refused(bad[i], badn[i], PORT);
            uint16_t f = word(screen.reply, 2);
            CHECK(word(screen.reply, 0) == 0x5151 && (f & 0x0110) == 0x0110 && !(f & 0x0400) && !(f & 0x0080) && (f & 0x8000));
        }
    }
    {   /* a reply echoes the question bytes exactly and copies RD and CD but not Z or opcode bits */
        n = datagram(0x0102, 0x0150, "gAtEwAy.FiPs", 44, FIPSD_DNS_CLASS_IN, d);
        fipsd_dns_query *q = accepted(d, n, PORT);
        on = fipsd_dns_reply(q, FIPSD_DNS_RCODE_NXDOMAIN, FIPSD_DNS_AA | 0x0200 | 0x0040, NULL, 0, NULL, 0, out, sizeof out);
        CHECK(on == n && !memcmp(out + 12, d + 12, n - 12) && word(out, 2) == (0x8000 | 0x0400 | 0x0110 | 3));
        static const uint8_t counts[8] = {0, 1, 0, 0, 0, 0, 0, 0}; CHECK(!memcmp(out + 4, counts, 8));
        n = datagram(0x0102, 4 << 11, "a.fips", FIPSD_DNS_TYPE_AAAA, FIPSD_DNS_CLASS_IN, d);
        fipsd_dns_query parsed; fipsd_dns_screen *err = malloc(sizeof *err);
        CHECK(fipsd_dns_parse_query(d, n, &parsed, err));
        on = fipsd_dns_reply(&parsed, FIPSD_DNS_RCODE_NOERROR, 0, NULL, 0, NULL, 0, out, sizeof out);
        CHECK(!(word(out, 2) & 0x7800));
        free(err);
    }
    {   /* a reply with an AAAA parses and is the query length plus 28 */
        n = aaaa_query("Host.fips", d); fipsd_dns_query *q = accepted(d, n, PORT);
        uint8_t addr[16] = {0xfd, 0x12, 0x34, 0x56, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
        fipsd_dns_record r = fipsd_dns_aaaa(addr, 300);
        on = fipsd_dns_reply(q, FIPSD_DNS_RCODE_NOERROR, FIPSD_DNS_AA, &r, 1, NULL, 0, out, sizeof out);
        CHECK(on == n + 28 && word(out, 0) == 0x1234 && word(out, 6) == 1);
        record a = parse_record(out, on, 0);
        CHECK(!strcmp(a.owner, "Host.fips") && a.ttl == 300 && a.type == 28 && a.rdlen == 16 && !memcmp(a.rdata, addr, 16));
    }
    {   /* a reply to a two-question datagram carries only the first question */
        static const size_t lens[4] = {63, 63, 63, 61};
        nn = labelled(lens, 4, 0x01, name);
        bn = question(name, nn, FIPSD_DNS_TYPE_AAAA, FIPSD_DNS_CLASS_IN, body);
        static const uint8_t second[] = {0xC0, 0x0D, 0x00, 0x1C, 0x00, 0x01};
        memcpy(body + bn, second, sizeof second); bn += sizeof second;
        static const uint16_t two[4] = {2, 0, 0, 0};
        n = message(9, 0, two, body, bn, d); CHECK(n == 277);
        fipsd_dns_query parsed; fipsd_dns_screen *err = malloc(sizeof *err);
        CHECK(fipsd_dns_parse_query(d, n, &parsed, err));
        static const uint8_t localhost[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
        fipsd_dns_record r = fipsd_dns_aaaa(localhost, 1);
        on = fipsd_dns_reply(&parsed, FIPSD_DNS_RCODE_NXDOMAIN, FIPSD_DNS_AA, &r, 1, NULL, 0, out, sizeof out);
        CHECK(word(out, 4) == 1 && on <= n + 28 && on == 12 + 259 + 28);
        free(err);
    }
    puts("dns msg: screen rules, replies, SOA, encode_query, reflection bounds: ok");
    return 0;
}
