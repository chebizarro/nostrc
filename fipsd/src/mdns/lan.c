/* LAN discovery candidate set: upstream peering/lan.rs. */
#include "mdns/lan.h"
#include <string.h>

static bool node_eq(const fips_node_addr *a, const fips_node_addr *b) { return !memcmp(a->bytes, b->bytes, sizeof a->bytes); }
bool fipsd_lan_nodes_contain(fipsd_lan_nodes set, const fips_node_addr *node)
{
    for (size_t i = 0; i < set.count; ++i) if (node_eq(&set.items[i], node)) return true;
    return false;
}
static bool addr_eq(const fipsd_mdns_sockaddr *a, const fipsd_mdns_sockaddr *b)
{
    return a->ip.width == b->ip.width && !memcmp(a->ip.bytes, b->ip.bytes, a->ip.width) && a->port == b->port &&
        (a->ip.width != 16 || a->scope_id == b->scope_id);
}
/* Rust's IpAddr::to_canonical: an IPv4-mapped IPv6 address as IPv4. */
static fips_ip_addr canonical(fips_ip_addr ip)
{
    static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    if (ip.width == 16 && !memcmp(ip.bytes, mapped, 12)) {
        fips_ip_addr v4 = {.width = 4};
        memcpy(v4.bytes, ip.bytes + 12, 4);
        return v4;
    }
    return ip;
}
static bool ip_eq(fips_ip_addr a, fips_ip_addr b) { return a.width == b.width && !memcmp(a.bytes, b.bytes, a.width); }

