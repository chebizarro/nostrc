#include <fips/proto/coord.h>
/* Ensure the deferred cache placeholder and real proto type coexist. */
#include <fips/cache/coord_cache.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); return 1; \
} } while (0)
/* Full wire-domain objects are deliberately not placed on the stack. */
static fips_tree_coordinate a, b, c, saved;
static fips_node_addr node(uint8_t v)
{ fips_node_addr n; memset(n.bytes, v, 16); return n; }
static bool addr_is(const fips_node_addr *addr, uint8_t v)
{ fips_node_addr n = node(v); return addr && !memcmp(addr->bytes, n.bytes, 16); }
static void path(fips_tree_coordinate *out, const uint8_t *ids, size_t n)
{
    out->len = (uint16_t)n;
    for (size_t i = 0; i < n; ++i) out->entries[i] = fips_coord_entry_addr_only(node(ids[i]));
}
#define PATH(out, ...) do { const uint8_t ids[] = {__VA_ARGS__}; path(out, ids, sizeof(ids)); } while (0)

/* All 13 core/src/proto/stp/tests/coordinate.rs behaviors, by name. coord.rs
 * itself has no inline tests at the pin. Extra C/wire cases follow. */
static int test_tree_coordinate_root(void)
{
    fips_tree_coordinate_root(&a, node(1));
    CHECK(fips_tree_coordinate_is_root(&a) && fips_tree_coordinate_depth(&a) == 0);
    CHECK(addr_is(fips_tree_coordinate_node_addr(&a), 1));
    CHECK(addr_is(fips_tree_coordinate_root_id(&a), 1));
    CHECK(addr_is(fips_tree_coordinate_parent_id(&a), 1));
    return 0;
}
static int test_tree_coordinate_path(void)
{
    const fips_node_addr addrs[] = {node(1), node(2), node(3)};
    CHECK(fips_tree_coordinate_from_addrs(&a, addrs, 3, NULL) == FIPS_OK);
    CHECK(!fips_tree_coordinate_is_root(&a) && fips_tree_coordinate_depth(&a) == 2);
    CHECK(addr_is(fips_tree_coordinate_node_addr(&a), 1));
    CHECK(addr_is(fips_tree_coordinate_parent_id(&a), 2));
    CHECK(addr_is(fips_tree_coordinate_root_id(&a), 3));
    return 0;
}
static int test_tree_coordinate_empty_fails(void)
{
    saved = a;
    fips_proto_error e;
    CHECK(fips_tree_coordinate_from_addrs(&a, NULL, 0, &e) == FIPS_ERR_RANGE);
    CHECK(e.variant == FIPS_PROTO_BadCoord && e.as.bad_coord == FIPS_COORD_EmptyCoordinate);
    CHECK(!memcmp(&a, &saved, sizeof(a)));
    CHECK(fips_tree_coordinate_new(&a, NULL, 0, &e) == FIPS_ERR_RANGE);
    CHECK(e.variant == FIPS_PROTO_BadCoord && !memcmp(&a, &saved, sizeof(a)));
    return 0;
}
static int test_tree_coordinate_entries_metadata(void)
{
    fips_coord_entry entries[] = {fips_coord_entry_new(node(1), 5, 1000), fips_coord_entry_new(node(0), 1, 500)};
    CHECK(fips_tree_coordinate_new(&a, entries, 2, NULL) == FIPS_OK);
    CHECK(a.entries[0].sequence == 5 && a.entries[0].timestamp == 1000);
    CHECK(a.entries[1].sequence == 1 && a.entries[1].timestamp == 500);
    fips_tree_coordinate_root_with_meta(&b, node(4), UINT64_MAX, UINT64_MAX);
    CHECK(b.len == 1 && b.entries[0].sequence == UINT64_MAX && b.entries[0].timestamp == UINT64_MAX);
    return 0;
}
static int test_tree_distance_same_node(void)
{ fips_tree_coordinate_root(&a, node(1)); CHECK(fips_tree_coordinate_distance_to(&a, &a) == 0); return 0; }
static int test_tree_distance_siblings(void)
{ PATH(&a, 1, 0); PATH(&b, 2, 0); CHECK(fips_tree_coordinate_distance_to(&a, &b) == 2); return 0; }
static int test_tree_distance_ancestor(void)
{ PATH(&a, 1, 0); PATH(&b, 2, 1, 0); CHECK(fips_tree_coordinate_distance_to(&b, &a) == 1); return 0; }
static int test_tree_distance_cousins(void)
{ PATH(&a, 3, 1, 0); PATH(&b, 4, 2, 0); CHECK(fips_tree_coordinate_distance_to(&a, &b) == 4); return 0; }
static int test_tree_distance_different_roots(void)
{
    fips_tree_coordinate_root(&a, node(1)); fips_tree_coordinate_root(&b, node(2));
    CHECK(fips_tree_coordinate_distance_to(&a, &b) == SIZE_MAX);
    CHECK(fips_tree_coordinate_lca(&a, &b) == NULL && fips_tree_coordinate_lca_depth(&a, &b) == 0);
    return 0;
}
static int test_has_ancestor(void)
{
    PATH(&a, 2, 1, 0); fips_node_addr parent = node(1), root = node(0), child = node(2);
    CHECK(fips_tree_coordinate_has_ancestor(&a, &parent) && fips_tree_coordinate_has_ancestor(&a, &root));
    CHECK(!fips_tree_coordinate_has_ancestor(&a, &child)); return 0;
}
static int test_contains(void)
{
    PATH(&a, 2, 1, 0);
    for (uint8_t i = 0; i < 3; ++i) { fips_node_addr n = node(i); CHECK(fips_tree_coordinate_contains(&a, &n)); }
    fips_node_addr other = node(99); CHECK(!fips_tree_coordinate_contains(&a, &other)); return 0;
}
static int test_ancestor_at(void)
{
    PATH(&a, 2, 1, 0);
    CHECK(addr_is(fips_tree_coordinate_ancestor_at(&a, 0), 2));
    CHECK(addr_is(fips_tree_coordinate_ancestor_at(&a, 1), 1));
    CHECK(addr_is(fips_tree_coordinate_ancestor_at(&a, 2), 0));
    CHECK(!fips_tree_coordinate_ancestor_at(&a, 3) && !fips_tree_coordinate_ancestor_at(&a, SIZE_MAX)); return 0;
}
static int test_lca(void)
{
    PATH(&a, 3, 1, 0); PATH(&b, 4, 2, 0); PATH(&c, 1, 0);
    CHECK(addr_is(fips_tree_coordinate_lca(&a, &b), 0));
    CHECK(addr_is(fips_tree_coordinate_lca(&a, &c), 1));
    CHECK(fips_tree_coordinate_lca_depth(&a, &c) == 1);
    return 0;
}
static int equality_suffix_debug(void)
{
    PATH(&a, 3, 1, 0); b = a;
    CHECK(fips_tree_coordinate_equal(&a, &b));
    b.entries[0].sequence = 1; CHECK(!fips_tree_coordinate_equal(&a, &b));
    b = a; b.entries[2].timestamp = 1; CHECK(!fips_tree_coordinate_equal(&a, &b));
    CHECK(fips_tree_coordinate_common_suffix_len(&a, &b) == 3); /* routing ignores metadata */
    CHECK(fips_tree_coordinate_distance_to(&a, &b) == 0);
    PATH(&b, 3, 1, 9); CHECK(!fips_tree_coordinate_equal(&a, &b));
    CHECK(fips_tree_coordinate_common_suffix_len(&a, &b) == 0); /* common prefix is not ancestry */
    PATH(&b, 1, 0); CHECK(!fips_tree_coordinate_equal(&a, &b));
    CHECK(fips_tree_coordinate_common_suffix_len(&a, &b) == 2);
    char debug[128], sentinel[128]; memset(debug, 42, sizeof(debug)); memcpy(sentinel, debug, sizeof(debug));
    const char expected[] = "TreeCoordinate(depth=2, path=[0303 → 0101 → 0000])";
    CHECK(fips_tree_coordinate_debug_size(&a) == sizeof(expected));
    for (size_t i = 0; i < sizeof(expected); ++i) {
        CHECK(fips_tree_coordinate_debug(&a, debug, i) == FIPS_ERR_RANGE);
        CHECK(!memcmp(debug, sentinel, sizeof(debug)));
    }
    CHECK(fips_tree_coordinate_debug(&a, debug, sizeof(expected)) == FIPS_OK && !strcmp(debug, expected));
    fips_tree_coordinate_root(&a, node(0xab));
    CHECK(fips_tree_coordinate_debug(&a, debug, sizeof(debug)) == FIPS_OK);
    CHECK(!strcmp(debug, "TreeCoordinate(depth=0, path=[abab])"));
    return 0;
}
static int wire_errors_and_offsets(void)
{
    PATH(&a, 2, 1, 0); a.entries[0].sequence = 42;
    uint8_t wire[52], output[52]; memset(wire, 42, sizeof(wire));
    fips_writer w; fips_reader r; fips_proto_error e = {.variant = FIPS_PROTO_InvalidSignature};
    fips_writer_init(&w, wire, sizeof(wire)); CHECK(fips_writer_write_u8(&w, 99, NULL) == FIPS_OK);
    CHECK(fips_encode_coords(&a, &w, &e) == FIPS_OK && e.variant == FIPS_PROTO_InvalidSignature);
    CHECK(fips_writer_len(&w) == 51 && fips_coords_wire_size(&a) == 50);
    CHECK(wire[1] == 3 && wire[2] == 0 && wire[51] == 42);
    memset(&b, 42, sizeof(b)); saved = b;
    for (size_t len = 1; len < 51; ++len) {
        fips_reader_init(&r, wire, len); CHECK(fips_reader_advance(&r, 1, NULL) == FIPS_OK);
        bool present = false;
        CHECK(fips_decode_coords(&r, &b, &e) == FIPS_ERR_RANGE);
        CHECK(e.variant == FIPS_PROTO_MessageTooShort && e.as.message_too_short.got == len);
        CHECK(e.as.message_too_short.expected == (len < 3 ? 3 : 51));
        CHECK(r.pos == 1 && !memcmp(&b, &saved, sizeof(b)));
        CHECK(fips_decode_optional_coords(&r, &b, &present, &e) == FIPS_ERR_RANGE && !present);
        CHECK(r.pos == 1 && !memcmp(&b, &saved, sizeof(b)));
    }
    fips_reader_init(&r, wire, sizeof(wire)); CHECK(fips_reader_advance(&r, 1, NULL) == FIPS_OK);
    bool present = false; e.variant = FIPS_PROTO_InvalidSignature;
    CHECK(fips_decode_optional_coords(&r, &b, &present, &e) == FIPS_OK && present && r.pos == 51);
    CHECK(e.variant == FIPS_PROTO_InvalidSignature && b.len == 3 && b.entries[0].sequence == 0);
    a.entries[0].sequence = 0; CHECK(fips_tree_coordinate_equal(&a, &b));
    for (size_t cap = 1; cap < 51; ++cap) {
        memset(output, 42, sizeof(output)); fips_writer_init(&w, output, cap);
        CHECK(fips_writer_write_u8(&w, 99, NULL) == FIPS_OK);
        CHECK(fips_encode_coords(&a, &w, &e) == FIPS_ERR_RANGE);
        CHECK(e.variant == FIPS_PROTO_MessageTooLong && e.as.message_too_long.max == cap && e.as.message_too_long.got == 51);
        CHECK(w.pos == 1 && output[0] == 99);
        for (size_t j = 1; j < sizeof(output); ++j) CHECK(output[j] == 42);
    }
    const uint8_t zero[] = {0, 0, 42}; saved = b;
    fips_reader_init(&r, zero, sizeof(zero));
    CHECK(fips_decode_coords(&r, &b, &e) == FIPS_ERR_RANGE && r.pos == 0);
    CHECK(e.variant == FIPS_PROTO_Malformed && !strcmp(e.as.malformed, "coordinate with zero entries"));
    CHECK(!memcmp(&b, &saved, sizeof(b)));
    CHECK(fips_decode_optional_coords(&r, &b, &present, NULL) == FIPS_OK && !present && r.pos == 2);
    CHECK(!memcmp(&b, &saved, sizeof(b)));
    fips_reader_init(&r, NULL, 0); CHECK(fips_decode_coords(&r, &b, NULL) == FIPS_ERR_RANGE);
    fips_writer_init(&w, NULL, 0); CHECK(fips_encode_empty_coords(&w, &e) == FIPS_ERR_RANGE);
    fips_writer_init(&w, output, 2); CHECK(fips_encode_empty_coords(&w, NULL) == FIPS_OK && output[0] == 0 && output[1] == 0);
    return 0;
}
static int entry_wire(void)
{
    fips_coord_entry entry = fips_coord_entry_new(node(0xab), UINT64_C(0x0123456789abcdef), UINT64_MAX);
    fips_coord_entry out = fips_coord_entry_addr_only(node(0));
    uint8_t bytes[34]; fips_reader r; fips_writer w; fips_proto_error e;
    fips_writer_init(&w, bytes + 1, 32);
    CHECK(fips_coord_entry_encode(&entry, &w, NULL) == FIPS_OK && w.pos == 32);
    for (size_t i = 1; i < 17; ++i) CHECK(bytes[i] == 0xab);
    const uint8_t seq[] = {0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 0x01};
    CHECK(!memcmp(bytes + 17, seq, 8));
    for (size_t i = 25; i < 33; ++i) CHECK(bytes[i] == 0xff);
    for (size_t len = 0; len < 32; ++len) {
        fips_reader_init(&r, bytes + 1, len);
        CHECK(fips_coord_entry_decode(&r, &out, &e) == FIPS_ERR_RANGE && r.pos == 0);
        CHECK(e.variant == FIPS_PROTO_MessageTooShort && e.as.message_too_short.expected == 32 && e.as.message_too_short.got == len);
        CHECK(out.sequence == 0 && addr_is(&out.node_addr, 0));
        uint8_t output[32]; memset(output, 42, sizeof(output)); fips_writer_init(&w, output, len);
        CHECK(fips_coord_entry_encode(&entry, &w, &e) == FIPS_ERR_RANGE && w.pos == 0);
        CHECK(e.variant == FIPS_PROTO_MessageTooLong && e.as.message_too_long.got == 32);
        for (size_t j = 0; j < sizeof(output); ++j) CHECK(output[j] == 42);
    }
    fips_reader_init(&r, bytes + 1, 33); CHECK(fips_coord_entry_decode(&r, &out, NULL) == FIPS_OK && r.pos == 32);
    CHECK(fips_coord_entry_equal(&entry, &out));
    return 0;
}
static int depth_bounds(void)
{
    fips_proto_error e;
    fips_node_addr *addrs = calloc(FIPS_COORD_MAX_ENTRIES, sizeof(*addrs));
    uint8_t *wire = malloc(FIPS_COORD_MAX_WIRE_SIZE);
    CHECK(addrs && wire);
    addrs[FIPS_COORD_MAX_ENTRIES - 1] = node(0xff);
    CHECK(fips_tree_coordinate_from_addrs(&a, addrs, FIPS_COORD_MAX_ENTRIES, NULL) == FIPS_OK);
    CHECK(a.len == 65535 && fips_tree_coordinate_depth(&a) == 65534 && FIPS_COORD_MAX_DEPTH == 65534);
    CHECK(fips_tree_coordinate_new(&b, a.entries, FIPS_COORD_MAX_ENTRIES, NULL) == FIPS_OK);
    CHECK(fips_tree_coordinate_equal(&a, &b)); saved = a;
    CHECK(fips_tree_coordinate_from_addrs(&a, NULL, (size_t)FIPS_COORD_MAX_ENTRIES + 1, &e) == FIPS_ERR_RANGE);
    CHECK(e.variant == FIPS_PROTO_MessageTooLong && e.as.message_too_long.max == 65535 && e.as.message_too_long.got == 65536);
    CHECK(fips_tree_coordinate_new(&a, NULL, SIZE_MAX, &e) == FIPS_ERR_RANGE && e.as.message_too_long.got == SIZE_MAX);
    CHECK(!memcmp(&a, &saved, sizeof(a)));
    fips_writer w; fips_reader r; fips_writer_init(&w, wire, FIPS_COORD_MAX_WIRE_SIZE);
    CHECK(fips_encode_coords(&a, &w, NULL) == FIPS_OK && w.pos == FIPS_COORD_MAX_WIRE_SIZE);
    CHECK(wire[0] == 0xff && wire[1] == 0xff);
    fips_reader_init(&r, wire, FIPS_COORD_MAX_WIRE_SIZE);
    CHECK(fips_decode_coords(&r, &b, NULL) == FIPS_OK && r.pos == FIPS_COORD_MAX_WIRE_SIZE);
    CHECK(fips_tree_coordinate_equal(&a, &b));
    size_t size = fips_tree_coordinate_debug_size(&a); char *debug = malloc(size); CHECK(debug);
    CHECK(fips_tree_coordinate_debug(&a, debug, size) == FIPS_OK && strlen(debug) + 1 == size);
    CHECK(!strncmp(debug, "TreeCoordinate(depth=65534, path=[", 33));
    fips_reader_init(&r, wire, 2); CHECK(fips_decode_coords(&r, &b, &e) == FIPS_ERR_RANGE);
    CHECK(e.as.message_too_short.expected == FIPS_COORD_MAX_WIRE_SIZE && e.as.message_too_short.got == 2);
    free(debug); free(wire); free(addrs); return 0;
}
int main(void)
{
    int (*tests[])(void) = {test_tree_coordinate_root, test_tree_coordinate_path,
        test_tree_coordinate_empty_fails, test_tree_coordinate_entries_metadata,
        test_tree_distance_same_node, test_tree_distance_siblings, test_tree_distance_ancestor,
        test_tree_distance_cousins, test_tree_distance_different_roots, test_has_ancestor,
        test_contains, test_ancestor_at, test_lca, equality_suffix_debug, wire_errors_and_offsets,
        entry_wire, depth_bounds};
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) if (tests[i]()) return 1;
    puts("coord: 13 upstream behaviors + 4 C/wire boundary groups passed"); return 0;
}
