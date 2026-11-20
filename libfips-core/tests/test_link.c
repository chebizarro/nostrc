#include <fips/proto/framing.h>
#include <fips/proto/link.h>
#include "diff/diff.h"
#include "diff/proto_link.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); return 1; \
} } while (0)

static const struct { uint8_t value; const char *name; } link_catalog[] = {
    {0x00, "SessionDatagram"}, {0x01, "SenderReport"}, {0x02, "ReceiverReport"},
    {0x10, "TreeAnnounce"}, {0x20, "FilterAnnounce"}, {0x30, "LookupRequest"},
    {0x31, "LookupResponse"}, {0x50, "Disconnect"}, {0x51, "Heartbeat"},
    {0x52, "PathProbe"}, {0x53, "PathAck"}, {0x54, "PathClose"},
};
static const struct { uint8_t value; const char *name; } session_catalog[] = {
    {0x10, "DataPacket"}, {0x11, "SenderReport"}, {0x12, "ReceiverReport"},
    {0x13, "PathMtuNotification"}, {0x14, "CoordsWarmup"},
};

static int catalogs(void)
{
    CHECK(sizeof(link_catalog) / sizeof(link_catalog[0]) == FIPS_LINK_MESSAGE_TYPE_COUNT);
    CHECK(sizeof(session_catalog) / sizeof(session_catalog[0]) == FIPS_SESSION_MESSAGE_TYPE_COUNT);
    size_t links = 0, sessions = 0;
    for (unsigned byte = 0; byte <= 0xFF; ++byte) {
        fips_proto_error e = {.variant = FIPS_PROTO_InvalidSignature};
        fips_link_message_type link = (fips_link_message_type)0x1000;
        fips_session_message_type session = (fips_session_message_type)0x1000;
        const char *expected = NULL;
        for (size_t i = 0; i < FIPS_LINK_MESSAGE_TYPE_COUNT; ++i) {
            if (link_catalog[i].value == byte) expected = link_catalog[i].name;
        }
        if (expected) {
            ++links;
            CHECK(fips_link_message_type_from_u8((uint8_t)byte, &link, &e) == FIPS_OK);
            CHECK((unsigned)link == byte && !strcmp(fips_link_message_type_name(link), expected));
            CHECK(e.variant == FIPS_PROTO_InvalidSignature);
        } else {
            CHECK(fips_link_message_type_from_u8((uint8_t)byte, &link, &e) == FIPS_ERR_RANGE);
            CHECK((unsigned)link == 0x1000 && e.variant == FIPS_PROTO_InvalidMessageType);
            CHECK(e.as.invalid_message_type == byte);
            CHECK(!strcmp(fips_proto_error_name(e.variant), "InvalidMessageType"));
            CHECK(!strcmp(fips_link_message_type_name((fips_link_message_type)byte), "Unknown"));
        }
        expected = NULL;
        for (size_t i = 0; i < FIPS_SESSION_MESSAGE_TYPE_COUNT; ++i) {
            if (session_catalog[i].value == byte) expected = session_catalog[i].name;
        }
        e.variant = FIPS_PROTO_InvalidSignature;
        if (expected) {
            ++sessions;
            CHECK(fips_session_message_type_from_u8((uint8_t)byte, &session, &e) == FIPS_OK);
            CHECK((unsigned)session == byte && !strcmp(fips_session_message_type_name(session), expected));
            CHECK(e.variant == FIPS_PROTO_InvalidSignature);
        } else {
            CHECK(fips_session_message_type_from_u8((uint8_t)byte, &session, &e) == FIPS_ERR_RANGE);
            CHECK((unsigned)session == 0x1000 && e.variant == FIPS_PROTO_InvalidMessageType);
            CHECK(e.as.invalid_message_type == byte);
            CHECK(!strcmp(fips_session_message_type_name((fips_session_message_type)byte), "Unknown"));
        }
    }
    CHECK(links == FIPS_LINK_MESSAGE_TYPE_COUNT && sessions == FIPS_SESSION_MESSAGE_TYPE_COUNT);
    /* Upstream test_link_message_type_invalid. */
    fips_link_message_type t;
    CHECK(fips_link_message_type_from_u8(0xFF, &t, NULL) == FIPS_ERR_RANGE);
    CHECK(fips_link_message_type_from_u8(0x03, &t, NULL) == FIPS_ERR_RANGE);
    CHECK(fips_link_message_type_from_u8(0x40, &t, NULL) == FIPS_ERR_RANGE);
    return 0;
}

