#include <fips/proto/coord.h>
#include <stdio.h>
#include <string.h>

static bool addr_equal(const fips_node_addr *a, const fips_node_addr *b)
{ return memcmp(a->bytes, b->bytes, sizeof(a->bytes)) == 0; }

static fips_status count_valid(size_t count, fips_proto_error *e)
{
    if (count == 0) {
        if (e) *e = (fips_proto_error){.variant = FIPS_PROTO_BadCoord,
            .as.bad_coord = FIPS_COORD_EmptyCoordinate};
        return FIPS_ERR_RANGE;
    }
    if (count > FIPS_COORD_MAX_ENTRIES) {
        if (e) *e = (fips_proto_error){.variant = FIPS_PROTO_MessageTooLong,
            .as.message_too_long = {FIPS_COORD_MAX_ENTRIES, count}};
        return FIPS_ERR_RANGE;
    }
    return FIPS_OK;
}
static fips_status writer_require(const fips_writer *w, size_t n, fips_proto_error *e)
{
    if (n <= fips_writer_remaining(w)) return FIPS_OK;
    if (e) *e = (fips_proto_error){.variant = FIPS_PROTO_MessageTooLong,
        .as.message_too_long = {w->capacity, n > SIZE_MAX - w->pos ? SIZE_MAX : w->pos + n}};
    return FIPS_ERR_RANGE;
}
fips_coord_entry fips_coord_entry_new(fips_node_addr addr, uint64_t sequence, uint64_t timestamp)
{ return (fips_coord_entry){addr, sequence, timestamp}; }
fips_coord_entry fips_coord_entry_addr_only(fips_node_addr addr)
{ return fips_coord_entry_new(addr, 0, 0); }
bool fips_coord_entry_equal(const fips_coord_entry *a, const fips_coord_entry *b)
{ return addr_equal(&a->node_addr, &b->node_addr) && a->sequence == b->sequence && a->timestamp == b->timestamp; }
fips_status fips_coord_entry_encode(const fips_coord_entry *entry, fips_writer *w, fips_proto_error *e)
{
    if (writer_require(w, FIPS_COORD_ENTRY_WIRE_SIZE, e) != FIPS_OK) return FIPS_ERR_RANGE;
    (void)fips_writer_write_bytes(w, entry->node_addr.bytes, 16, NULL);
    (void)fips_writer_write_u64_le(w, entry->sequence, NULL);
    (void)fips_writer_write_u64_le(w, entry->timestamp, NULL);
    return FIPS_OK;
}
fips_status fips_coord_entry_decode(fips_reader *r, fips_coord_entry *out, fips_proto_error *e)
{
    if (fips_reader_require(r, FIPS_COORD_ENTRY_WIRE_SIZE, e) != FIPS_OK) return FIPS_ERR_RANGE;
    (void)fips_reader_read_array(r, out->node_addr.bytes, 16, NULL);
    (void)fips_reader_read_u64_le(r, &out->sequence, NULL);
    (void)fips_reader_read_u64_le(r, &out->timestamp, NULL);
    return FIPS_OK;
}
fips_status fips_tree_coordinate_new(fips_tree_coordinate *out, const fips_coord_entry *entries, size_t count, fips_proto_error *e)
{
    if (count_valid(count, e) != FIPS_OK) return FIPS_ERR_RANGE;
    memmove(out->entries, entries, count * sizeof(*entries));
    out->len = (uint16_t)count;
    return FIPS_OK;
}
fips_status fips_tree_coordinate_from_addrs(fips_tree_coordinate *out, const fips_node_addr *addrs, size_t count, fips_proto_error *e)
{
    if (count_valid(count, e) != FIPS_OK) return FIPS_ERR_RANGE;
    for (size_t i = 0; i < count; ++i) out->entries[i] = fips_coord_entry_addr_only(addrs[i]);
    out->len = (uint16_t)count;
    return FIPS_OK;
}
void fips_tree_coordinate_root(fips_tree_coordinate *out, fips_node_addr addr)
{ fips_tree_coordinate_root_with_meta(out, addr, 0, 0); }
void fips_tree_coordinate_root_with_meta(fips_tree_coordinate *out, fips_node_addr addr, uint64_t sequence, uint64_t timestamp)
{
    out->entries[0] = fips_coord_entry_new(addr, sequence, timestamp);
    out->len = 1;
}
bool fips_tree_coordinate_equal(const fips_tree_coordinate *a, const fips_tree_coordinate *b)
{
    if (a->len != b->len) return false;
    for (size_t i = 0; i < a->len; ++i)
        if (!fips_coord_entry_equal(&a->entries[i], &b->entries[i])) return false;
    return true;
}
const fips_node_addr *fips_tree_coordinate_node_addr(const fips_tree_coordinate *c)
{ return &c->entries[0].node_addr; }
const fips_node_addr *fips_tree_coordinate_root_id(const fips_tree_coordinate *c)
{ return &c->entries[c->len - 1].node_addr; }
const fips_node_addr *fips_tree_coordinate_parent_id(const fips_tree_coordinate *c)
{ return &c->entries[c->len > 1 ? 1 : 0].node_addr; }
size_t fips_tree_coordinate_depth(const fips_tree_coordinate *c) { return c->len - 1u; }
bool fips_tree_coordinate_is_root(const fips_tree_coordinate *c) { return c->len == 1; }
const fips_node_addr *fips_tree_coordinate_ancestor_at(const fips_tree_coordinate *c, size_t depth)
{ return depth < c->len ? &c->entries[depth].node_addr : NULL; }
bool fips_tree_coordinate_has_ancestor(const fips_tree_coordinate *c, const fips_node_addr *addr)
{
    for (size_t i = 1; i < c->len; ++i) if (addr_equal(&c->entries[i].node_addr, addr)) return true;
    return false;
}
bool fips_tree_coordinate_contains(const fips_tree_coordinate *c, const fips_node_addr *addr)
{ return addr_equal(fips_tree_coordinate_node_addr(c), addr) || fips_tree_coordinate_has_ancestor(c, addr); }
size_t fips_tree_coordinate_common_suffix_len(const fips_tree_coordinate *a, const fips_tree_coordinate *b)
{
    size_t n = 0;
    while (n < a->len && n < b->len &&
        addr_equal(&a->entries[a->len - n - 1].node_addr, &b->entries[b->len - n - 1].node_addr)) ++n;
    return n;
}
size_t fips_tree_coordinate_lca_depth(const fips_tree_coordinate *a, const fips_tree_coordinate *b)
{
    size_t n = fips_tree_coordinate_common_suffix_len(a, b);
    return n ? n - 1 : 0;
}
const fips_node_addr *fips_tree_coordinate_lca(const fips_tree_coordinate *a, const fips_tree_coordinate *b)
{
    size_t n = fips_tree_coordinate_common_suffix_len(a, b);
    return n ? &a->entries[a->len - n].node_addr : NULL;
}
size_t fips_tree_coordinate_distance_to(const fips_tree_coordinate *a, const fips_tree_coordinate *b)
{
    size_t n = fips_tree_coordinate_common_suffix_len(a, b);
    return n ? (size_t)a->len + b->len - 2 * n : SIZE_MAX;
}
size_t fips_coords_wire_size(const fips_tree_coordinate *c) { return 2 + (size_t)c->len * 16; }
fips_status fips_encode_coords(const fips_tree_coordinate *c, fips_writer *w, fips_proto_error *e)
{
    if (count_valid(c->len, e) != FIPS_OK || writer_require(w, fips_coords_wire_size(c), e) != FIPS_OK) return FIPS_ERR_RANGE;
    (void)fips_writer_write_u16_le(w, c->len, NULL);
    for (size_t i = 0; i < c->len; ++i) (void)fips_writer_write_bytes(w, c->entries[i].node_addr.bytes, 16, NULL);
    return FIPS_OK;
}
static fips_status decode(fips_reader *r, fips_tree_coordinate *out, bool *present, bool optional, fips_proto_error *e)
{
    fips_reader probe = *r;
    uint16_t count;
    if (fips_reader_read_u16_le(&probe, &count, e) != FIPS_OK) return FIPS_ERR_RANGE;
    /* Require the complete field before writing even one entry. Upstream checks
     * this before rejecting count zero, and ignores trailing message bytes. */
    if (fips_reader_require(r, 2 + (size_t)count * 16, e) != FIPS_OK) return FIPS_ERR_RANGE;
    if (count == 0 && !optional) {
        if (e) *e = (fips_proto_error){.variant = FIPS_PROTO_Malformed,
            .as.malformed = "coordinate with zero entries"};
        return FIPS_ERR_RANGE;
    }
    for (size_t i = 0; i < count; ++i) {
        (void)fips_reader_read_array(&probe, out->entries[i].node_addr.bytes, 16, NULL);
        out->entries[i].sequence = out->entries[i].timestamp = 0;
    }
    if (count) out->len = count;
    if (present) *present = count != 0;
    *r = probe;
    return FIPS_OK;
}
fips_status fips_decode_coords(fips_reader *r, fips_tree_coordinate *out, fips_proto_error *e)
{ return decode(r, out, NULL, false, e); }
fips_status fips_decode_optional_coords(fips_reader *r, fips_tree_coordinate *out, bool *present, fips_proto_error *e)
{ return decode(r, out, present, true, e); }
fips_status fips_encode_empty_coords(fips_writer *w, fips_proto_error *e)
{ return fips_writer_write_u16_le(w, 0, e); }
static size_t debug_head(const fips_tree_coordinate *c, char head[48])
{ return (size_t)snprintf(head, 48, "TreeCoordinate(depth=%zu, path=[", fips_tree_coordinate_depth(c)); }
size_t fips_tree_coordinate_debug_size(const fips_tree_coordinate *c)
{
    char head[48];
    return debug_head(c, head) + (size_t)c->len * 4 + ((size_t)c->len - 1) * 5 + 3;
}
fips_status fips_tree_coordinate_debug(const fips_tree_coordinate *c, char *out, size_t capacity)
{
    if (capacity < fips_tree_coordinate_debug_size(c)) return FIPS_ERR_RANGE;
    static const char hex[] = "0123456789abcdef";
    char head[48];
    size_t pos = debug_head(c, head);
    memcpy(out, head, pos);
    for (size_t i = 0; i < c->len; ++i) {
        if (i) { memcpy(out + pos, " → ", 5); pos += 5; }
        for (size_t j = 0; j < 2; ++j) {
            uint8_t b = c->entries[i].node_addr.bytes[j];
            out[pos++] = hex[b >> 4]; out[pos++] = hex[b & 15];
        }
    }
    memcpy(out + pos, "])", 3);
    return FIPS_OK;
}
