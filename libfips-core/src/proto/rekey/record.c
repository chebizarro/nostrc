#include <fips/proto/rekey/record.h>
#include <string.h>
void fips_rekey_record_new(fips_rekey_record *r) { memset(r, 0, sizeof *r); }
void fips_rekey_record_setup(fips_rekey_record *r, fips_rekey_digest msg,
                             fips_rekey_digest ephemeral, const fips_rekey_wire *answer) {
    fips_rekey_record_new(r);
    r->setup = msg;
    r->origin = ephemeral;
    r->has_setup = r->has_origin = true;
    if (answer) {
        r->answer = *answer;
        r->has_answer = true;
    }
}
void fips_rekey_record_push(fips_rekey_record *r, fips_rekey_digest d) {
    if (r->count == FIPS_REKEY_RECORD_BOUND) {
        r->head = (r->head + 1) % FIPS_REKEY_RECORD_BOUND;
        --r->count;
    }
    r->seen[(r->head + r->count) % FIPS_REKEY_RECORD_BOUND] = d;
    ++r->count;
}
bool fips_rekey_record_holds(const fips_rekey_record *r, fips_rekey_digest d) {
    if (r->has_origin && !memcmp(&r->origin, &d, sizeof d))
        return true;
    for (size_t i = 0; i < r->count; ++i)
        if (!memcmp(&r->seen[(r->head + i) % FIPS_REKEY_RECORD_BOUND], &d, sizeof d))
            return true;
    return false;
}
size_t fips_rekey_record_len(const fips_rekey_record *r) {
    return r->count + (size_t)r->has_origin;
}
void fips_rekey_record_absorb(fips_rekey_record *r, fips_rekey_record *older) {
    fips_rekey_record joined = {0};
    for (size_t i = 0; i < older->count; ++i)
        fips_rekey_record_push(&joined, older->seen[(older->head + i) % FIPS_REKEY_RECORD_BOUND]);
    if (older->has_origin)
        fips_rekey_record_push(&joined, older->origin);
    for (size_t i = 0; i < r->count; ++i)
        fips_rekey_record_push(&joined, r->seen[(r->head + i) % FIPS_REKEY_RECORD_BOUND]);
    memcpy(r->seen, joined.seen, sizeof r->seen);
    r->head = joined.head;
    r->count = joined.count;
    fips_rekey_record_new(older);
}
void fips_rekey_record_store_new(fips_rekey_record_store *s, fips_rekey_record_entry *storage,
                                 size_t bound) {
    *s = (fips_rekey_record_store){storage, 0, bound};
    if (bound)
        memset(storage, 0, bound * sizeof *storage);
}
static size_t find(const fips_rekey_record_store *s, const fips_node_addr *p, const uint8_t e[8]) {
    for (size_t i = 0; i < s->count; ++i)
        if (!memcmp(&s->entries[i].peer, p, sizeof *p) && !memcmp(s->entries[i].epoch, e, 8))
            return i;
    return s->count;
}
static void remove_entry(fips_rekey_record_store *s, size_t i) {
    if (i >= s->count)
        return;
    memmove(s->entries + i, s->entries + i + 1, (s->count - i - 1) * sizeof *s->entries);
    --s->count;
    memset(s->entries + s->count, 0, sizeof *s->entries);
}
void fips_rekey_record_store_file(fips_rekey_record_store *s, fips_node_addr p, const uint8_t e[8],
                                  fips_rekey_record *r) {
    fips_rekey_record kept = {0};
    fips_rekey_record_absorb(&kept, r);
    if (!fips_rekey_record_len(&kept) || !s->bound)
        return;
    remove_entry(s, find(s, &p, e));
    if (s->count == s->bound)
        remove_entry(s, 0);
    fips_rekey_record_entry *dst = &s->entries[s->count++];
    dst->peer = p;
    memcpy(dst->epoch, e, 8);
    dst->record = kept;
}
void fips_rekey_record_store_take(fips_rekey_record_store *s, const fips_node_addr *p,
                                  const uint8_t e[8], fips_rekey_record *out) {
    size_t i = find(s, p, e);
    fips_rekey_record_new(out);
    if (i < s->count) {
        *out = s->entries[i].record;
        remove_entry(s, i);
    }
}
