#include <fips/proto/link.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) abort(); } while (0)

/* Input is a SessionDatagram body (everything after the msg_type byte), as
 * SessionDatagramRef::decode receives it. Rejection is an ordinary outcome with
 * an exact error; acceptance must round-trip through encode byte for byte. */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    fips_reader r;
    fips_session_datagram d = {.ttl = 7, .path_mtu = 9, .payload = NULL, .payload_len = 11};
    fips_proto_error error = {.variant = FIPS_PROTO_InvalidSignature};
    fips_reader_init(&r, data, size);
    if (fips_session_datagram_decode(&r, &d, &error) != FIPS_OK) {
        CHECK(size < FIPS_SESSION_DATAGRAM_BODY_HEADER_SIZE);
        CHECK(error.variant == FIPS_PROTO_MessageTooShort);
        CHECK(error.as.message_too_short.expected == FIPS_SESSION_DATAGRAM_BODY_HEADER_SIZE);
        CHECK(error.as.message_too_short.got == size);
        CHECK(fips_reader_position(&r) == 0);
        CHECK(d.ttl == 7 && d.path_mtu == 9 && d.payload == NULL && d.payload_len == 11);
        return 0;
    }
    CHECK(size >= FIPS_SESSION_DATAGRAM_BODY_HEADER_SIZE);
    CHECK(error.variant == FIPS_PROTO_InvalidSignature);
    CHECK(fips_reader_remaining(&r) == 0);
    CHECK(d.ttl == data[0]);
    CHECK(d.path_mtu == (uint16_t)(data[1] | (data[2] << 8)));
    CHECK(!memcmp(d.src_addr.bytes, data + 3, 16) && !memcmp(d.dest_addr.bytes, data + 19, 16));
    CHECK(d.payload == data + 35 && d.payload_len == size - 35);
    uint8_t left = 0;
    bool forward = fips_ttl_after_hop(d.ttl, &left);
    CHECK(forward == (d.ttl > 1) && fips_session_datagram_can_forward(&d) == forward);
    CHECK(!forward || left == d.ttl - 1);

    size_t total = fips_session_datagram_encoded_len(&d);
    CHECK(total == size + 1);
    uint8_t *encoded = malloc(total);
    CHECK(encoded);
    fips_writer w;
    fips_writer_init(&w, encoded, size); /* one byte short: atomic rejection */
    CHECK(fips_session_datagram_encode(&d, &w, &error) == FIPS_ERR_RANGE);
    CHECK(error.variant == FIPS_PROTO_MessageTooLong);
    CHECK(error.as.message_too_long.max == size && error.as.message_too_long.got == total);
    CHECK(fips_writer_len(&w) == 0);
    fips_writer_init(&w, encoded, total);
    CHECK(fips_session_datagram_encode(&d, &w, NULL) == FIPS_OK && fips_writer_len(&w) == total);
    CHECK(encoded[0] == FIPS_LINK_SessionDatagram && !memcmp(encoded + 1, data, size));

    fips_session_datagram again;
    fips_reader_init(&r, encoded + 1, size);
    CHECK(fips_session_datagram_decode(&r, &again, NULL) == FIPS_OK);
    CHECK(again.ttl == d.ttl && again.path_mtu == d.path_mtu && again.payload_len == d.payload_len);
    CHECK(!memcmp(again.src_addr.bytes, d.src_addr.bytes, 16) && !memcmp(again.dest_addr.bytes, d.dest_addr.bytes, 16));
    CHECK(!memcmp(again.payload, d.payload, d.payload_len));
    free(encoded);

    /* The first payload byte is where a session message type would sit. */
    if (d.payload_len) {
        fips_session_message_type session;
        fips_link_message_type link;
        fips_proto_error e = {.variant = FIPS_PROTO_InvalidSignature};
        if (fips_session_message_type_from_u8(d.payload[0], &session, &e) == FIPS_OK) {
            CHECK((unsigned)session == d.payload[0] && strcmp(fips_session_message_type_name(session), "Unknown"));
        } else {
            CHECK(e.variant == FIPS_PROTO_InvalidMessageType && e.as.invalid_message_type == d.payload[0]);
        }
        e.variant = FIPS_PROTO_InvalidSignature;
        if (fips_link_message_type_from_u8(d.payload[0], &link, &e) == FIPS_OK) {
            CHECK((unsigned)link == d.payload[0] && strcmp(fips_link_message_type_name(link), "Unknown"));
        } else {
            CHECK(e.variant == FIPS_PROTO_InvalidMessageType && e.as.invalid_message_type == d.payload[0]);
        }
    }
    return 0;
}
