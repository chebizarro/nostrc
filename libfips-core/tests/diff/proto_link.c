#include "proto_link.h"
#include "../fakes/host.h"
#include <fips/identity/local.h>
#include <fips/proto/framing.h>
#include <fips/proto/link.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

static bool compare(diff_context *ctx, diff_bytes expected, const unsigned char *actual, size_t len)
{
    return diff_compare(ctx->id, expected.data, expected.len, actual, len);
}

result check_proto_constants(diff_context *ctx, const diff_value *constants)
{
    const struct { const char *name; uint64_t actual; } table[] = {
        {"FIPS_OVERHEAD", FIPS_OVERHEAD},
        {"FMP_ESTABLISHED_HEADER_SIZE", FIPS_FRAMING_FMP_ESTABLISHED_HEADER_SIZE},
        {"FMP_FRAME_OVERHEAD", FIPS_FRAMING_FMP_FRAME_OVERHEAD},
        {"FMP_MSG1_WIRE_SIZE", FIPS_FRAMING_FMP_MSG1_WIRE_SIZE},
        {"FMP_MSG2_WIRE_SIZE", FIPS_FRAMING_FMP_MSG2_WIRE_SIZE},
        {"FSP_HEADER_SIZE", FIPS_FRAMING_FSP_HEADER_SIZE},
        {"FSP_INNER_HEADER_SIZE", FIPS_FRAMING_FSP_INNER_HEADER_SIZE},
        {"FSP_PORT_HEADER_SIZE", FIPS_FRAMING_FSP_PORT_HEADER_SIZE},
        {"SESSION_DATAGRAM_HEADER_SIZE", FIPS_SESSION_DATAGRAM_HEADER_SIZE},
        {"payload_limit_1280", fips_framing_payload_limit(1280)},
    };
    const size_t count = sizeof(table) / sizeof(table[0]);
    bool ok = true;
    if (constants->type != D_OBJECT || COUNT(constants) != count) {
        return diff_error(ctx, "expected %zu constants, fixture has %zu", count,
                          constants->type == D_OBJECT ? COUNT(constants) : (size_t)0), FAILED;
    }
    for (size_t i = 0; i < count; ++i) {
        const diff_value *v = diff_get(constants, table[i].name);
        if (!v || v->type != D_UINT) { ok = diff_error(ctx, "constant %s missing", table[i].name); continue; }
        if (v->as.number != table[i].actual) {
            ok = diff_error(ctx, "%s: C %" PRIu64 ", Rust %" PRIu64, table[i].name,
                            table[i].actual, v->as.number);
        }
    }
    return ok ? CHECKED : FAILED;
}

/* Both catalogs are swept the way proto.rs produced them: every byte 0..=255
 * through from_byte, accepted values must appear with the Debug name, and
 * rejected ones must be absent and carry InvalidMessageType{byte}. */
typedef struct catalog_impl {
    const char *what;
    size_t count;
    fips_status (*from_u8)(uint8_t byte, unsigned *out, fips_proto_error *error);
    const char *(*name)(unsigned value);
} catalog_impl;

static fips_status link_from_u8(uint8_t byte, unsigned *out, fips_proto_error *error)
{
    fips_link_message_type type;
    fips_status status = fips_link_message_type_from_u8(byte, &type, error);
    if (status == FIPS_OK) *out = (unsigned)type;
    return status;
}
static const char *link_name(unsigned value) { return fips_link_message_type_name((fips_link_message_type)value); }
static fips_status session_from_u8(uint8_t byte, unsigned *out, fips_proto_error *error)
{
    fips_session_message_type type;
    fips_status status = fips_session_message_type_from_u8(byte, &type, error);
    if (status == FIPS_OK) *out = (unsigned)type;
    return status;
}
static const char *session_name(unsigned value) { return fips_session_message_type_name((fips_session_message_type)value); }

static const diff_value *catalog_entry(const diff_value *catalog, unsigned value)
{
    for (size_t i = 0; i < COUNT(catalog); ++i) {
        if (UINT(ITEM(catalog, i), "value") == value) return ITEM(catalog, i);
    }
    return NULL;
}

