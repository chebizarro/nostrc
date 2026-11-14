#include <fips/proto/codec.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); return 1; \
} } while (0)

static int empty(void)
{
    fips_reader r;
    fips_writer w;
    fips_proto_error e = {.variant = FIPS_PROTO_InvalidSignature};
    const uint8_t *borrow = (const uint8_t *)"sentinel";
    uint8_t v = 42;
    fips_reader_init(&r, NULL, 0);
    fips_writer_init(&w, NULL, 0);
    CHECK(fips_reader_rest(&r) == NULL && fips_reader_remaining(&r) == 0);
    CHECK(fips_reader_require(&r, 0, &e) == FIPS_OK);
    CHECK(fips_reader_advance(&r, 0, &e) == FIPS_OK);
    CHECK(fips_reader_read_array(&r, NULL, 0, &e) == FIPS_OK);
    CHECK(fips_reader_read_bytes(&r, 0, &borrow, &e) == FIPS_OK && borrow == NULL);
    CHECK(fips_writer_write_bytes(&w, NULL, 0, &e) == FIPS_OK);
    CHECK(e.variant == FIPS_PROTO_InvalidSignature); /* Success preserves error. */
    CHECK(fips_reader_read_u8(&r, &v, &e) == FIPS_ERR_RANGE && v == 42);
    CHECK(e.variant == FIPS_PROTO_MessageTooShort);
    CHECK(e.as.message_too_short.expected == 1 && e.as.message_too_short.got == 0);
    CHECK(fips_reader_position(&r) == 0);
    CHECK(fips_writer_write_u8(&w, 1, &e) == FIPS_ERR_RANGE);
    CHECK(e.variant == FIPS_PROTO_MessageTooLong);
    CHECK(e.as.message_too_long.max == 0 && e.as.message_too_long.got == 1);
    CHECK(fips_writer_len(&w) == 0 && fips_writer_remaining(&w) == 0);
    CHECK(fips_reader_require(&r, SIZE_MAX, &e) == FIPS_ERR_RANGE);
    CHECK(e.as.message_too_short.expected == SIZE_MAX);
    return 0;
}

static int failures(void)
{
    const uint8_t input[] = {7, 8, 9, 10};
    uint8_t output[] = {42, 42, 42, 42};
    fips_reader r;
    fips_proto_error e;
    fips_reader_init(&r, input, sizeof(input));
    CHECK(fips_reader_advance(&r, 3, &e) == FIPS_OK);
    uint16_t u16 = 123;
    uint32_t u32 = 456;
    uint64_t u64 = 789;
    const uint8_t *bytes = input;
    CHECK(fips_reader_read_u16_le(&r, &u16, &e) == FIPS_ERR_RANGE && u16 == 123);
    CHECK(e.as.message_too_short.expected == 5 && e.as.message_too_short.got == 4);
    CHECK(fips_reader_read_u32_le(&r, &u32, &e) == FIPS_ERR_RANGE && u32 == 456);
    CHECK(e.as.message_too_short.expected == 7 && e.as.message_too_short.got == 4);
    CHECK(fips_reader_read_u64_le(&r, &u64, &e) == FIPS_ERR_RANGE && u64 == 789);
    CHECK(e.as.message_too_short.expected == 11 && e.as.message_too_short.got == 4);
    CHECK(fips_reader_read_array(&r, output, 2, &e) == FIPS_ERR_RANGE);
    CHECK(output[0] == 42 && output[1] == 42);
    CHECK(fips_reader_read_bytes(&r, 2, &bytes, &e) == FIPS_ERR_RANGE && bytes == input);
    CHECK(fips_reader_advance(&r, 2, &e) == FIPS_ERR_RANGE);
    CHECK(fips_reader_require(&r, SIZE_MAX, &e) == FIPS_ERR_RANGE);
    CHECK(e.as.message_too_short.expected == SIZE_MAX && e.as.message_too_short.got == 4);
    CHECK(fips_reader_read_array(&r, output, SIZE_MAX, &e) == FIPS_ERR_RANGE);
    CHECK(fips_reader_read_bytes(&r, SIZE_MAX, &bytes, NULL) == FIPS_ERR_RANGE);
    CHECK(fips_reader_position(&r) == 3 && fips_reader_remaining(&r) == 1);
    uint8_t last;
    CHECK(fips_reader_read_u8(&r, &last, NULL) == FIPS_OK && last == 10);
    CHECK(fips_reader_rest(&r) == input + sizeof(input));
    CHECK(fips_reader_read_bytes(&r, 0, &bytes, NULL) == FIPS_OK && bytes == input + 4);

    fips_writer w;
    fips_writer_init(&w, output, sizeof(output));
    CHECK(fips_writer_write_u8(&w, 1, &e) == FIPS_OK);
    CHECK(fips_writer_write_u32_le(&w, UINT32_MAX, &e) == FIPS_ERR_RANGE);
    CHECK(e.variant == FIPS_PROTO_MessageTooLong);
    CHECK(e.as.message_too_long.max == 4 && e.as.message_too_long.got == 5);
    CHECK(fips_writer_write_u64_le(&w, UINT64_MAX, &e) == FIPS_ERR_RANGE);
    CHECK(e.as.message_too_long.got == 9);
    CHECK(fips_writer_write_bytes(&w, input, SIZE_MAX, &e) == FIPS_ERR_RANGE);
    CHECK(e.as.message_too_long.got == SIZE_MAX);
    CHECK(fips_writer_len(&w) == 1 && output[1] == 42 && output[3] == 42);
    CHECK(fips_writer_write_u16_le(&w, 0x0302, NULL) == FIPS_OK);
    CHECK(fips_writer_write_u16_le(&w, 0xffff, NULL) == FIPS_ERR_RANGE);
    CHECK(fips_writer_write_u8(&w, 4, NULL) == FIPS_OK);
    CHECK(!memcmp(output, (uint8_t[]){1, 2, 3, 4}, 4));
    CHECK(fips_writer_len(&w) == 4 && fips_writer_remaining(&w) == 0);
    CHECK(fips_writer_write_u8(&w, 5, NULL) == FIPS_ERR_RANGE);
    CHECK(fips_writer_write_bytes(&w, NULL, 0, NULL) == FIPS_OK);
    CHECK(fips_writer_len(&w) == 4 && output[3] == 4);
    return 0;
}

