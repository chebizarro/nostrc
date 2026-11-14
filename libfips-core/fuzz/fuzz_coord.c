#include <fips/proto/coord.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(expr) do { if (!(expr)) abort(); } while (0)
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    fips_tree_coordinate *coord = malloc(sizeof(*coord));
    CHECK(coord);
    memset(coord, 0xa5, sizeof(*coord));
    fips_reader r;
    fips_reader_init(&r, data, size);
    bool present = false;
    fips_proto_error e = {.variant = FIPS_PROTO_InvalidSignature};
    fips_status status = fips_decode_optional_coords(&r, coord, &present, &e);
    size_t count = size >= 2 ? (size_t)data[0] | ((size_t)data[1] << 8) : 0;
    size_t needed = size < 2 ? 2 : 2 + count * 16;
    if (status == FIPS_OK) {
        CHECK(size >= needed && r.pos == needed && present == (count != 0));
        CHECK(e.variant == FIPS_PROTO_InvalidSignature);
        uint8_t *encoded = malloc(needed);
        CHECK(encoded);
        fips_writer w;
        fips_writer_init(&w, encoded, needed);
        if (present) {
            CHECK(coord->len == count && fips_tree_coordinate_depth(coord) == count - 1);
            for (size_t i = 0; i < count; ++i) {
                CHECK(coord->entries[i].sequence == 0 && coord->entries[i].timestamp == 0);
                CHECK(!memcmp(coord->entries[i].node_addr.bytes, data + 2 + i * 16, 16));
            }
            CHECK(fips_encode_coords(coord, &w, NULL) == FIPS_OK);
            size_t capacity = fips_tree_coordinate_debug_size(coord);
            char *debug = malloc(capacity); CHECK(debug);
            CHECK(fips_tree_coordinate_debug(coord, debug, capacity) == FIPS_OK && strlen(debug) + 1 == capacity);
            free(debug);
        } else {
            CHECK(coord->len == UINT16_C(0xa5a5));
            CHECK(fips_encode_empty_coords(&w, NULL) == FIPS_OK);
        }
        CHECK(w.pos == needed && !memcmp(encoded, data, needed));
        free(encoded);
    } else {
        CHECK(size < needed && r.pos == 0 && !present);
        CHECK(e.variant == FIPS_PROTO_MessageTooShort && e.as.message_too_short.expected == needed && e.as.message_too_short.got == size);
        const uint8_t *raw = (const uint8_t *)coord;
        for (size_t i = 0; i < sizeof(*coord); ++i) CHECK(raw[i] == 0xa5);
    }
    fips_reader_init(&r, data, size);
    status = fips_decode_coords(&r, coord, &e);
    if (size >= 2 && count == 0) {
        CHECK(status == FIPS_ERR_RANGE && e.variant == FIPS_PROTO_Malformed && r.pos == 0);
        CHECK(!strcmp(e.as.malformed, "coordinate with zero entries"));
    } else CHECK((status == FIPS_OK) == (size >= needed));
    /* Exercise the separate 32-byte metadata entry codec on arbitrary bytes. */
    fips_coord_entry entry;
    fips_reader_init(&r, data, size);
    if (fips_coord_entry_decode(&r, &entry, NULL) == FIPS_OK) {
        uint8_t bytes[32]; fips_writer w; fips_writer_init(&w, bytes, sizeof(bytes));
        CHECK(fips_coord_entry_encode(&entry, &w, NULL) == FIPS_OK && !memcmp(bytes, data, 32));
    } else CHECK(r.pos == 0);
    free(coord);
    return 0;
}