static fips_node_addr make_node_addr(uint8_t value)
{
    fips_node_addr a = {{0}};
    a.bytes[0] = value;
    return a;
}

static int datagram_roundtrip(void)
{
    const uint8_t payload[] = {0x10, 0x00, 0x05, 0x00, 1, 2, 3, 4, 5};
    uint8_t encoded[FIPS_SESSION_DATAGRAM_HEADER_SIZE + sizeof(payload)];
    fips_session_datagram d, decoded;
    fips_writer w;
    fips_reader r;
    fips_proto_error e = {.variant = FIPS_PROTO_InvalidSignature};
    fips_session_datagram_init(&d, make_node_addr(0xAA), make_node_addr(0xBB), payload, sizeof(payload));
    CHECK(d.ttl == 64 && d.path_mtu == UINT16_MAX && d.payload == payload && d.payload_len == sizeof(payload));
    d.ttl = 32; d.path_mtu = 1400;
    CHECK(fips_session_datagram_encoded_len(&d) == sizeof(encoded));
    fips_writer_init(&w, encoded, sizeof(encoded));
    CHECK(fips_session_datagram_encode(&d, &w, &e) == FIPS_OK && fips_writer_len(&w) == sizeof(encoded));
    CHECK(e.variant == FIPS_PROTO_InvalidSignature);
    CHECK(encoded[0] == 0x00 && encoded[1] == 32 && encoded[2] == 0x78 && encoded[3] == 0x05);
    CHECK(encoded[4] == 0xAA && encoded[20] == 0xBB && !memcmp(encoded + 36, payload, sizeof(payload)));
    fips_reader_init(&r, encoded + 1, sizeof(encoded) - 1);
    CHECK(fips_session_datagram_decode(&r, &decoded, &e) == FIPS_OK && fips_reader_remaining(&r) == 0);
    CHECK(e.variant == FIPS_PROTO_InvalidSignature);
    CHECK(!memcmp(decoded.src_addr.bytes, d.src_addr.bytes, 16) && !memcmp(decoded.dest_addr.bytes, d.dest_addr.bytes, 16));
    CHECK(decoded.ttl == 32 && decoded.path_mtu == 1400);
    CHECK(decoded.payload == encoded + FIPS_SESSION_DATAGRAM_HEADER_SIZE); /* borrowed, not copied */
    CHECK(decoded.payload_len == sizeof(payload) && !memcmp(decoded.payload, payload, sizeof(payload)));

    /* Empty payload: header only; NULL payload is allowed at length zero. */
    fips_session_datagram_init(&d, make_node_addr(1), make_node_addr(2), NULL, 0);
    fips_writer_init(&w, encoded, sizeof(encoded));
    CHECK(fips_session_datagram_encode(&d, &w, &e) == FIPS_OK && fips_writer_len(&w) == FIPS_SESSION_DATAGRAM_HEADER_SIZE);
    fips_reader_init(&r, encoded + 1, FIPS_SESSION_DATAGRAM_BODY_HEADER_SIZE);
    CHECK(fips_session_datagram_decode(&r, &decoded, &e) == FIPS_OK && decoded.payload_len == 0);
    CHECK(decoded.ttl == 64 && decoded.path_mtu == UINT16_MAX);

    /* TTL round trip and the forwarding rule over every value. */
    const uint8_t one = 0x42;
    for (unsigned ttl = 0; ttl <= 0xFF; ++ttl) {
        fips_session_datagram_init(&d, make_node_addr(1), make_node_addr(2), &one, 1);
        d.ttl = (uint8_t)ttl;
        fips_writer_init(&w, encoded, sizeof(encoded));
        CHECK(fips_session_datagram_encode(&d, &w, NULL) == FIPS_OK);
        fips_reader_init(&r, encoded + 1, fips_writer_len(&w) - 1);
        CHECK(fips_session_datagram_decode(&r, &decoded, NULL) == FIPS_OK && decoded.ttl == ttl);
        uint8_t left = 0xEE;
        bool forward = fips_ttl_after_hop((uint8_t)ttl, &left);
        CHECK(forward == (ttl > 1));
        CHECK(forward ? left == ttl - 1 : left == 0xEE);
        CHECK(fips_session_datagram_can_forward(&decoded) == forward);
        CHECK(fips_session_datagram_decrement_ttl(&decoded) == forward);
        CHECK(decoded.ttl == (ttl ? ttl - 1 : 0));
    }
    return 0;
}

