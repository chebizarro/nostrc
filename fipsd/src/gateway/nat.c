#include "gateway/nat.h"
#include "log.h"
#include <errno.h>
#include <jansson.h>
#include <nsync.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MODULE "gateway"
/* nftables standard priorities. */
#define DSTNAT_PRIORITY (-100)
#define SRCNAT_PRIORITY 100
#define FILTER_PRIORITY 0
/* Runs before connection tracking (-200), so a packet dropped there never
 * reaches a conntrack entry. */
#define RAW_PRIORITY (-300)
#define LOOPBACK_IFACE "lo"

bool fipsd_gw_parse_target(const char *text, struct in6_addr *addr, uint16_t *port)
{
    if (!text || text[0] != '[') return false;
    const char *end = strchr(text, ']');
    if (!end || end[1] != ':' || !end[2]) return false;
    size_t n = (size_t)(end - text - 1);
    char host[64];
    if (!n || n >= sizeof host) return false;
    memcpy(host, text + 1, n); host[n] = 0;
    if (!fipsd_gw_ip6_parse(host, addr)) return false;
    unsigned long v = 0;
    for (const char *p = end + 2; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        v = v * 10 + (unsigned long)(*p - '0');
        if (v > 65535) return false;
    }
    *port = (uint16_t)v;
    return true;
}

const char *fipsd_gw_nat_error_text(const fipsd_gw_nat_error *e, char *buf, size_t cap)
{
    char a[48];
    switch (e->kind) {
    case FIPSD_GW_NAT_OK: snprintf(buf, cap, "ok"); break;
    case FIPSD_GW_NAT_NFTABLES: snprintf(buf, cap, "nftables error: %s", e->message); break;
    case FIPSD_GW_NAT_RULE_NOT_FOUND: snprintf(buf, cap, "rule not found for virtual IP %s", fipsd_gw_ip6_text(&e->addr, a, sizeof a)); break;
    case FIPSD_GW_NAT_WORKER_STOPPED: snprintf(buf, cap, "the NAT worker has stopped"); break;
    }
    return buf;
}
static void set_error(fipsd_gw_nat_error *e, fipsd_gw_nat_error_kind kind, const struct in6_addr *addr, const char *message)
{
    if (!e) return;
    memset(e, 0, sizeof *e);
    e->kind = kind;
    if (addr) e->addr = *addr;
    if (message) snprintf(e->message, sizeof e->message, "%s", message);
}

int fipsd_gw_pairs_push(fipsd_gw_pairs *p, const struct in6_addr *virtual_ip, const struct in6_addr *mesh_addr)
{
    if (p->len == p->cap) {
        size_t cap = p->cap ? p->cap * 2 : 8;
        fipsd_gw_ip_pair *grown = realloc(p->items, cap * sizeof *grown);
        if (!grown) return -1;
        p->items = grown; p->cap = cap;
    }
    p->items[p->len].virtual_ip = *virtual_ip; p->items[p->len].mesh_addr = *mesh_addr; p->len++;
    return 0;
}
void fipsd_gw_pairs_clear(fipsd_gw_pairs *p) { free(p->items); memset(p, 0, sizeof *p); }
static bool pairs_contains(const fipsd_gw_pairs *p, const struct in6_addr *v, const struct in6_addr *m)
{
    for (size_t i = 0; i < p->len; ++i)
        if (fipsd_gw_ip6_eq(&p->items[i].virtual_ip, v) && fipsd_gw_ip6_eq(&p->items[i].mesh_addr, m)) return true;
    return false;
}
/* Set insertion: a pair already present is not added again. */
static int pairs_insert(fipsd_gw_pairs *p, const struct in6_addr *v, const struct in6_addr *m)
{ return pairs_contains(p, v, m) ? 0 : fipsd_gw_pairs_push(p, v, m); }
/* Move every pair of src onto the end of dst. */
static void pairs_take(fipsd_gw_pairs *dst, fipsd_gw_pairs *src)
{
    for (size_t i = 0; i < src->len; ++i) (void)fipsd_gw_pairs_push(dst, &src->items[i].virtual_ip, &src->items[i].mesh_addr);
    src->len = 0;
}
void fipsd_gw_nat_applied_clear(fipsd_gw_nat_applied *a)
{
    free(a->commands); free(a->outcomes); fipsd_gw_pairs_clear(&a->removed);
    memset(a, 0, sizeof *a);
}

/* ---- the manager -------------------------------------------------------- */
typedef struct nat_mapping { struct in6_addr virtual_ip, mesh_addr; } nat_mapping;
struct fipsd_gw_nat {
    char table[64], lan[64], tun[64];
    struct in6_addr network; uint8_t prefix_len;
    nat_mapping *maps; size_t len, cap;    /* desired state, keyed by virtual IP */
    fipsd_gw_port_forward *forwards; size_t nforwards;
    bool rebuild_pending;                  /* desired state not yet acknowledged */
    fipsd_gw_pairs unconfirmed;            /* removed pairs not yet acknowledged */
    fipsd_gw_pairs removed;                /* acknowledged removals not yet reported */
    fipsd_gw_nft_backend backend;
};

