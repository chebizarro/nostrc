#include <fips/proto/fsp/wire.h>
#include <fips/noise/mod.h>
#include <string.h>
_Static_assert(FIPS_FRAMING_FSP_COMMON_PREFIX_SIZE == 4, "prefix");
_Static_assert(FIPS_FRAMING_FSP_HEADER_SIZE == 12, "AAD header");
_Static_assert(FIPS_FRAMING_FSP_INNER_HEADER_SIZE == 6, "inner header");
_Static_assert(FIPS_FRAMING_FSP_TAG_SIZE == 16, "tag");
_Static_assert(FIPS_FRAMING_FSP_TAG_SIZE == FIPS_NOISE_TAG_SIZE, "Noise tag");
_Static_assert(FIPS_FRAMING_FSP_ENCRYPTED_MIN_SIZE == 28, "encrypted minimum");
_Static_assert(FIPS_FRAMING_FSP_ENCRYPTED_MIN_SIZE == FIPS_FRAMING_FSP_HEADER_SIZE + FIPS_FRAMING_FSP_TAG_SIZE, "minimum composition");
_Static_assert(FIPS_FRAMING_FSP_PORT_HEADER_SIZE == 4, "ports");
_Static_assert(FIPS_SESSION_DataPacket == 0x10 && FIPS_SESSION_SenderReport == 0x11 &&
    FIPS_SESSION_ReceiverReport == 0x12 && FIPS_SESSION_PathMtuNotification == 0x13 &&
    FIPS_SESSION_CoordsWarmup == 0x14 && FIPS_SESSION_MESSAGE_TYPE_COUNT == 5, "shared session catalog");
