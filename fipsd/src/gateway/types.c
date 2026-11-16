#include "gateway/types.h"
#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

fipsd_gw_instant fipsd_gw_now(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) abort();
    return (uint64_t)t.tv_sec * FIPSD_GW_SEC + (uint64_t)t.tv_nsec;
}
fipsd_gw_instant fipsd_gw_later(fipsd_gw_instant now, uint64_t secs)
{
    if (secs > UINT64_MAX / FIPSD_GW_SEC) return UINT64_MAX;
    uint64_t add = secs * FIPSD_GW_SEC;
    return now > UINT64_MAX - add ? UINT64_MAX : now + add;
}
int fipsd_gw_events_push(fipsd_gw_events *events, const fipsd_gw_pool_event *event)
{
    if (events->len == events->cap) {
        size_t cap = events->cap ? events->cap * 2 : 8;
        fipsd_gw_pool_event *grown = realloc(events->items, cap * sizeof *grown);
        if (!grown) return -1;
        events->items = grown; events->cap = cap;
    }
    events->items[events->len++] = *event;
    return 0;
}
void fipsd_gw_events_clear(fipsd_gw_events *events) { free(events->items); events->items = NULL; events->len = events->cap = 0; }

bool fipsd_gw_ip6_parse(const char *text, struct in6_addr *out) { return text && inet_pton(AF_INET6, text, out) == 1; }
const char *fipsd_gw_ip6_text(const struct in6_addr *addr, char *buf, size_t cap)
{
    if (!inet_ntop(AF_INET6, addr, buf, (socklen_t)cap)) { if (cap) buf[0] = 0; }
    return buf;
}
int fipsd_gw_ip6_cmp(const struct in6_addr *a, const struct in6_addr *b) { return memcmp(a, b, 16); }
bool fipsd_gw_cidr_parse(const char *cidr, struct in6_addr *addr, uint32_t *prefix_len)
{
    if (!cidr) return false;
    const char *slash = strchr(cidr, '/');
    if (!slash || strchr(slash + 1, '/')) return false;
    size_t alen = (size_t)(slash - cidr);
    char text[64];
    if (alen == 0 || alen >= sizeof text) return false;
    memcpy(text, cidr, alen); text[alen] = 0;
    if (!fipsd_gw_ip6_parse(text, addr)) return false;
    const char *p = slash + 1;
    if (!*p) return false;
    for (const char *q = p; *q; ++q) if (*q < '0' || *q > '9') return false;
    errno = 0;
    char *end = NULL;
    unsigned long long v = strtoull(p, &end, 10);
    if (errno || !end || *end || v > UINT32_MAX) return false;
    *prefix_len = (uint32_t)v;
    return true;
}
void fipsd_gw_ip6_mask(uint32_t prefix_len, struct in6_addr *mask)
{
    uint8_t *m = (uint8_t *)mask;
    if (prefix_len > 128) prefix_len = 128;
    for (uint32_t i = 0; i < 16; ++i) {
        uint32_t bits = prefix_len > i * 8 ? prefix_len - i * 8 : 0;
        if (bits >= 8) m[i] = 0xff; else m[i] = (uint8_t)(0xff00 >> bits);
    }
}
void fipsd_gw_ip6_network(const struct in6_addr *addr, uint32_t prefix_len, struct in6_addr *out)
{
    struct in6_addr mask; fipsd_gw_ip6_mask(prefix_len, &mask);
    for (size_t i = 0; i < 16; ++i) ((uint8_t *)out)[i] = ((const uint8_t *)addr)[i] & ((const uint8_t *)&mask)[i];
}
bool fipsd_gw_ip6_in_prefix(const struct in6_addr *addr, const struct in6_addr *network, uint32_t prefix_len)
{
    struct in6_addr masked; fipsd_gw_ip6_network(addr, prefix_len, &masked);
    return fipsd_gw_ip6_eq(&masked, network);
}
void fipsd_gw_ip6_add(const struct in6_addr *base, uint32_t offset, struct in6_addr *out)
{
    uint8_t *o = (uint8_t *)out;
    memcpy(o, base, 16);
    uint64_t carry = offset;
    for (int i = 15; i >= 0 && carry; --i) { carry += o[i]; o[i] = (uint8_t)carry; carry >>= 8; }
}
bool fipsd_gw_ip6_offset(const struct in6_addr *addr, const struct in6_addr *base, uint32_t *offset)
{
    const uint8_t *a = (const uint8_t *)addr, *b = (const uint8_t *)base;
    uint8_t diff[16]; int borrow = 0;
    for (int i = 15; i >= 0; --i) {
        int d = (int)a[i] - (int)b[i] - borrow;
        borrow = d < 0; if (d < 0) d += 256;
        diff[i] = (uint8_t)d;
    }
    if (borrow) return false;
    for (size_t i = 0; i < 12; ++i) if (diff[i]) return false;
    *offset = ((uint32_t)diff[12] << 24) | ((uint32_t)diff[13] << 16) | ((uint32_t)diff[14] << 8) | diff[15];
    return true;
}
