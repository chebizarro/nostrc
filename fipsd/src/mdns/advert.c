/* LAN advert checks: upstream src/mdns/mod.rs's pure half. */
#include "mdns/advert.h"
#include <fips/transport/types.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

bool fipsd_mdns_advert_label_of(const char *npub, size_t len, char out[FIPSD_MDNS_LABEL_SIZE])
{
    if (len < 16) return false;
    for (size_t i = 0; i < 16; ++i) if ((unsigned char)npub[i] >= 0x80 || !npub[i]) return false;
    memcpy(out, "fips-", 5); memcpy(out + 5, npub, 16); out[21] = 0;
    return true;
}
void fipsd_mdns_advert_label(const char *npub, char *out, size_t cap)
{
    char label[FIPSD_MDNS_LABEL_SIZE];
    if (fipsd_mdns_advert_label_of(npub, strlen(npub), label)) snprintf(out, cap, "%s", label);
    else snprintf(out, cap, "fips-%s", npub);
}
bool fipsd_mdns_host_is_advertisers(const char *host, const char *npub, size_t npub_len)
{
    char expected[FIPSD_MDNS_LABEL_SIZE];
    if (!fipsd_mdns_advert_label_of(npub, npub_len, expected)) return false;
    size_t n = strlen(host);
    char buf[FIPSD_MDNS_TEXT_MAX];
    if (n >= sizeof buf) return false;
    for (size_t i = 0; i <= n; ++i) buf[i] = lower(host[i]);
    if (n >= 7 && !strcmp(buf + n - 7, ".local.")) n -= 7;
    else if (n >= 6 && !strcmp(buf + n - 6, ".local")) n -= 6;
    else return false;
    buf[n] = 0;
    for (size_t i = 0; expected[i]; ++i) expected[i] = lower(expected[i]);
    size_t e = strlen(expected);
    if (n < e || memcmp(buf, expected, e)) return false;
    const char *rest = buf + e;
    if (!*rest) return true;
    if (*rest++ != '-' || !*rest) return false;
    for (; *rest; ++rest) if (*rest < '0' || *rest > '9') return false;
    return true;
}
static bool bytes_is(const uint8_t *p, size_t n, const char *s) { return n == strlen(s) && !memcmp(p, s, n); }
static bool ieq(const char *a, size_t an, const char *b, size_t bn)
{
    if (an != bn) return false;
    for (size_t i = 0; i < an; ++i) if (lower(a[i]) != lower(b[i])) return false;
    return true;
}
static void copy_value(char *dst, size_t *dlen, const uint8_t *v, size_t n)
{
    if (n >= FIPSD_MDNS_TEXT_MAX) n = FIPSD_MDNS_TEXT_MAX - 1;
    memcpy(dst, v, n); dst[n] = 0; *dlen = n;
}
fipsd_mdns_skip fipsd_mdns_check_advert(const fipsd_mdns_resolved *info, const char *own_npub,
                                        const char *scope_filter, fipsd_mdns_advert_fields *out)
{
    memset(out, 0, sizeof *out);
    bool has_npub = false;
    size_t pos = 0; const uint8_t *k, *v; size_t kl, vl; bool hv;
    while (fipsd_mdns_txt_next(info->txt, info->txt_len, &pos, &k, &kl, &v, &vl, &hv)) {
        if (bytes_is(k, kl, FIPSD_MDNS_TXT_NPUB)) { copy_value(out->npub, &out->npub_len, v, vl); has_npub = true; }
        else if (bytes_is(k, kl, FIPSD_MDNS_TXT_SCOPE)) { copy_value(out->scope, &out->scope_len, v, vl); out->has_scope = true; }
    }
    if (!has_npub) return FIPSD_MDNS_SKIP_NO_NPUB;
    /* An npub names its key in any letter case. */
    if (ieq(out->npub, out->npub_len, own_npub, strlen(own_npub))) return FIPSD_MDNS_SKIP_OWN;
    if (scope_filter && (!out->has_scope || out->scope_len != strlen(scope_filter) ||
                         memcmp(out->scope, scope_filter, out->scope_len)))
        return FIPSD_MDNS_SKIP_CROSS_SCOPE;
    if (!fipsd_mdns_host_is_advertisers(info->host, out->npub, out->npub_len)) return FIPSD_MDNS_SKIP_FOREIGN_HOST;
    out->port = info->port;
    if (!info->port) return FIPSD_MDNS_SKIP_ZERO_PORT;
    return FIPSD_MDNS_ADVERT_OK;
}
size_t fipsd_mdns_short_len(const char *npub, size_t len)
{
    size_t end = len < 16 ? len : 16;
    while (end > 0 && end < len && ((unsigned char)npub[end] & 0xC0) == 0x80) --end;
    return end;
}
/* Rust's Debug of Option<&str>, bytes outside printable ASCII escaped. */
static void option_text(bool has, const char *s, size_t n, char *out, size_t cap)
{
    if (!has) { snprintf(out, cap, "None"); return; }
    size_t at = (size_t)snprintf(out, cap, "Some(\"");
    for (size_t i = 0; i < n && at + 8 < cap; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') { out[at++] = '\\'; out[at++] = (char)c; }
        else if (c >= 0x20 && c < 0x7f) out[at++] = (char)c;
        else at += (size_t)snprintf(out + at, cap - at, "\\x%02x", c);
    }
    snprintf(out + at, cap - at, "\")");
}
const char *fipsd_mdns_skip_line(const fipsd_mdns_resolved *info, fipsd_mdns_skip skip,
                                 const fipsd_mdns_advert_fields *f, const char *scope_filter, char *out, size_t cap)
{
    char theirs[FIPSD_MDNS_TEXT_MAX * 4 + 16], ours[FIPSD_MDNS_TEXT_MAX * 4 + 16];
    switch (skip) {
    case FIPSD_MDNS_SKIP_NO_NPUB:
        snprintf(out, cap, "lan: skip advert without npub TXT instance=%s", info->fullname); break;
    case FIPSD_MDNS_SKIP_CROSS_SCOPE:
        option_text(f->has_scope, f->scope, f->scope_len, theirs, sizeof theirs);
        option_text(scope_filter != NULL, scope_filter ? scope_filter : "", scope_filter ? strlen(scope_filter) : 0,
                    ours, sizeof ours);
        snprintf(out, cap, "lan: skip cross-scope advert npub=%.*s their_scope=%s our_scope=%s",
                 (int)fipsd_mdns_short_len(f->npub, f->npub_len), f->npub, theirs, ours);
        break;
    case FIPSD_MDNS_SKIP_FOREIGN_HOST:
        snprintf(out, cap, "lan: skip advert whose service host is not the advertised node's instance=%s host=%s reason=foreign-host",
                 info->fullname, info->host);
        break;
    default:
        if (cap) out[0] = 0;
        break;
    }
    return out;
}
bool fipsd_mdns_ipv6_is_unicast_link_local(const uint8_t a[16]) { return a[0] == 0xfe && (a[1] & 0xc0) == 0x80; }
bool fipsd_mdns_socket_addr(const fipsd_mdns_scoped_ip *scoped, uint16_t port, fipsd_mdns_sockaddr *out)
{
    *out = (fipsd_mdns_sockaddr){.ip = scoped->ip, .port = port};
    if (scoped->ip.width == 4) return true;
    if (scoped->ip.width != 16) return false;
    if (fipsd_mdns_ipv6_is_unicast_link_local(scoped->ip.bytes) && !scoped->scope) return false;
    out->scope_id = scoped->scope;
    return true;
}
size_t fipsd_mdns_arrival_interfaces(const fipsd_mdns_scoped_ip *scoped, uint32_t out[FIPSD_MDNS_MAX_IFACES])
{
    size_t n = 0;
    if (scoped->ip.width == 4) {
        for (size_t i = 0; i < scoped->iface_count && i < FIPSD_MDNS_MAX_IFACES; ++i)
            if (scoped->ifaces[i]) out[n++] = scoped->ifaces[i];
    } else if (scoped->ip.width == 16 && scoped->scope) out[n++] = scoped->scope;
    return n;
}
size_t fipsd_mdns_sockaddr_text(const fipsd_mdns_sockaddr *a, char *out, size_t cap)
{
    fips_transport_addr t; const char *text; size_t len;
    if (!cap) return 0;
    if (fips_transport_addr_from_socket_addr(a->ip.bytes, a->ip.width, a->port, a->scope_id, &t) != FIPS_OK ||
        fips_transport_addr_as_str(&t, &text, &len) != FIPS_OK || len >= cap) { out[0] = 0; return 0; }
    memcpy(out, text, len); out[len] = 0;
    return len;
}
static void trimmed(const char *s, const char **start, size_t *len)
{
    if (!s) { *start = ""; *len = 0; return; }
    while (*s && isspace((unsigned char)*s)) ++s;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) --n;
    *start = s; *len = n;
}
bool fipsd_mdns_resolve_scope(const char *lan_scope, const char *nostr_app, char *out, size_t cap)
{
    const char *s; size_t n;
    trimmed(lan_scope, &s, &n);
    if (n) { snprintf(out, cap, "%.*s", (int)n, s); return true; }
    trimmed(nostr_app, &s, &n);
    if (!n) return false;
    static const char prefix[] = "fips-overlay-v1:";
    if (n >= sizeof prefix - 1 && !memcmp(s, prefix, sizeof prefix - 1)) {
        const char *rest = s + sizeof prefix - 1; size_t rn = n - (sizeof prefix - 1);
        while (rn && isspace((unsigned char)*rest)) { ++rest; --rn; }
        if (!rn) return false;
        snprintf(out, cap, "%.*s", (int)rn, rest);
        return true;
    }
    snprintf(out, cap, "%.*s", (int)n, s);
    return true;
}