fips_fsp_session_flags fips_fsp_session_flags_new(void) { return (fips_fsp_session_flags){0}; }
uint8_t fips_fsp_session_flags_to_byte(fips_fsp_session_flags f) { return f.bits; }
fips_fsp_session_flags fips_fsp_session_flags_from_byte(uint8_t b) { return (fips_fsp_session_flags){b}; }
size_t fips_fsp_encrypted_header_data_offset(const fips_fsp_encrypted_header *h)
{ (void)h; return FIPS_FRAMING_FSP_HEADER_SIZE; }
static fips_status malformed(const char *s, fips_proto_error *e)
{
    if (e) *e = (fips_proto_error){.variant = FIPS_PROTO_Malformed, .as.malformed = s};
    return FIPS_ERR_RANGE;
}
static fips_status too_long(size_t max, size_t got, fips_proto_error *e)
{
    if (e) *e = (fips_proto_error){.variant = FIPS_PROTO_MessageTooLong, .as.message_too_long = {max, got}};
    return FIPS_ERR_RANGE;
}
static fips_status reserve(fips_writer *w, size_t n, fips_proto_error *e)
{
    if (n > w->capacity - w->pos)
        return too_long(w->capacity, n > SIZE_MAX - w->pos ? SIZE_MAX : w->pos + n, e);
    return FIPS_OK;
}
uint8_t fips_fsp_flags_to_byte(fips_fsp_flags f)
{ return (uint8_t)((f.coords_present ? FIPS_FSP_FLAG_CP : 0) | (f.key_epoch ? FIPS_FSP_FLAG_K : 0) | (f.unencrypted ? FIPS_FSP_FLAG_U : 0)); }
fips_fsp_flags fips_fsp_flags_from_byte(uint8_t b)
{ return (fips_fsp_flags){(b & FIPS_FSP_FLAG_CP) != 0, (b & FIPS_FSP_FLAG_K) != 0, (b & FIPS_FSP_FLAG_U) != 0}; }
uint8_t fips_fsp_inner_flags_to_byte(fips_fsp_inner_flags f) { return f.spin_bit ? FIPS_FSP_INNER_FLAG_SP : 0; }
fips_fsp_inner_flags fips_fsp_inner_flags_from_byte(uint8_t b) { return (fips_fsp_inner_flags){(b & FIPS_FSP_INNER_FLAG_SP) != 0}; }
static fips_status read_coords(fips_reader *r, bool optional, fips_fsp_coords *out, fips_proto_error *e)
{
    uint16_t count;
    if (fips_reader_read_u16_le(r, &count, e) != FIPS_OK) return FIPS_ERR_RANGE;
    if (!count && !optional) return malformed("coordinate with zero entries", e);
    const uint8_t *p;
    if (fips_reader_read_bytes(r, (size_t)count * 16, &p, e) != FIPS_OK) return FIPS_ERR_RANGE;
    *out = (fips_fsp_coords){count ? p : NULL, count};
    return FIPS_OK;
}
static fips_status encode_handshake(uint8_t phase, uint8_t flags, const fips_fsp_coords *src,
    const fips_fsp_coords *dest, fips_fsp_bytes hs, fips_writer *w, fips_proto_error *e)
{
    size_t body = 3;
    if (hs.len > UINT16_MAX) return too_long(UINT16_MAX, hs.len, e);
    if (src) {
        if (!src->count || !dest->count) return malformed("coordinate with zero entries", e);
        if (src->count > UINT16_MAX) return too_long(UINT16_MAX, src->count, e);
        if (dest->count > UINT16_MAX) return too_long(UINT16_MAX, dest->count, e);
        body += 4 + 16 * (src->count + dest->count);
    }
    body += hs.len;
    if (body > UINT16_MAX) return too_long(UINT16_MAX, body, e);
    if (reserve(w, FIPS_FRAMING_FSP_COMMON_PREFIX_SIZE + body, e) != FIPS_OK) return FIPS_ERR_RANGE;
    fips_writer_write_u8(w, phase, NULL);
    fips_writer_write_u8(w, 0, NULL);
    fips_writer_write_u16_le(w, (uint16_t)body, NULL);
    fips_writer_write_u8(w, flags, NULL);
    if (src) {
        fips_writer_write_u16_le(w, (uint16_t)src->count, NULL);
        fips_writer_write_bytes(w, src->addrs, 16 * src->count, NULL);
        fips_writer_write_u16_le(w, (uint16_t)dest->count, NULL);
        fips_writer_write_bytes(w, dest->addrs, 16 * dest->count, NULL);
    }
    fips_writer_write_u16_le(w, (uint16_t)hs.len, NULL);
    return fips_writer_write_bytes(w, hs.data, hs.len, NULL);
}
/* Preflight on a local cursor makes the compound decoders transactional. */
static fips_status decode_handshake(fips_reader *r, bool coords, bool strict, uint8_t *flags,
    fips_fsp_coords *src, fips_fsp_coords *dest, fips_fsp_bytes *hs, fips_proto_error *e)
{
    fips_reader t = *r;
    uint8_t f;
    uint16_t n;
    fips_fsp_coords s = {0}, d = {0};
    const uint8_t *p;
    if (fips_reader_read_u8(&t, &f, e) != FIPS_OK) return FIPS_ERR_RANGE;
    if (coords && (read_coords(&t, false, &s, e) != FIPS_OK || read_coords(&t, false, &d, e) != FIPS_OK)) return FIPS_ERR_RANGE;
    if (fips_reader_read_u16_le(&t, &n, e) != FIPS_OK || fips_reader_read_bytes(&t, n, &p, e) != FIPS_OK) return FIPS_ERR_RANGE;
    if (strict) {
        if (f) return malformed("handshake body flags set", e);
        if (t.pos != t.len) return too_long(t.pos, t.len, e);
    }
    *r = t; *flags = f; *hs = (fips_fsp_bytes){p, n};
    if (coords) { *src = s; *dest = d; }
    return FIPS_OK;
}
fips_status fips_fsp_session_setup_encode(const fips_fsp_session_setup *m, fips_writer *w, fips_proto_error *e)
{ return encode_handshake(FIPS_FSP_PHASE_MSG1, m->flags.bits, &m->src_coords, &m->dest_coords, m->handshake_payload, w, e); }
fips_status fips_fsp_session_ack_encode(const fips_fsp_session_ack *m, fips_writer *w, fips_proto_error *e)
{ return encode_handshake(FIPS_FSP_PHASE_MSG2, m->flags, &m->src_coords, &m->dest_coords, m->handshake_payload, w, e); }
fips_status fips_fsp_session_msg3_encode(const fips_fsp_session_msg3 *m, fips_writer *w, fips_proto_error *e)
{ return encode_handshake(FIPS_FSP_PHASE_MSG3, m->flags, NULL, NULL, m->handshake_payload, w, e); }
#define DECODE_COORDS(name, flag_field) \
static fips_status decode_##name(fips_reader *r, bool strict, fips_fsp_session_##name *out, fips_proto_error *e) \
{ \
    fips_fsp_session_##name m; \
    if (decode_handshake(r, true, strict, &m.flag_field, &m.src_coords, &m.dest_coords, &m.handshake_payload, e) != FIPS_OK) return FIPS_ERR_RANGE; \
    *out = m; return FIPS_OK; \
} \
fips_status fips_fsp_session_##name##_decode(fips_reader *r, fips_fsp_session_##name *out, fips_proto_error *e) \
{ return decode_##name(r, false, out, e); } \
fips_status fips_fsp_session_##name##_decode_strict(fips_reader *r, fips_fsp_session_##name *out, fips_proto_error *e) \
{ return decode_##name(r, true, out, e); }
DECODE_COORDS(setup, flags.bits)
DECODE_COORDS(ack, flags)
#undef DECODE_COORDS
static fips_status decode_msg3(fips_reader *r, bool strict, fips_fsp_session_msg3 *out, fips_proto_error *e)
{
    fips_fsp_session_msg3 m;
    if (decode_handshake(r, false, strict, &m.flags, NULL, NULL, &m.handshake_payload, e) != FIPS_OK) return FIPS_ERR_RANGE;
    *out = m; return FIPS_OK;
}
fips_status fips_fsp_session_msg3_decode(fips_reader *r, fips_fsp_session_msg3 *out, fips_proto_error *e) { return decode_msg3(r, false, out, e); }
fips_status fips_fsp_session_msg3_decode_strict(fips_reader *r, fips_fsp_session_msg3 *out, fips_proto_error *e) { return decode_msg3(r, true, out, e); }
bool fips_fsp_common_prefix_parse(const uint8_t *data, size_t len, fips_fsp_common_prefix *out)
{
    if (len < FIPS_FRAMING_FSP_COMMON_PREFIX_SIZE) return false;
    *out = (fips_fsp_common_prefix){data[0] >> 4, data[0] & 15, data[1], (uint16_t)((uint16_t)data[2] | (uint16_t)data[3] << 8)};
    return true;
}
bool fips_fsp_common_prefix_is_unencrypted(const fips_fsp_common_prefix *p) { return (p->flags & FIPS_FSP_FLAG_U) != 0; }
bool fips_fsp_common_prefix_has_coords(const fips_fsp_common_prefix *p) { return (p->flags & FIPS_FSP_FLAG_CP) != 0; }
bool fips_fsp_encrypted_header_has_coords(const fips_fsp_encrypted_header *h) { return (h->flags & FIPS_FSP_FLAG_CP) != 0; }
bool fips_fsp_encrypted_header_parse(const uint8_t *data, size_t len, fips_fsp_encrypted_header *out)
{
    if (len < FIPS_FRAMING_FSP_ENCRYPTED_MIN_SIZE || data[0] != 0 || (data[1] & FIPS_FSP_FLAG_U)) return false;
    fips_fsp_encrypted_header h = {.flags = data[1], .payload_len = (uint16_t)((uint16_t)data[2] | (uint16_t)data[3] << 8)};
    for (unsigned i = 0; i < 8; ++i) h.counter |= (uint64_t)data[4 + i] << (8 * i);
    memcpy(h.header_bytes, data, sizeof(h.header_bytes));
    *out = h; return true;
}
void fips_fsp_build_header(uint64_t counter, uint8_t flags, uint16_t n, uint8_t out[FIPS_FRAMING_FSP_HEADER_SIZE])
{
    fips_writer w;
    fips_writer_init(&w, out, FIPS_FRAMING_FSP_HEADER_SIZE);
    fips_writer_write_u8(&w, 0, NULL); fips_writer_write_u8(&w, flags, NULL);
    fips_writer_write_u16_le(&w, n, NULL); fips_writer_write_u64_le(&w, counter, NULL);
}
fips_status fips_fsp_build_encrypted(const uint8_t h[FIPS_FRAMING_FSP_HEADER_SIZE], const uint8_t *c, size_t n, fips_writer *w, fips_proto_error *e)
{
    if (n > SIZE_MAX - FIPS_FRAMING_FSP_HEADER_SIZE) return too_long(SIZE_MAX - FIPS_FRAMING_FSP_HEADER_SIZE, n, e);
    if (reserve(w, FIPS_FRAMING_FSP_HEADER_SIZE + n, e) != FIPS_OK) return FIPS_ERR_RANGE;
    fips_writer_write_bytes(w, h, FIPS_FRAMING_FSP_HEADER_SIZE, NULL);
    return fips_writer_write_bytes(w, c, n, NULL);
}
void fips_fsp_build_handshake_prefix(uint8_t phase, uint16_t n, uint8_t out[FIPS_FRAMING_FSP_COMMON_PREFIX_SIZE])
{
    out[0] = phase & 15; out[1] = 0; out[2] = (uint8_t)n; out[3] = (uint8_t)(n >> 8);
}
void fips_fsp_build_error_prefix(uint16_t n, uint8_t out[FIPS_FRAMING_FSP_COMMON_PREFIX_SIZE])
{ fips_fsp_build_handshake_prefix(0, n, out); out[1] = FIPS_FSP_FLAG_U; }
fips_status fips_fsp_prepend_inner_header(uint32_t ts, uint8_t mt, uint8_t flags, const uint8_t *p, size_t n, fips_writer *w, fips_proto_error *e)
{
    if (n > SIZE_MAX - FIPS_FRAMING_FSP_INNER_HEADER_SIZE) return too_long(SIZE_MAX - FIPS_FRAMING_FSP_INNER_HEADER_SIZE, n, e);
    if (reserve(w, FIPS_FRAMING_FSP_INNER_HEADER_SIZE + n, e) != FIPS_OK) return FIPS_ERR_RANGE;
    fips_writer_write_u32_le(w, ts, NULL); fips_writer_write_u8(w, mt, NULL); fips_writer_write_u8(w, flags, NULL);
    return fips_writer_write_bytes(w, p, n, NULL);
}
bool fips_fsp_strip_inner_header(const uint8_t *data, size_t n, fips_fsp_inner_header *out, fips_fsp_bytes *payload)
{
    if (n < FIPS_FRAMING_FSP_INNER_HEADER_SIZE) return false;
    uint32_t ts = 0;
    for (unsigned i = 0; i < 4; ++i) ts |= (uint32_t)data[i] << (8 * i);
    *out = (fips_fsp_inner_header){ts, data[4], data[5]};
    *payload = (fips_fsp_bytes){data + FIPS_FRAMING_FSP_INNER_HEADER_SIZE, n - FIPS_FRAMING_FSP_INNER_HEADER_SIZE};
    return true;
}
fips_status fips_fsp_parse_encrypted_coords(fips_reader *r, fips_fsp_coords *src, fips_fsp_coords *dest, fips_proto_error *e)
{
    fips_reader t = *r;
    fips_fsp_coords s, d;
    if (read_coords(&t, true, &s, e) != FIPS_OK || read_coords(&t, true, &d, e) != FIPS_OK) return FIPS_ERR_RANGE;
    *r = t; *src = s; *dest = d; return FIPS_OK;
}
fips_status fips_fsp_check_prefix(const fips_fsp_common_prefix *p, size_t rest, fips_proto_error *e)
{
    bool hs = p->phase >= FIPS_FSP_PHASE_MSG1 && p->phase <= FIPS_FSP_PHASE_MSG3;
    bool signal = p->phase == FIPS_FSP_PHASE_ESTABLISHED && fips_fsp_common_prefix_is_unencrypted(p);
    if (!hs && !signal) return FIPS_OK;
    if (hs && p->flags) return malformed("handshake prefix flags set", e);
    if (signal && (p->flags & FIPS_FSP_FLAGS_RESERVED)) return malformed("reserved prefix flag set", e);
    if (p->payload_len != rest) return malformed("payload_len does not match the message", e);
    return FIPS_OK;
}
fips_status fips_fsp_check_header(const fips_fsp_encrypted_header *h, fips_proto_error *e)
{ return h->flags & FIPS_FSP_FLAGS_RESERVED ? malformed("reserved prefix flag set", e) : FIPS_OK; }
fips_status fips_fsp_check_sealed(const fips_fsp_encrypted_header *h, size_t n, fips_proto_error *e)
{
    if (n < FIPS_FRAMING_FSP_TAG_SIZE || n - FIPS_FRAMING_FSP_TAG_SIZE != h->payload_len)
        return malformed("payload_len does not match the message", e);
    return FIPS_OK;
}
fips_status fips_fsp_check_inner(uint8_t flags, fips_proto_error *e)
{ return flags & FIPS_FSP_INNER_FLAGS_RESERVED ? malformed("reserved inner flag set", e) : FIPS_OK; }
fips_status fips_fsp_port_header_encode(fips_fsp_port_header p, fips_writer *w, fips_proto_error *e)
{
    if (reserve(w, FIPS_FRAMING_FSP_PORT_HEADER_SIZE, e) != FIPS_OK) return FIPS_ERR_RANGE;
    fips_writer_write_u16_le(w, p.src_port, NULL); return fips_writer_write_u16_le(w, p.dst_port, NULL);
}
fips_status fips_fsp_port_header_decode(fips_reader *r, fips_fsp_port_header *out, fips_proto_error *e)
{
    fips_reader t = *r;
    fips_fsp_port_header p;
    if (fips_reader_read_u16_le(&t, &p.src_port, e) != FIPS_OK || fips_reader_read_u16_le(&t, &p.dst_port, e) != FIPS_OK) return FIPS_ERR_RANGE;
    *r = t; *out = p; return FIPS_OK;
}