static int datagram_malformed(void)
{
    uint8_t body[FIPS_SESSION_DATAGRAM_BODY_HEADER_SIZE + 8];
    for (size_t i = 0; i < sizeof(body); ++i) body[i] = (uint8_t)(0xA0 + i);
    const fips_session_datagram sentinel = {.ttl = 7, .path_mtu = 9, .payload = body, .payload_len = 11};
    /* Every truncation below the 35-byte body header is Rust's MessageTooShort
     * {expected: 35, got: len}; out and the cursor stay untouched. */
    for (size_t n = 0; n < FIPS_SESSION_DATAGRAM_BODY_HEADER_SIZE; ++n) {
        fips_reader r;
        fips_proto_error e = {.variant = FIPS_PROTO_InvalidSignature};
        fips_session_datagram d = sentinel;
        fips_reader_init(&r, n ? body : NULL, n);
        CHECK(fips_session_datagram_decode(&r, &d, &e) == FIPS_ERR_RANGE);
        CHECK(e.variant == FIPS_PROTO_MessageTooShort);
        CHECK(e.as.message_too_short.expected == FIPS_SESSION_DATAGRAM_BODY_HEADER_SIZE);
        CHECK(e.as.message_too_short.got == n);
        CHECK(fips_reader_position(&r) == 0 && !memcmp(&d, &sentinel, sizeof(d)));
        CHECK(fips_session_datagram_decode(&r, &d, NULL) == FIPS_ERR_RANGE); /* NULL error is fine */
    }
    /* Exactly 35 bytes decode with an empty borrowed payload; each extra byte is payload. */
    for (size_t n = FIPS_SESSION_DATAGRAM_BODY_HEADER_SIZE; n <= sizeof(body); ++n) {
        fips_reader r;
        fips_session_datagram d;
        fips_reader_init(&r, body, n);
        CHECK(fips_session_datagram_decode(&r, &d, NULL) == FIPS_OK);
        CHECK(d.ttl == body[0] && d.path_mtu == (uint16_t)(body[1] | (body[2] << 8)));
        CHECK(!memcmp(d.src_addr.bytes, body + 3, 16) && !memcmp(d.dest_addr.bytes, body + 19, 16));
        CHECK(d.payload_len == n - 35 && d.payload == body + 35 && fips_reader_remaining(&r) == 0);
    }
    /* A reader positioned mid-buffer reports the codec's cumulative offset. */
    fips_reader r;
    fips_proto_error e;
    fips_session_datagram d = sentinel;
    fips_reader_init(&r, body, sizeof(body));
    CHECK(fips_reader_advance(&r, 10, NULL) == FIPS_OK);
    CHECK(fips_session_datagram_decode(&r, &d, &e) == FIPS_ERR_RANGE);
    CHECK(e.as.message_too_short.expected == 45 && e.as.message_too_short.got == sizeof(body));
    CHECK(fips_reader_position(&r) == 10 && !memcmp(&d, &sentinel, sizeof(d)));
    fips_reader_init(&r, body, sizeof(body));
    CHECK(fips_reader_advance(&r, 8, NULL) == FIPS_OK);
    CHECK(fips_session_datagram_decode(&r, &d, &e) == FIPS_OK && d.ttl == body[8] && d.payload_len == 0);
    return 0;
}

