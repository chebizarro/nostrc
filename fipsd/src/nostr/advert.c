#include "nostr/advert.h"
#include "nostr/internal.h"
#include <nostr/nip40/nip40.h>
#include <fips/transport/types.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *transport(fipsd_nostr_transport t)
{
    switch (t) { case FIPSD_NOSTR_UDP: return "udp"; case FIPSD_NOSTR_TCP: return "tcp";
        case FIPSD_NOSTR_TOR: return "tor"; default: return NULL; }
}
static bool public_ip(const char *host)
{
    uint8_t a[16];
    if (inet_pton(AF_INET, host, a) == 1) {
        return !(a[0] == 10 || a[0] == 127 || (a[0] >= 224 && a[0] <= 239) ||
            (!a[0] && !a[1] && !a[2] && !a[3]) ||
            (a[0] == 255 && a[1] == 255 && a[2] == 255 && a[3] == 255) ||
            (a[0] == 100 && (a[1] & 192) == 64) ||
            (a[0] == 169 && a[1] == 254) || (a[0] == 172 && (a[1] & 240) == 16) ||
            (a[0] == 192 && a[1] == 168) ||
            (a[0] == 192 && a[1] == 0 && a[2] == 2) ||
            (a[0] == 198 && a[1] == 51 && a[2] == 100) ||
            (a[0] == 203 && a[1] == 0 && a[2] == 113));
    }
    if (inet_pton(AF_INET6, host, a) == 1) {
        static const uint8_t zero[16] = {0}, loop[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
        if (!memcmp(a, zero, 16) || !memcmp(a, loop, 16) || a[0] == 255 ||
            (a[0] == 254 && (a[1] & 192) == 128) || (a[0] & 254) == 252) return false;
        /* Rust's IpAddr checks IPv4-mapped IPv6 as IPv6, not as IPv4. */
    }
    return true;
}
static bool usable(const fipsd_nostr_endpoint *e)
{
    char addr[FIPSD_NOSTR_ADDR_MAX + 1];
    size_t n = strnlen(e->addr, sizeof(addr));
    if (n == sizeof(addr)) return false;
    memcpy(addr, e->addr, n + 1);
    while (n && isspace((unsigned char)addr[n - 1])) addr[--n] = 0;
    char *s = addr; while (isspace((unsigned char)*s)) ++s;
    if (!*s || !transport(e->transport)) return false;
    if (!strcasecmp(s, "nat")) return e->transport == FIPSD_NOSTR_UDP;
    if (e->transport == FIPSD_NOSTR_TOR) return true;
    char *colon = strrchr(s, ':');
    if (!colon) return false;
    *colon++ = 0; char *end;
    unsigned long port = strtoul(colon, &end, 10);
    while (isspace((unsigned char)*end)) ++end;
    if (!*colon || *end || port == 0 || port > UINT16_MAX) return false;
    size_t h = strlen(s);
    while (h && (isspace((unsigned char)s[h - 1]) || s[h - 1] == ']')) s[--h] = 0;
    while (isspace((unsigned char)*s) || *s == '[') ++s;
    return *s && strcasecmp(s, "localhost") && public_ip(s);
}
static bool nat(const fipsd_nostr_advert *a)
{
    for (size_t i = 0; i < a->endpoint_count; ++i)
        if (a->endpoints[i].transport == FIPSD_NOSTR_UDP && !strcasecmp(a->endpoints[i].addr, "nat")) return true;
    return false;
}
static bool bounded(const fipsd_nostr_advert *a)
{
    if (!a || !a->endpoint_count || a->endpoint_count > FIPSD_NOSTR_ENDPOINTS_MAX ||
        a->relay_count > FIPSD_NOSTR_RELAYS_MAX || a->stun_count > FIPSD_NOSTR_STUN_MAX ||
        (!a->has_relays && a->relay_count) || (!a->has_stun && a->stun_count)) return false;
    for (size_t i = 0; i < a->endpoint_count; ++i)
        if (!transport(a->endpoints[i].transport) || strnlen(a->endpoints[i].addr, FIPSD_NOSTR_ADDR_MAX + 1) > FIPSD_NOSTR_ADDR_MAX) return false;
    for (size_t i = 0; i < a->relay_count; ++i)
        if (strnlen(a->relays[i], FIPSD_NOSTR_URL_MAX + 1) > FIPSD_NOSTR_URL_MAX) return false;
    for (size_t i = 0; i < a->stun_count; ++i)
        if (strnlen(a->stun[i], FIPSD_NOSTR_URL_MAX + 1) > FIPSD_NOSTR_URL_MAX) return false;
    return !nat(a) || (a->has_relays && a->relay_count && a->has_stun && a->stun_count);
}
/* Rust IpAddr/SocketAddr FromStr: a dotted quad (no leading zeros) or RFC
 * 4291 text, bracketed in a socket address; the port is decimal (zero prefix
 * allowed). No hostnames, no IPv6 zone ids. */
static bool parse_port(const char *s, size_t n, uint16_t *out)
{
    unsigned long v = 0;
    if (!n) return false;
    for (size_t i = 0; i < n; ++i) {
        if (s[i] < '0' || s[i] > '9' || (v = v * 10 + (unsigned long)(s[i] - '0')) > UINT16_MAX) return false;
    }
    *out = (uint16_t)v; return true;
}
/* Rust's dotted quad: four 1-3 digit octets <= 255, no leading zero. Some
 * libc inet_pton (macOS) accept "01.2.3.4"; Rust and glibc do not. */
static bool dotted_quad(const char *s)
{
    for (int octet = 0; octet < 4; ++octet) {
        unsigned v = 0; int digits = 0;
        while (s[digits] >= '0' && s[digits] <= '9' && digits < 4) v = v * 10 + (unsigned)(s[digits++] - '0');
        if (!digits || digits > 3 || v > 255 || (digits > 1 && s[0] == '0')) return false;
        s += digits;
        if (octet < 3 && *s++ != '.') return false;
    }
    return !*s;
}
static bool parse_ip(const char *s, size_t n, int *family, uint8_t ip[16])
{
    char text[64];
    if (!n || n >= sizeof text) return false;
    memcpy(text, s, n); text[n] = 0;
    if (!strchr(text, ':')) {
        *family = AF_INET; return dotted_quad(text) && inet_pton(AF_INET, text, ip) == 1;
    }
    const char *tail = strrchr(text, ':') + 1; /* an embedded IPv4 is the last group */
    *family = AF_INET6;
    return (!strchr(tail, '.') || dotted_quad(tail)) && inet_pton(AF_INET6, text, ip) == 1;
}
static bool parse_socket(const char *s, int *family, uint8_t ip[16], uint16_t *port)
{
    size_t n = s ? strlen(s) : 0;
    if (n && s[0] == '[') {
        const char *close = memchr(s, ']', n);
        return close && close[1] == ':' && parse_ip(s + 1, (size_t)(close - s - 1), family, ip) &&
            *family == AF_INET6 && parse_port(close + 2, n - (size_t)(close + 2 - s), port);
    }
    const char *colon = n ? strrchr(s, ':') : NULL;
    return colon && parse_ip(s, (size_t)(colon - s), family, ip) && *family == AF_INET &&
        parse_port(colon + 1, n - (size_t)(colon + 1 - s), port);
}
/* Rust SocketAddr Display, through the core's canonical address text. */
static bool socket_text(int family, const uint8_t ip[16], uint16_t port, char out[FIPSD_NOSTR_ADDR_MAX + 1])
{
    fips_transport_addr a; const char *text; size_t len;
    if (fips_transport_addr_from_socket_addr(ip, family == AF_INET ? 4 : 16, port, 0, &a) != FIPS_OK ||
        fips_transport_addr_as_str(&a, &text, &len) != FIPS_OK || len > FIPSD_NOSTR_ADDR_MAX) return false;
    memcpy(out, text, len); out[len] = 0; return true;
}
bool fipsd_nostr_advert_external(const char *raw, const char *bind, char out[FIPSD_NOSTR_ADDR_MAX + 1])
{
    int family; uint8_t ip[16]; uint16_t port, bind_port;
    if (!raw || !out || !parse_socket(bind, &family, ip, &bind_port)) return false;
    if (parse_socket(raw, &family, ip, &port)) return socket_text(family, ip, port, out);
    return parse_ip(raw, strlen(raw), &family, ip) && socket_text(family, ip, bind_port, out);
}
static const char wildcard_udp[] = "udp public=true bound to wildcard but STUN observation failed; advertising no UDP "
    "endpoint. Either set transports.udp.external_addr, bind to a specific public IP, or ensure "
    "node.rendezvous.nostr.stun_servers is reachable";
static const char wildcard_tcp[] = "tcp advertise_on_nostr=true bound to wildcard and no transports.tcp.external_addr "
    "set; advertising no TCP endpoint. Either set external_addr to the public IP (recommended for cloud 1:1-NAT "
    "setups) or bind explicitly to the public IP";
bool fipsd_nostr_advert_source_endpoint(const fipsd_nostr_advert_source *s, fipsd_nostr_endpoint *e, const char **why)
{
    static const uint8_t zero[16] = {0};
    if (why) *why = NULL;
    if (!s || !e || !s->advertise || !transport(s->transport)) return false;
    *e = (fipsd_nostr_endpoint){.transport = s->transport};
    if (s->transport == FIPSD_NOSTR_TOR) {
        int n = s->onion ? snprintf(e->addr, sizeof e->addr, "%s:%u", s->onion, (unsigned)s->advertised_port) : -1;
        return n > 0 && (size_t)n < sizeof e->addr;
    }
    if (s->transport == FIPSD_NOSTR_UDP && !s->is_public) { strcpy(e->addr, "nat"); return true; }
    if (s->external) {
        size_t n = strnlen(s->external, sizeof e->addr);
        if (n == sizeof e->addr) return false;
        memcpy(e->addr, s->external, n + 1); return true;
    }
    int family; uint8_t ip[16]; uint16_t port;
    if (!parse_socket(s->local, &family, ip, &port)) return false;
    if (memcmp(ip, zero, family == AF_INET ? 4 : 16)) return socket_text(family, ip, port, e->addr);
    /* Wildcard: UDP publishes the STUN-observed IP with the bound port (the
     * port 1:1 NAT preserves); TCP has no STUN equivalent. */
    if (s->transport == FIPSD_NOSTR_UDP && s->reflexive && parse_ip(s->reflexive, strlen(s->reflexive), &family, ip))
        return socket_text(family, ip, port, e->addr);
    if (why) *why = s->transport == FIPSD_NOSTR_UDP ? wildcard_udp : wildcard_tcp;
    return false;
}
bool fipsd_nostr_advert_publishable(fipsd_nostr_advert *a)
{
    if (!a || a->endpoint_count > FIPSD_NOSTR_ENDPOINTS_MAX) return false;
    size_t kept = 0;
    for (size_t i = 0; i < a->endpoint_count; ++i)
        if (usable(&a->endpoints[i])) a->endpoints[kept++] = a->endpoints[i];
    a->endpoint_count = kept;
    if (!kept) return false;
    if (nat(a)) return a->has_relays && a->relay_count && a->has_stun && a->stun_count;
    a->has_relays = a->has_stun = false; a->relay_count = a->stun_count = 0;
    return true;
}
static void strings(fn_writer *w, const char *key, const char *base, size_t count, size_t stride)
{
    fn_raw(w, key);
    for (size_t i = 0; i < count; ++i) { if (i) fn_raw(w, ","); fn_quote(w, base + i * stride, stride); }
    fn_raw(w, "]");
}
char *fipsd_nostr_advert_content(const fipsd_nostr_advert *a)
{
    if (!bounded(a)) return NULL;
    fn_writer w = {.ok = true};
    fn_raw(&w, "{\"identifier\":\"" FIPSD_NOSTR_IDENTIFIER "\",\"version\":1,\"endpoints\":[");
    for (size_t i = 0; i < a->endpoint_count; ++i) {
        if (i) fn_raw(&w, ",");
        fn_raw(&w, "{\"transport\":"); fn_quote(&w, transport(a->endpoints[i].transport), 4);
        fn_raw(&w, ",\"addr\":"); fn_quote(&w, a->endpoints[i].addr, sizeof(a->endpoints[i].addr)); fn_raw(&w, "}");
    }
    fn_raw(&w, "]");
    if (a->has_relays) strings(&w, ",\"signalRelays\":[", &a->relays[0][0], a->relay_count, sizeof(a->relays[0]));
    if (a->has_stun) strings(&w, ",\"stunServers\":[", &a->stun[0][0], a->stun_count, sizeof(a->stun[0]));
    fn_raw(&w, "}"); return fn_finish(&w);
}
fipsd_nostr_status fipsd_nostr_advert_build(const fips_identity *id, const fipsd_nostr_advert *a,
    const char *app, uint64_t at, uint64_t ttl, NostrEvent **out)
{
    if (!id || !out || !app || strnlen(app, 129) > 128 || !*app ||
        at > INT64_MAX || ttl > (uint64_t)INT64_MAX - at) return FIPSD_NOSTR_INVALID;
    char *content = fipsd_nostr_advert_content(a);
    if (!content) return FIPSD_NOSTR_LIMIT;
    NostrEvent *e = nostr_event_new();
    if (!e) { free(content); return FIPSD_NOSTR_LIMIT; }
    nostr_event_set_kind(e, NOSTR_KIND_FIPS_OVERLAY_ADVERT);
    nostr_event_set_created_at(e, (int64_t)at); nostr_event_set_content(e, content); free(content);
    NostrTags *tags = nostr_tags_new(3,
        nostr_tag_new("d", FIPSD_NOSTR_IDENTIFIER, NULL),
        nostr_tag_new("protocol", app, NULL), nostr_tag_new("version", "1", NULL));
    nostr_event_set_tags(e, tags);
    fipsd_nostr_status rc = FIPSD_NOSTR_LIMIT;
    if (nostr_nip40_set_expiration(e, (int64_t)(at + ttl)) == 0) rc = fn_sign(e, id);
    if (rc != FIPSD_NOSTR_OK) { nostr_event_free(e); return rc; }
    *out = e; return rc;
}
static bool list(const fn_json *j, unsigned t, char *out, size_t cap, size_t *count, bool *present)
{
    *present = !fn_null(j, t); *count = 0;
    if (!*present) return true;
    if (j->t[t].type != '[' || j->t[t].count > cap) return false;
    for (unsigned k = t + 1; k < j->t[t].next; k = j->t[k].next)
        if (!fn_string(j, k, out + (*count)++ * (FIPSD_NOSTR_URL_MAX + 1), FIPSD_NOSTR_URL_MAX + 1)) return false;
    return true;
}
fipsd_nostr_status fipsd_nostr_advert_parse(const NostrEvent *e, const char *app,
    uint64_t now, uint64_t max_age, fipsd_nostr_advert *out, uint64_t *until)
{
    if (!out || !until || !app || !fn_tags_bounded(e) || e->kind != NOSTR_KIND_FIPS_OVERLAY_ADVERT ||
        !fn_tag(e, "protocol") || strcmp(fn_tag(e, "protocol"), app) ||
        !fn_tag(e, "d") || strcmp(fn_tag(e, "d"), FIPSD_NOSTR_IDENTIFIER) ||
        !fn_tag(e, "version") || strcmp(fn_tag(e, "version"), "1")) return FIPSD_NOSTR_INVALID;
    fn_json j;
    if (!fn_parse(&j, e->content, strnlen(e->content, FIPSD_NOSTR_BODY_MAX + 1), FIPSD_NOSTR_BODY_MAX)) return FIPSD_NOSTR_LIMIT;
    uint64_t version;
    if (!fn_eq(&j, fn_field(&j, 1, "identifier"), FIPSD_NOSTR_IDENTIFIER) ||
        !fn_u64(&j, fn_field(&j, 1, "version"), &version) || version != 1) return FIPSD_NOSTR_INVALID;
    unsigned ends = fn_field(&j, 1, "endpoints");
    if (!ends || j.t[ends].type != '[' || !j.t[ends].count || j.t[ends].count > FIPSD_NOSTR_ENDPOINTS_MAX) return FIPSD_NOSTR_LIMIT;
    fipsd_nostr_advert a = {0};
    for (unsigned k = ends + 1; k < j.t[ends].next; k = j.t[k].next) {
        fipsd_nostr_endpoint ep = {0}; char name[65];
        if (!fn_string(&j, fn_field(&j, k, "transport"), name, sizeof(name)) ||
            !fn_string(&j, fn_field(&j, k, "addr"), ep.addr, sizeof(ep.addr))) return FIPSD_NOSTR_INVALID;
        if (!strcmp(name, "udp")) ep.transport = FIPSD_NOSTR_UDP;
        else if (!strcmp(name, "tcp")) ep.transport = FIPSD_NOSTR_TCP;
        else if (!strcmp(name, "tor")) ep.transport = FIPSD_NOSTR_TOR;
        else continue; /* serde(other), then retain only known usable endpoints. */
        if (usable(&ep)) a.endpoints[a.endpoint_count++] = ep;
    }
    if (!list(&j, fn_field(&j, 1, "signalRelays"), &a.relays[0][0], FIPSD_NOSTR_RELAYS_MAX, &a.relay_count, &a.has_relays) ||
        !list(&j, fn_field(&j, 1, "stunServers"), &a.stun[0][0], FIPSD_NOSTR_STUN_MAX, &a.stun_count, &a.has_stun) || !bounded(&a)) return FIPSD_NOSTR_INVALID;
    if (!nat(&a)) { a.has_relays = a.has_stun = false; a.relay_count = a.stun_count = 0; }
    if (e->created_at < 0) return FIPSD_NOSTR_INVALID;
    uint64_t created = (uint64_t)e->created_at, ceiling = fn_add(now, FIPSD_NOSTR_SKEW_MS) / 1000;
    uint64_t horizon = fn_add(fn_mul1000(created < ceiling ? created : ceiling), max_age);
    int64_t expiry;
    int rc = nostr_nip40_get_expiration(e, &expiry);
    if (rc == 0) { if (expiry < 0) return FIPSD_NOSTR_INVALID;
        uint64_t ms = fn_mul1000((uint64_t)expiry); if (ms < horizon) horizon = ms; }
    else if (fn_tag(e, "expiration")) return FIPSD_NOSTR_INVALID;
    if (horizon <= now) return FIPSD_NOSTR_STALE;
    if (nostr_event_validate(e, NULL) != NOSTR_EVENT_VALIDATION_OK) return FIPSD_NOSTR_CRYPTO;
    *out = a; *until = horizon; return FIPSD_NOSTR_OK;
}