fipsd_gw_nat *fipsd_gw_nat_new_state(const char *table, const char *lan_interface, const char *tun_interface,
                                     const struct in6_addr *pool_network, uint8_t prefix_len, fipsd_gw_nft_backend backend)
{
    fipsd_gw_nat *nat = calloc(1, sizeof *nat);
    if (!nat) return NULL;
    snprintf(nat->table, sizeof nat->table, "%s", table ? table : FIPSD_GW_NAT_TABLE);
    snprintf(nat->lan, sizeof nat->lan, "%s", lan_interface ? lan_interface : "");
    snprintf(nat->tun, sizeof nat->tun, "%s", tun_interface ? tun_interface : "fips0");
    nat->prefix_len = prefix_len > 128 ? 128 : prefix_len;
    /* The pool as a masked destination match: any host bits cleared. */
    fipsd_gw_ip6_network(pool_network, nat->prefix_len, &nat->network);
    nat->backend = backend;
    return nat;
}
void fipsd_gw_nat_free(fipsd_gw_nat *nat)
{
    if (!nat) return;
    free(nat->maps); free(nat->forwards);
    fipsd_gw_pairs_clear(&nat->unconfirmed); fipsd_gw_pairs_clear(&nat->removed);
    free(nat);
}
static nat_mapping *find(const fipsd_gw_nat *nat, const struct in6_addr *vip)
{
    for (size_t i = 0; i < nat->len; ++i) if (fipsd_gw_ip6_eq(&nat->maps[i].virtual_ip, vip)) return &nat->maps[i];
    return NULL;
}
static void drop_mapping(fipsd_gw_nat *nat, nat_mapping *m) { *m = nat->maps[--nat->len]; }
size_t fipsd_gw_nat_mapping_count(const fipsd_gw_nat *nat) { return nat->len; }
bool fipsd_gw_nat_rebuild_pending(const fipsd_gw_nat *nat) { return nat->rebuild_pending; }

/* ---- rendering ---------------------------------------------------------- */
static json_t *addr_json(const struct in6_addr *a) { char t[48]; return json_string(fipsd_gw_ip6_text(a, t, sizeof t)); }
static json_t *meta(const char *key) { return json_pack("{s:{s:s}}", "meta", "key", key); }
static json_t *payload(const char *protocol, const char *field) { return json_pack("{s:{s:s,s:s}}", "payload", "protocol", protocol, "field", field); }
static json_t *match(const char *op, json_t *left, json_t *right) { return json_pack("{s:{s:s,s:o,s:o}}", "match", "op", op, "left", left, "right", right); }
static json_t *nfproto_ipv6(void) { return match("==", meta("nfproto"), json_string("ipv6")); }
static json_t *iface(const char *key, const char *op, const char *name) { return match(op, meta(key), json_string(name)); }
static json_t *in_pool(const fipsd_gw_nat *nat, const char *field)
{
    return match("==", payload("ip6", field), json_pack("{s:{s:o,s:i}}", "prefix", "addr", addr_json(&nat->network), "len", (int)nat->prefix_len));
}
static json_t *in_mesh_sources(const char *field) { return match("==", payload("ip6", field), json_string("@" FIPSD_GW_NAT_MESH_SOURCE_SET)); }
static json_t *verdict(const char *name) { return json_pack("{s:n}", name); }
static json_t *counter(void) { return json_pack("{s:n}", "counter"); }
static json_t *nat_stmt(const char *kind, const struct in6_addr *addr, int port)
{
    json_t *body = json_pack("{s:s,s:o}", "family", "ip6", "addr", addr_json(addr));
    if (port >= 0) json_object_set_new(body, "port", json_integer(port));
    return json_pack("{s:o}", kind, body);
}
/* A command that cannot be built leaves a null in the batch, which finish
 * refuses: a batch missing one rule (a drop, say) must never be sent. */
