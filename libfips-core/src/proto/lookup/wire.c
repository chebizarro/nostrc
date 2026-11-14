#include "internal.h"
bool fips_lookup_request_can_forward(const fips_lookup_request *r) { return r->ttl > 0; }
bool fips_lookup_request_forward(fips_lookup_request *r) {
    if (!r->ttl) return false;
    --r->ttl;
    return true;
}
size_t fips_lookup_request_encoded_len(const fips_lookup_request *r) { return 44 + fips_coords_wire_size(&r->origin_coords); }
size_t fips_lookup_response_encoded_len(const fips_lookup_response *r) { return 91 + fips_coords_wire_size(&r->target_coords); }
static fips_status valid_coords(const fips_tree_coordinate *c, fips_proto_error *e) {
    if (c->len) return FIPS_OK;
    if (e) *e = (fips_proto_error){.variant = FIPS_PROTO_BadCoord, .as.bad_coord = FIPS_COORD_EmptyCoordinate};
    return FIPS_ERR_RANGE;
}
fips_status fips_lookup_request_encode(const fips_lookup_request *r, fips_writer *w, fips_proto_error *e) {
    if (valid_coords(&r->origin_coords, e) != FIPS_OK ||
        lookup_writer_require(w, fips_lookup_request_encoded_len(r), e) != FIPS_OK) return FIPS_ERR_RANGE;
    fips_writer_write_u8(w, 0x30, NULL);
    fips_writer_write_u64_le(w, r->request_id, NULL);
    fips_writer_write_bytes(w, r->target.bytes, 16, NULL);
    fips_writer_write_bytes(w, r->origin.bytes, 16, NULL);
    fips_writer_write_u8(w, r->ttl, NULL);
    fips_writer_write_u16_le(w, r->min_mtu, NULL);
    return fips_encode_coords(&r->origin_coords, w, e);
}
/* Probe the whole coordinate field before touching the large output object. */
static fips_status probe_coords(fips_reader *r, fips_proto_error *e) {
    fips_reader q = *r;
    uint16_t count;
    if (fips_reader_read_u16_le(&q, &count, e) != FIPS_OK ||
        fips_reader_require(r, 2 + (size_t)count * 16, e) != FIPS_OK) return FIPS_ERR_RANGE;
    if (!count) {
        if (e) *e = (fips_proto_error){.variant = FIPS_PROTO_Malformed, .as.malformed = "coordinate with zero entries"};
        return FIPS_ERR_RANGE;
    }
    return fips_reader_advance(r, 2 + (size_t)count * 16, e);
}
fips_status fips_lookup_request_decode(fips_reader *r, fips_lookup_request *out, fips_proto_error *e) {
    fips_reader q = *r;
    if (fips_reader_require(&q, 45, e) != FIPS_OK) return FIPS_ERR_RANGE;
    uint64_t id;
    fips_node_addr target, origin;
    uint8_t ttl;
    uint16_t mtu;
    fips_reader_read_u64_le(&q, &id, NULL);
    fips_reader_read_array(&q, target.bytes, 16, NULL);
    fips_reader_read_array(&q, origin.bytes, 16, NULL);
    fips_reader_read_u8(&q, &ttl, NULL);
    fips_reader_read_u16_le(&q, &mtu, NULL);
    fips_reader coords = q;
    if (probe_coords(&q, e) != FIPS_OK) return FIPS_ERR_RANGE;
    fips_decode_coords(&coords, &out->origin_coords, NULL);
    out->request_id = id; out->target = target; out->origin = origin;
    out->ttl = ttl; out->min_mtu = mtu;
    *r = q;
    return FIPS_OK;
}
fips_status fips_lookup_response_encode(const fips_lookup_response *r, fips_writer *w, fips_proto_error *e) {
    if (valid_coords(&r->target_coords, e) != FIPS_OK ||
        lookup_writer_require(w, fips_lookup_response_encoded_len(r), e) != FIPS_OK) return FIPS_ERR_RANGE;
    fips_writer_write_u8(w, 0x31, NULL);
    fips_writer_write_u64_le(w, r->request_id, NULL);
    fips_writer_write_bytes(w, r->target.bytes, 16, NULL);
    fips_writer_write_u16_le(w, r->path_mtu, NULL);
    fips_encode_coords(&r->target_coords, w, NULL);
    return fips_writer_write_bytes(w, r->proof, 64, e);
}
fips_status fips_lookup_response_decode(fips_reader *r, fips_lookup_response *out, fips_proto_error *e) {
    fips_reader q = *r;
    if (fips_reader_require(&q, 92, e) != FIPS_OK) return FIPS_ERR_RANGE;
    uint64_t id;
    fips_node_addr target;
    uint16_t mtu;
    const uint8_t *proof;
    fips_reader_read_u64_le(&q, &id, NULL);
    fips_reader_read_array(&q, target.bytes, 16, NULL);
    fips_reader_read_u16_le(&q, &mtu, NULL);
    fips_reader coords = q;
    if (probe_coords(&q, e) != FIPS_OK || fips_reader_read_bytes(&q, 64, &proof, e) != FIPS_OK) return FIPS_ERR_RANGE;
    /* Rust Signature::from_slice checks length only, not curve validity. */
    fips_decode_coords(&coords, &out->target_coords, NULL);
    out->request_id = id; out->target = target; out->path_mtu = mtu;
    memcpy(out->proof, proof, 64);
    *r = q;
    return FIPS_OK;
}
fips_status fips_lookup_response_proof_bytes(uint64_t id, const fips_node_addr *target,
    const fips_tree_coordinate *coords, fips_writer *w, fips_proto_error *e) {
    if (valid_coords(coords, e) != FIPS_OK ||
        lookup_writer_require(w, 24 + fips_coords_wire_size(coords), e) != FIPS_OK) return FIPS_ERR_RANGE;
    fips_writer_write_u64_le(w, id, NULL);
    fips_writer_write_bytes(w, target->bytes, 16, NULL);
    return fips_encode_coords(coords, w, e);
}
void fips_lookup_response_proof_digest(const fips_lookup_response *r, uint8_t out[8]) { memcpy(out, r->proof, 8); }
fips_status fips_lookup_response_verify(const fips_lookup_response *r, const fips_secp *secp,
    const fips_peer_identity *peer, uint8_t *scratch, size_t capacity, fips_proto_error *e) {
    if (lookup_addr_cmp(r->target, peer->node_addr)) return lookup_error(e, FIPS_PROTO_InvalidSignature);
    fips_writer w;
    fips_writer_init(&w, scratch, capacity);
    if (fips_lookup_response_proof_bytes(r->request_id, &r->target, &r->target_coords, &w, e) != FIPS_OK) return FIPS_ERR_RANGE;
    if (fips_peer_identity_verify(secp, peer, scratch, w.pos, r->proof) != FIPS_IDENTITY_OK)
        return lookup_error(e, FIPS_PROTO_InvalidSignature);
    return FIPS_OK;
}
fips_status fips_lookup_response_sign(fips_lookup_response *r, const fips_identity *identity,
    fips_lookup_sign_limiter *limiter, const fips_node_addr *from, fips_instant now,
    uint8_t *scratch, size_t capacity, bool *signed_out, fips_proto_error *e) {
    if (lookup_addr_cmp(r->target, fips_identity_node_addr(identity))) return lookup_error(e, FIPS_PROTO_InvalidSignature);
    fips_writer w;
    fips_writer_init(&w, scratch, capacity);
    if (fips_lookup_response_proof_bytes(r->request_id, &r->target, &r->target_coords, &w, e) != FIPS_OK) return FIPS_ERR_RANGE;
    bool allowed;
    if (fips_lookup_should_sign(limiter, from->bytes, now, &allowed) != FIPS_OK) return FIPS_ERR_RANGE;
    if (!allowed) { *signed_out = false; return FIPS_OK; }
    uint8_t signature[64];
    if (fips_identity_sign(identity, scratch, w.pos, signature) != FIPS_IDENTITY_OK)
        return lookup_error(e, FIPS_PROTO_InvalidSignature);
    memcpy(r->proof, signature, 64);
    r->path_mtu = UINT16_MAX;
    *signed_out = true;
    return FIPS_OK;
}
