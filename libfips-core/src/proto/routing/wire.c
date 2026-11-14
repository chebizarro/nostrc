#include <fips/proto/routing/wire.h>

static fips_status room(const fips_writer *w, size_t n, fips_proto_error *e)
{
    if (n <= fips_writer_remaining(w)) return FIPS_OK;
    if (e) *e = (fips_proto_error){.variant = FIPS_PROTO_MessageTooLong,
        .as.message_too_long = {w->capacity, n > SIZE_MAX - w->pos ? SIZE_MAX : w->pos + n}};
    return FIPS_ERR_RANGE;
}
static fips_status check_body(uint8_t flags, size_t body, size_t len, fips_proto_error *e)
{
    if (flags) {
        if (e) *e = (fips_proto_error){.variant = FIPS_PROTO_Malformed,
            .as.malformed = "routing signal sig_flags set"};
        return FIPS_ERR_RANGE;
    }
    if (body != len) {
        if (e) *e = (fips_proto_error){.variant = FIPS_PROTO_MessageTooLong,
            .as.message_too_long = {body, len}};
        return FIPS_ERR_RANGE;
    }
    return FIPS_OK;
}
bool fips_routing_signal_type_from_byte(uint8_t byte, fips_routing_signal_type *out)
{
    if (byte < 0x20 || byte > 0x22) return false;
    *out = (fips_routing_signal_type)byte;
    return true;
}
uint8_t fips_routing_signal_type_to_byte(fips_routing_signal_type type)
{ return (uint8_t)type; }
const char *fips_routing_signal_type_name(fips_routing_signal_type type)
{
    switch (type) {
    case FIPS_ROUTING_CoordsRequired: return "CoordsRequired";
    case FIPS_ROUTING_PathBroken: return "PathBroken";
    case FIPS_ROUTING_MtuExceeded: return "MtuExceeded";
    default: return "Unknown";
    }
}
fips_coords_required fips_coords_required_new(fips_node_addr dest, fips_node_addr reporter)
{ return (fips_coords_required){dest, reporter}; }
fips_path_broken fips_path_broken_new(fips_node_addr dest, fips_node_addr reporter)
{ return (fips_path_broken){dest, reporter, NULL}; }
void fips_path_broken_with_last_coords(fips_path_broken *p, const fips_tree_coordinate *coords)
{ p->last_known_coords = coords; }
fips_mtu_exceeded fips_mtu_exceeded_new(fips_node_addr dest, fips_node_addr reporter, uint16_t mtu)
{ return (fips_mtu_exceeded){dest, reporter, mtu}; }
static void header(fips_writer *w, fips_routing_signal_type type, size_t body,
                   const fips_node_addr *dest, const fips_node_addr *reporter)
{
    (void)fips_writer_write_u8(w, 0x00, NULL);
    (void)fips_writer_write_u8(w, 0x04, NULL);
    (void)fips_writer_write_u16_le(w, (uint16_t)body, NULL);
    (void)fips_writer_write_u8(w, (uint8_t)type, NULL);
    (void)fips_writer_write_u8(w, 0, NULL);
    (void)fips_writer_write_bytes(w, dest->bytes, 16, NULL);
    (void)fips_writer_write_bytes(w, reporter->bytes, 16, NULL);
}
size_t fips_path_broken_encoded_len(const fips_path_broken *p)
{ return 38 + (p->last_known_coords ? fips_coords_wire_size(p->last_known_coords) : 2); }
fips_status fips_coords_required_encode(const fips_coords_required *p, fips_writer *w, fips_proto_error *e)
{
    if (room(w, 4 + FIPS_COORDS_REQUIRED_SIZE, e) != FIPS_OK) return FIPS_ERR_RANGE;
    header(w, FIPS_ROUTING_CoordsRequired, FIPS_COORDS_REQUIRED_SIZE, &p->dest_addr, &p->reporter);
    return FIPS_OK;
}
fips_status fips_path_broken_encode(const fips_path_broken *p, fips_writer *w, fips_proto_error *e)
{
    if (p->last_known_coords && !p->last_known_coords->len) {
        if (e) *e = (fips_proto_error){.variant = FIPS_PROTO_BadCoord, .as.bad_coord = FIPS_COORD_EmptyCoordinate};
        return FIPS_ERR_RANGE;
    }
    size_t n = fips_path_broken_encoded_len(p);
    if (room(w, n, e) != FIPS_OK) return FIPS_ERR_RANGE;
    header(w, FIPS_ROUTING_PathBroken, n - 4, &p->dest_addr, &p->reporter);
    if (p->last_known_coords) (void)fips_encode_coords(p->last_known_coords, w, NULL);
    else (void)fips_encode_empty_coords(w, NULL);
    return FIPS_OK;
}
fips_status fips_mtu_exceeded_encode(const fips_mtu_exceeded *p, fips_writer *w, fips_proto_error *e)
{
    if (room(w, 4 + FIPS_MTU_EXCEEDED_SIZE, e) != FIPS_OK) return FIPS_ERR_RANGE;
    header(w, FIPS_ROUTING_MtuExceeded, FIPS_MTU_EXCEEDED_SIZE, &p->dest_addr, &p->reporter);
    (void)fips_writer_write_u16_le(w, p->mtu, NULL);
    return FIPS_OK;
}
static void addresses(fips_reader *r, fips_node_addr *dest, fips_node_addr *reporter)
{
    (void)fips_reader_advance(r, 1, NULL);
    (void)fips_reader_read_array(r, dest->bytes, 16, NULL);
    (void)fips_reader_read_array(r, reporter->bytes, 16, NULL);
}
fips_status fips_coords_required_decode(fips_reader *r, fips_coords_required *out, fips_proto_error *e)
{
    if (fips_reader_require(r, 33, e) != FIPS_OK) return FIPS_ERR_RANGE;
    if (check_body(r->buf[r->pos], 33, fips_reader_remaining(r), e) != FIPS_OK) return FIPS_ERR_RANGE;
    addresses(r, &out->dest_addr, &out->reporter);
    return FIPS_OK;
}
fips_status fips_mtu_exceeded_decode(fips_reader *r, fips_mtu_exceeded *out, fips_proto_error *e)
{
    if (fips_reader_require(r, 35, e) != FIPS_OK) return FIPS_ERR_RANGE;
    if (check_body(r->buf[r->pos], 35, fips_reader_remaining(r), e) != FIPS_OK) return FIPS_ERR_RANGE;
    addresses(r, &out->dest_addr, &out->reporter);
    (void)fips_reader_read_u16_le(r, &out->mtu, NULL);
    return FIPS_OK;
}
fips_status fips_path_broken_decode(fips_reader *r, fips_path_broken *out,
                                   fips_tree_coordinate *storage, fips_proto_error *e)
{
    if (fips_reader_require(r, 35, e) != FIPS_OK) return FIPS_ERR_RANGE;
    /* Rust's coordinate helper reads a fresh tail slice: its errors are
     * relative to that slice, not the surrounding signal body. */
    fips_reader coords;
    fips_reader_init(&coords, r->buf + r->pos + 33, fips_reader_remaining(r) - 33);
    fips_reader probe = coords;
    uint16_t count;
    (void)fips_reader_read_u16_le(&probe, &count, NULL);
    size_t coord_len = 2 + (size_t)count * 16;
    if (fips_reader_require(&coords, coord_len, e) != FIPS_OK) return FIPS_ERR_RANGE;
    if (check_body(r->buf[r->pos], 33 + coord_len, fips_reader_remaining(r), e) != FIPS_OK)
        return FIPS_ERR_RANGE;
    bool present;
    (void)fips_decode_optional_coords(&coords, storage, &present, NULL);
    addresses(r, &out->dest_addr, &out->reporter);
    (void)fips_reader_advance(r, coord_len, NULL);
    out->last_known_coords = present ? storage : NULL;
    return FIPS_OK;
}