static int datagram_encode_capacity(void)
{
    const uint8_t payload[5] = {1, 2, 3, 4, 5};
    uint8_t buf[64];
    fips_session_datagram d;
    fips_session_datagram_init(&d, make_node_addr(1), make_node_addr(2), payload, sizeof(payload));
    const size_t total = FIPS_SESSION_DATAGRAM_HEADER_SIZE + sizeof(payload);
    for (size_t capacity = 0; capacity < total; ++capacity) {
        fips_writer w;
        fips_proto_error e = {.variant = FIPS_PROTO_InvalidSignature};
        memset(buf, 0xCC, sizeof(buf));
        fips_writer_init(&w, capacity ? buf : NULL, capacity);
        CHECK(fips_session_datagram_encode(&d, &w, &e) == FIPS_ERR_RANGE);
        CHECK(e.variant == FIPS_PROTO_MessageTooLong);
        CHECK(e.as.message_too_long.max == capacity && e.as.message_too_long.got == total);
        CHECK(fips_writer_len(&w) == 0 && buf[0] == 0xCC); /* nothing written */
    }
    /* Appending after existing output counts cumulative length, like the writer. */
    fips_writer w;
    fips_proto_error e;
    fips_writer_init(&w, buf, total + 2);
    CHECK(fips_writer_write_u8(&w, 0xEE, NULL) == FIPS_OK);
    CHECK(fips_writer_write_u8(&w, 0xEE, NULL) == FIPS_OK);
    CHECK(fips_writer_write_u8(&w, 0xEE, NULL) == FIPS_OK);
    CHECK(fips_session_datagram_encode(&d, &w, &e) == FIPS_ERR_RANGE);
    CHECK(e.as.message_too_long.max == total + 2 && e.as.message_too_long.got == total + 3);
    CHECK(fips_writer_len(&w) == 3);
    fips_writer_init(&w, buf, total + 2);
    CHECK(fips_writer_write_u8(&w, 0xEE, NULL) == FIPS_OK);
    CHECK(fips_session_datagram_encode(&d, &w, &e) == FIPS_OK && fips_writer_len(&w) == total + 1);
    CHECK(buf[0] == 0xEE && buf[1] == 0x00 && buf[2] == 64);
    /* Length saturation. */
    d.payload_len = SIZE_MAX;
    CHECK(fips_session_datagram_encoded_len(&d) == SIZE_MAX);
    fips_writer_init(&w, buf, sizeof(buf));
    CHECK(fips_session_datagram_encode(&d, &w, &e) == FIPS_ERR_RANGE);
    CHECK(e.as.message_too_long.max == sizeof(buf) && e.as.message_too_long.got == SIZE_MAX);
    d.payload_len = SIZE_MAX - FIPS_SESSION_DATAGRAM_HEADER_SIZE;
    CHECK(fips_session_datagram_encoded_len(&d) == SIZE_MAX);
    return 0;
}

/* framing.rs tests: the literal two-step subtraction the limit was first written as. */
static uint16_t old_limit(uint16_t mtu)
{
    unsigned after = mtu > 106 ? mtu - 106u : 0u;
    return (uint16_t)(after > 4 ? after - 4u : 0u);
}

static int framing(void)
{
    CHECK(FIPS_OVERHEAD == 106 && FIPS_FRAMING_FMP_FRAME_OVERHEAD == 36);
    CHECK(FIPS_FRAMING_FMP_ESTABLISHED_HEADER_SIZE == 16 && FIPS_FRAMING_FMP_INNER_HEADER_SIZE == 5);
    CHECK(FIPS_FRAMING_FMP_MSG1_WIRE_SIZE == 114 && FIPS_FRAMING_FMP_MSG2_WIRE_SIZE == 69);
    CHECK(FIPS_FRAMING_FSP_HEADER_SIZE == 12 && FIPS_FRAMING_FSP_INNER_HEADER_SIZE == 6);
    CHECK(FIPS_FRAMING_FSP_PORT_HEADER_SIZE == 4 && FIPS_FRAMING_FSP_TAG_SIZE == 16);
    CHECK(FIPS_FRAMING_NOISE_TAG_SIZE == 16 && FIPS_FRAMING_NOISE_HANDSHAKE_MSG1_SIZE == 106);
    CHECK(FIPS_SESSION_DATAGRAM_HEADER_SIZE == 36);
    /* The 110-byte knee: zero through 110, then mtu - 110. */
    const struct { uint16_t mtu, limit; } table[] = {
        {0, 0}, {1, 0}, {105, 0}, {106, 0}, {107, 0}, {109, 0}, {110, 0}, {111, 1}, {112, 2},
        {576, 466}, {1280, 1170}, {1357, 1247}, {1500, 1390}, {9000, 8890}, {65535, 65425},
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
        uint16_t actual = fips_framing_payload_limit(table[i].mtu);
        if (actual != table[i].limit) {
            fprintf(stderr, "payload_limit(%u) = %u, expected %u\n", table[i].mtu, actual, table[i].limit);
            return 1;
        }
    }
    for (unsigned mtu = 0; mtu <= UINT16_MAX; ++mtu) {
        CHECK(fips_framing_payload_limit((uint16_t)mtu) == old_limit((uint16_t)mtu));
        CHECK(fips_framing_payload_limit((uint16_t)mtu) == (mtu > 110 ? mtu - 110 : 0));
    }
    return 0;
}

