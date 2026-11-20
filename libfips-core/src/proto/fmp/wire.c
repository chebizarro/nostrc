#include <fips/proto/fmp/wire.h>
#include <fips/noise/mod.h>
#include <string.h>

_Static_assert(FIPS_FRAMING_FMP_COMMON_PREFIX_SIZE == 1 + 1 + 2, "common prefix");
_Static_assert(FIPS_FRAMING_FMP_ESTABLISHED_HEADER_SIZE == FIPS_FRAMING_FMP_COMMON_PREFIX_SIZE + 4 + 8, "established header");
_Static_assert(FIPS_FRAMING_NOISE_HANDSHAKE_MSG1_SIZE == 106, "opaque Noise msg1");
_Static_assert(FIPS_FRAMING_NOISE_HANDSHAKE_MSG2_SIZE == 57, "opaque Noise msg2");
_Static_assert(FIPS_FRAMING_FMP_MSG1_WIRE_SIZE == FIPS_FRAMING_FMP_COMMON_PREFIX_SIZE + 4 + FIPS_FRAMING_NOISE_HANDSHAKE_MSG1_SIZE, "msg1");
_Static_assert(FIPS_FRAMING_FMP_MSG2_WIRE_SIZE == FIPS_FRAMING_FMP_COMMON_PREFIX_SIZE + 8 + FIPS_FRAMING_NOISE_HANDSHAKE_MSG2_SIZE, "msg2");
_Static_assert(FIPS_FRAMING_FMP_INNER_HEADER_SIZE == 4 + 1, "inner header");
_Static_assert(FIPS_FRAMING_NOISE_TAG_SIZE == FIPS_NOISE_TAG_SIZE, "Noise tag");
_Static_assert(FIPS_FRAMING_FMP_ENCRYPTED_MIN_SIZE == FIPS_FRAMING_FMP_ESTABLISHED_HEADER_SIZE + FIPS_NOISE_TAG_SIZE, "encrypted minimum");
_Static_assert(FIPS_FRAMING_FMP_FRAME_OVERHEAD == FIPS_FRAMING_FMP_ENCRYPTED_MIN_SIZE + FIPS_FRAMING_FMP_INNER_HEADER_SIZE - 1, "shared type byte");