static int unaligned_roundtrip(void)
{
    uint8_t storage[17] = {0};
    const uint8_t expected[] = {0xff, 0xcd, 0xab, 0xef, 0xcd, 0xab, 0x89,
        0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 0x01};
    fips_writer w;
    fips_writer_init(&w, storage + 1, sizeof(expected));
    CHECK(fips_writer_write_u8(&w, 255, NULL) == FIPS_OK);
    CHECK(fips_writer_write_u16_le(&w, 0xabcd, NULL) == FIPS_OK);
    CHECK(fips_writer_write_u32_le(&w, UINT32_C(0x89abcdef), NULL) == FIPS_OK);
    CHECK(fips_writer_write_u64_le(&w, UINT64_C(0x0123456789abcdef), NULL) == FIPS_OK);
    CHECK(!memcmp(storage + 1, expected, sizeof(expected)));
    CHECK(storage[0] == 0 && storage[16] == 0);
    fips_reader r;
    fips_reader_init(&r, storage + 1, fips_writer_len(&w));
    uint8_t a; uint16_t b; uint32_t c; uint64_t d;
    CHECK(fips_reader_read_u8(&r, &a, NULL) == FIPS_OK && a == 255);
    CHECK(fips_reader_read_u16_le(&r, &b, NULL) == FIPS_OK && b == 0xabcd);
    CHECK(fips_reader_read_u32_le(&r, &c, NULL) == FIPS_OK && c == UINT32_C(0x89abcdef));
    CHECK(fips_reader_read_u64_le(&r, &d, NULL) == FIPS_OK && d == UINT64_C(0x0123456789abcdef));
    CHECK(fips_reader_position(&r) == sizeof(expected) && fips_reader_remaining(&r) == 0);
    return 0;
}

static int names(void)
{
    const char *expected[] = {"InvalidMessageType", "MessageTooShort", "MessageTooLong",
        "InvalidSignature", "UnsupportedVersion", "Malformed", "BadSizeClass", "BadCoord",
        "BadBloom", "HopLimitExceeded", "TtlExpired"};
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i)
        CHECK(!strcmp(fips_proto_error_name((fips_proto_error_variant)i), expected[i]));
    CHECK(!strcmp(fips_proto_error_name((fips_proto_error_variant)123), "Unknown"));
    return 0;
}

int main(void) { return empty() || failures() || unaligned_roundtrip() || names(); }
