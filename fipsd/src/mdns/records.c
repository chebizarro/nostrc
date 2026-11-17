/* mDNS message codec: decompressing decoder and uncompressed writer. */
#include "mdns/records.h"
#include <string.h>

static uint8_t lower(uint8_t c) { return c >= 'A' && c <= 'Z' ? (uint8_t)(c + 32) : c; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t *p)
{ return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v)
{ p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

bool fipsd_mdns_name_from_text(const char *text, fipsd_mdns_name *out)
{
    memset(out, 0, sizeof *out);
    if (!text) return false;
    size_t n = strlen(text), at = 0, i = 0;
    if (n && text[n - 1] == '.') --n;
    while (i < n) {
        size_t j = i;
        while (j < n && text[j] != '.') ++j;
        size_t label = j - i;
        if (!label || label > 63 || at + 1 + label + 1 > FIPSD_DNS_MAX_NAME) return false;
        out->wire[at++] = (uint8_t)label;
        memcpy(out->wire + at, text + i, label); at += label;
        i = j + 1;
    }
    out->wire[at++] = 0;
    out->len = (uint8_t)at;
    return true;
}
size_t fipsd_mdns_name_to_text(const fipsd_mdns_name *name, char *out, size_t cap)
{
    size_t at = 0, i = 0;
    if (!cap) return 0;
    while (i < name->len && name->wire[i]) {
        uint8_t label = name->wire[i++];
        if (at + label + 2 > cap) { out[0] = 0; return 0; }
        for (uint8_t k = 0; k < label; ++k) {
            uint8_t c = name->wire[i + k];
            out[at++] = c >= 0x20 && c < 0x7f ? (char)c : '?';
        }
        out[at++] = '.';
        i += label;
    }
    if (!at) { if (cap < 2) return 0; out[at++] = '.'; }
    out[at] = 0;
    return at;
}
bool fipsd_mdns_name_equal(const fipsd_mdns_name *a, const fipsd_mdns_name *b)
{
    if (a->len != b->len) return false;
    for (size_t i = 0; i < a->len; ++i) if (lower(a->wire[i]) != lower(b->wire[i])) return false;
    return true;
}
bool fipsd_mdns_name_prepend(const char *label, const fipsd_mdns_name *suffix, fipsd_mdns_name *out)
{
    size_t n = label ? strlen(label) : 0;
    if (!n || n > 63 || memchr(label, '.', n) || 1 + n + suffix->len > FIPSD_DNS_MAX_NAME) return false;
    fipsd_mdns_name name = {0};
    name.wire[0] = (uint8_t)n; memcpy(name.wire + 1, label, n);
    memcpy(name.wire + 1 + n, suffix->wire, suffix->len);
    name.len = (uint8_t)(1 + n + suffix->len);
    *out = name;
    return true;
}
bool fipsd_mdns_name_child_of(const fipsd_mdns_name *name, const fipsd_mdns_name *suffix, char label[64])
{
    if (!name->len || !name->wire[0]) return false;
    size_t n = name->wire[0];
    if (1 + n + suffix->len != name->len) return false;
    for (size_t i = 0; i < suffix->len; ++i) if (lower(name->wire[1 + n + i]) != lower(suffix->wire[i])) return false;
    memcpy(label, name->wire + 1, n); label[n] = 0;
    return true;
}

/* Read a possibly compressed name at *pos into out; *pos moves past the name
 * as it appears at that point. Pointers must point strictly backwards, which
 * bounds the walk. */
static bool read_name(const uint8_t *buf, size_t len, size_t *pos, fipsd_mdns_name *out)
{
    size_t at = *pos, wrote = 0, end = 0;
    bool jumped = false;
    size_t limit = at; /* a pointer must target an offset below this */
    for (;;) {
        if (at >= len) return false;
        uint8_t c = buf[at];
        if ((c & 0xC0) == 0xC0) {
            if (at + 1 >= len) return false;
            size_t target = ((size_t)(c & 0x3F) << 8) | buf[at + 1];
            if (target >= limit) return false;
            if (!jumped) { end = at + 2; jumped = true; }
            limit = target; at = target;
            continue;
        }
        if (c & 0xC0) return false;
        if (wrote + 1 + c > FIPSD_DNS_MAX_NAME) return false;
        if (at + 1 + c > len) return false;
        out->wire[wrote++] = c;
        if (!c) { ++at; break; }
        memcpy(out->wire + wrote, buf + at + 1, c);
        wrote += c; at += 1 + (size_t)c;
    }
    out->len = (uint8_t)wrote;
    *pos = jumped ? end : at;
    return true;
}
static bool read_rr(const uint8_t *buf, size_t len, size_t *pos, fipsd_mdns_rr *rr)
{
    if (!read_name(buf, len, pos, &rr->name) || *pos + 10 > len) return false;
    const uint8_t *p = buf + *pos;
    rr->rtype = be16(p);
    uint16_t cls = be16(p + 2);
    rr->cache_flush = (cls & FIPSD_MDNS_CLASS_FLAG) != 0;
    rr->rclass = cls & (uint16_t)~FIPSD_MDNS_CLASS_FLAG;
    rr->ttl = be32(p + 4);
    size_t rdlen = be16(p + 8), start = *pos + 10;
    if (start + rdlen > len) return false;
    *pos = start + rdlen;
    switch (rr->rtype) {
    case FIPSD_MDNS_TYPE_A:
        if (rdlen != 4) return false;
        break;
    case FIPSD_MDNS_TYPE_AAAA:
        if (rdlen != 16) return false;
        break;
    case FIPSD_MDNS_TYPE_PTR: {
        size_t at = start; fipsd_mdns_name target;
        if (!read_name(buf, start + rdlen, &at, &target) || at != start + rdlen) return false;
        memcpy(rr->rdata, target.wire, target.len); rr->rdlen = target.len;
        return true;
    }
    case FIPSD_MDNS_TYPE_SRV: {
        if (rdlen < 7) return false;
        size_t at = start + 6; fipsd_mdns_name target;
        if (!read_name(buf, start + rdlen, &at, &target) || at != start + rdlen) return false;
        memcpy(rr->rdata, buf + start, 6); memcpy(rr->rdata + 6, target.wire, target.len);
        rr->rdlen = (uint16_t)(6 + target.len);
        return true;
    }
    default:
        break;
    }
    /* Other types are kept as received; longer ones are clipped, which only
     * matters for TXT records of other services. */
    size_t keep = rdlen > FIPSD_MDNS_MAX_RDATA ? FIPSD_MDNS_MAX_RDATA : rdlen;
    memcpy(rr->rdata, buf + start, keep); rr->rdlen = (uint16_t)keep;
    return true;
}
bool fipsd_mdns_decode(const uint8_t *buf, size_t len, fipsd_mdns_message *out)
{
    out->qdcount = out->ancount = out->nscount = out->arcount = 0;
    out->truncated = false;
    if (len < 12) return false;
    out->id = be16(buf); out->flags = be16(buf + 2);
    size_t counts[4] = {be16(buf + 4), be16(buf + 6), be16(buf + 8), be16(buf + 10)};
    size_t pos = 12;
    for (size_t i = 0; i < counts[0]; ++i) {
        fipsd_mdns_question q;
        if (!read_name(buf, len, &pos, &q.name) || pos + 4 > len) return false;
        q.qtype = be16(buf + pos);
        uint16_t cls = be16(buf + pos + 2); pos += 4;
        q.unicast = (cls & FIPSD_MDNS_CLASS_FLAG) != 0;
        q.qclass = cls & (uint16_t)~FIPSD_MDNS_CLASS_FLAG;
        if (out->qdcount < FIPSD_MDNS_MAX_QUESTIONS) out->questions[out->qdcount++] = q;
        else out->truncated = true;
    }
    size_t *kept[3] = {&out->ancount, &out->nscount, &out->arcount};
    for (int s = 0; s < 3; ++s) {
        for (size_t i = 0; i < counts[s + 1]; ++i) {
            size_t used = out->ancount + out->nscount + out->arcount;
            if (used < FIPSD_MDNS_MAX_RECORDS) {
                if (!read_rr(buf, len, &pos, &out->records[used])) return false;
                ++*kept[s];
            } else {
                /* Skip the record without keeping it: name, fixed part, rdata. */
                fipsd_mdns_name skip;
                if (!read_name(buf, len, &pos, &skip) || pos + 10 > len) return false;
                size_t rdlen = be16(buf + pos + 8);
                if (pos + 10 + rdlen > len) return false;
                pos += 10 + rdlen;
                out->truncated = true;
            }
        }
    }
    return true;
}

static void set_owner(fipsd_mdns_rr *rr, const fipsd_mdns_name *owner, uint16_t rtype, uint32_t ttl, bool flush)
{
    rr->name = *owner; rr->rtype = rtype; rr->rclass = FIPSD_MDNS_CLASS_IN; rr->ttl = ttl; rr->cache_flush = flush;
    rr->rdlen = 0;
}
void fipsd_mdns_rr_ptr(fipsd_mdns_rr *rr, const fipsd_mdns_name *owner, const fipsd_mdns_name *target, uint32_t ttl)
{
    set_owner(rr, owner, FIPSD_MDNS_TYPE_PTR, ttl, false);
    memcpy(rr->rdata, target->wire, target->len); rr->rdlen = target->len;
}
void fipsd_mdns_rr_srv(fipsd_mdns_rr *rr, const fipsd_mdns_name *owner, uint16_t port, const fipsd_mdns_name *target,
                       uint32_t ttl, bool flush)
{
    set_owner(rr, owner, FIPSD_MDNS_TYPE_SRV, ttl, flush);
    put16(rr->rdata, 0); put16(rr->rdata + 2, 0); put16(rr->rdata + 4, port);
    memcpy(rr->rdata + 6, target->wire, target->len); rr->rdlen = (uint16_t)(6 + target->len);
}
void fipsd_mdns_rr_a(fipsd_mdns_rr *rr, const fipsd_mdns_name *owner, const uint8_t addr[4], uint32_t ttl, bool flush)
{ set_owner(rr, owner, FIPSD_MDNS_TYPE_A, ttl, flush); memcpy(rr->rdata, addr, 4); rr->rdlen = 4; }
void fipsd_mdns_rr_aaaa(fipsd_mdns_rr *rr, const fipsd_mdns_name *owner, const uint8_t addr[16], uint32_t ttl, bool flush)
{ set_owner(rr, owner, FIPSD_MDNS_TYPE_AAAA, ttl, flush); memcpy(rr->rdata, addr, 16); rr->rdlen = 16; }
bool fipsd_mdns_rr_txt(fipsd_mdns_rr *rr, const fipsd_mdns_name *owner, const fipsd_mdns_txt_entry *entries, size_t n,
                       uint32_t ttl, bool flush)
{
    set_owner(rr, owner, FIPSD_MDNS_TYPE_TXT, ttl, flush);
    size_t at = 0;
    for (size_t i = 0; i < n; ++i) {
        size_t k = strlen(entries[i].key), v = entries[i].value ? strlen(entries[i].value) : 0;
        size_t s = k + (entries[i].value ? 1 + v : 0);
        if (!k || s > 255 || at + 1 + s > FIPSD_MDNS_MAX_RDATA) return false;
        rr->rdata[at++] = (uint8_t)s;
        memcpy(rr->rdata + at, entries[i].key, k); at += k;
        if (entries[i].value) { rr->rdata[at++] = '='; memcpy(rr->rdata + at, entries[i].value, v); at += v; }
    }
    if (!at) rr->rdata[at++] = 0;
    rr->rdlen = (uint16_t)at;
    return true;
}
bool fipsd_mdns_txt_next(const uint8_t *rdata, size_t rdlen, size_t *pos, const uint8_t **key, size_t *klen,
                         const uint8_t **value, size_t *vlen, bool *has_value)
{
    for (;;) {
        if (*pos >= rdlen) return false;
        size_t n = rdata[*pos];
        if (*pos + 1 + n > rdlen) return false;
        const uint8_t *s = rdata + *pos + 1;
        *pos += 1 + n;
        if (!n) continue; /* the empty string carries no attribute */
        const uint8_t *eq = memchr(s, '=', n);
        *key = s;
        if (eq) { *klen = (size_t)(eq - s); *value = eq + 1; *vlen = n - *klen - 1; *has_value = true; }
        else { *klen = n; *value = s + n; *vlen = 0; *has_value = false; }
        return true;
    }
}
bool fipsd_mdns_srv_fields(const fipsd_mdns_rr *rr, uint16_t *port, fipsd_mdns_name *target)
{
    if (rr->rtype != FIPSD_MDNS_TYPE_SRV || rr->rdlen < 7) return false;
    *port = be16(rr->rdata + 4);
    target->len = (uint8_t)(rr->rdlen - 6); memcpy(target->wire, rr->rdata + 6, target->len);
    return true;
}
bool fipsd_mdns_ptr_target(const fipsd_mdns_rr *rr, fipsd_mdns_name *target)
{
    if (rr->rtype != FIPSD_MDNS_TYPE_PTR || !rr->rdlen) return false;
    target->len = (uint8_t)rr->rdlen; memcpy(target->wire, rr->rdata, rr->rdlen);
    return true;
}
bool fipsd_mdns_rr_same(const fipsd_mdns_rr *a, const fipsd_mdns_rr *b)
{
    return a->rtype == b->rtype && a->rclass == b->rclass && a->rdlen == b->rdlen &&
        fipsd_mdns_name_equal(&a->name, &b->name) && !memcmp(a->rdata, b->rdata, a->rdlen);
}
int fipsd_mdns_rr_compare(const fipsd_mdns_rr *a, const fipsd_mdns_rr *b)
{
    if (a->rclass != b->rclass) return a->rclass < b->rclass ? -1 : 1;
    if (a->rtype != b->rtype) return a->rtype < b->rtype ? -1 : 1;
    size_t n = a->rdlen < b->rdlen ? a->rdlen : b->rdlen;
    int c = memcmp(a->rdata, b->rdata, n);
    if (c) return c < 0 ? -1 : 1;
    return a->rdlen == b->rdlen ? 0 : a->rdlen < b->rdlen ? -1 : 1;
}

void fipsd_mdns_writer_init(fipsd_mdns_writer *w, uint8_t *buf, size_t cap, uint16_t id, uint16_t flags)
{
    *w = (fipsd_mdns_writer){.buf = buf, .cap = cap, .len = 12};
    if (cap < 12) { w->overflow = true; return; }
    memset(buf, 0, 12); put16(buf, id); put16(buf + 2, flags);
}
static bool room(fipsd_mdns_writer *w, size_t n)
{
    if (w->overflow || w->len + n > w->cap) { w->overflow = true; return false; }
    return true;
}
bool fipsd_mdns_put_question(fipsd_mdns_writer *w, const fipsd_mdns_name *name, uint16_t qtype, bool unicast)
{
    if (w->section > 0) { w->overflow = true; return false; }
    if (!room(w, (size_t)name->len + 4)) return false;
    memcpy(w->buf + w->len, name->wire, name->len); w->len += name->len;
    put16(w->buf + w->len, qtype);
    put16(w->buf + w->len + 2, (uint16_t)(FIPSD_MDNS_CLASS_IN | (unicast ? FIPSD_MDNS_CLASS_FLAG : 0)));
    w->len += 4; ++w->counts[0];
    return true;
}
bool fipsd_mdns_put_rr(fipsd_mdns_writer *w, fipsd_mdns_section section, const fipsd_mdns_rr *rr,
                       uint32_t ttl_cap, bool no_flush)
{
    if ((int)section < w->section) { w->overflow = true; return false; }
    if (!room(w, (size_t)rr->name.len + 10 + rr->rdlen)) return false;
    w->section = (int)section;
    memcpy(w->buf + w->len, rr->name.wire, rr->name.len); w->len += rr->name.len;
    uint8_t *p = w->buf + w->len;
    put16(p, rr->rtype);
    put16(p + 2, (uint16_t)(rr->rclass | (rr->cache_flush && !no_flush ? FIPSD_MDNS_CLASS_FLAG : 0)));
    put32(p + 4, ttl_cap && rr->ttl > ttl_cap ? ttl_cap : rr->ttl);
    put16(p + 8, rr->rdlen);
    memcpy(p + 10, rr->rdata, rr->rdlen);
    w->len += 10 + (size_t)rr->rdlen;
    ++w->counts[section];
    return true;
}
size_t fipsd_mdns_writer_finish(fipsd_mdns_writer *w)
{
    if (w->overflow) return 0;
    for (int i = 0; i < 4; ++i) put16(w->buf + 4 + 2 * i, w->counts[i]);
    return w->len;
}
