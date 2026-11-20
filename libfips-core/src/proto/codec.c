#include <fips/proto/codec.h>
#include <string.h>

const char *fips_proto_error_name(fips_proto_error_variant variant)
{
    switch (variant) {
    case FIPS_PROTO_InvalidMessageType: return "InvalidMessageType";
    case FIPS_PROTO_MessageTooShort: return "MessageTooShort";
    case FIPS_PROTO_MessageTooLong: return "MessageTooLong";
    case FIPS_PROTO_InvalidSignature: return "InvalidSignature";
    case FIPS_PROTO_UnsupportedVersion: return "UnsupportedVersion";
    case FIPS_PROTO_Malformed: return "Malformed";
    case FIPS_PROTO_BadSizeClass: return "BadSizeClass";
    case FIPS_PROTO_BadCoord: return "BadCoord";
    case FIPS_PROTO_BadBloom: return "BadBloom";
    case FIPS_PROTO_HopLimitExceeded: return "HopLimitExceeded";
    case FIPS_PROTO_TtlExpired: return "TtlExpired";
    }
    return "Unknown";
}

static size_t offset(size_t pos, size_t n)
{
    return n > SIZE_MAX - pos ? SIZE_MAX : pos + n;
}

void fips_reader_init(fips_reader *r, const uint8_t *buf, size_t len)
{
    *r = (fips_reader){buf, len, 0};
}
size_t fips_reader_position(const fips_reader *r) { return r->pos; }
size_t fips_reader_remaining(const fips_reader *r) { return r->len - r->pos; }
const uint8_t *fips_reader_rest(const fips_reader *r)
{
    return r->buf ? r->buf + r->pos : NULL;
}
fips_status fips_reader_require(const fips_reader *r, size_t n, fips_proto_error *error)
{
    if (n > fips_reader_remaining(r)) {
        if (error) {
            *error = (fips_proto_error){.variant = FIPS_PROTO_MessageTooShort,
                .as.message_too_short = {offset(r->pos, n), r->len}};
        }
        return FIPS_ERR_RANGE;
    }
    return FIPS_OK;
}
fips_status fips_reader_advance(fips_reader *r, size_t n, fips_proto_error *error)
{
    if (fips_reader_require(r, n, error) != FIPS_OK) return FIPS_ERR_RANGE;
    r->pos += n;
    return FIPS_OK;
}
fips_status fips_reader_read_bytes(fips_reader *r, size_t n, const uint8_t **out, fips_proto_error *error)
{
    if (fips_reader_require(r, n, error) != FIPS_OK) return FIPS_ERR_RANGE;
    *out = fips_reader_rest(r);
    r->pos += n;
    return FIPS_OK;
}
fips_status fips_reader_read_array(fips_reader *r, uint8_t *out, size_t n, fips_proto_error *error)
{
    if (fips_reader_require(r, n, error) != FIPS_OK) return FIPS_ERR_RANGE;
    if (n) memcpy(out, r->buf + r->pos, n);
    r->pos += n;
    return FIPS_OK;
}
fips_status fips_reader_read_u8(fips_reader *r, uint8_t *out, fips_proto_error *error)
{
    return fips_reader_read_array(r, out, 1, error);
}
fips_status fips_reader_read_u16_le(fips_reader *r, uint16_t *out, fips_proto_error *error)
{
    uint8_t b[2];
    if (fips_reader_read_array(r, b, sizeof(b), error) != FIPS_OK) return FIPS_ERR_RANGE;
    *out = (uint16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
    return FIPS_OK;
}
fips_status fips_reader_read_u32_le(fips_reader *r, uint32_t *out, fips_proto_error *error)
{
    uint8_t b[4];
    if (fips_reader_read_array(r, b, sizeof(b), error) != FIPS_OK) return FIPS_ERR_RANGE;
    uint32_t v = 0;
    for (size_t i = 0; i < sizeof(b); ++i) v |= (uint32_t)b[i] << (8 * i);
    *out = v;
    return FIPS_OK;
}
fips_status fips_reader_read_u64_le(fips_reader *r, uint64_t *out, fips_proto_error *error)
{
    uint8_t b[8];
    if (fips_reader_read_array(r, b, sizeof(b), error) != FIPS_OK) return FIPS_ERR_RANGE;
    uint64_t v = 0;
    for (size_t i = 0; i < sizeof(b); ++i) v |= (uint64_t)b[i] << (8 * i);
    *out = v;
    return FIPS_OK;
}

void fips_writer_init(fips_writer *w, uint8_t *buf, size_t capacity)
{
    *w = (fips_writer){buf, capacity, 0};
}
size_t fips_writer_len(const fips_writer *w) { return w->pos; }
size_t fips_writer_remaining(const fips_writer *w) { return w->capacity - w->pos; }
fips_status fips_writer_write_bytes(fips_writer *w, const uint8_t *bytes, size_t n, fips_proto_error *error)
{
    if (n > fips_writer_remaining(w)) {
        if (error) {
            *error = (fips_proto_error){.variant = FIPS_PROTO_MessageTooLong,
                .as.message_too_long = {w->capacity, offset(w->pos, n)}};
        }
        return FIPS_ERR_RANGE;
    }
    if (n) memcpy(w->buf + w->pos, bytes, n);
    w->pos += n;
    return FIPS_OK;
}
fips_status fips_writer_write_u8(fips_writer *w, uint8_t v, fips_proto_error *error)
{
    return fips_writer_write_bytes(w, &v, 1, error);
}
fips_status fips_writer_write_u16_le(fips_writer *w, uint16_t v, fips_proto_error *error)
{
    uint8_t b[2];
    for (size_t i = 0; i < sizeof(b); ++i) b[i] = (uint8_t)(v >> (8 * i));
    return fips_writer_write_bytes(w, b, sizeof(b), error);
}
fips_status fips_writer_write_u32_le(fips_writer *w, uint32_t v, fips_proto_error *error)
{
    uint8_t b[4];
    for (size_t i = 0; i < sizeof(b); ++i) b[i] = (uint8_t)(v >> (8 * i));
    return fips_writer_write_bytes(w, b, sizeof(b), error);
}
fips_status fips_writer_write_u64_le(fips_writer *w, uint64_t v, fips_proto_error *error)
{
    uint8_t b[8];
    for (size_t i = 0; i < sizeof(b); ++i) b[i] = (uint8_t)(v >> (8 * i));
    return fips_writer_write_bytes(w, b, sizeof(b), error);
}
