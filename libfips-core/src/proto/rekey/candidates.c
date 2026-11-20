#include "internal.h"
size_t rk_cand_find(const fips_rekey_candidates *c, const uint8_t *msg, size_t len) {
    for (size_t i = 0; i < c->count; ++i)
        if (c->list[i].msg1.len == len && !memcmp(c->list[i].msg1.bytes, msg, len))
            return i;
    return SIZE_MAX;
}
static size_t count(const fips_rekey_candidates *c, fips_node_addr carrier) {
    size_t n = 0;
    for (size_t i = 0; i < c->count; ++i)
        if (!memcmp(&carrier, &c->list[i].carrier, sizeof carrier))
            ++n;
    return n;
}
bool rk_cand_room(const fips_rekey_candidates *c, const fips_rekey_profile *p,
                  fips_node_addr carrier) {
    return c->count < p->candidates && count(c, carrier) < p->per_carrier;
}
size_t rk_cand_victim(const fips_rekey_candidates *c, const fips_rekey_profile *p,
                      fips_node_addr carrier, fips_instant now, fips_duration young) {
    fips_node_addr target = carrier;
    size_t n = count(c, carrier);
    if (n < p->per_carrier) {
        n = 0;
        for (size_t i = 0; i < c->count; ++i) {
            size_t hits = count(c, c->list[i].carrier);
            if (hits >= n) {
                n = hits;
                target = c->list[i].carrier;
            }
        }
    }
    if (n < 2)
        return SIZE_MAX;
    for (size_t i = 0; i < c->count; ++i)
        if (!memcmp(&target, &c->list[i].carrier, sizeof target) &&
            rk_dcmp(rk_since(now, c->list[i].born), young) > 0)
            return i;
    return SIZE_MAX;
}
void rk_cand_remove(fips_rekey_candidates *c, size_t i) {
    assert(i < c->count);
    fips_rekey_hs_clear(&c->list[i].hs);
    memmove(c->list + i, c->list + i + 1, (c->count - i - 1) * sizeof *c->list);
    --c->count;
    sodium_memzero(c->list + c->count, sizeof *c->list);
}
void rk_cand_clear(fips_rekey_candidates *c) {
    while (c->count)
        rk_cand_remove(c, c->count - 1);
}
void rk_cand_add(fips_rekey_candidates *c, fips_rekey_candidate *candidate) {
    assert(c->count < FIPS_REKEY_CANDIDATES_MAX);
    candidate->seq = c->next;
    if (c->next < UINT64_MAX)
        ++c->next;
    c->list[c->count++] = *candidate;
    sodium_memzero(candidate, sizeof *candidate);
}
