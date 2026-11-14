#include "proto_stp.h"
#include "../fakes/host.h"
#include <fips/proto/stp/mod.h>
#include <fips/identity/local.h>
#include <stdlib.h>
#include <string.h>
static bool eq(diff_context *ctx, diff_bytes b, const uint8_t *p, size_t n) {
    if (!diff_compare(ctx->id, b.data, b.len, p, n))
        return diff_error(ctx, "STP bytes differ");
    return true;
}
static bool declaration_fields(diff_context *ctx, const fips_parent_declaration *d,
                               const diff_value *v) {
    return eq(ctx, BYTES(v, "node_addr"), d->node_addr.bytes, 16) &&
           eq(ctx, BYTES(v, "parent_addr"), d->parent_id.bytes, 16) &&
           (d->sequence == UINT(v, "sequence") && d->timestamp == UINT(v, "timestamp")
                ? true
                : diff_error(ctx, "declaration sequence/timestamp differ"));
}
result check_proto_parent_declaration(diff_context *ctx, const proto_case *c) {
    const diff_value *v = c->record;
    if (strcmp(c->name, "parent-declaration"))
        return diff_error(ctx, "wrong STP case"), FAILED;
    diff_bytes b = BYTES(v, "signing_bytes");
    fips_reader r;
    fips_parent_declaration d;
    fips_proto_error error;
    fips_reader_init(&r, b.data, b.len);
    if (fips_parent_declaration_decode(&r, &d, &error) != FIPS_OK ||
        !declaration_fields(ctx, &d, v) || r.pos != 48 || fips_reader_remaining(&r))
        return diff_error(ctx, "preimage decode/consumed mismatch"), FAILED;
    uint8_t encoded[48], signature[64], secret[32];
    fips_writer w;
    fips_writer_init(&w, encoded, sizeof(encoded));
    if (fips_parent_declaration_signing_bytes(&d, &w, &error) != FIPS_OK ||
        !eq(ctx, b, encoded, w.pos))
        return FAILED;
    /* Reproduce the exporter identity and auxiliary entropy; do not copy its
     * signature and call that a signature comparison. */
    fips_seeded_entropy seeded = {ITEM(FIELD(c->common_inputs, "identity_seeds"), 0)->as.number ^
                                  UINT64_C(0x6b65797061697273)};
    fips_entropy entropy = fips_seeded_entropy_adapter(&seeded);
    entropy.fill(entropy.ctx, secret, 32);
    fips_fixed_entropy fixed = {(uint8_t)UINT(v, "aux_entropy_byte")};
    fips_entropy aux = fips_fixed_entropy_adapter(&fixed);
    fips_secp *secp = NULL;
    fips_identity *identity = NULL;
    bool ok = fips_identity_init_secp(aux, &secp) == FIPS_IDENTITY_OK &&
              fips_identity_from_secret_bytes(secp, aux, secret, &identity) == FIPS_IDENTITY_OK;
    memset(secret, 0, sizeof(secret));
    if (ok)
        ok = fips_identity_sign(identity, encoded, w.pos, signature) == FIPS_IDENTITY_OK &&
             eq(ctx, BYTES(v, "signature_hex"), signature, 64);
    if (ok) {
        fips_parent_declaration_set_signature(&d, signature);
        ok = d.is_signed && fips_parent_declaration_is_root(&d);
    }
    fips_identity_destroy(identity);
    fips_identity_destroy_secp(secp);
    return ok ? CHECKED : (diff_error(ctx, "seeded declaration signature differs"), FAILED);
}
result check_proto_tree_announce(diff_context *ctx, const proto_case *c) {
    if (strcmp(c->name, "tree-announce"))
        return diff_error(ctx, "wrong STP case"), FAILED;
    const diff_value *v = c->record, *input = FIELD(v, "input");
    diff_bytes b = BYTES(v, "bytes");
    size_t offset = (size_t)UINT(v, "decode_offset");
    if (offset != 1 || b.len < 1 || b.data[0] != 0x10)
        return diff_error(ctx, "TreeAnnounce dispatch byte/offset"), FAILED;
    fips_tree_announce *a = malloc(sizeof(*a));
    uint8_t *encoded = malloc(b.len);
    if (!a || !encoded) {
        free(a);
        free(encoded);
        return diff_error(ctx, "allocation failed"), FAILED;
    }
    fips_reader r;
    fips_writer w;
    fips_proto_error error = {.variant = FIPS_PROTO_InvalidSignature};
    fips_reader_init(&r, b.data + offset, b.len - offset);
    bool ok = fips_tree_announce_decode(&r, a, &error) == FIPS_OK;
    if (ok) {
        /* The exporter records no consumed field for this decode(slice) API.
         * Its entire encoded payload must be consumed, unlike trailing-data tests. */
        ok = r.pos == b.len - offset && !fips_reader_remaining(&r) &&
             fips_tree_announce_validate_semantics(a, NULL) == FIPS_OK;
        diff_value *nodes = FIELD(input, "ancestry_node_addrs");
        ok = ok && COUNT(nodes) == a->ancestry.len;
        for (size_t i = 0; ok && i < COUNT(nodes); i++)
            ok = eq(ctx, ITEM(nodes, i)->as.bytes, a->ancestry.entries[i].node_addr.bytes, 16);
        /* This fixture is a self-root carrying the same preimage as its sole
         * coordinate entry. Compare metadata, not only Debug/address text. */
        ok = ok && a->declaration.is_signed && fips_parent_declaration_is_root(&a->declaration) &&
             a->ancestry.entries[0].sequence == a->declaration.sequence &&
             a->ancestry.entries[0].timestamp == a->declaration.timestamp;
        fips_writer_init(&w, encoded, b.len);
        ok = ok && fips_tree_announce_encoded_len(a) == b.len &&
             fips_tree_announce_encode(a, &w, &error) == FIPS_OK && eq(ctx, b, encoded, w.pos);
    }
    fips_reader_init(&r, NULL, 0);
    const diff_value *expected = FIELD(v, "empty_input_error");
    ok = ok && fips_tree_announce_decode(&r, a, &error) == FIPS_ERR_RANGE &&
         error.variant == FIPS_PROTO_MessageTooShort &&
         !strcmp(TEXT(expected, "variant"), fips_proto_error_name(error.variant)) &&
         error.as.message_too_short.expected == UINT(expected, "expected") &&
         error.as.message_too_short.got == UINT(expected, "got") && r.pos == 0;
    free(a);
    free(encoded);
    if (!ok)
        diff_error(ctx, "TreeAnnounce decode/fields/re-encode/consumed/error mismatch");
    return ok ? CHECKED : FAILED;
}
