#include "proto_lookup.h"
#include "../fakes/host.h"
#include <fips/proto/lookup/mod.h>
#include <stdlib.h>
#include <string.h>
/* Reconstruct the exporter's independent per-seed keypair streams. Entropy for
 * signatures is FixedEntropy<0x5a>, not the secret-generation stream. */
static bool identity(const proto_case *c, size_t index, fips_secp *secp, fips_entropy aux, fips_identity **out) {
    const diff_value *seeds = FIELD(c->common_inputs, "identity_seeds");
    if (index >= COUNT(seeds)) return false;
    fips_seeded_entropy stream = {ITEM(seeds, index)->as.number ^ UINT64_C(0x6b65797061697273)};
    fips_entropy entropy = fips_seeded_entropy_adapter(&stream);
    uint8_t secret[32];
    entropy.fill(entropy.ctx, secret, sizeof(secret));
    bool ok = fips_identity_from_secret_bytes(secp, aux, secret, out) == FIPS_IDENTITY_OK;
    volatile uint8_t *wipe = secret;
    for (size_t i = 0; i < sizeof(secret); i++) wipe[i] = 0;
    return ok;
}
static bool eq(diff_context *ctx, diff_bytes expected, const uint8_t *p, size_t n) {
    return diff_compare(ctx->id, expected.data, expected.len, p, n) || diff_error(ctx, "lookup bytes differ");
}
static bool coords_match(const fips_tree_coordinate *coords, fips_node_addr first, fips_node_addr root, size_t count) {
    if (coords->len != count || memcmp(coords->entries[0].node_addr.bytes, first.bytes, 16) ||
        memcmp(coords->entries[count - 1].node_addr.bytes, root.bytes, 16)) return false;
    for (size_t i = 0; i < coords->len; i++)
        if (coords->entries[i].sequence || coords->entries[i].timestamp) return false;
    return true;
}
static bool empty_error(const diff_value *v, const fips_proto_error *e) {
    const diff_value *expected = FIELD(v, "empty_input_error");
    return e->variant == FIPS_PROTO_MessageTooShort && !strcmp(TEXT(expected, "variant"), fips_proto_error_name(e->variant)) &&
        UINT(expected, "expected") == e->as.message_too_short.expected && UINT(expected, "got") == e->as.message_too_short.got;
}
result check_proto_lookup_request(diff_context *ctx, const proto_case *c) {
    if (strcmp(c->name, "lookup-request")) return diff_error(ctx, "wrong lookup request case"), FAILED;
    const diff_value *v = c->record, *in = FIELD(v, "input");
    diff_bytes b = BYTES(v, "bytes");
    size_t offset = (size_t)UINT(v, "decode_offset");
    if (offset != 1 || b.len < offset || b.data[0] != 0x30) return diff_error(ctx, "lookup request dispatch/offset"), FAILED;
    fips_lookup_request *r = malloc(sizeof(*r));
    uint8_t *encoded = malloc(b.len);
    fips_fixed_entropy fixed = {0x5a};
    fips_entropy aux = fips_fixed_entropy_adapter(&fixed);
    fips_secp *secp = NULL;
    fips_identity *target = NULL, *origin = NULL;
    bool ok = r && encoded && fips_identity_init_secp(aux, &secp) == FIPS_IDENTITY_OK &&
        identity(c, (size_t)UINT(in, "target_seed"), secp, aux, &target) &&
        identity(c, (size_t)UINT(in, "origin_seed"), secp, aux, &origin);
    fips_reader reader;
    fips_proto_error e;
    fips_reader_init(&reader, b.data + offset, b.len - offset);
    if (ok) {
        fips_node_addr t = fips_identity_node_addr(target), o = fips_identity_node_addr(origin);
        ok = fips_lookup_request_decode(&reader, r, &e) == FIPS_OK && reader.pos == b.len - offset &&
            !fips_reader_remaining(&reader) && r->request_id == UINT(in, "request_id") &&
            !memcmp(r->target.bytes, t.bytes, 16) && !memcmp(r->origin.bytes, o.bytes, 16) &&
            r->ttl == UINT(in, "ttl") && r->min_mtu == UINT(in, "min_mtu") &&
            !strcmp(TEXT(in, "origin_coord"), "src") && coords_match(&r->origin_coords, o, o, 1);
        fips_writer w;
        fips_writer_init(&w, encoded, b.len);
        ok = ok && fips_lookup_request_encode(r, &w, &e) == FIPS_OK && eq(ctx, b, encoded, w.pos);
        fips_reader_init(&reader, NULL, 0);
        ok = ok && fips_lookup_request_decode(&reader, r, &e) == FIPS_ERR_RANGE && empty_error(v, &e) && reader.pos == 0;
    }
    fips_identity_destroy(target); fips_identity_destroy(origin); fips_identity_destroy_secp(secp);
    free(r); free(encoded);
    if (!ok) diff_error(ctx, "lookup request decode/fields/re-encode/consumed/error mismatch");
    return ok ? CHECKED : FAILED;
}
result check_proto_lookup_response(diff_context *ctx, const proto_case *c) {
    if (strcmp(c->name, "lookup-response")) return diff_error(ctx, "wrong lookup response case"), FAILED;
    const diff_value *v = c->record, *in = FIELD(v, "input");
    diff_bytes b = BYTES(v, "bytes");
    size_t offset = (size_t)UINT(v, "decode_offset");
    if (offset != 1 || b.len < offset || b.data[0] != 0x31) return diff_error(ctx, "lookup response dispatch/offset"), FAILED;
    fips_lookup_response *r = malloc(sizeof(*r));
    uint8_t *encoded = malloc(b.len), *scratch = malloc(b.len);
    fips_fixed_entropy fixed = {(uint8_t)UINT(in, "aux_entropy_byte")};
    fips_entropy aux = fips_fixed_entropy_adapter(&fixed);
    fips_secp *secp = NULL;
    fips_identity *target = NULL, *origin = NULL;
    bool ok = r && encoded && scratch && fips_identity_init_secp(aux, &secp) == FIPS_IDENTITY_OK &&
        identity(c, (size_t)UINT(in, "target_seed"), secp, aux, &target) && identity(c, 0, secp, aux, &origin);
    fips_reader reader;
    fips_writer w;
    fips_proto_error e;
    fips_reader_init(&reader, b.data + offset, b.len - offset);
    if (ok) {
        fips_node_addr t = fips_identity_node_addr(target), o = fips_identity_node_addr(origin);
        fips_xonly_pubkey key = fips_identity_pubkey(target);
        fips_peer_identity peer;
        ok = fips_lookup_response_decode(&reader, r, &e) == FIPS_OK && reader.pos == b.len - offset &&
            !fips_reader_remaining(&reader) && fips_peer_identity_from_pubkey(secp, &key, &peer) == FIPS_IDENTITY_OK &&
            fips_lookup_response_verify(r, secp, &peer, scratch, b.len, &e) == FIPS_OK &&
            r->request_id == UINT(in, "request_id") && !memcmp(r->target.bytes, t.bytes, 16) &&
            r->path_mtu == UINT16_MAX && !strcmp(TEXT(in, "target_coord"), "dst") &&
            coords_match(&r->target_coords, t, o, 2) && eq(ctx, BYTES(in, "proof_hex"), r->proof, 64);
        fips_writer_init(&w, scratch, b.len);
        ok = ok && fips_lookup_response_proof_bytes(r->request_id, &r->target, &r->target_coords, &w, &e) == FIPS_OK &&
            eq(ctx, BYTES(in, "proof_signing_hex"), scratch, w.pos);
        uint8_t signature[64];
        ok = ok && fips_identity_sign(target, scratch, w.pos, signature) == FIPS_IDENTITY_OK &&
            eq(ctx, BYTES(in, "proof_hex"), signature, 64);
        fips_writer_init(&w, encoded, b.len);
        ok = ok && fips_lookup_response_encode(r, &w, &e) == FIPS_OK && eq(ctx, b, encoded, w.pos);
        fips_reader_init(&reader, NULL, 0);
        ok = ok && fips_lookup_response_decode(&reader, r, &e) == FIPS_ERR_RANGE && empty_error(v, &e) && reader.pos == 0;
    }
    fips_identity_destroy(target); fips_identity_destroy(origin); fips_identity_destroy_secp(secp);
    free(r); free(encoded); free(scratch);
    if (!ok) diff_error(ctx, "lookup response decode/identity/signature/fields/re-encode/consumed/error mismatch");
    return ok ? CHECKED : FAILED;
}
