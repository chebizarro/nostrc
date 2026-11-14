#include <fips/proto/bloom/wire.h>
#include <fips/proto/link.h>

void fips_filter_announce_init(fips_filter_announce *a, const fips_bloom_filter *f, uint64_t sequence)
{
    a->filter = *f; a->sequence = sequence;
    a->hash_count = f->hash_count; a->size_class = FIPS_BLOOM_V1_SIZE_CLASS;
}
bool fips_filter_announce_is_valid(const fips_filter_announce *a)
{
    return a->size_class <= 3 && a->filter.num_bits / 8 == ((size_t)512 << a->size_class) && a->filter.hash_count == a->hash_count;
}
bool fips_filter_announce_is_v1_compliant(const fips_filter_announce *a)
{
    return a->size_class == FIPS_BLOOM_V1_SIZE_CLASS && a->hash_count == FIPS_BLOOM_DEFAULT_HASH_COUNT;
}
fips_status fips_filter_announce_encode(const fips_filter_announce *a, fips_writer *w, fips_proto_error *e)
{
    if (!fips_filter_announce_is_valid(a)) {
        if (e) *e = (fips_proto_error){.variant = FIPS_PROTO_Malformed, .as.malformed = "filter size does not match size_class"};
        return FIPS_ERR_RANGE;
    }
    size_t n = FIPS_FILTER_ANNOUNCE_HEADER_SIZE + a->filter.num_bits / 8;
    if (n > fips_writer_remaining(w)) {
        if (e) *e = (fips_proto_error){.variant = FIPS_PROTO_MessageTooLong,
            .as.message_too_long = {w->capacity, w->pos > SIZE_MAX - n ? SIZE_MAX : w->pos + n}};
        return FIPS_ERR_RANGE;
    }
    (void)fips_writer_write_u8(w, FIPS_LINK_FilterAnnounce, NULL);
    (void)fips_writer_write_u64_le(w, a->sequence, NULL);
    (void)fips_writer_write_u8(w, a->hash_count, NULL);
    (void)fips_writer_write_u8(w, a->size_class, NULL);
    return fips_bloom_encode(&a->filter, w, NULL);
}
fips_status fips_filter_announce_decode(fips_reader *r, fips_filter_announce *out, fips_proto_error *e)
{
    if (fips_reader_require(r, 10, e) != FIPS_OK) return FIPS_ERR_RANGE;
    fips_reader copy = *r;
    fips_filter_announce a;
    (void)fips_reader_read_u64_le(&copy, &a.sequence, NULL);
    (void)fips_reader_read_u8(&copy, &a.hash_count, NULL);
    (void)fips_reader_read_u8(&copy, &a.size_class, NULL);
    if (a.size_class != FIPS_BLOOM_V1_SIZE_CLASS) {
        if (e) *e = (fips_proto_error){.variant = FIPS_PROTO_BadSizeClass,
            .as.bad_size_class = {a.size_class, a.size_class > 3 ? 3 : FIPS_BLOOM_V1_SIZE_CLASS}};
        return FIPS_ERR_RANGE;
    }
    if (fips_reader_remaining(&copy) != 1024) {
        if (e) *e = (fips_proto_error){.variant = FIPS_PROTO_MessageTooShort,
            .as.message_too_short = {r->pos > SIZE_MAX - 1034 ? SIZE_MAX : r->pos + 1034, r->len}};
        return FIPS_ERR_RANGE;
    }
    if (fips_bloom_decode(&copy, a.hash_count, &a.filter, e) != FIPS_OK) return FIPS_ERR_RANGE;
    *out = a; *r = copy;
    return FIPS_OK;
}
