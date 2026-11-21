#include <fips/utils/onlink.h>
#include <string.h>

static fips_ip_addr canonical(fips_ip_addr ip)
{
    static const uint8_t mapped[12] = {0,0,0,0,0,0,0,0,0,0,255,255};
    if (ip.width == 16 && memcmp(ip.bytes, mapped, sizeof(mapped)) == 0) {
        memmove(ip.bytes, ip.bytes + 12, 4);
        memset(ip.bytes + 4, 0, 12);
        ip.width = 4;
    }
    return ip;
}
fips_status fips_onlink_from_indexed(fips_onlink_prefixes *out,
    fips_onlink_prefix *storage, size_t capacity, const fips_onlink_prefix *entries, size_t count)
{
    size_t kept = 0;
    for (size_t i = 0; i < count; ++i) {
        if (entries[i].net.width != 4 && entries[i].net.width != 16) return FIPS_ERR_RANGE;
        if (entries[i].len != 0) ++kept;
    }
    if (kept > capacity) return FIPS_ERR_RANGE;
    size_t pos = 0;
    for (size_t i = 0; i < count; ++i) {
        if (entries[i].len == 0) continue;
        fips_onlink_prefix p = entries[i];
        p.net = canonical(p.net);
        uint8_t width = (uint8_t)(p.net.width * 8);
        if (p.len > width) p.len = width;
        storage[pos++] = p;
    }
    *out = (fips_onlink_prefixes){storage, kept, count - kept, false};
    return FIPS_OK;
}
fips_status fips_onlink_from_pairs(fips_onlink_prefixes *out,
    fips_onlink_prefix *storage, size_t capacity, const fips_onlink_prefix *entries, size_t count)
{
    fips_status status = fips_onlink_from_indexed(out, storage, capacity, entries, count);
    if (status == FIPS_OK)
        for (size_t i = 0; i < out->count; ++i) out->entries[i].has_index = false;
    return status;
}
fips_onlink_prefixes fips_onlink_failed(void) { return (fips_onlink_prefixes){.failed = true}; }
bool fips_onlink_read_failed(const fips_onlink_prefixes *set) { return set->failed; }
size_t fips_onlink_discarded(const fips_onlink_prefixes *set) { return set->discarded; }
bool fips_onlink_is_empty(const fips_onlink_prefixes *set) { return set->count == 0; }
static bool holds(const fips_onlink_prefix *p, fips_ip_addr ip)
{
    if (p->net.width != ip.width) return false;
    size_t whole = p->len / 8;
    unsigned bits = p->len % 8;
    if (whole != 0 && memcmp(p->net.bytes, ip.bytes, whole) != 0) return false;
    return bits == 0 || ((p->net.bytes[whole] ^ ip.bytes[whole]) & (255u << (8 - bits))) == 0;
}
bool fips_onlink_contains(const fips_onlink_prefixes *set, fips_ip_addr ip)
{
    ip = canonical(ip);
    for (size_t i = 0; i < set->count; ++i) if (holds(&set->entries[i], ip)) return true;
    return false;
}
bool fips_onlink_contains_on(const fips_onlink_prefixes *set,
    fips_ip_addr ip, const uint32_t *interfaces, size_t n)
{
    ip = canonical(ip);
    for (size_t i = 0; i < set->count; ++i) {
        const fips_onlink_prefix *p = &set->entries[i];
        if (!p->has_index) continue;
        for (size_t j = 0; j < n; ++j)
            if (p->index == interfaces[j] && holds(p, ip)) return true;
    }
    return false;
}
bool fips_onlink_share_link(const fips_onlink_prefixes *set, fips_ip_addr a, fips_ip_addr b)
{
    a = canonical(a); b = canonical(b);
    for (size_t i = 0; i < set->count; ++i)
        if (holds(&set->entries[i], a) && holds(&set->entries[i], b)) return true;
    return false;
}
bool fips_onlink_read_suspect(size_t considered, const fips_onlink_prefixes *set)
{
    return set->failed || (considered > 0 && set->count == 0);
}