static void append(json_t *cmds, json_t *value)
{ if (!value || json_array_append_new(cmds, value) != 0) json_array_append_new(cmds, json_null()); }
static void add_cmd(json_t *cmds, const char *verb, const char *kind, json_t *object)
{ append(cmds, json_pack("{s:{s:o}}", verb, kind, object)); }
static json_t *table_obj(const fipsd_gw_nat *nat) { return json_pack("{s:s,s:s}", "family", "inet", "name", nat->table); }
static void add_chain(json_t *cmds, const fipsd_gw_nat *nat, const char *name, const char *type, const char *hook, int prio)
{
    add_cmd(cmds, "add", "chain", json_pack("{s:s,s:s,s:s,s:s,s:s,s:i,s:s}", "family", "inet", "table", nat->table, "name", name,
                                            "type", type, "hook", hook, "prio", prio, "policy", "accept"));
}
static void add_rule(json_t *cmds, const fipsd_gw_nat *nat, const char *chain, json_t *expr)
{
    add_cmd(cmds, "add", "rule", json_pack("{s:s,s:s,s:s,s:o}", "family", "inet", "table", nat->table, "chain", chain, "expr", expr));
}
static int cmp_addr(const void *a, const void *b) { return memcmp(a, b, 16); }
static int cmp_mapping(const void *a, const void *b) { return memcmp(&((const nat_mapping *)a)->virtual_ip, &((const nat_mapping *)b)->virtual_ip, 16); }
static char *finish(json_t *cmds)
{
    for (size_t i = 0; i < json_array_size(cmds); ++i)
        if (json_is_null(json_array_get(cmds, i))) { json_decref(cmds); return NULL; }
    json_t *root = json_pack("{s:o}", "nftables", cmds);
    char *text = root ? json_dumps(root, JSON_COMPACT) : NULL;
    json_decref(root);
    if (!text) return NULL;
    /* Hand back plain malloc memory whatever jansson's allocator is. */
    char *out = strdup(text);
    json_malloc_t m; json_free_t f; json_get_alloc_funcs(&m, &f); f(text);
    return out;
}
char *fipsd_gw_nat_render(const fipsd_gw_nat *nat)
{
    json_t *c = json_array();
    if (!c) return NULL;
    append(c, json_pack("{s:{s:i}}", "metainfo", "json_schema_version", 1));
    /* One transaction: the leading add gives the delete a target when no
     * table exists yet, and the recreate keeps the table in the packet path. */
    add_cmd(c, "add", "table", table_obj(nat));
    add_cmd(c, "delete", "table", table_obj(nat));
    add_cmd(c, "add", "table", table_obj(nat));
    add_chain(c, nat, "prerouting", "nat", "prerouting", DSTNAT_PRIORITY);
    add_chain(c, nat, "output", "nat", "output", DSTNAT_PRIORITY);
    add_chain(c, nat, "postrouting", "nat", "postrouting", SRCNAT_PRIORITY);
    add_chain(c, nat, "forward", "filter", "forward", FILTER_PRIORITY);
    add_chain(c, nat, "raw_prerouting", "filter", "prerouting", RAW_PRIORITY);
    add_chain(c, nat, "input", "filter", "input", FILTER_PRIORITY);
    add_cmd(c, "add", "set", json_pack("{s:s,s:s,s:s,s:s}", "family", "inet", "table", nat->table, "name", FIPSD_GW_NAT_MESH_SOURCE_SET,
                                       "type", "ipv6_addr"));
    nat_mapping *sorted = nat->len ? malloc(nat->len * sizeof *sorted) : NULL;
    if (nat->len && !sorted) { json_decref(c); return NULL; }
    if (nat->len) memcpy(sorted, nat->maps, nat->len * sizeof *sorted);
    if (nat->len) qsort(sorted, nat->len, sizeof *sorted, cmp_mapping);
    if (nat->len) {
        /* Each mesh address once, although several virtual IPs may map to it. */
        struct in6_addr *sources = malloc(nat->len * sizeof *sources);
        if (!sources) { free(sorted); json_decref(c); return NULL; }
        for (size_t i = 0; i < nat->len; ++i) sources[i] = sorted[i].mesh_addr;
        qsort(sources, nat->len, sizeof *sources, cmp_addr);
        json_t *elem = json_array();
        for (size_t i = 0; i < nat->len; ++i)
            if (!i || !fipsd_gw_ip6_eq(&sources[i], &sources[i - 1])) json_array_append_new(elem, addr_json(&sources[i]));
        free(sources);
        add_cmd(c, "add", "element", json_pack("{s:s,s:s,s:s,s:o}", "family", "inet", "table", nat->table,
                                               "name", FIPSD_GW_NAT_MESH_SOURCE_SET, "elem", elem));
    }
    /* Pool drop, before conntrack: traffic to the pool that arrives on
     * neither the LAN interface nor loopback. */
    add_rule(c, nat, "raw_prerouting", json_pack("[o,o,o,o,o,o]", nfproto_ipv6(), iface("iifname", "!=", nat->lan),
             iface("iifname", "!=", LOOPBACK_IFACE), in_pool(nat, "daddr"), counter(), verdict("drop")));
    /* Forged-source drop: a mapped mesh address arriving on neither the TUN
     * nor loopback. */
    add_rule(c, nat, "raw_prerouting", json_pack("[o,o,o,o,o,o]", nfproto_ipv6(), iface("iifname", "!=", nat->tun),
             iface("iifname", "!=", LOOPBACK_IFACE), in_mesh_sources("saddr"), counter(), verdict("drop")));
    /* Forward drop: new, invalid and untracked packets into the TUN from any
     * interface but the LAN. */
    add_rule(c, nat, "forward", json_pack("[o,o,o,o,o,o]", nfproto_ipv6(), iface("oifname", "==", nat->tun),
             iface("iifname", "!=", nat->lan),
             match("!", json_pack("{s:{s:s}}", "ct", "key", "state"), json_pack("[s,s]", "established", "related")),
             counter(), verdict("drop")));
    /* A neighbour solicitation to the pool is accepted ahead of the reject,
     * so proxy NDP still answers it. */
    add_rule(c, nat, "input", json_pack("[o,o,o,o,o]", nfproto_ipv6(), in_pool(nat, "daddr"),
             match("==", meta("l4proto"), json_string("ipv6-icmp")),
             match("==", payload("icmpv6", "type"), json_string("nd-neighbor-solicit")), verdict("accept")));
    /* Whatever reaches input still addressed to the pool has no mapping. */
    add_rule(c, nat, "input", json_pack("[o,o,o,{s:{s:s,s:s}}]", nfproto_ipv6(), in_pool(nat, "daddr"), counter(),
             "reject", "type", "icmpx", "expr", "port-unreachable"));
    /* The LAN's masquerade into the TUN, then the gateway host's own. */
    add_rule(c, nat, "postrouting", json_pack("[o,o,{s:n}]", iface("iifname", "==", nat->lan), iface("oifname", "==", nat->tun), "masquerade"));
    add_rule(c, nat, "postrouting", json_pack("[o,o,o,{s:n}]", nfproto_ipv6(), iface("oifname", "==", nat->tun),
             in_mesh_sources("daddr"), "masquerade"));
    /* The LAN-side masquerade for inbound forwards, ahead of every SNAT:
     * those match on source alone and NAT statements are terminal. */
    if (nat->nforwards)
        add_rule(c, nat, "postrouting", json_pack("[o,o,o,{s:n}]", iface("iifname", "==", nat->tun), iface("oifname", "==", nat->lan),
                 nfproto_ipv6(), "masquerade"));
    for (size_t i = 0; i < nat->len; ++i) {
        const nat_mapping *m = &sorted[i];
        add_rule(c, nat, "prerouting", json_pack("[o,o,o,o]", iface("iifname", "==", nat->lan), nfproto_ipv6(),
                 match("==", payload("ip6", "daddr"), addr_json(&m->virtual_ip)), nat_stmt("dnat", &m->mesh_addr, -1)));
        add_rule(c, nat, "output", json_pack("[o,o,o]", nfproto_ipv6(),
                 match("==", payload("ip6", "daddr"), addr_json(&m->virtual_ip)), nat_stmt("dnat", &m->mesh_addr, -1)));
        add_rule(c, nat, "postrouting", json_pack("[o,o,o]", nfproto_ipv6(),
                 match("==", payload("ip6", "saddr"), addr_json(&m->mesh_addr)), nat_stmt("snat", &m->virtual_ip, -1)));
    }
    free(sorted);
    for (size_t i = 0; i < nat->nforwards; ++i) {
        const fipsd_gw_port_forward *pf = &nat->forwards[i];
        const char *proto = pf->proto == FIPSD_GW_PROTO_TCP ? "tcp" : "udp";
        add_rule(c, nat, "prerouting", json_pack("[o,o,o,o,o]", iface("iifname", "==", nat->tun), nfproto_ipv6(),
                 match("==", meta("l4proto"), json_string(proto)), match("==", payload(proto, "dport"), json_integer(pf->listen_port)),
                 nat_stmt("dnat", &pf->target, pf->target_port)));
    }
    return finish(c);
}
char *fipsd_gw_nat_render_cleanup(const fipsd_gw_nat *nat)
{
    json_t *c = json_array();
    if (!c) return NULL;
    append(c, json_pack("{s:{s:i}}", "metainfo", "json_schema_version", 1));
    /* The add makes the teardown idempotent: deleting an absent table fails. */
    add_cmd(c, "add", "table", table_obj(nat));
    add_cmd(c, "delete", "table", table_obj(nat));
    return finish(c);
}