/* Replay the pinned proto fixture through the harness loader and the four
 * differential checks; every one must report CHECKED, none skipped. */
static int differential(const char *dir)
{
    diff_context ctx = {.dir = dir, .pin = FIPS_DIFF_PIN};
    snprintf(ctx.id, sizeof(ctx.id), "proto");
    diff_value *cases = diff_load(&ctx, "proto");
    if (!cases) { fprintf(stderr, "FAIL %s: %s\n", ctx.id, ctx.error); return 1; }
    diff_value *wire = FIELD(cases, "wire_cases"), *datagram = NULL;
    for (size_t i = 0; i < COUNT(wire); ++i) {
        if (!strcmp(TEXT(ITEM(wire, i), "name"), "session-datagram")) datagram = ITEM(wire, i);
    }
    const proto_case session = {"session-datagram", datagram, FIELD(cases, "common_inputs"), FIELD(cases, "constants")};
    struct { const char *id; result r; } runs[4];
    size_t checked = 0;
    snprintf(ctx.id, sizeof(ctx.id), "proto/constants"); ctx.error[0] = '\0';
    runs[0].id = "proto/constants"; runs[0].r = check_proto_constants(&ctx, FIELD(cases, "constants"));
    if (runs[0].r != CHECKED) fprintf(stderr, "FAIL %s: %s\n", runs[0].id, ctx.error);
    snprintf(ctx.id, sizeof(ctx.id), "proto/link_catalog"); ctx.error[0] = '\0';
    runs[1].id = "proto/link_catalog"; runs[1].r = check_proto_link_catalog(&ctx, FIELD(cases, "link_catalog"));
    if (runs[1].r != CHECKED) fprintf(stderr, "FAIL %s: %s\n", runs[1].id, ctx.error);
    snprintf(ctx.id, sizeof(ctx.id), "proto/session_catalog"); ctx.error[0] = '\0';
    runs[2].id = "proto/session_catalog"; runs[2].r = check_proto_session_catalog(&ctx, FIELD(cases, "session_catalog"));
    if (runs[2].r != CHECKED) fprintf(stderr, "FAIL %s: %s\n", runs[2].id, ctx.error);
    snprintf(ctx.id, sizeof(ctx.id), "proto/session-datagram"); ctx.error[0] = '\0';
    runs[3].id = "proto/session-datagram";
    runs[3].r = datagram ? check_proto_session_datagram(&ctx, &session) : FAILED;
    if (runs[3].r != CHECKED) fprintf(stderr, "FAIL %s: %s\n", runs[3].id, datagram ? ctx.error : "case missing");
    for (size_t i = 0; i < 4; ++i) {
        if (runs[i].r == CHECKED) { ++checked; printf("PASS %s\n", runs[i].id); }
    }
    printf("proto (link subset): %zu checked, 0 skipped, %zu failed\n", checked, 4 - checked);
    diff_free(cases);
    return checked == 4 ? 0 : 1;
}

/* An optional vectors directory, as fips_diff accepts, lets a mutated fixture
 * copy prove the checks fail on real mismatches. */
int main(int argc, char **argv)
{
    int failures = catalogs() + datagram_roundtrip() + datagram_malformed() +
                   datagram_encode_capacity() + framing() +
                   differential(argc > 1 ? argv[1] : FIPS_DIFF_VECTORS);
    if (failures) { fprintf(stderr, "%d link test group(s) failed\n", failures); return 1; }
    puts("fips link/framing tests passed");
    return 0;
}