static result check_catalog(diff_context *ctx, const diff_value *catalog, const catalog_impl *impl)
{
    bool ok = true;
    if (catalog->type != D_ARRAY || COUNT(catalog) != impl->count) {
        return diff_error(ctx, "%s catalog: expected %zu entries, fixture has %zu", impl->what,
                          impl->count, catalog->type == D_ARRAY ? COUNT(catalog) : (size_t)0), FAILED;
    }
    for (size_t i = 0; i < COUNT(catalog); ++i) {
        uint64_t value = UINT(ITEM(catalog, i), "value");
        if (value > 0xFF) ok = diff_error(ctx, "%s catalog entry %zu is not a byte", impl->what, i);
        else if (catalog_entry(catalog, (unsigned)value) != ITEM(catalog, i)) {
            ok = diff_error(ctx, "%s catalog repeats value %" PRIu64, impl->what, value);
        }
    }
    size_t accepted = 0;
    for (unsigned byte = 0; byte <= 0xFF && ok; ++byte) {
        const diff_value *entry = catalog_entry(catalog, byte);
        fips_proto_error error = {.variant = FIPS_PROTO_InvalidSignature};
        unsigned out = 0x1000;
        fips_status status = impl->from_u8((uint8_t)byte, &out, &error);
        if (status == FIPS_OK) {
            ++accepted;
            if (!entry) ok = diff_error(ctx, "%s 0x%02x accepted by C, absent upstream", impl->what, byte);
            else if (out != byte) ok = diff_error(ctx, "%s 0x%02x decoded as 0x%02x", impl->what, byte, out);
            else if (strcmp(impl->name(out), TEXT(entry, "name"))) {
                ok = diff_error(ctx, "%s 0x%02x: C %s, Rust %s", impl->what, byte, impl->name(out), TEXT(entry, "name"));
            } else if (error.variant != FIPS_PROTO_InvalidSignature) {
                ok = diff_error(ctx, "%s 0x%02x: success touched error", impl->what, byte);
            }
        } else if (entry) {
            ok = diff_error(ctx, "%s 0x%02x (%s) rejected by C", impl->what, byte, TEXT(entry, "name"));
        } else if (out != 0x1000 || error.variant != FIPS_PROTO_InvalidMessageType ||
                   error.as.invalid_message_type != byte ||
                   strcmp(fips_proto_error_name(error.variant), "InvalidMessageType")) {
            ok = diff_error(ctx, "%s 0x%02x: expected untouched out and InvalidMessageType{0x%02x}", impl->what, byte, byte);
        } else if (strcmp(impl->name(byte), "Unknown")) {
            ok = diff_error(ctx, "%s 0x%02x: name helper must say Unknown", impl->what, byte);
        }
    }
    if (ok && accepted != impl->count) ok = diff_error(ctx, "%s: %zu accepted bytes", impl->what, accepted);
    return ok ? CHECKED : FAILED;
}

result check_proto_link_catalog(diff_context *ctx, const diff_value *catalog)
{
    const catalog_impl impl = {"link", FIPS_LINK_MESSAGE_TYPE_COUNT, link_from_u8, link_name};
    return check_catalog(ctx, catalog, &impl);
}

result check_proto_session_catalog(diff_context *ctx, const diff_value *catalog)
{
    const catalog_impl impl = {"session", FIPS_SESSION_MESSAGE_TYPE_COUNT, session_from_u8, session_name};
    return check_catalog(ctx, catalog, &impl);
}

/* support.rs seeded_keypair: fresh SplitMix64 XOR-separated by "keypairs", four
 * little-endian words as the secret, then the node address of that identity. */
static bool seeded_node_addr(diff_context *ctx, uint64_t seed, fips_node_addr *out)
{
    fips_seeded_entropy state = {seed ^ UINT64_C(0x6b65797061697273)};
    fips_entropy seeded = fips_seeded_entropy_adapter(&state);
    fips_fixed_entropy fixed = {0x5a};
    fips_entropy blinding = fips_fixed_entropy_adapter(&fixed);
    uint8_t secret[32];
    fips_secp *secp = NULL;
    fips_identity *id = NULL;
    bool ok = true;
    seeded.fill(seeded.ctx, secret, sizeof(secret));
    if (fips_identity_init_secp(blinding, &secp) != FIPS_IDENTITY_OK ||
        fips_identity_from_secret_bytes(secp, blinding, secret, &id) != FIPS_IDENTITY_OK) {
        ok = diff_error(ctx, "seeded identity %" PRIu64 " construction failed", seed);
    } else {
        *out = fips_identity_node_addr(id);
    }
    memset(secret, 0, sizeof(secret));
    fips_identity_destroy(id);
    fips_identity_destroy_secp(secp);
    return ok;
}

static bool short_error(diff_context *ctx, const diff_value *expected, const fips_proto_error *actual)
{
    if (strcmp(TEXT(expected, "variant"), fips_proto_error_name(actual->variant)) ||
        actual->variant != FIPS_PROTO_MessageTooShort ||
        UINT(expected, "expected") != actual->as.message_too_short.expected ||
        UINT(expected, "got") != actual->as.message_too_short.got) {
        return diff_error(ctx, "error mismatch: got %s, expected %s (expected/got payload must match)",
                          fips_proto_error_name(actual->variant), TEXT(expected, "variant"));
    }
    return true;
}