static fips_status short_message(size_t expected, size_t got, fips_proto_error *e)
{
    if (e) *e = (fips_proto_error){.variant = FIPS_PROTO_MessageTooShort,
        .as.message_too_short = {expected, got}};
    return FIPS_ERR_RANGE;
}
static fips_status long_message(size_t max, size_t got, fips_proto_error *e)
{
    if (e) *e = (fips_proto_error){.variant = FIPS_PROTO_MessageTooLong,
        .as.message_too_long = {max, got}};
    return FIPS_ERR_RANGE;
}
static fips_status malformed(const char *field, fips_proto_error *e)
{
    if (e) *e = (fips_proto_error){.variant = FIPS_PROTO_Malformed, .as.malformed = field};
    return FIPS_ERR_RANGE;
}
static fips_status reserve(const fips_writer *w, size_t n, fips_proto_error *e)
{
    if (n > w->capacity - w->pos)
        return long_message(w->capacity, n > SIZE_MAX - w->pos ? SIZE_MAX : w->pos + n, e);
    return FIPS_OK;
}
static uint16_t read16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8); }
static uint32_t read32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
bool fips_fmp_is_handshake(uint8_t b) { return b == 1 || b == 2; }
bool fips_fmp_handshake_message_type_from_byte(uint8_t b, fips_fmp_handshake_message_type *out)
{
    if (!fips_fmp_is_handshake(b)) return false;
    *out = (fips_fmp_handshake_message_type)b;
    return true;
}
const char *fips_fmp_handshake_message_type_name(fips_fmp_handshake_message_type t)
{
    switch (t) {
    case FIPS_FMP_NoiseIKMsg1: return "NoiseIKMsg1";
    case FIPS_FMP_NoiseIKMsg2: return "NoiseIKMsg2";
    default: return "Unknown";
    }
}
bool fips_fmp_disconnect_reason_from_byte(uint8_t b, fips_fmp_disconnect_reason *out)
{
    if (b > 7 && b != 255) return false;
    *out = (fips_fmp_disconnect_reason)b;
    return true;
}
const char *fips_fmp_disconnect_reason_name(fips_fmp_disconnect_reason r)
{
    switch (r) {
    case FIPS_FMP_Shutdown: return "Shutdown";
    case FIPS_FMP_Restart: return "Restart";
    case FIPS_FMP_ProtocolError: return "ProtocolError";
    case FIPS_FMP_TransportFailure: return "TransportFailure";
    case FIPS_FMP_ResourceExhaustion: return "ResourceExhaustion";
    case FIPS_FMP_SecurityViolation: return "SecurityViolation";
    case FIPS_FMP_ConfigurationChange: return "ConfigurationChange";
    case FIPS_FMP_Timeout: return "Timeout";
    case FIPS_FMP_Other: return "Other";
    default: return "Unknown";
    }
}
fips_status fips_fmp_disconnect_encode(const fips_fmp_disconnect *d, fips_writer *w, fips_proto_error *e)
{
    if (reserve(w, 2, e) != FIPS_OK) return FIPS_ERR_RANGE;
    fips_writer_write_u8(w, FIPS_LINK_Disconnect, NULL);
    return fips_writer_write_u8(w, (uint8_t)d->reason, NULL);
}
fips_status fips_fmp_disconnect_decode(fips_reader *r, fips_fmp_disconnect *out, fips_proto_error *e)
{
    uint8_t b;
    if (fips_reader_read_u8(r, &b, e) != FIPS_OK) return FIPS_ERR_RANGE;
    fips_fmp_disconnect d = {FIPS_FMP_Other};
    fips_fmp_disconnect_reason_from_byte(b, &d.reason);
    *out = d;
    return FIPS_OK;
}
bool fips_fmp_common_prefix_parse(const uint8_t *d, size_t n, fips_fmp_common_prefix *out)
{
    if (n < FIPS_FRAMING_FMP_COMMON_PREFIX_SIZE) return false;
    *out = (fips_fmp_common_prefix){d[0] >> 4, d[0] & 15, d[1], read16(d + 2)};
    return true;
}
bool fips_fmp_msg1_header_parse(const uint8_t *d, size_t n, fips_fmp_msg1_header *out)
{
    if (n != FIPS_FRAMING_FMP_MSG1_WIRE_SIZE || d[0] != FIPS_FMP_PHASE_MSG1 || d[1]) return false;
    *out = (fips_fmp_msg1_header){{read32(d + 4)}, FIPS_FRAMING_FMP_COMMON_PREFIX_SIZE + 4};
    return true;
}
bool fips_fmp_msg2_header_parse(const uint8_t *d, size_t n, fips_fmp_msg2_header *out)
{
    if (n != FIPS_FRAMING_FMP_MSG2_WIRE_SIZE || d[0] != FIPS_FMP_PHASE_MSG2 || d[1]) return false;
    *out = (fips_fmp_msg2_header){{read32(d + 4)}, {read32(d + 8)}, FIPS_FRAMING_FMP_COMMON_PREFIX_SIZE + 8};
    return true;
}
bool fips_fmp_encrypted_header_parse(const uint8_t *d, size_t n, fips_fmp_encrypted_header *out)
{
    if (n < FIPS_FRAMING_FMP_ENCRYPTED_MIN_SIZE || d[0] != FIPS_FMP_PHASE_ESTABLISHED) return false;
    fips_fmp_encrypted_header h = {.flags = d[1], .payload_len = read16(d + 2), .receiver_idx = {read32(d + 4)}};
    for (unsigned i = 0; i < 8; ++i) h.counter |= (uint64_t)d[8 + i] << (i * 8);
    memcpy(h.header_bytes, d, sizeof(h.header_bytes));
    *out = h;
    return true;
}
bool fips_fmp_expected_payload_len(uint8_t phase, size_t total, uint16_t *out)
{
    size_t n;
    switch (phase) {
    case FIPS_FMP_PHASE_MSG1: n = FIPS_FRAMING_FMP_MSG1_WIRE_SIZE - FIPS_FRAMING_FMP_COMMON_PREFIX_SIZE; break;
    case FIPS_FMP_PHASE_MSG2: n = FIPS_FRAMING_FMP_MSG2_WIRE_SIZE - FIPS_FRAMING_FMP_COMMON_PREFIX_SIZE; break;
    case FIPS_FMP_PHASE_ESTABLISHED:
        if (total < FIPS_FRAMING_FMP_ENCRYPTED_MIN_SIZE) return false;
        n = total - FIPS_FRAMING_FMP_ENCRYPTED_MIN_SIZE;
        if (n > UINT16_MAX) return false;
        break;
    default: return false;
    }
    *out = (uint16_t)n;
    return true;
}
/* Header-only validation is shared with assembly; no read of a fictitious body. */
static fips_status validate(const uint8_t *d, size_t n, fips_proto_error *e)
{
    fips_fmp_common_prefix p;
    if (!fips_fmp_common_prefix_parse(d, n, &p)) return short_message(FIPS_FRAMING_FMP_COMMON_PREFIX_SIZE, n, e);
    if (p.version != FIPS_FMP_VERSION) {
        if (e) *e = (fips_proto_error){.variant = FIPS_PROTO_UnsupportedVersion, .as.unsupported_version = p.version};
        return FIPS_ERR_RANGE;
    }
    size_t min, max;
    switch (p.phase) {
    case FIPS_FMP_PHASE_MSG1: min = max = FIPS_FRAMING_FMP_MSG1_WIRE_SIZE; break;
    case FIPS_FMP_PHASE_MSG2: min = max = FIPS_FRAMING_FMP_MSG2_WIRE_SIZE; break;
    case FIPS_FMP_PHASE_ESTABLISHED:
        min = FIPS_FRAMING_FMP_ENCRYPTED_MIN_SIZE;
        max = FIPS_FRAMING_FMP_ENCRYPTED_MIN_SIZE + (size_t)UINT16_MAX;
        break;
    default:
        if (e) *e = (fips_proto_error){.variant = FIPS_PROTO_InvalidMessageType, .as.invalid_message_type = p.phase};
        return FIPS_ERR_RANGE;
    }
    if (n < min) return short_message(min, n, e);
    if (n > max) return long_message(max, n, e);
    if (p.phase != FIPS_FMP_PHASE_ESTABLISHED && p.flags) return malformed("flags", e);
    uint16_t expected;
    if (!fips_fmp_expected_payload_len(p.phase, n, &expected) || p.payload_len != expected)
        return malformed("payload_len", e);
    return FIPS_OK;
}
fips_status fips_fmp_validate_frame(const uint8_t *d, size_t n, fips_proto_error *e) { return validate(d, n, e); }
static fips_status build_handshake(uint8_t phase, fips_session_index sender, fips_session_index receiver,
    const uint8_t *noise, size_t n, fips_writer *w, fips_proto_error *e)
{
    size_t expected = phase == FIPS_FMP_PHASE_MSG1 ? FIPS_FRAMING_NOISE_HANDSHAKE_MSG1_SIZE : FIPS_FRAMING_NOISE_HANDSHAKE_MSG2_SIZE;
    size_t total = phase == FIPS_FMP_PHASE_MSG1 ? FIPS_FRAMING_FMP_MSG1_WIRE_SIZE : FIPS_FRAMING_FMP_MSG2_WIRE_SIZE;
    if (n < expected) return short_message(expected, n, e);
    if (n > expected) return long_message(expected, n, e);
    if (reserve(w, total, e) != FIPS_OK) return FIPS_ERR_RANGE;
    fips_writer_write_u8(w, phase, NULL);
    fips_writer_write_u8(w, 0, NULL);
    fips_writer_write_u16_le(w, (uint16_t)(total - FIPS_FRAMING_FMP_COMMON_PREFIX_SIZE), NULL);
    fips_writer_write_u32_le(w, sender.value, NULL);
    if (phase == FIPS_FMP_PHASE_MSG2) fips_writer_write_u32_le(w, receiver.value, NULL);
    return fips_writer_write_bytes(w, noise, n, NULL);
}
fips_status fips_fmp_build_msg1(fips_session_index s, const uint8_t *noise, size_t n, fips_writer *w, fips_proto_error *e)
{ return build_handshake(FIPS_FMP_PHASE_MSG1, s, (fips_session_index){0}, noise, n, w, e); }
fips_status fips_fmp_build_msg2(fips_session_index s, fips_session_index r, const uint8_t *noise, size_t n, fips_writer *w, fips_proto_error *e)
{ return build_handshake(FIPS_FMP_PHASE_MSG2, s, r, noise, n, w, e); }
void fips_fmp_build_established_header(fips_session_index r, uint64_t counter, uint8_t flags, uint16_t n, uint8_t out[FIPS_FRAMING_FMP_ESTABLISHED_HEADER_SIZE])
{
    fips_writer w;
    fips_writer_init(&w, out, FIPS_FRAMING_FMP_ESTABLISHED_HEADER_SIZE);
    fips_writer_write_u8(&w, FIPS_FMP_PHASE_ESTABLISHED, NULL);
    fips_writer_write_u8(&w, flags, NULL);
    fips_writer_write_u16_le(&w, n, NULL);
    fips_writer_write_u32_le(&w, r.value, NULL);
    fips_writer_write_u64_le(&w, counter, NULL);
}
fips_status fips_fmp_build_encrypted(const uint8_t header[FIPS_FRAMING_FMP_ESTABLISHED_HEADER_SIZE], const uint8_t *ciphertext, size_t n, fips_writer *w, fips_proto_error *e)
{
    const size_t h = FIPS_FRAMING_FMP_ESTABLISHED_HEADER_SIZE;
    if (n > SIZE_MAX - h) return long_message(SIZE_MAX - h, n, e);
    if (validate(header, h + n, e) != FIPS_OK) return FIPS_ERR_RANGE;
    if (header[0] != FIPS_FMP_PHASE_ESTABLISHED) return malformed("phase", e);
    if (reserve(w, h + n, e) != FIPS_OK) return FIPS_ERR_RANGE;
    fips_writer_write_bytes(w, header, h, NULL);
    return fips_writer_write_bytes(w, ciphertext, n, NULL);
}
fips_status fips_fmp_prepend_inner_header(uint32_t ts, const uint8_t *p, size_t n, fips_writer *w, fips_proto_error *e)
{
    if (n > SIZE_MAX - 4) return long_message(SIZE_MAX - 4, n, e);
    if (reserve(w, n + 4, e) != FIPS_OK) return FIPS_ERR_RANGE;
    fips_writer_write_u32_le(w, ts, NULL);
    return fips_writer_write_bytes(w, p, n, NULL);
}
bool fips_fmp_strip_inner_header(const uint8_t *p, size_t n, uint32_t *ts, const uint8_t **payload, size_t *len)
{
    if (n < FIPS_FRAMING_FMP_INNER_HEADER_SIZE) return false;
    *ts = read32(p); *payload = p + 4; *len = n - 4;
    return true;
}
