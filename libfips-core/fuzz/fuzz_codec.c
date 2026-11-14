#include <fips/proto/codec.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) abort(); } while (0)

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    fips_reader r;
    fips_reader_init(&r, data, size);
    while (fips_reader_remaining(&r)) {
        uint8_t command;
        CHECK(fips_reader_read_u8(&r, &command, NULL) == FIPS_OK);
        size_t start = fips_reader_position(&r), n;
        uint8_t encoded[32], array[32];
        const uint8_t *bytes = NULL;
        uint8_t a = 0; uint16_t b = 0; uint32_t c = 0; uint64_t d = 0;
        fips_proto_error error = {.variant = FIPS_PROTO_InvalidSignature};
        fips_status status;
        unsigned int op = command & 7u;
        switch (op) {
        case 0: n = 1; status = fips_reader_read_u8(&r, &a, &error); break;
        case 1: n = 2; status = fips_reader_read_u16_le(&r, &b, &error); break;
        case 2: n = 4; status = fips_reader_read_u32_le(&r, &c, &error); break;
        case 3: n = 8; status = fips_reader_read_u64_le(&r, &d, &error); break;
        case 4: n = command >> 3; status = fips_reader_read_array(&r, array, n, &error); break;
        case 5: n = command >> 3; status = fips_reader_read_bytes(&r, n, &bytes, &error); break;
        default: n = command >> 3; status = fips_reader_advance(&r, n, &error); break;
        }
        if (status != FIPS_OK) {
            CHECK(error.variant == FIPS_PROTO_MessageTooShort);
            CHECK(error.as.message_too_short.expected == start + n);
            CHECK(error.as.message_too_short.got == size);
            CHECK(fips_reader_position(&r) == start);
            break;
        }
        CHECK(fips_reader_position(&r) == start + n);
        CHECK(fips_reader_remaining(&r) == size - start - n);
        CHECK(error.variant == FIPS_PROTO_InvalidSignature);
        fips_writer w;
        fips_writer_init(&w, encoded, n);
        switch (op) {
        case 0: status = fips_writer_write_u8(&w, a, &error); break;
        case 1: status = fips_writer_write_u16_le(&w, b, &error); break;
        case 2: status = fips_writer_write_u32_le(&w, c, &error); break;
        case 3: status = fips_writer_write_u64_le(&w, d, &error); break;
        case 4: status = fips_writer_write_bytes(&w, array, n, &error); break;
        case 5: status = fips_writer_write_bytes(&w, bytes, n, &error); break;
        default: status = fips_writer_write_bytes(&w, data + start, n, &error); break;
        }
        CHECK(status == FIPS_OK && fips_writer_len(&w) == n);
        CHECK(!memcmp(encoded, data + start, n));
        CHECK(fips_writer_write_u8(&w, 0, &error) == FIPS_ERR_RANGE);
        CHECK(error.variant == FIPS_PROTO_MessageTooLong);
        CHECK(error.as.message_too_long.max == n && error.as.message_too_long.got == n + 1);
        CHECK(fips_writer_len(&w) == n && !memcmp(encoded, data + start, n));
    }
    fips_proto_error error;
    CHECK(fips_reader_require(&r, SIZE_MAX, &error) == FIPS_ERR_RANGE);
    CHECK(error.variant == FIPS_PROTO_MessageTooShort);
    CHECK(error.as.message_too_short.expected == SIZE_MAX && error.as.message_too_short.got == size);
    return 0;
}