result check_proto_session_datagram(diff_context *ctx, const proto_case *c)
{
    const diff_value *v = c->record, *input = FIELD(v, "input");
    diff_bytes bytes = BYTES(v, "bytes");
    size_t offset = (size_t)UINT(v, "decode_offset");
    if (strcmp(c->name, "session-datagram")) return diff_error(ctx, "not the session-datagram case"), FAILED;
    if (offset != 1 || bytes.len < FIPS_SESSION_DATAGRAM_HEADER_SIZE) {
        return diff_error(ctx, "decode_offset/length outside the SessionDatagram layout"), FAILED;
    }
    bool ok = true;
    fips_proto_error error = {.variant = FIPS_PROTO_InvalidSignature};
    fips_link_message_type type;
    if (fips_link_message_type_from_u8(bytes.data[0], &type, &error) != FIPS_OK ||
        type != FIPS_LINK_SessionDatagram) {
        ok = diff_error(ctx, "leading byte 0x%02x is not SessionDatagram", bytes.data[0]);
    }
    /* Decode the body after msg_type, as SessionDatagram::decode(&bytes[1..]). */
    fips_reader r;
    fips_session_datagram d;
    fips_reader_init(&r, bytes.data + offset, bytes.len - offset);
    if (fips_session_datagram_decode(&r, &d, &error) != FIPS_OK) {
        return diff_error(ctx, "valid session datagram rejected"), FAILED;
    }
    if (fips_reader_remaining(&r) || error.variant != FIPS_PROTO_InvalidSignature) {
        ok = diff_error(ctx, "decode must consume the input and preserve error");
    }
    if (d.ttl != UINT(input, "ttl") || d.path_mtu != UINT(input, "path_mtu")) {
        ok = diff_error(ctx, "ttl/path_mtu: C %u/%u, input %" PRIu64 "/%" PRIu64, d.ttl, d.path_mtu,
                        UINT(input, "ttl"), UINT(input, "path_mtu"));
    }
    if (d.payload != bytes.data + FIPS_SESSION_DATAGRAM_HEADER_SIZE ||
        d.payload_len != bytes.len - FIPS_SESSION_DATAGRAM_HEADER_SIZE) {
        ok = diff_error(ctx, "payload must borrow bytes[36..]");
    }
    if (fips_session_datagram_can_forward(&d) != (d.ttl > 1)) ok = diff_error(ctx, "can_forward rule");
    /* The addresses are the seeded identities' node addresses, not bytes copied
     * back out of the fixture. */
    fips_node_addr src, dest;
    if (seeded_node_addr(ctx, UINT(input, "src_seed"), &src) &&
        seeded_node_addr(ctx, UINT(input, "dest_seed"), &dest)) {
        diff_bytes expected_src = {src.bytes, sizeof(src.bytes)}, expected_dest = {dest.bytes, sizeof(dest.bytes)};
        ok &= compare(ctx, expected_src, d.src_addr.bytes, sizeof(d.src_addr.bytes));
        ok &= compare(ctx, expected_dest, d.dest_addr.bytes, sizeof(d.dest_addr.bytes));
    } else ok = false;
    /* Re-encode the borrowed view and compare with the exporter's encode(). */
    uint8_t *encoded = malloc(bytes.len);
    if (!encoded) return diff_error(ctx, "allocation failed"), FAILED;
    fips_writer w;
    fips_writer_init(&w, encoded, bytes.len);
    if (fips_session_datagram_encoded_len(&d) != bytes.len ||
        fips_session_datagram_encode(&d, &w, &error) != FIPS_OK ||
        fips_writer_len(&w) != bytes.len) {
        ok = diff_error(ctx, "re-encoding failed or changed length");
    } else ok &= compare(ctx, bytes, encoded, fips_writer_len(&w));
    fips_writer_init(&w, encoded, bytes.len - 1);
    if (fips_session_datagram_encode(&d, &w, &error) != FIPS_ERR_RANGE || fips_writer_len(&w) ||
        error.variant != FIPS_PROTO_MessageTooLong || error.as.message_too_long.max != bytes.len - 1 ||
        error.as.message_too_long.got != bytes.len) {
        ok = diff_error(ctx, "one-byte-short writer must reject atomically with MessageTooLong");
    }
    free(encoded);
    /* <$ty>::decode(&[]) is the exporter's recorded error. */
    fips_session_datagram untouched = d;
    fips_reader_init(&r, NULL, 0);
    error.variant = FIPS_PROTO_InvalidSignature;
    if (fips_session_datagram_decode(&r, &untouched, &error) != FIPS_ERR_RANGE ||
        !short_error(ctx, FIELD(v, "empty_input_error"), &error) ||
        memcmp(&untouched, &d, sizeof(d)) || fips_reader_position(&r)) {
        ok = diff_error(ctx, "empty input error/atomicity mismatch");
    }
    return ok ? CHECKED : FAILED;
}
