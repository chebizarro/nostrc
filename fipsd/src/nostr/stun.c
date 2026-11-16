#include "nostr/stun.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
static const uint8_t cookie[4] = {0x21, 0x12, 0xa4, 0x42};
static uint16_t u16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] << 8 | p[1]); }
static uint64_t add(uint64_t a, uint64_t b) { return UINT64_MAX - a < b ? UINT64_MAX : a + b; }
void fipsd_stun_request(const uint8_t id[12], uint8_t out[20])
{ memset(out, 0, 20); out[1] = 1; memcpy(out + 4, cookie, 4); memcpy(out + 8, id, 12); }
bool fipsd_stun_packet(const uint8_t *p, size_t n)
{ return n >= 8 && !(p[0] & 0xc0) && !memcmp(p + 4, cookie, 4); }
bool fipsd_stun_response(const uint8_t *p, size_t n, const uint8_t id[12], fips_transport_addr *out)
{
    if (n < 20 || u16(p) != 0x0101 || memcmp(p + 4, cookie, 4) || memcmp(p + 8, id, 12)) return false;
    size_t length = u16(p + 2), end = 20 + length;
    if (length % 4 || end > n) return false;
    fips_transport_addr found = {0}; bool have = false;
    for (size_t at = 20; at < end;) {
        if (end - at < 4) return false;
        uint16_t type = u16(p + at); size_t len = u16(p + at + 2), padded = (len + 3) & ~(size_t)3;
        at += 4;
        if (padded > end - at) return false;
        const uint8_t *v = p + at;
        if ((type == 1 || type == 0x20) && len >= 8) {
            int family = v[1] == 1 ? AF_INET : v[1] == 2 ? AF_INET6 : 0;
            size_t bytes = family == AF_INET ? 4 : 16;
            if (family && len >= bytes + 4) {
                uint8_t ip[16]; uint16_t port = u16(v + 2);
                if (type == 0x20) port ^= 0x2112;
                for (size_t i = 0; i < bytes; ++i) ip[i] = v[4 + i] ^
                    (type == 0x20 ? (i < 4 ? cookie[i] : id[i - 4]) : 0);
                char host[INET6_ADDRSTRLEN], text[80];
                if (!port || !inet_ntop(family, ip, host, sizeof host)) return false;
                snprintf(text, sizeof text, family == AF_INET ? "%s:%u" : "[%s]:%u", host, port);
                if (!have || type == 0x20) {
                    if (fips_transport_addr_from_string(text, strlen(text), &found) != FIPS_OK) return false;
                    have = true;
                }
            }
        }
        at += padded;
    }
    if (have) *out = found;
    return have;
}
bool fipsd_stun_server(const char *s, fips_transport_addr *out)
{
    if (!s) return false;
    if (!strncmp(s, "stun:", 5)) s += 5;
    const char *colon = strrchr(s, ':');
    if (!colon || colon == s || !colon[1]) return false;
    unsigned port = 0;
    for (const char *p = colon + 1; *p; ++p) {
        if (*p < '0' || *p > '9' || port > 6553) return false;
        port = port * 10 + (unsigned)(*p - '0');
    }
    return port && port <= 65535 && fips_transport_addr_from_string(s, strlen(s), out) == FIPS_OK;
}
void fipsd_stun_begin(fipsd_stun *s, const fips_transport_addr *server, const uint8_t id[12], uint64_t now, uint64_t timeout)
{
    *s = (fipsd_stun){.server = *server, .deadline = add(now, timeout), .resend_at = now,
        .rto = FIPSD_STUN_RTO_MS, .active = true};
    memcpy(s->id, id, 12); fipsd_stun_request(id, s->request);
}
fipsd_stun_action fipsd_stun_tick(fipsd_stun *s, uint64_t now)
{
    if (s->complete) return FIPSD_STUN_DONE;
    if (!s->active || now >= s->deadline) { s->active = false; return FIPSD_STUN_FAILED; }
    if (now < s->resend_at) return FIPSD_STUN_WAIT;
    s->resend_at = add(now, s->rto); s->rto = add(s->rto, s->rto); return FIPSD_STUN_SEND;
}
bool fipsd_stun_receive(fipsd_stun *s, const fips_transport_addr *source, const uint8_t *p, size_t n)
{
    if (!s->active || !fips_transport_addr_equal(source, &s->server) ||
        !fipsd_stun_response(p, n, s->id, &s->mapped)) return false;
    s->complete = true; s->active = false; return true;
}
uint64_t fipsd_stun_deadline(const fipsd_stun *s)
{ return !s->active ? UINT64_MAX : s->resend_at < s->deadline ? s->resend_at : s->deadline; }
int fipsd_stun_send(const fipsd_stun *s, fipsd_transport *t)
{ return t->ops->send(t, &s->server, s->request, sizeof s->request); }