bool fipsd_lan_target_on_link(const fipsd_mdns_sockaddr *addr, const uint32_t *ifaces, size_t n,
                              const fips_onlink_prefixes *on_link)
{
    if (addr->ip.width == 16 && fipsd_mdns_ipv6_is_unicast_link_local(addr->ip.bytes)) return addr->scope_id != 0;
    return fips_onlink_contains_on(on_link, addr->ip, n ? ifaces : NULL, n);
}
bool fipsd_lan_mdns_lane(const fipsd_lan_lane *lanes, size_t n, const fipsd_mdns_sockaddr *addr,
                         const uint32_t *ifaces, size_t nif, fips_transport_id *out)
{
    const fipsd_lan_lane *bound = NULL, *any = NULL;
    bool v4 = addr->ip.width == 4;
    for (size_t i = 0; i < n; ++i) {
        const fipsd_lan_lane *l = &lanes[i];
        if (l->v4 != v4) continue;
        if (!any || l->id.value < any->id.value) any = l;
        bool on = false;
        for (size_t k = 0; k < nif && !on; ++k) on = l->ifindex && l->ifindex == ifaces[k];
        if (on && (!bound || l->id.value < bound->id.value)) bound = l;
    }
    const fipsd_lan_lane *pick = bound ? bound : any;
    if (pick) *out = pick->id;
    return pick != NULL;
}
static size_t count_node(const fipsd_lan_pending *s, const fips_node_addr *node)
{
    size_t n = 0;
    for (size_t i = 0; i < s->count; ++i) n += node_eq(&s->pairs[i].identity.node_addr, node);
    return n;
}
static void remove_at(fipsd_lan_pending *s, size_t i)
{
    memmove(&s->pairs[i], &s->pairs[i + 1], sizeof s->pairs[0] * (s->count - i - 1));
    --s->count;
}
fipsd_lan_offer fipsd_lan_offer_pair(fipsd_lan_pending *s, const fipsd_lan_pair *pair, fipsd_lan_nodes configured,
                                     uint64_t now_ms, fipsd_mdns_rng rng, fipsd_lan_pair *displaced, bool *first_at_bound)
{
    fipsd_lan_pair p = *pair;
    p.first_seen_ms = now_ms;
    if (first_at_bound) *first_at_bound = false;
    for (size_t i = 0; i < s->count; ++i)
        if (node_eq(&s->pairs[i].identity.node_addr, &p.identity.node_addr) && addr_eq(&s->pairs[i].addr, &p.addr))
            return FIPSD_LAN_DUPLICATE;
    if (count_node(s, &p.identity.node_addr) >= FIPSD_LAN_MAX_ADDRS_PER_NODE) return FIPSD_LAN_REFUSED_PER_NODE;
    if (s->count < FIPSD_LAN_MAX_PENDING) { s->pairs[s->count++] = p; return FIPSD_LAN_HELD; }
    bool first = !s->latched;
    s->latched = true;
    ++s->at_bound;
    s->has_below_since = false;
    if (first_at_bound) *first_at_bound = first;
    size_t replaceable[FIPSD_LAN_MAX_PENDING], n = 0;
    for (size_t i = 0; i < s->count; ++i)
        if (!fipsd_lan_nodes_contain(configured, &s->pairs[i].identity.node_addr)) replaceable[n++] = i;
    if (!n) return FIPSD_LAN_REFUSED;
    size_t victim = replaceable[rng.uniform(rng.user, (uint32_t)n)];
    if (displaced) *displaced = s->pairs[victim];
    remove_at(s, victim);
    s->pairs[s->count++] = p;
    return FIPSD_LAN_REPLACED;
}
bool fipsd_lan_expire(fipsd_lan_pending *s, uint64_t now_ms, uint64_t *refused)
{
    size_t kept = 0;
    for (size_t i = 0; i < s->count; ++i)
        if (now_ms - s->pairs[i].first_seen_ms < FIPSD_LAN_PENDING_MAX_AGE_MS || now_ms < s->pairs[i].first_seen_ms)
            s->pairs[kept++] = s->pairs[i];
    s->count = kept;
    if (!s->latched) return false;
    if (s->count >= FIPSD_LAN_MAX_PENDING) { s->has_below_since = false; return false; }
    if (!s->has_below_since) { s->has_below_since = true; s->below_since_ms = now_ms; }
    if (now_ms - s->below_since_ms < FIPSD_LAN_BOUND_LOG_HOLD_MS) return false;
    if (refused) *refused = s->at_bound;
    s->latched = false; s->at_bound = 0; s->has_below_since = false;
    return true;
}
void fipsd_lan_prune_connected(fipsd_lan_pending *s, fipsd_lan_nodes connected)
{
    size_t kept = 0;
    for (size_t i = 0; i < s->count; ++i)
        if (!fipsd_lan_nodes_contain(connected, &s->pairs[i].identity.node_addr)) s->pairs[kept++] = s->pairs[i];
    s->count = kept;
}
size_t fipsd_lan_retain_on_link(fipsd_lan_pending *s, const fips_onlink_prefixes *on_link)
{
    size_t kept = 0, before = s->count;
    for (size_t i = 0; i < s->count; ++i)
        if (fipsd_lan_target_on_link(&s->pairs[i].addr, s->pairs[i].ifaces, s->pairs[i].iface_count, on_link))
            s->pairs[kept++] = s->pairs[i];
    s->count = kept;
    return before - kept;
}
typedef struct claim { fips_ip_addr ip; fips_node_addr node; } claim;
static size_t nodes_at(const claim *c, size_t n, fips_ip_addr ip, const fips_node_addr *node, bool *has_node)
{
    size_t count = 0; *has_node = false;
    for (size_t i = 0; i < n; ++i) if (ip_eq(c[i].ip, ip)) { ++count; if (node_eq(&c[i].node, node)) *has_node = true; }
    return count;
}
size_t fipsd_lan_ordered(const fipsd_lan_pending *s, fipsd_lan_nodes configured, fipsd_mdns_rng rng,
                         size_t out[FIPSD_LAN_MAX_PENDING])
{
    size_t n = 0, rest[FIPSD_LAN_MAX_PENDING], nr = 0;
    for (size_t i = 0; i < s->count; ++i) {
        if (fipsd_lan_nodes_contain(configured, &s->pairs[i].identity.node_addr)) out[n++] = i;
        else rest[nr++] = i;
    }
    for (size_t i = nr; i > 1; --i) {
        size_t j = rng.uniform(rng.user, (uint32_t)i), t = rest[i - 1];
        rest[i - 1] = rest[j]; rest[j] = t;
    }
    claim claims[FIPSD_LAN_MAX_PENDING * 2]; size_t nc = 0;
    for (size_t i = 0; i < s->dialed_count; ++i) {
        bool has; fips_ip_addr ip = canonical(s->dialed[i].ip);
        (void)nodes_at(claims, nc, ip, &s->dialed[i].node, &has);
        if (!has) claims[nc++] = (claim){ip, s->dialed[i].node};
    }
    for (size_t i = 0; i < nr; ++i) {
        const fipsd_lan_pair *p = &s->pairs[rest[i]];
        fips_ip_addr ip = canonical(p->addr.ip); bool has;
        size_t at = nodes_at(claims, nc, ip, &p->identity.node_addr, &has);
        if (has) { out[n++] = rest[i]; continue; }
        if (at < FIPSD_LAN_MAX_DIALS_PER_ADDR) { claims[nc++] = (claim){ip, p->identity.node_addr}; out[n++] = rest[i]; }
    }
    return n;
}
void fipsd_lan_take(fipsd_lan_pending *s, const fips_node_addr *node, const fipsd_mdns_sockaddr *addr)
{
    size_t kept = 0;
    for (size_t i = 0; i < s->count; ++i)
        if (!(node_eq(&s->pairs[i].identity.node_addr, node) && addr_eq(&s->pairs[i].addr, addr))) s->pairs[kept++] = s->pairs[i];
    s->count = kept;
}
void fipsd_lan_note_dialed(fipsd_lan_pending *s, const fips_node_addr *node, const fipsd_mdns_sockaddr *addr,
                           uint64_t now_ms, uint64_t wall_ms)
{
    fipsd_lan_dialed d = {.node = *node, .ip = canonical(addr->ip), .at_ms = now_ms, .at_wall_ms = wall_ms};
    for (size_t i = 0; i < s->dialed_count; ++i) if (node_eq(&s->dialed[i].node, node)) { s->dialed[i] = d; return; }
    if (s->dialed_count < FIPSD_LAN_MAX_PENDING) s->dialed[s->dialed_count++] = d;
}
void fipsd_lan_prune_dialed(fipsd_lan_pending *s, bool (*done)(void *user, const fipsd_lan_dialed *d), void *user)
{
    size_t kept = 0;
    for (size_t i = 0; i < s->dialed_count; ++i) if (!done(user, &s->dialed[i])) s->dialed[kept++] = s->dialed[i];
    s->dialed_count = kept;
}
bool fipsd_lan_is_dialed(const fipsd_lan_pending *s, const fips_node_addr *node)
{
    for (size_t i = 0; i < s->dialed_count; ++i) if (node_eq(&s->dialed[i].node, node)) return true;
    return false;
}
uint64_t fipsd_lan_next_expiry(const fipsd_lan_pending *s)
{
    uint64_t at = UINT64_MAX;
    for (size_t i = 0; i < s->count; ++i)
        if (s->pairs[i].first_seen_ms + FIPSD_LAN_PENDING_MAX_AGE_MS < at) at = s->pairs[i].first_seen_ms + FIPSD_LAN_PENDING_MAX_AGE_MS;
    return at;
}
static bool label_key(const char *npub, char out[FIPSD_MDNS_LABEL_SIZE])
{
    if (!npub || !fipsd_mdns_advert_label_of(npub, strlen(npub), out)) return false;
    for (char *c = out; *c; ++c) if (*c >= 'A' && *c <= 'Z') *c = (char)(*c + 32);
    return true;
}
void fipsd_lan_known_labels_add(fipsd_lan_known_labels *k, const char *npub, const fips_node_addr *owner)
{
    if (k->count >= FIPSD_LAN_KNOWN_MAX || !label_key(npub, k->labels[k->count])) return;
    k->owners[k->count++] = *owner;
}
bool fipsd_lan_known_labels_is_impostor(const fipsd_lan_known_labels *k, const char *npub, const fips_node_addr *node)
{
    char label[FIPSD_MDNS_LABEL_SIZE];
    if (!label_key(npub, label)) return false;
    for (size_t i = 0; i < k->count; ++i)
        if (!strcmp(k->labels[i], label)) return !node_eq(&k->owners[i], node);
    return false;
}
