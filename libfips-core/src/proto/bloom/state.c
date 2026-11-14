#include <fips/proto/bloom/state.h>
#include <sodium/crypto_hash_sha256.h>
#include <string.h>

static bool same(const fips_node_addr *a, const fips_node_addr *b) { return !memcmp(a->bytes, b->bytes, 16); }
void fips_bloom_state_init(fips_bloom_state *s, fips_node_addr own, bool leaf_only, fips_bloom_peer *peers, size_t pc, fips_node_addr *leaves, size_t lc)
{
    *s = (fips_bloom_state){.own_addr = own, .is_leaf_only = leaf_only, .peers = peers, .peer_capacity = pc,
        .leaves = leaves, .leaf_capacity = lc, .update_debounce_ms = 500, .fallback_ms = FIPS_BLOOM_FALLBACK_MS,
        .max_inbound_fpr = FIPS_BLOOM_ACCEPT_FPR_CAP, .child_echo_threshold = 0.8};
}
fips_bloom_peer *fips_bloom_state_peer(fips_bloom_state *s, const fips_node_addr *addr)
{
    for (size_t i = 0; i < s->peer_count; ++i) if (same(&s->peers[i].addr, addr)) return &s->peers[i];
    return NULL;
}
fips_status fips_bloom_state_add_peer(fips_bloom_state *s, fips_node_addr addr)
{
    if (fips_bloom_state_peer(s, &addr)) return FIPS_OK;
    if (s->peer_count == s->peer_capacity) return FIPS_ERR_RANGE;
    fips_bloom_peer *p = &s->peers[s->peer_count++];
    memset(p, 0, sizeof(*p)); p->addr = addr; p->pending = true;
    s->inbound_dirty = true;
    return FIPS_OK;
}
bool fips_bloom_state_remove_peer(fips_bloom_state *s, const fips_node_addr *addr)
{
    fips_bloom_peer *p = fips_bloom_state_peer(s, addr);
    if (!p) return false;
    size_t i = (size_t)(p - s->peers);
    memmove(p, p + 1, (s->peer_count - i - 1) * sizeof(*p));
    --s->peer_count; s->inbound_dirty = true;
    return true;
}
fips_status fips_bloom_state_add_leaf(fips_bloom_state *s, fips_node_addr addr)
{
    for (size_t i = 0; i < s->leaf_count; ++i) if (same(&s->leaves[i], &addr)) return FIPS_OK;
    if (s->leaf_count == s->leaf_capacity) return FIPS_ERR_RANGE;
    s->leaves[s->leaf_count++] = addr; s->inbound_dirty = true;
    return FIPS_OK;
}
bool fips_bloom_state_remove_leaf(fips_bloom_state *s, const fips_node_addr *addr)
{
    for (size_t i = 0; i < s->leaf_count; ++i) if (same(&s->leaves[i], addr)) {
        memmove(&s->leaves[i], &s->leaves[i + 1], (s->leaf_count - i - 1) * sizeof(*s->leaves));
        --s->leaf_count; s->inbound_dirty = true; return true;
    }
    return false;
}
void fips_bloom_state_peer_role(fips_bloom_state *s, fips_bloom_peer *p, bool tree, bool child, bool authenticated)
{
    if (tree != p->tree) s->inbound_dirty = true;
    p->tree = tree; p->child = child; p->authenticated = authenticated;
}
void fips_bloom_state_mark_all(fips_bloom_state *s) { for (size_t i = 0; i < s->peer_count; ++i) s->peers[i].pending = true; }
void fips_bloom_state_clear_pending(fips_bloom_state *s)
{
    for (size_t i = 0; i < s->peer_count; ++i) {
        s->peers[i].pending = false; s->peers[i].pending_resend = FIPS_BLOOM_RESEND_NONE;
    }
}
void fips_bloom_state_note_inbound_change(fips_bloom_state *s) { s->inbound_dirty = true; }
bool fips_bloom_state_take_inbound_change(fips_bloom_state *s) { bool dirty = s->inbound_dirty; s->inbound_dirty = false; return dirty; }
void fips_bloom_state_base_filter(const fips_bloom_state *s, fips_bloom_filter *out)
{
    fips_bloom_init(out); fips_bloom_insert(out, &s->own_addr);
    for (size_t i = 0; i < s->leaf_count; ++i) fips_bloom_insert(out, &s->leaves[i]);
}
void fips_bloom_state_outgoing(const fips_bloom_state *s, const fips_node_addr *exclude, fips_bloom_filter *out)
{
    fips_bloom_state_base_filter(s, out);
    for (size_t i = 0; i < s->peer_count; ++i) {
        const fips_bloom_peer *p = &s->peers[i];
        if (p->tree && p->has_inbound && !same(&p->addr, exclude)) (void)fips_bloom_merge(out, &p->inbound, NULL);
    }
}
/* Bit multiplicities are an allocation-free equivalent of Rust's prefix/
 * suffix OR unions. A target's own contribution subtracts at most one. */
