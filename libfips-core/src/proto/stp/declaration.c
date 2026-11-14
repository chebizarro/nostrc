#include <fips/proto/stp/declaration.h>
#include "internal.h"
fips_parent_declaration fips_parent_declaration_new(fips_node_addr n, fips_node_addr p, uint64_t s,
                                                    uint64_t t) {
    return (fips_parent_declaration){.node_addr = n, .parent_id = p, .sequence = s, .timestamp = t};
}
fips_parent_declaration fips_parent_declaration_self_root(fips_node_addr n, uint64_t s,
                                                          uint64_t t) {
    return fips_parent_declaration_new(n, n, s, t);
}
void fips_parent_declaration_set_signature(fips_parent_declaration *d, const uint8_t s[64]) {
    memcpy(d->signature, s, 64);
    d->is_signed = true;
}
fips_parent_declaration fips_parent_declaration_with_signature(fips_node_addr n, fips_node_addr p,
                                                               uint64_t s, uint64_t t,
                                                               const uint8_t sig[64]) {
    fips_parent_declaration d = fips_parent_declaration_new(n, p, s, t);
    fips_parent_declaration_set_signature(&d, sig);
    return d;
}
bool fips_parent_declaration_is_root(const fips_parent_declaration *d) {
    return !addr_cmp(d->node_addr, d->parent_id);
}
bool fips_parent_declaration_equal(const fips_parent_declaration *a,
                                   const fips_parent_declaration *b) {
    return !addr_cmp(a->node_addr, b->node_addr) && !addr_cmp(a->parent_id, b->parent_id) &&
           a->sequence == b->sequence && a->timestamp == b->timestamp;
}
bool fips_parent_declaration_is_fresher_than(const fips_parent_declaration *a,
                                             const fips_parent_declaration *b) {
    return a->sequence > b->sequence;
}
fips_status fips_parent_declaration_signing_bytes(const fips_parent_declaration *d, fips_writer *w,
                                                  fips_proto_error *e) {
    if (writer_require(w, 48, e) != FIPS_OK)
        return FIPS_ERR_RANGE;
    fips_writer_write_bytes(w, d->node_addr.bytes, 16, NULL);
    fips_writer_write_bytes(w, d->parent_id.bytes, 16, NULL);
    fips_writer_write_u64_le(w, d->sequence, NULL);
    fips_writer_write_u64_le(w, d->timestamp, NULL);
    return FIPS_OK;
}
fips_status fips_parent_declaration_decode(fips_reader *r, fips_parent_declaration *out,
                                           fips_proto_error *e) {
    if (fips_reader_require(r, 48, e) != FIPS_OK)
        return FIPS_ERR_RANGE;
    fips_parent_declaration d = {0};
    fips_reader_read_array(r, d.node_addr.bytes, 16, NULL);
    fips_reader_read_array(r, d.parent_id.bytes, 16, NULL);
    fips_reader_read_u64_le(r, &d.sequence, NULL);
    fips_reader_read_u64_le(r, &d.timestamp, NULL);
    *out = d;
    return FIPS_OK;
}
