#include <fips/proto/bloom/core.h>
#include <fips/proto/math.h>
#include <sodium/crypto_hash_sha256.h>
#include "log.h"
#include <string.h>

static fips_status bad_bloom(fips_proto_error *e, fips_bloom_error b)
{
    if (e) *e = (fips_proto_error){.variant = FIPS_PROTO_BadBloom, .as.bad_bloom = b};
    return FIPS_ERR_RANGE;
}
fips_status fips_bloom_with_params(fips_bloom_filter *out, size_t n, uint8_t k, fips_proto_error *e)
{
    if (!n || n % 8) return bad_bloom(e, (fips_bloom_error){.variant = FIPS_BLOOM_SizeNotByteAligned, .as.size_not_byte_aligned = n});
    if (!k) return bad_bloom(e, (fips_bloom_error){.variant = FIPS_BLOOM_ZeroHashCount});
    if (n / 8 > FIPS_BLOOM_MAX_BYTES) {
        if (e) *e = (fips_proto_error){.variant = FIPS_PROTO_MessageTooLong, .as.message_too_long = {FIPS_BLOOM_MAX_BYTES, n / 8}};
        return FIPS_ERR_RANGE;
    }
    memset(out, 0, sizeof(*out));
    out->num_bits = n; out->hash_count = k;
    return FIPS_OK;
}
void fips_bloom_init(fips_bloom_filter *f)
{
    (void)fips_bloom_with_params(f, FIPS_BLOOM_DEFAULT_BITS, FIPS_BLOOM_DEFAULT_HASH_COUNT, NULL);
}
fips_status fips_bloom_from_bytes(fips_bloom_filter *out, const uint8_t *bytes, size_t len, uint8_t k, fips_proto_error *e)
{
    /* Rust from_bytes checks k before empty length. */
    if (!k) return bad_bloom(e, (fips_bloom_error){.variant = FIPS_BLOOM_ZeroHashCount});
    if (len > FIPS_BLOOM_MAX_BYTES) {
        if (e) *e = (fips_proto_error){.variant = FIPS_PROTO_MessageTooLong, .as.message_too_long = {FIPS_BLOOM_MAX_BYTES, len}};
        return FIPS_ERR_RANGE;
    }
    if (fips_bloom_with_params(out, len * 8, k, e) != FIPS_OK) return FIPS_ERR_RANGE;
    memcpy(out->bits, bytes, len);
    return FIPS_OK;
}
fips_status fips_bloom_encode(const fips_bloom_filter *f, fips_writer *w, fips_proto_error *e)
{
    return fips_writer_write_bytes(w, f->bits, f->num_bits / 8, e);
}
fips_status fips_bloom_decode(fips_reader *r, uint8_t k, fips_bloom_filter *out, fips_proto_error *e)
{
    size_t n = fips_reader_remaining(r);
    fips_status status = fips_bloom_from_bytes(out, fips_reader_rest(r), n, k, e);
    if (status == FIPS_OK) status = fips_reader_advance(r, n, NULL);
    return status;
}
static uint64_t le64(const uint8_t *p)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (8 * i);
    return v;
}
static void hashes(const uint8_t *bytes, size_t len, uint64_t *h1, uint64_t *h2)
{
    uint8_t digest[crypto_hash_sha256_BYTES];
    (void)crypto_hash_sha256(digest, bytes, (unsigned long long)len);
    *h1 = le64(digest); *h2 = le64(digest + 8);
}
void fips_bloom_insert_bytes(fips_bloom_filter *f, const uint8_t *bytes, size_t len)
{
    uint64_t h1, h2;
    hashes(bytes, len, &h1, &h2);
    for (unsigned i = 0; i < f->hash_count; ++i) {
        size_t bit = (size_t)(h1 + (uint64_t)i * h2) % f->num_bits;
        f->bits[bit / 8] |= (uint8_t)(1u << (bit % 8));
    }
}
bool fips_bloom_contains_bytes(const fips_bloom_filter *f, const uint8_t *bytes, size_t len)
{
    uint64_t h1, h2;
    hashes(bytes, len, &h1, &h2);
    for (unsigned i = 0; i < f->hash_count; ++i) {
        size_t bit = (size_t)(h1 + (uint64_t)i * h2) % f->num_bits;
        if (!(f->bits[bit / 8] & (1u << (bit % 8)))) return false;
    }
    return true;
}
void fips_bloom_insert(fips_bloom_filter *f, const fips_node_addr *a) { fips_bloom_insert_bytes(f, a->bytes, sizeof(a->bytes)); }
bool fips_bloom_contains(const fips_bloom_filter *f, const fips_node_addr *a) { return fips_bloom_contains_bytes(f, a->bytes, sizeof(a->bytes)); }
fips_status fips_bloom_merge(fips_bloom_filter *f, const fips_bloom_filter *b, fips_proto_error *e)
{
    if (f->num_bits != b->num_bits) return bad_bloom(e, (fips_bloom_error){.variant = FIPS_BLOOM_InvalidSize, .as.invalid_size = {f->num_bits, b->num_bits}});
    for (size_t i = 0; i < f->num_bits / 8; ++i) f->bits[i] |= b->bits[i];
    return FIPS_OK;
}
fips_status fips_bloom_union(const fips_bloom_filter *a, const fips_bloom_filter *b, fips_bloom_filter *out, fips_proto_error *e)
{
    fips_bloom_filter f = *a;
    fips_status status = fips_bloom_merge(&f, b, e);
    if (status == FIPS_OK) *out = f;
    return status;
}
void fips_bloom_clear(fips_bloom_filter *f) { memset(f->bits, 0, f->num_bits / 8); }
bool fips_bloom_equal(const fips_bloom_filter *a, const fips_bloom_filter *b)
{
    return a->num_bits == b->num_bits && a->hash_count == b->hash_count && !memcmp(a->bits, b->bits, a->num_bits / 8);
}
static unsigned popcount(uint8_t b)
{
    unsigned n = 0;
    while (b) { b = (uint8_t)(b & (b - 1)); ++n; }
    return n;
}
size_t fips_bloom_count_ones(const fips_bloom_filter *f)
{
    size_t n = 0;
    for (size_t i = 0; i < f->num_bits / 8; ++i) n += popcount(f->bits[i]);
    return n;
}
bool fips_bloom_is_empty(const fips_bloom_filter *f) { return fips_bloom_count_ones(f) == 0; }
double fips_bloom_fill_ratio(const fips_bloom_filter *f) { return (double)fips_bloom_count_ones(f) / (double)f->num_bits; }
double fips_bloom_fpr(const fips_bloom_filter *f) { return fips_powi(fips_bloom_fill_ratio(f), f->hash_count); }
bool fips_bloom_estimated_count(const fips_bloom_filter *f, double cap, double *out)
{
    double m = (double)f->num_bits, k = (double)f->hash_count, x = (double)fips_bloom_count_ones(f);
    if (x >= m || fips_bloom_fpr(f) > cap) return false;
    *out = -(m / k) * fips_bloom_log(1.0 - x / m);
    return true;
}
bool fips_bloom_overlap(const fips_bloom_filter *f, const fips_bloom_filter *sent, double *out)
{
    if (f->num_bits != sent->num_bits) return false;
    size_t n = fips_bloom_count_ones(sent), both = 0;
    if (!n) return false;
    for (size_t i = 0; i < f->num_bits / 8; ++i) both += popcount((uint8_t)(f->bits[i] & sent->bits[i]));
    *out = (double)both / (double)n;
    return true;
}
double fips_bloom_echo_bound(const fips_bloom_filter *f, double threshold)
{
    double fill = fips_bloom_fill_ratio(f);
    return fill + threshold * (1.0 - fill);
}
bool fips_bloom_echoes(const fips_bloom_filter *f, const fips_bloom_filter *sent, double threshold)
{
    double overlap;
    if (threshold >= 1.0 || fips_bloom_count_ones(sent) < FIPS_BLOOM_ECHO_MIN_BITS ||
        !fips_bloom_overlap(f, sent, &overlap) || fips_bloom_count_ones(f) >= f->num_bits) return false;
    return overlap > fips_bloom_echo_bound(f, threshold);
}
