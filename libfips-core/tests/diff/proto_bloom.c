#include "proto_bloom.h"
#include "../fakes/host.h"
#include <fips/proto/bloom/mod.h>
#include <fips/identity/local.h>
#include <stdlib.h>
#include <string.h>

/* Independently reconstruct proto.rs's seeded identities, so the announce
 * check does not merely decode and re-encode a possibly wrong filter. */
static bool seeded_filter(diff_context *ctx, const proto_case *c, fips_bloom_filter *out)
{
    const diff_value *seeds = FIELD(c->common_inputs, "identity_seeds");
    fips_bloom_init(out);
    for (size_t i = 0; i < COUNT(seeds); ++i) {
        fips_seeded_entropy rng = {ITEM(seeds, i)->as.number ^ UINT64_C(0x6b65797061697273)};
        fips_entropy entropy = fips_seeded_entropy_adapter(&rng);
        fips_fixed_entropy fixed = {0x5a};
        fips_entropy blind = fips_fixed_entropy_adapter(&fixed);
        uint8_t secret[32];
        entropy.fill(entropy.ctx, secret, sizeof(secret));
        fips_secp *secp = NULL;
        fips_identity *id = NULL;
        bool ok = fips_identity_init_secp(blind, &secp) == FIPS_IDENTITY_OK &&
            fips_identity_from_secret_bytes(secp, blind, secret, &id) == FIPS_IDENTITY_OK;
        if (ok) { fips_node_addr addr = fips_identity_node_addr(id); fips_bloom_insert(out, &addr); }
        memset(secret, 0, sizeof(secret));
        fips_identity_destroy(id); fips_identity_destroy_secp(secp);
        if (!ok) return diff_error(ctx, "seeded bloom identity construction failed");
    }
    return true;
}
result check_proto_bloom(diff_context *ctx, const proto_case *c)
{
    if (strcmp(c->name, "bloom")) return diff_error(ctx, "not the bloom case"), FAILED;
    const diff_value *v = c->record, *addrs = FIELD(v, "inserted_node_addrs");
    diff_bytes bytes = BYTES(v, "bytes");
    fips_bloom_filter f, seeded, decoded;
    fips_proto_error error = {.variant = FIPS_PROTO_InvalidSignature};
    if (UINT(v, "num_bits") > SIZE_MAX || UINT(v, "hash_count") > UINT8_MAX ||
        fips_bloom_with_params(&f, (size_t)UINT(v, "num_bits"), (uint8_t)UINT(v, "hash_count"), &error) != FIPS_OK)
        return diff_error(ctx, "invalid bloom parameters"), FAILED;
    bool ok = f.num_bits == FIPS_BLOOM_DEFAULT_BITS && f.hash_count == FIPS_BLOOM_DEFAULT_HASH_COUNT;
    for (size_t i = 0; i < COUNT(addrs); ++i) {
        diff_bytes b = ITEM(addrs, i)->as.bytes;
        if (b.len != 16) return diff_error(ctx, "bad node address length"), FAILED;
        fips_bloom_insert_bytes(&f, b.data, b.len);
    }
    for (size_t i = 0; i < COUNT(addrs); ++i) {
        diff_bytes b = ITEM(addrs, i)->as.bytes;
        ok &= fips_bloom_contains_bytes(&f, b.data, b.len);
    }
    ok &= fips_bloom_count_ones(&f) == UINT(v, "ones");
    if (!seeded_filter(ctx, c, &seeded)) return FAILED;
    ok &= fips_bloom_equal(&f, &seeded);
    uint8_t encoded[FIPS_BLOOM_MAX_BYTES];
    fips_writer w; fips_reader r;
    fips_writer_init(&w, encoded, sizeof(encoded));
    ok &= fips_bloom_encode(&f, &w, &error) == FIPS_OK;
    ok &= diff_compare(ctx->id, bytes.data, bytes.len, encoded, fips_writer_len(&w));
    fips_reader_init(&r, bytes.data, bytes.len);
    if (fips_bloom_decode(&r, f.hash_count, &decoded, &error) != FIPS_OK)
        return diff_error(ctx, "raw bloom decode failed"), FAILED;
    ok &= fips_bloom_equal(&f, &decoded) && fips_reader_remaining(&r) == 0;
    ok &= error.variant == FIPS_PROTO_InvalidSignature;
    if (!ok) diff_error(ctx, "bloom bytes/parameters/population/seeded identities differ");
    return ok ? CHECKED : FAILED;
}
result check_proto_filter_announce(diff_context *ctx, const proto_case *c)
{
    if (strcmp(c->name, "filter-announce")) return diff_error(ctx, "not filter-announce case"), FAILED;
    const diff_value *v = c->record, *input = FIELD(v, "input"), *empty = FIELD(v, "empty_input_error");
    diff_bytes bytes = BYTES(v, "bytes");
    if (UINT(v, "decode_offset") != 1 || bytes.len != FIPS_FILTER_ANNOUNCE_V1_WIRE_SIZE ||
        strcmp(TEXT(input, "filter_case"), "bloom")) return diff_error(ctx, "announce layout/reference mismatch"), FAILED;
    fips_bloom_filter f;
    if (!seeded_filter(ctx, c, &f)) return FAILED;
    fips_filter_announce a, decoded;
    fips_filter_announce_init(&a, &f, UINT(input, "sequence"));
    fips_proto_error e = {.variant = FIPS_PROTO_InvalidSignature};
    fips_writer w; fips_reader r;
    uint8_t encoded[FIPS_FILTER_ANNOUNCE_V1_WIRE_SIZE];
    fips_writer_init(&w, encoded, sizeof(encoded));
    bool ok = fips_filter_announce_encode(&a, &w, &e) == FIPS_OK;
    ok &= diff_compare(ctx->id, bytes.data, bytes.len, encoded, fips_writer_len(&w));
    fips_reader_init(&r, bytes.data + 1, bytes.len - 1);
    if (fips_filter_announce_decode(&r, &decoded, &e) != FIPS_OK) return diff_error(ctx, "valid announce rejected"), FAILED;
    ok &= fips_bloom_equal(&decoded.filter, &f) && decoded.sequence == a.sequence &&
        decoded.hash_count == 5 && decoded.size_class == 1 && fips_reader_remaining(&r) == 0 && e.variant == FIPS_PROTO_InvalidSignature;
    fips_filter_announce sentinel = decoded;
    fips_reader_init(&r, NULL, 0);
    ok &= fips_filter_announce_decode(&r, &decoded, &e) == FIPS_ERR_RANGE;
    ok &= !strcmp(TEXT(empty, "variant"), fips_proto_error_name(e.variant)) &&
        e.variant == FIPS_PROTO_MessageTooShort && UINT(empty, "expected") == e.as.message_too_short.expected &&
        UINT(empty, "got") == e.as.message_too_short.got && !memcmp(&sentinel, &decoded, sizeof(decoded)) && r.pos == 0;
    fips_writer_init(&w, encoded, sizeof(encoded) - 1);
    ok &= fips_filter_announce_encode(&a, &w, &e) == FIPS_ERR_RANGE && w.pos == 0 && e.variant == FIPS_PROTO_MessageTooLong;
    if (!ok) diff_error(ctx, "FilterAnnounce wire/fields/empty-input error/atomicity mismatch");
    return ok ? CHECKED : FAILED;
}
