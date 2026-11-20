#ifndef FIPS_PROTO_CODEC_H
#define FIPS_PROTO_CODEC_H

#include <fips/host.h>
#include <fips/proto/error.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Borrowed caller-owned storage; fields must not be modified after init.
 * NULL storage is allowed only for length/capacity zero. Required object and
 * result pointers must be valid. Byte-copy ranges must not overlap.
 * No allocation or ownership transfer occurs. Rest/read_bytes return borrows
 * valid while the input lives. For a NULL empty buffer that borrow is NULL.
 * Failure leaves cursor, destination and result out-parameters unchanged;
 * only error is written. error may be NULL and is untouched on success.
 * Reader errors use cumulative expected offset and total input length, not
 * remaining length. A size_t-overflowing offset is rejected with expected
 * saturated to SIZE_MAX. advance is bounds-checked (unlike Rust's debug-only
 * assertion). There is no implicit length prefix in read/write_bytes.
 */
typedef struct fips_reader {
    const uint8_t *buf;
    size_t len, pos;
} fips_reader;
typedef struct fips_writer {
    uint8_t *buf;
    size_t capacity, pos;
} fips_writer;

FIPS_CORE_API void fips_reader_init(fips_reader *r, const uint8_t *buf, size_t len);
FIPS_CORE_API size_t fips_reader_position(const fips_reader *r);
FIPS_CORE_API size_t fips_reader_remaining(const fips_reader *r);
/** Length is remaining(); no pointer arithmetic on a NULL empty buffer. */
FIPS_CORE_API const uint8_t *fips_reader_rest(const fips_reader *r);
FIPS_CORE_API fips_status fips_reader_require(const fips_reader *r, size_t n, fips_proto_error *error);
FIPS_CORE_API fips_status fips_reader_advance(fips_reader *r, size_t n, fips_proto_error *error);
FIPS_CORE_API fips_status fips_reader_read_u8(fips_reader *r, uint8_t *out, fips_proto_error *error);
FIPS_CORE_API fips_status fips_reader_read_u16_le(fips_reader *r, uint16_t *out, fips_proto_error *error);
FIPS_CORE_API fips_status fips_reader_read_u32_le(fips_reader *r, uint32_t *out, fips_proto_error *error);
FIPS_CORE_API fips_status fips_reader_read_u64_le(fips_reader *r, uint64_t *out, fips_proto_error *error);
/** C equivalent of read_array::<N>(); out may be NULL when n == 0. */
FIPS_CORE_API fips_status fips_reader_read_array(fips_reader *r, uint8_t *out, size_t n, fips_proto_error *error);
FIPS_CORE_API fips_status fips_reader_read_bytes(fips_reader *r, size_t n, const uint8_t **out, fips_proto_error *error);

/** Rust's growable Vec is replaced with a fixed caller-provided capacity.
 * Capacity failure returns FIPS_ERR_RANGE / MessageTooLong {max: capacity,
 * got: requested cumulative length}, saturating got to SIZE_MAX on overflow.
 * len() bytes in buf are the output (no allocating into_vec equivalent).
 */
FIPS_CORE_API void fips_writer_init(fips_writer *w, uint8_t *buf, size_t capacity);
FIPS_CORE_API size_t fips_writer_len(const fips_writer *w);
FIPS_CORE_API size_t fips_writer_remaining(const fips_writer *w);
FIPS_CORE_API fips_status fips_writer_write_u8(fips_writer *w, uint8_t v, fips_proto_error *error);
FIPS_CORE_API fips_status fips_writer_write_u16_le(fips_writer *w, uint16_t v, fips_proto_error *error);
FIPS_CORE_API fips_status fips_writer_write_u32_le(fips_writer *w, uint32_t v, fips_proto_error *error);
FIPS_CORE_API fips_status fips_writer_write_u64_le(fips_writer *w, uint64_t v, fips_proto_error *error);
FIPS_CORE_API fips_status fips_writer_write_bytes(fips_writer *w, const uint8_t *bytes, size_t n, fips_proto_error *error);

#ifdef __cplusplus
}
#endif
#endif /* FIPS_PROTO_CODEC_H */
