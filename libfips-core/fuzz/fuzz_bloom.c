#include <fips/proto/bloom/mod.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) abort(); } while (0)
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* Both decoders accept bounded readers, and failure must be atomic. */
    fips_reader r; fips_filter_announce a, before;
    memset(&before, 0xa5, sizeof(before)); a = before;
    fips_reader_init(&r, data, size);
    fips_proto_error e = {.variant = FIPS_PROTO_InvalidSignature};
    if (fips_filter_announce_decode(&r, &a, &e) == FIPS_OK) {
        CHECK(size == 1034 && r.pos == size && a.size_class == 1 && a.hash_count != 0);
        uint8_t encoded[1035]; fips_writer w; fips_writer_init(&w, encoded, sizeof(encoded));
        CHECK(fips_filter_announce_encode(&a, &w, NULL) == FIPS_OK && w.pos == size + 1 && encoded[0] == 0x20 && !memcmp(encoded + 1, data, size));
        CHECK(e.variant == FIPS_PROTO_InvalidSignature);
        fips_writer_init(&w, encoded, sizeof(encoded) - 1); encoded[0] = 0xcc;
        CHECK(fips_filter_announce_encode(&a, &w, &e) == FIPS_ERR_RANGE && w.pos == 0 && encoded[0] == 0xcc);
    } else {
        CHECK(r.pos == 0 && !memcmp(&a, &before, sizeof(a)));
        CHECK(e.variant == FIPS_PROTO_MessageTooShort || e.variant == FIPS_PROTO_BadSizeClass || e.variant == FIPS_PROTO_BadBloom);
    }
    uint8_t k = size ? data[0] : 0;
    fips_bloom_filter f, original, g;
    memset(&original, 0xa5, sizeof(original)); f = original;
    fips_reader_init(&r, data, size);
    if (fips_bloom_decode(&r, k, &f, &e) != FIPS_OK) {
        CHECK(!size || !k || size > FIPS_BLOOM_MAX_BYTES);
        CHECK(r.pos == 0 && !memcmp(&f, &original, sizeof(f)));
        return 0;
    }
    CHECK(r.pos == size && f.num_bits == size * 8 && f.hash_count == k);
    uint8_t raw[FIPS_BLOOM_MAX_BYTES]; fips_writer w; fips_writer_init(&w, raw, sizeof(raw));
    CHECK(fips_bloom_encode(&f, &w, NULL) == FIPS_OK && w.pos == size && !memcmp(raw, data, size));
    g = f; fips_bloom_clear(&g); fips_bloom_insert_bytes(&g, data, size);
    CHECK(fips_bloom_contains_bytes(&g, data, size)); original = f;
    CHECK(fips_bloom_merge(&f, &g, NULL) == FIPS_OK);
    for (size_t i = 0; i < size; ++i) CHECK(f.bits[i] == (uint8_t)(original.bits[i] | g.bits[i]));
    CHECK(fips_bloom_contains_bytes(&f, data, size));
    double count = 42;
    if (fips_bloom_estimated_count(&f, 1.0, &count)) CHECK(count >= 0.0);
    else CHECK(fips_bloom_count_ones(&f) == f.num_bits && count == 42);
    fips_bloom_filter other; fips_bloom_init(&other);
    if (f.num_bits != other.num_bits) {
        original = f; CHECK(fips_bloom_merge(&f, &other, &e) == FIPS_ERR_RANGE && e.variant == FIPS_PROTO_BadBloom && e.as.bad_bloom.variant == FIPS_BLOOM_InvalidSize);
        CHECK(fips_bloom_equal(&f, &original));
    }
    return 0;
}