static int send_batch(fipsd_gw_nat *nat, const char *batch, fipsd_gw_nat_error *err)
{
    if (!batch) { set_error(err, FIPSD_GW_NAT_NFTABLES, NULL, "cannot render the NAT batch: out of memory"); return -1; }
    if (!nat->backend.apply) { set_error(err, FIPSD_GW_NAT_NFTABLES, NULL, "no nftables backend on this platform"); return -1; }
    char message[512] = "";
    if (nat->backend.apply(nat->backend.user, batch, message, sizeof message) != 0) {
        set_error(err, FIPSD_GW_NAT_NFTABLES, NULL, *message ? message : "the batch was refused");
        return -1;
    }
    return 0;
}
static int rebuild(fipsd_gw_nat *nat, fipsd_gw_nat_error *err)
{
    char *batch = fipsd_gw_nat_render(nat);
    int rc = send_batch(nat, batch, err);
    free(batch);
    return rc;
}
/* After a rebuild the kernel accepted, move every removed pair the accepted
 * table no longer holds to the removals reported to the caller. */
static void confirm_removals(fipsd_gw_nat *nat)
{
    for (size_t i = 0; i < nat->unconfirmed.len;) {
        fipsd_gw_ip_pair *p = &nat->unconfirmed.items[i];
        const nat_mapping *m = find(nat, &p->virtual_ip);
        if (!m || !fipsd_gw_ip6_eq(&m->mesh_addr, &p->mesh_addr)) {
            (void)fipsd_gw_pairs_push(&nat->removed, &p->virtual_ip, &p->mesh_addr);
            nat->unconfirmed.items[i] = nat->unconfirmed.items[--nat->unconfirmed.len];
        } else ++i;
    }
}
/* Rebuild the desired state, leaving it marked pending unless it is accepted. */
static int rebuild_desired(fipsd_gw_nat *nat, fipsd_gw_nat_error *err)
{
    nat->rebuild_pending = true;
    if (rebuild(nat, err) != 0) return -1;
    nat->rebuild_pending = false;
    confirm_removals(nat);
    return 0;
}
static void set_mapping(fipsd_gw_nat *nat, const struct in6_addr *vip, const struct in6_addr *mesh)
{
    nat_mapping *m = find(nat, vip);
    if (m) {
        if (!fipsd_gw_ip6_eq(&m->mesh_addr, mesh)) (void)pairs_insert(&nat->unconfirmed, vip, &m->mesh_addr);
        m->mesh_addr = *mesh;
        return;
    }
    if (nat->len == nat->cap) {
        size_t cap = nat->cap ? nat->cap * 2 : 16;
        nat_mapping *grown = realloc(nat->maps, cap * sizeof *grown);
        if (!grown) return;
        nat->maps = grown; nat->cap = cap;
    }
    nat->maps[nat->len].virtual_ip = *vip; nat->maps[nat->len].mesh_addr = *mesh; nat->len++;
}
/* Only a mapping to the command's mesh address is removed: a removal queued
 * behind the reissue of its address to another node must not unmap the new
 * holder. An absent mapping has no rules, so its removal is reported at
 * once unless an earlier removal of the pair still waits for the kernel. */