static void contributions(const fips_bloom_state *s, size_t counts[FIPS_BLOOM_DEFAULT_BITS])
{
    memset(counts, 0, sizeof(size_t) * FIPS_BLOOM_DEFAULT_BITS);
    for (size_t i = 0; i < s->peer_count; ++i) {
        const fips_bloom_peer *p = &s->peers[i];
        if (!p->tree || !p->has_inbound || p->inbound.num_bits != FIPS_BLOOM_DEFAULT_BITS) continue;
        for (size_t bit = 0; bit < FIPS_BLOOM_DEFAULT_BITS; ++bit) counts[bit] += (p->inbound.bits[bit / 8] >> (bit % 8)) & 1u;
    }
}
static void outgoing_from_counts(const fips_bloom_peer *p, const fips_bloom_filter *base, const size_t counts[FIPS_BLOOM_DEFAULT_BITS], fips_bloom_filter *out)
{
    *out = *base;
    bool subtract = p->tree && p->has_inbound && p->inbound.num_bits == FIPS_BLOOM_DEFAULT_BITS;
    for (size_t bit = 0; bit < FIPS_BLOOM_DEFAULT_BITS; ++bit) {
        size_t own = subtract ? (size_t)((p->inbound.bits[bit / 8] >> (bit % 8)) & 1u) : 0;
        if (counts[bit] > own) out->bits[bit / 8] |= (uint8_t)(1u << (bit % 8));
    }
}
void fips_bloom_state_mark_changed(fips_bloom_state *s, const fips_node_addr *exclude)
{
    size_t counts[FIPS_BLOOM_DEFAULT_BITS];
    fips_bloom_filter base, out;
    contributions(s, counts); fips_bloom_state_base_filter(s, &base); ++s->inbound_recomputes;
    for (size_t i = 0; i < s->peer_count; ++i) {
        fips_bloom_peer *p = &s->peers[i];
        if (exclude && same(&p->addr, exclude)) continue;
        outgoing_from_counts(p, &base, counts, &out);
        if (!p->sent_len || !fips_bloom_equal(&out, &p->sent[0])) p->pending = true;
    }
}
bool fips_bloom_peer_should_send(const fips_bloom_state *s, const fips_bloom_peer *p, uint64_t now)
{
    return p->pending && (!p->has_last_sent || now < p->last_sent_ms || now - p->last_sent_ms >= s->update_debounce_ms);
}
uint64_t fips_bloom_peer_next_sequence(fips_bloom_state *s, fips_bloom_peer *p, uint64_t floor)
{
    ++s->sequence;
    p->sequence = p->has_sequence ? (p->sequence == UINT64_MAX ? UINT64_MAX : p->sequence + 1) : floor;
    p->has_sequence = true;
    return p->sequence;
}
void fips_bloom_peer_record_update(fips_bloom_peer *p, uint64_t now) { p->last_sent_ms = now; p->has_last_sent = true; p->pending = false; p->pending_resend = FIPS_BLOOM_RESEND_NONE; }
void fips_bloom_peer_record_filter(fips_bloom_peer *p, const fips_bloom_filter *f)
{
    fips_bloom_filter copy = *f; /* permits resending a borrowed history entry */
    size_t n = p->sent_len;
    for (size_t i = 0; i < n; ++i) if (fips_bloom_equal(&copy, &p->sent[i])) {
        memmove(&p->sent[i], &p->sent[i + 1], (n - i - 1) * sizeof(*f)); --n; break;
    }
    if (n == FIPS_BLOOM_SENT_HISTORY) --n;
    memmove(&p->sent[1], &p->sent[0], n * sizeof(*f)); p->sent[0] = copy; p->sent_len = n + 1;
}
void fips_bloom_peer_forget_sent(fips_bloom_peer *p) { p->sent_len = 0; }
bool fips_bloom_peer_warn_echo(fips_bloom_peer *p, uint64_t now)
{
    bool due = !p->has_echo_warned || now < p->echo_warned_ms || now - p->echo_warned_ms >= FIPS_BLOOM_ECHO_WARN_MS;
    if (due) { p->echo_warned_ms = now; p->has_echo_warned = true; }
    return due;
}
bool fips_bloom_peer_warn_mtu(fips_bloom_peer *p) { bool first = !p->mtu_warned; p->mtu_warned = true; return first; }
fips_status fips_bloom_state_receive(fips_bloom_state *s, fips_bloom_peer *p, const fips_filter_announce *a, uint64_t now, fips_bloom_actions *actions)
{
    fips_bloom_refusal why = FIPS_BLOOM_REFUSE_NONE;
    const fips_bloom_filter *echo = NULL;
    if (!fips_filter_announce_is_valid(a) || !fips_filter_announce_is_v1_compliant(a)) why = FIPS_BLOOM_REFUSE_PARAMETERS;
    else if (p->has_inbound && a->sequence <= p->inbound_sequence) return FIPS_OK;
    else if (fips_bloom_fpr(&a->filter) > s->max_inbound_fpr) why = FIPS_BLOOM_REFUSE_FPR;
    else if (p->tree && p->child && p->has_inbound) {
        for (size_t i = 0; i < p->sent_len; ++i) if (fips_bloom_echoes(&a->filter, &p->sent[i], s->child_echo_threshold)) {
            why = FIPS_BLOOM_REFUSE_CHILD_ECHO; echo = &p->sent[i]; break;
        }
    }
    if (why != FIPS_BLOOM_REFUSE_NONE) {
        if (actions->len == actions->capacity) return FIPS_ERR_RANGE;
        fips_bloom_action action = {.kind = FIPS_BLOOM_REFUSED, .peer = p->addr, .refusal = why, .warn = true};
        action.announce.sequence = a->sequence;
        action.announce.hash_count = a->hash_count; action.announce.size_class = a->size_class;
        if (why != FIPS_BLOOM_REFUSE_PARAMETERS) action.fpr = fips_bloom_fpr(&a->filter);
        if (echo) {
            action.warn = fips_bloom_peer_warn_echo(p, now);
            (void)fips_bloom_overlap(&a->filter, echo, &action.overlap);
            action.bound = fips_bloom_echo_bound(&a->filter, s->child_echo_threshold);
            (void)crypto_hash_sha256(action.received_digest, a->filter.bits, (unsigned long long)(a->filter.num_bits / 8));
            (void)crypto_hash_sha256(action.sent_digest, echo->bits, (unsigned long long)(echo->num_bits / 8));
        }
        actions->items[actions->len++] = action;
        return FIPS_OK;
    }
    bool changed = !p->has_inbound || !fips_bloom_equal(&p->inbound, &a->filter);
    p->inbound = a->filter; p->inbound_sequence = a->sequence; p->has_inbound = true;
    if (changed) s->inbound_dirty = true;
    return FIPS_OK;
}
void fips_bloom_state_peer_restarted(fips_bloom_state *s, fips_bloom_peer *p)
{
    (void)s;
    fips_bloom_peer_forget_sent(p); p->pending = true;
}
fips_status fips_bloom_state_refresh(fips_bloom_state *s, uint64_t now, uint64_t floor, fips_bloom_actions *actions)
{
    size_t counts[FIPS_BLOOM_DEFAULT_BITS];
    fips_bloom_filter base, outgoing;
    contributions(s, counts); fips_bloom_state_base_filter(s, &base);
    bool dirty = s->inbound_dirty;
    size_t needed = 0;
    /* Count actual sends without changing pending bits, dirty state, sequences
     * or diagnostics. An undersized action list is an atomic failure. */
    for (size_t i = 0; i < s->peer_count; ++i) {
        const fips_bloom_peer *p = &s->peers[i];
        bool pending = p->pending;
        if (dirty) {
            outgoing_from_counts(p, &base, counts, &outgoing);
            pending |= !p->sent_len || !fips_bloom_equal(&outgoing, &p->sent[0]);
        }
        bool due = !p->has_last_sent || now < p->last_sent_ms || now - p->last_sent_ms >= s->update_debounce_ms;
        if (p->authenticated && pending && due) ++needed;
    }
    if (needed > actions->capacity - actions->len) return FIPS_ERR_RANGE;
    if (dirty) { s->inbound_dirty = false; ++s->inbound_recomputes; }
    for (size_t i = 0; i < s->peer_count; ++i) {
        fips_bloom_peer *p = &s->peers[i];
        outgoing_from_counts(p, &base, counts, &outgoing);
        if (dirty && (!p->sent_len || !fips_bloom_equal(&outgoing, &p->sent[0]))) p->pending = true;
        if (!p->authenticated || !fips_bloom_peer_should_send(s, p, now)) continue;
        fips_bloom_action action = {.kind = FIPS_BLOOM_SEND, .peer = p->addr, .resend = p->pending_resend};
        uint64_t seq = fips_bloom_peer_next_sequence(s, p, floor);
        fips_filter_announce_init(&action.announce, &outgoing, seq);
        actions->items[actions->len++] = action;
    }
    return FIPS_OK;
}

