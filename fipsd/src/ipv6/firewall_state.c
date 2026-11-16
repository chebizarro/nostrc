#define _POSIX_C_SOURCE 200809L
#include "ipv6/firewall_state.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#define MOD "control/firewall_state"
typedef enum match_kind { M_IIFNAME, M_L4PROTO, M_DPORT, M_UNRECOGNIZED } match_kind;
typedef enum port_kind { PM_SINGLE, PM_SET, PM_RANGE } port_kind;
typedef struct match {
    match_kind kind;
    char op[24];
    bool has_name;                 /* IIFNAME: a literal name on the right-hand side */
    char name[FIPSD_IPV6_IFACE_NAME];
    fipsd_ipv6_proto proto;        /* L4PROTO, DPORT */
    port_kind pk;
    uint16_t single, lo, hi;
    uint16_t *set; size_t set_len;
} match;
typedef enum verdict { V_ACCEPT, V_DROP, V_RETURN, V_OTHER } verdict;
typedef struct rule { match *matches; size_t count; verdict verdict; } rule;
struct fipsd_ipv6_filter_classifier { bool active; rule *rules; size_t count; };
const char *fipsd_ipv6_filter_state_str(fipsd_ipv6_filter_state s)
{
    static const char *const names[] = {"no_firewall", "accept", "drop", "unknown"};
    return names[s];
}
static bool port_matches(const match *m, uint16_t port)
{
    switch (m->pk) {
    case PM_SINGLE: return m->single == port;
    case PM_RANGE: return m->lo <= port && port <= m->hi;
    default: for (size_t i = 0; i < m->set_len; ++i) if (m->set[i] == port) return true; return false;
    }
}
static bool parse_proto(const char *s, fipsd_ipv6_proto *out)
{
    if (s && !strcmp(s, "tcp")) { *out = FIPSD_IPV6_PROTO_TCP; return true; }
    if (s && !strcmp(s, "udp")) { *out = FIPSD_IPV6_PROTO_UDP; return true; }
    return false;
}
/* The meta key of left when it names the input interface. */
static const char *input_iface_key(const json_t *left)
{
    const char *key = json_string_value(json_object_get(json_object_get(left, "meta"), "key"));
    static const char *const keys[] = {"iif", "iifname", "iifgroup", "iiftype", "iifkind"};
    for (size_t i = 0; key && i < 5; ++i) if (!strcmp(key, keys[i])) return key;
    return NULL;
}
static bool u16_of(const json_t *v, uint16_t *out)
{
    if (!json_is_integer(v) || json_integer_value(v) < 0 || json_integer_value(v) > 0xffff) return false;
    *out = (uint16_t)json_integer_value(v); return true;
}
static void parse_match(const json_t *m, match *out)
{
    memset(out, 0, sizeof *out); out->kind = M_UNRECOGNIZED;
    const char *op = json_string_value(json_object_get(m, "op"));
    snprintf(out->op, sizeof out->op, "%s", op ? op : "==");
    const json_t *left = json_object_get(m, "left"), *right = json_object_get(m, "right");
    const char *key = input_iface_key(left);
    if (key) {
        out->kind = M_IIFNAME;
        const char *name = json_string_value(right);
        if (name && !strcmp(key, "iifname") && name[0] != '@' && !strchr(name, '*')) {
            out->has_name = true; snprintf(out->name, sizeof out->name, "%s", name);
        }
        return;
    }
    const json_t *meta = json_object_get(left, "meta");
    const char *meta_key = json_string_value(json_object_get(meta, "key"));
    if (meta_key && !strcmp(meta_key, "l4proto") && parse_proto(json_string_value(right), &out->proto) && !strcmp(out->op, "==")) {
        out->kind = M_L4PROTO; return;
    }
    const json_t *payload = json_object_get(left, "payload");
    const char *field = json_string_value(json_object_get(payload, "field"));
    if (field && !strcmp(field, "dport") && parse_proto(json_string_value(json_object_get(payload, "protocol")), &out->proto) &&
        !strcmp(out->op, "==")) {
        if (u16_of(right, &out->single)) { out->kind = M_DPORT; out->pk = PM_SINGLE; return; }
        const json_t *set = json_object_get(right, "set");
        if (json_is_array(set)) {
            size_t n = json_array_size(set);
            uint16_t *ports = calloc(n ? n : 1, sizeof *ports); size_t got = 0;
            for (size_t i = 0; ports && i < n; ++i) if (u16_of(json_array_get(set, i), &ports[i])) ++got;
            /* Bail if the set held anything not a plain integer. */
            if (ports && got == n) { out->kind = M_DPORT; out->pk = PM_SET; out->set = ports; out->set_len = n; return; }
            free(ports);
        }
        const json_t *range = json_object_get(right, "range");
        if (json_is_array(range) && json_array_size(range) == 2 &&
            u16_of(json_array_get(range, 0), &out->lo) && u16_of(json_array_get(range, 1), &out->hi)) {
            out->kind = M_DPORT; out->pk = PM_RANGE; return;
        }
    }
}
static bool parse_rule(const json_t *r, rule *out)
{
    const json_t *exprs = json_object_get(r, "expr");
    size_t n = json_is_array(exprs) ? json_array_size(exprs) : 0;
    out->matches = calloc(n ? n : 1, sizeof *out->matches); out->count = 0; out->verdict = V_OTHER;
    if (!out->matches) return false;
    for (size_t i = 0; i < n; ++i) {
        const json_t *e = json_array_get(exprs, i), *m, *map;
        if ((m = json_object_get(e, "match"))) parse_match(m, &out->matches[out->count++]);
        else if ((map = json_object_get(e, "vmap"))) {
            /* A verdict map picks the verdict per packet; one keyed on the
             * input interface is recorded as an unreadable scope. */
            const char *key = input_iface_key(json_object_get(map, "key"));
            if (key) {
                match *x = &out->matches[out->count++];
                memset(x, 0, sizeof *x); x->kind = M_IIFNAME; snprintf(x->op, sizeof x->op, "vmap %s", key);
            }
            out->verdict = V_OTHER;
        }
        else if (json_object_get(e, "accept")) out->verdict = V_ACCEPT;
        else if (json_object_get(e, "drop")) out->verdict = V_DROP;
        else if (json_object_get(e, "counter")) { /* observational; keeps any earlier verdict */ }
        else if (json_object_get(e, "return")) out->verdict = V_RETURN;
        else if (json_object_get(e, "jump") || json_object_get(e, "goto") || json_object_get(e, "continue") ||
                 json_object_get(e, "reject") || json_object_get(e, "queue")) out->verdict = V_OTHER;
    }
    return true;
}
fipsd_ipv6_filter_classifier *fipsd_ipv6_filter_no_firewall(void) { return calloc(1, sizeof(fipsd_ipv6_filter_classifier)); }
fipsd_ipv6_filter_classifier *fipsd_ipv6_filter_from_nft_json(const json_t *json)
{
    fipsd_ipv6_filter_classifier *c = fipsd_ipv6_filter_no_firewall();
    if (!c) return NULL;
    c->active = true;
    const json_t *arr = json_object_get(json, "nftables");
    size_t n = json_is_array(arr) ? json_array_size(arr) : 0;
    c->rules = calloc(n ? n : 1, sizeof *c->rules);
    if (!c->rules) { free(c); return NULL; }
    for (size_t i = 0; i < n; ++i) {
        const json_t *r = json_object_get(json_array_get(arr, i), "rule");
        const char *chain = json_string_value(json_object_get(r, "chain")), *table = json_string_value(json_object_get(r, "table"));
        if (!r || !chain || !table || strcmp(chain, "inbound") || strcmp(table, "fips")) continue;
        if (parse_rule(r, &c->rules[c->count])) ++c->count;
    }
    return c;
}
void fipsd_ipv6_filter_destroy(fipsd_ipv6_filter_classifier *c)
{
    if (!c) return;
    for (size_t i = 0; i < c->count; ++i) {
        for (size_t j = 0; j < c->rules[i].count; ++j) free(c->rules[i].matches[j].set);
        free(c->rules[i].matches);
    }
    free(c->rules); free(c);
}
bool fipsd_ipv6_filter_is_active(const fipsd_ipv6_filter_classifier *c) { return c && c->active; }
fipsd_ipv6_scope fipsd_ipv6_filter_scope(const fipsd_ipv6_filter_classifier *c)
{
    fipsd_ipv6_scope scope = {.kind = FIPSD_IPV6_SCOPE_NONE};
    if (!c || !c->active) return scope;
    const rule *found = NULL;
    for (size_t i = 0; i < c->count && !found; ++i) {
        if (c->rules[i].verdict == V_RETURN) found = &c->rules[i];
        for (size_t j = 0; j < c->rules[i].count && !found; ++j) if (c->rules[i].matches[j].kind == M_IIFNAME) found = &c->rules[i];
    }
    if (!found) { scope.kind = FIPSD_IPV6_SCOPE_UNSCOPED; return scope; }
    const match *m = found->matches;
    if (found->count == 1 && m->kind == M_IIFNAME && m->has_name && !strcmp(m->op, "!=") && found->verdict == V_RETURN) {
        scope.kind = FIPSD_IPV6_SCOPE_IFACE; memcpy(scope.name, m->name, sizeof scope.name);
    } else scope.kind = FIPSD_IPV6_SCOPE_UNREADABLE;
    return scope;
}
bool fipsd_ipv6_filter_polices(const fipsd_ipv6_filter_classifier *c, const char *tun)
{
    fipsd_ipv6_scope s = fipsd_ipv6_filter_scope(c);
    return s.kind == FIPSD_IPV6_SCOPE_UNSCOPED || (s.kind == FIPSD_IPV6_SCOPE_IFACE && tun && !strcmp(s.name, tun));
}
bool fipsd_ipv6_filter_foreign_scope(const fipsd_ipv6_filter_classifier *c, const char *tun, char *out, size_t cap)
{
    fipsd_ipv6_scope s = fipsd_ipv6_filter_scope(c);
    if (s.kind != FIPSD_IPV6_SCOPE_IFACE || (tun && !strcmp(s.name, tun))) return false;
    if (out && cap) snprintf(out, cap, "%s", s.name);
    return true;
}
/* Does this rule, taken as a whole, reference our port under our proto? */
static bool rule_might_reference_port(const rule *r, fipsd_ipv6_proto proto, uint16_t port)
{
    for (size_t j = 0; j < r->count; ++j) {
        const match *m = &r->matches[j];
        if (m->kind == M_DPORT && m->proto == proto && port_matches(m, port)) return true;
    }
    return false;
}
fipsd_ipv6_filter_state fipsd_ipv6_filter_classify(const fipsd_ipv6_filter_classifier *c, fipsd_ipv6_proto proto, uint16_t port)
{
    if (!c || !c->active) return FIPSD_IPV6_FILTER_NO_FIREWALL;
    bool saw_unknown_for_port = false;
    for (size_t i = 0; i < c->count; ++i) {
        const rule *r = &c->rules[i];
        bool references_port = false, canonical_for_port = true, has_proto_match = false;
        fipsd_ipv6_proto matched_proto = proto;
        for (size_t j = 0; j < r->count; ++j) {
            const match *m = &r->matches[j];
            switch (m->kind) {
            case M_IIFNAME:
                if (m->has_name) break; /* the table's iif scoping: skip */
                /* fall through: a set/other shape is not decomposed */
            case M_UNRECOGNIZED:
                if (rule_might_reference_port(r, proto, port)) saw_unknown_for_port = true;
                canonical_for_port = false;
                break;
            case M_L4PROTO:
                has_proto_match = true; matched_proto = m->proto;
                if (m->proto != proto) canonical_for_port = false;
                break;
            case M_DPORT:
                if (m->proto == proto && port_matches(m, port)) references_port = true;
                else if (!port_matches(m, port)) canonical_for_port = false;
                /* a dport match for the other proto references our number, not our pair */
                break;
            }
        }
        if (!references_port) continue;
        if (!canonical_for_port) { saw_unknown_for_port = true; continue; }
        if (has_proto_match && matched_proto != proto) continue;
        switch (r->verdict) {
        case V_ACCEPT: return FIPSD_IPV6_FILTER_ACCEPT;
        case V_DROP: return FIPSD_IPV6_FILTER_DROP; /* an explicit deny */
        default: saw_unknown_for_port = true; break;
        }
    }
    return saw_unknown_for_port ? FIPSD_IPV6_FILTER_UNKNOWN : FIPSD_IPV6_FILTER_DROP;
}
void fipsd_ipv6_filter_warn_scope(const fipsd_ipv6_filter_classifier *c, const char *tun)
{
    char scope[FIPSD_IPV6_IFACE_NAME];
    if (!fipsd_ipv6_filter_foreign_scope(c, tun, scope, sizeof scope)) return;
    /* Kept on the target it had in the control module, so log filters
     * written for it still match. */
    FIPSD_WARN(MOD, "The inet fips firewall polices %s, not the TUN %s: listeners on %s are reachable from the mesh. "
        "Set fips_if in /etc/fips/fips.nft to the tun.name and reload the firewall", scope, tun, tun);
}
#ifdef __linux__
json_t *fipsd_ipv6_filter_run_nft(void)
{
    FILE *p = popen("nft -j list table inet fips 2>/dev/null", "r");
    if (!p) return NULL;
    size_t cap = 65536, len = 0; char *text = malloc(cap);
    while (text) {
        size_t n = fread(text + len, 1, cap - len - 1, p);
        len += n;
        if (n == 0) break;
        if (len + 1 == cap) { char *more = realloc(text, cap * 2); if (!more) { free(text); text = NULL; break; } text = more; cap *= 2; }
    }
    int status = pclose(p);
    /* Common case: the table does not exist -> exit 1. Missing nft -> 127. */
    if (!text || !WIFEXITED(status) || WEXITSTATUS(status) != 0) { free(text); return NULL; }
    text[len] = 0;
    json_t *json = json_loads(text, 0, NULL);
    free(text);
    return json;
}
#else
json_t *fipsd_ipv6_filter_run_nft(void) { return NULL; }
#endif
fipsd_ipv6_filter_classifier *fipsd_ipv6_filter_query(void)
{
    json_t *json = fipsd_ipv6_filter_run_nft();
    if (!json) return fipsd_ipv6_filter_no_firewall();
    fipsd_ipv6_filter_classifier *c = fipsd_ipv6_filter_from_nft_json(json);
    json_decref(json);
    return c ? c : fipsd_ipv6_filter_no_firewall();
}