static int remove_command(fipsd_gw_nat *nat, const struct in6_addr *vip, const struct in6_addr *mesh, fipsd_gw_nat_error *err)
{
    nat_mapping *m = find(nat, vip);
    if (!m) {
        if (!pairs_contains(&nat->unconfirmed, vip, mesh)) (void)fipsd_gw_pairs_push(&nat->removed, vip, mesh);
        set_error(err, FIPSD_GW_NAT_RULE_NOT_FOUND, vip, NULL);
        return -1;
    }
    if (!fipsd_gw_ip6_eq(&m->mesh_addr, mesh)) {
        (void)pairs_insert(&nat->unconfirmed, vip, mesh);
        set_error(err, FIPSD_GW_NAT_RULE_NOT_FOUND, vip, NULL);
        return -1;
    }
    drop_mapping(nat, m);
    (void)pairs_insert(&nat->unconfirmed, vip, mesh);
    return 0;
}
fipsd_gw_nat *fipsd_gw_nat_new(const char *lan_interface, const char *tun_interface, const struct in6_addr *pool_network,
                               uint8_t prefix_len, fipsd_gw_nft_backend backend, fipsd_gw_nat_error *err)
{
    fipsd_gw_nat *nat = fipsd_gw_nat_new_state(FIPSD_GW_NAT_TABLE, lan_interface, tun_interface, pool_network, prefix_len, backend);
    if (!nat) { set_error(err, FIPSD_GW_NAT_NFTABLES, NULL, "out of memory"); return NULL; }
    if (rebuild(nat, err) != 0) { fipsd_gw_nat_free(nat); return NULL; }
    FIPSD_INFO(MODULE, "Created nftables table '%s'", FIPSD_GW_NAT_TABLE);
    return nat;
}
int fipsd_gw_nat_set_port_forwards(fipsd_gw_nat *nat, const fipsd_gw_port_forward *forwards, size_t count, fipsd_gw_nat_error *err)
{
    fipsd_gw_port_forward *copy = count ? malloc(count * sizeof *copy) : NULL;
    if (count && !copy) { set_error(err, FIPSD_GW_NAT_NFTABLES, NULL, "out of memory"); return -1; }
    if (count) memcpy(copy, forwards, count * sizeof *copy);
    free(nat->forwards); nat->forwards = copy; nat->nforwards = count;
    if (rebuild_desired(nat, err) != 0) return -1;
    FIPSD_INFO(MODULE, "Applied inbound port forwards: count=%zu", count);
    return 0;
}
int fipsd_gw_nat_apply(fipsd_gw_nat *nat, const fipsd_gw_nat_command *commands, size_t count, fipsd_gw_nat_applied *out)
{
    memset(out, 0, sizeof *out);
    out->commands = count ? malloc(count * sizeof *out->commands) : NULL;
    out->outcomes = count ? calloc(count, sizeof *out->outcomes) : NULL;
    if (count && (!out->commands || !out->outcomes)) { fipsd_gw_nat_applied_clear(out); return -1; }
    out->len = count;
    bool any_ok = false, added = false;
    for (size_t i = 0; i < count; ++i) {
        out->commands[i] = commands[i];
        if (commands[i].kind == FIPSD_GW_NAT_ADD) { set_mapping(nat, &commands[i].virtual_ip, &commands[i].mesh_addr); any_ok = true; added = true; }
        else if (remove_command(nat, &commands[i].virtual_ip, &commands[i].mesh_addr, &out->outcomes[i]) == 0) any_ok = true;
    }
    if (any_ok) {
        fipsd_gw_nat_error rebuilt = {0};
        fipsd_gw_instant started = fipsd_gw_now();
        int rc = rebuild_desired(nat, &rebuilt);
        unsigned long long elapsed_us = (unsigned long long)(fipsd_gw_since(fipsd_gw_now(), started) / 1000u);
        char text[600];
        if (rc == 0) FIPSD_DEBUG(MODULE, "%s DNAT/SNAT rules: mappings=%zu commands=%zu elapsed_us=%llu",
                                 added ? "Added" : "Removed", nat->len, count, elapsed_us);
        else FIPSD_DEBUG(MODULE, "%s DNAT/SNAT rules: mappings=%zu commands=%zu elapsed_us=%llu error=%s",
                         added ? "Added" : "Removed", nat->len, count, elapsed_us, fipsd_gw_nat_error_text(&rebuilt, text, sizeof text));
        for (size_t i = 0; i < count; ++i) if (out->outcomes[i].kind == FIPSD_GW_NAT_OK) out->outcomes[i] = rebuilt;
    }
    pairs_take(&out->removed, &nat->removed);
    return 0;
}
int fipsd_gw_nat_retry_pending(fipsd_gw_nat *nat, fipsd_gw_pairs *removed, fipsd_gw_nat_error *err)
{
    int rc = 0;
    if (nat->rebuild_pending) rc = rebuild_desired(nat, err) == 0 ? 1 : -1;
    if (removed) pairs_take(removed, &nat->removed);
    return rc;
}
int fipsd_gw_nat_cleanup(fipsd_gw_nat *nat, fipsd_gw_nat_error *err)
{
    char *batch = fipsd_gw_nat_render_cleanup(nat);
    int rc = send_batch(nat, batch, err);
    free(batch);
    if (rc == 0) FIPSD_INFO(MODULE, "Deleted nftables table '%s'", nat->table);
    return rc;
}
void fipsd_gw_nat_test_set_mapping(fipsd_gw_nat *nat, const struct in6_addr *virtual_ip, const struct in6_addr *mesh_addr)
{ set_mapping(nat, virtual_ip, mesh_addr); }
int fipsd_gw_nat_test_remove_mapping(fipsd_gw_nat *nat, const struct in6_addr *virtual_ip, fipsd_gw_nat_error *err)
{
    nat_mapping *m = find(nat, virtual_ip);
    if (!m) { set_error(err, FIPSD_GW_NAT_RULE_NOT_FOUND, virtual_ip, NULL); return -1; }
    struct in6_addr mesh = m->mesh_addr;
    drop_mapping(nat, m);
    (void)pairs_insert(&nat->unconfirmed, virtual_ip, &mesh);
    return rebuild_desired(nat, err);
}