fips_status fips_bloom_state_sent(fips_bloom_state *s, fips_bloom_peer *p, const fips_bloom_action *send, fips_bloom_send_outcome outcome, uint64_t counter, const fips_bloom_link_evidence *ev, uint64_t now, fips_bloom_actions *actions)
{
    (void)s;
    if (outcome.result == FIPS_BLOOM_SEND_ACCEPTED) {
        fips_bloom_peer_record_announce(p, &send->announce.filter, counter, ev, now);
        fips_bloom_peer_record_filter(p, &send->announce.filter);
        fips_bloom_peer_record_update(p, now);
        return FIPS_OK;
    }
    if (actions->len == actions->capacity) return FIPS_ERR_RANGE;
    fips_bloom_action a = {.kind = FIPS_BLOOM_REFUSED, .peer = p->addr,
        .refusal = outcome.result == FIPS_BLOOM_SEND_TOO_LARGE ? FIPS_BLOOM_REFUSE_MTU : FIPS_BLOOM_REFUSE_TRANSPORT,
        .warn = outcome.result == FIPS_BLOOM_SEND_TOO_LARGE ? fips_bloom_peer_warn_mtu(p) : true,
        .packet_size = outcome.packet_size, .mtu = outcome.mtu};
    a.announce.sequence = send->announce.sequence;
    actions->items[actions->len++] = a; p->pending = true;
    return FIPS_OK;
}