static int table_apply(void *self, const fipsd_gw_nat_command *c, size_t n, fipsd_gw_nat_applied *out) { return fipsd_gw_nat_apply(self, c, n, out); }
static int table_retry(void *self, fipsd_gw_pairs *removed, fipsd_gw_nat_error *err) { return fipsd_gw_nat_retry_pending(self, removed, err); }
static size_t table_count(void *self) { return fipsd_gw_nat_mapping_count(self); }
static int table_cleanup(void *self, fipsd_gw_nat_error *err) { return fipsd_gw_nat_cleanup(self, err); }
static void table_destroy(void *self) { fipsd_gw_nat_free(self); }
fipsd_gw_nat_table fipsd_gw_nat_table_of(fipsd_gw_nat *nat)
{ return (fipsd_gw_nat_table){table_apply, table_retry, table_count, table_cleanup, table_destroy, nat}; }

/* ---- outcome latches ---------------------------------------------------- */
fipsd_gw_retry_report fipsd_gw_retry_log_observe(fipsd_gw_retry_log *log, const char *failure)
{
    if (failure) {
        fipsd_gw_retry_report r = log->failed && !strcmp(log->last, failure) ? FIPSD_GW_RETRY_REPEATED : FIPSD_GW_RETRY_FAILED;
        log->failed = true; snprintf(log->last, sizeof log->last, "%s", failure);
        return r;
    }
    if (log->failed) { log->failed = false; log->last[0] = 0; return FIPSD_GW_RETRY_RECOVERED; }
    return FIPSD_GW_RETRY_CLEAN;
}
fipsd_gw_retry_report fipsd_gw_retry_log_unchanged(const fipsd_gw_retry_log *log)
{ return log->failed ? FIPSD_GW_RETRY_REPEATED : FIPSD_GW_RETRY_CLEAN; }
const char *fipsd_gw_nat_latch_key(const fipsd_gw_nat_error *e, char *buf, size_t cap) { return fipsd_gw_nat_error_text(e, buf, cap); }
fipsd_gw_retry_report fipsd_gw_nat_report_apply(fipsd_gw_retry_log *log, const fipsd_gw_nat_command *commands, size_t count,
                                                const fipsd_gw_nat_applied *applied)
{
    const fipsd_gw_nat_error *failure = NULL;
    bool rebuilt = false;
    char text[600], a[48];
    for (size_t i = 0; i < applied->len; ++i) {
        const fipsd_gw_nat_error *e = &applied->outcomes[i];
        if (e->kind != FIPSD_GW_NAT_RULE_NOT_FOUND) rebuilt = true;
        if (!failure && e->kind != FIPSD_GW_NAT_OK && e->kind != FIPSD_GW_NAT_RULE_NOT_FOUND) failure = e;
        if (e->kind != FIPSD_GW_NAT_OK)
            FIPSD_DEBUG(MODULE, "NAT command not applied: command=%s virtual_ip=%s error=%s",
                        applied->commands[i].kind == FIPSD_GW_NAT_ADD ? "add" : "remove",
                        fipsd_gw_ip6_text(&applied->commands[i].virtual_ip, a, sizeof a), fipsd_gw_nat_error_text(e, text, sizeof text));
    }
    size_t adds = 0;
    for (size_t i = 0; i < count; ++i) if (commands[i].kind == FIPSD_GW_NAT_ADD) adds++;
    size_t removes = count - adds;
    /* A batch made only of removes of absent or reissued mappings rebuilt
     * nothing, so it leaves the outcome as it was. */
    if (!rebuilt) return fipsd_gw_retry_log_unchanged(log);
    char key[600];
    fipsd_gw_retry_report r = fipsd_gw_retry_log_observe(log, failure ? fipsd_gw_nat_latch_key(failure, key, sizeof key) : NULL);
    if (failure) fipsd_gw_nat_error_text(failure, text, sizeof text);
    switch (r) {
    case FIPSD_GW_RETRY_FAILED:
        if (adds) FIPSD_ERROR(MODULE, "Failed to add NAT rules: error=%s adds=%zu removes=%zu", text, adds, removes);
        else FIPSD_WARN(MODULE, "Failed to remove NAT rules: error=%s removes=%zu", text, removes);
        break;
    case FIPSD_GW_RETRY_REPEATED:
        if (failure && adds) FIPSD_DEBUG(MODULE, "Failed to add NAT rules: error=%s adds=%zu removes=%zu", text, adds, removes);
        else if (failure) FIPSD_DEBUG(MODULE, "Failed to remove NAT rules: error=%s removes=%zu", text, removes);
        break;
    case FIPSD_GW_RETRY_RECOVERED: FIPSD_INFO(MODULE, "NAT rules applied again after failures"); break;
    case FIPSD_GW_RETRY_CLEAN: break;
    }
    return r;
}
void fipsd_gw_nat_report_retry(fipsd_gw_retry_log *log, int result, const fipsd_gw_nat_error *err)
{
    char key[600], text[600];
    fipsd_gw_retry_report r = fipsd_gw_retry_log_observe(log, result < 0 ? fipsd_gw_nat_latch_key(err, key, sizeof key) : NULL);
    if (result < 0) fipsd_gw_nat_error_text(err, text, sizeof text);
    switch (r) {
    case FIPSD_GW_RETRY_FAILED: FIPSD_WARN(MODULE, "Failed to retry pending NAT rules: error=%s", text); break;
    case FIPSD_GW_RETRY_REPEATED: if (result < 0) FIPSD_DEBUG(MODULE, "Pending NAT rules still failing to apply: error=%s", text); break;
    case FIPSD_GW_RETRY_RECOVERED:
        if (result == 1) FIPSD_INFO(MODULE, "Applied pending NAT rules; retries recovered");
        else FIPSD_INFO(MODULE, "Pending NAT rules were applied by a later update; retries recovered");
        break;
    case FIPSD_GW_RETRY_CLEAN: if (result == 1) FIPSD_INFO(MODULE, "Applied pending NAT rules"); break;
    }
}

/* ---- the worker --------------------------------------------------------- */
struct fipsd_gw_nat_driver {
    nsync_mu mu;
    nsync_cv wake, done;
    fipsd_gw_nat_command *queue; size_t qlen, qcap; /* under mu */
    bool stop, dropped;                                /* under mu */
    bool running;                                      /* under mu: false once the worker ends */
    bool stopped_ready; int stopped_rc; fipsd_gw_nat_error stopped_err; /* under mu */
    fipsd_gw_pairs removed;                            /* under mu */
    unsigned refs;                                     /* under mu: owner and worker */
    atomic_size_t count;
    fipsd_gw_nat_table table;                          /* the worker's */
    fipsd_gw_nat_exit_fn on_exit; void *user;
    pthread_t thread; bool has_thread;
};
static void release(fipsd_gw_nat_driver *d)
{
    nsync_mu_lock(&d->mu);
    unsigned left = --d->refs;
    nsync_mu_unlock(&d->mu);
    if (left) return;
    free(d->queue); fipsd_gw_pairs_clear(&d->removed);
    free(d);
}
static void publish(fipsd_gw_nat_driver *d, fipsd_gw_pairs *report)
{
    atomic_store(&d->count, d->table.mapping_count(d->table.self));
    if (!report->len) return;
    nsync_mu_lock(&d->mu);
    pairs_take(&d->removed, report);
    nsync_mu_unlock(&d->mu);
}
static void finish_stop(fipsd_gw_nat_driver *d)
{
    fipsd_gw_nat_error err = {0};
    int rc = d->table.cleanup(d->table.self, &err);
    nsync_mu_lock(&d->mu);
    d->stopped_ready = true; d->stopped_rc = rc; d->stopped_err = err;
    nsync_cv_broadcast(&d->done);
    nsync_mu_unlock(&d->mu);
}
static void *worker(void *arg)
{
    fipsd_gw_nat_driver *d = arg;
    fipsd_gw_retry_log retry_log = {0}, apply_log = {0};
    nsync_time next_retry = nsync_time_add(nsync_time_now(), nsync_time_ms(FIPSD_GW_NAT_RETRY_INTERVAL_MS));
    nsync_mu_lock(&d->mu);
    for (;;) {
        bool timed_out = false;
        while (!d->qlen && !d->stop && !d->dropped && !timed_out)
            timed_out = nsync_cv_wait_with_deadline(&d->wake, &d->mu, next_retry, NULL) != 0;
        if (d->dropped) break; /* the driver was dropped without a shutdown: leave the table */
        if (d->qlen) {
            fipsd_gw_nat_command *batch = d->queue; size_t n = d->qlen;
            bool stop = d->stop;
            d->queue = NULL; d->qlen = d->qcap = 0;
            nsync_mu_unlock(&d->mu);
            fipsd_gw_nat_applied applied;
            if (d->table.apply(d->table.self, batch, n, &applied) == 0) {
                (void)fipsd_gw_nat_report_apply(&apply_log, batch, n, &applied);
                publish(d, &applied.removed);
                fipsd_gw_nat_applied_clear(&applied);
            }
            free(batch);
            if (stop) { finish_stop(d); nsync_mu_lock(&d->mu); break; }
            nsync_mu_lock(&d->mu);
            continue;
        }
        if (d->stop) { nsync_mu_unlock(&d->mu); finish_stop(d); nsync_mu_lock(&d->mu); break; }
        nsync_mu_unlock(&d->mu);
        fipsd_gw_pairs report = {0}; fipsd_gw_nat_error err = {0};
        int rc = d->table.retry_pending(d->table.self, &report, &err);
        fipsd_gw_nat_report_retry(&retry_log, rc, &err);
        publish(d, &report);
        fipsd_gw_pairs_clear(&report);
        next_retry = nsync_time_add(nsync_time_now(), nsync_time_ms(FIPSD_GW_NAT_RETRY_INTERVAL_MS));
        nsync_mu_lock(&d->mu);
    }
    d->running = false;
    nsync_cv_broadcast(&d->done);
    nsync_mu_unlock(&d->mu);
    if (d->table.destroy) d->table.destroy(d->table.self);
    if (d->on_exit) d->on_exit(d->user);
    release(d);
    return NULL;
}
fipsd_gw_nat_driver *fipsd_gw_nat_driver_start(fipsd_gw_nat_table table, fipsd_gw_nat_exit_fn on_exit, void *user)
{
    fipsd_gw_nat_driver *d = calloc(1, sizeof *d);
    if (!d) { if (table.destroy) table.destroy(table.self); if (on_exit) on_exit(user); return NULL; }
    nsync_mu_init(&d->mu); nsync_cv_init(&d->wake); nsync_cv_init(&d->done);
    d->table = table; d->on_exit = on_exit; d->user = user;
    atomic_init(&d->count, table.mapping_count(table.self));
    d->refs = 2; d->running = true;
    if (pthread_create(&d->thread, NULL, worker, d) != 0) {
        FIPSD_ERROR(MODULE, "Failed to start the NAT worker thread: %s", strerror(errno));
        d->running = false; d->refs = 1;
        if (table.destroy) table.destroy(table.self);
        d->table.destroy = NULL;
        if (on_exit) on_exit(user);
        return d;
    }
    d->has_thread = true;
    return d;
}
int fipsd_gw_nat_driver_submit(fipsd_gw_nat_driver *d, const fipsd_gw_nat_command *command)
{
    int rc = -1;
    nsync_mu_lock(&d->mu);
    if (d->running && !d->stop) {
        if (d->qlen == d->qcap) {
            size_t cap = d->qcap ? d->qcap * 2 : 16;
            fipsd_gw_nat_command *grown = realloc(d->queue, cap * sizeof *grown);
            if (grown) { d->queue = grown; d->qcap = cap; }
        }
        if (d->qlen < d->qcap) { d->queue[d->qlen++] = *command; nsync_cv_signal(&d->wake); rc = 0; }
    }
    nsync_mu_unlock(&d->mu);
    return rc;
}
size_t fipsd_gw_nat_driver_mapping_count(const fipsd_gw_nat_driver *d) { return atomic_load(&((fipsd_gw_nat_driver *)d)->count); }
void fipsd_gw_nat_driver_take_removed(fipsd_gw_nat_driver *d, fipsd_gw_pairs *out)
{
    nsync_mu_lock(&d->mu);
    pairs_take(out, &d->removed);
    nsync_mu_unlock(&d->mu);
}
int fipsd_gw_nat_driver_shutdown(fipsd_gw_nat_driver *d, unsigned timeout_ms, fipsd_gw_nat_error *err)
{
    set_error(err, FIPSD_GW_NAT_OK, NULL, NULL);
    nsync_mu_lock(&d->mu);
    if (!d->running || d->stop) {
        nsync_mu_unlock(&d->mu);
        set_error(err, FIPSD_GW_NAT_WORKER_STOPPED, NULL, NULL);
        if (d->has_thread) pthread_join(d->thread, NULL);
        release(d);
        return -1;
    }
    d->stop = true;
    nsync_cv_signal(&d->wake);
    nsync_time deadline = nsync_time_add(nsync_time_now(), nsync_time_ms(timeout_ms));
    while (!d->stopped_ready && d->running)
        if (nsync_cv_wait_with_deadline(&d->done, &d->mu, deadline, NULL) != 0) break;
    bool ready = d->stopped_ready;
    int rc = d->stopped_rc;
    fipsd_gw_nat_error stopped = d->stopped_err;
    nsync_mu_unlock(&d->mu);
    if (!ready) {
        FIPSD_WARN(MODULE, "NAT worker did not finish deleting the table in time: timeout_ms=%u", timeout_ms);
        if (d->has_thread) pthread_detach(d->thread);
        release(d);
        return 0;
    }
    if (d->has_thread) pthread_join(d->thread, NULL);
    release(d);
    if (rc != 0 && err) *err = stopped;
    return rc;
}
void fipsd_gw_nat_driver_drop(fipsd_gw_nat_driver *d)
{
    nsync_mu_lock(&d->mu);
    d->dropped = true;
    nsync_cv_signal(&d->wake);
    nsync_mu_unlock(&d->mu);
    if (d->has_thread) pthread_join(d->thread, NULL);
    release(d);
}
