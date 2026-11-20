#ifndef FIPS_PROTO_COORD_H
#define FIPS_PROTO_COORD_H
#include <fips/proto/codec.h>
#include <fips/identity/node_addr.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Upstream coord.rs has no depth constant: decode_coords reads a u16 entry
 * count. Preserve its entire wire domain, not a guessed frame-size limit.
 * Rust constructors can exceed this, but encode_coords truncates their count.
 * C rejects such non-wire-representable paths before changing output. */
#define FIPS_COORD_MAX_ENTRIES UINT16_MAX
#define FIPS_COORD_MAX_DEPTH (FIPS_COORD_MAX_ENTRIES - 1u)
#define FIPS_COORD_ENTRY_WIRE_SIZE 32u
#define FIPS_COORD_MAX_WIRE_SIZE (2u + 16u * FIPS_COORD_MAX_ENTRIES)
typedef struct fips_coord_entry {
    fips_node_addr node_addr;
    uint64_t sequence, timestamp; /* declaration timestamp: Unix seconds */
} fips_coord_entry;
/* ~2 MiB: use caller heap/static storage, not small event-loop stacks.
 * Valid paths have len >= 1, self -> parent -> ... -> root. Fields are public
 * for reading; construct/decode before use. Unused entries are unspecified.
 * No allocation, clock or I/O. Required pointers must be valid; byte buffers
 * must not overlap coordinate storage. Failures preserve outputs/cursors,
 * success preserves error. Optional decode with count 0 leaves out untouched.
 * Reader errors use cumulative offsets as in codec.h. */
typedef struct fips_tree_coordinate {
    fips_coord_entry entries[FIPS_COORD_MAX_ENTRIES];
    uint16_t len;
} fips_tree_coordinate;
FIPS_CORE_API fips_coord_entry fips_coord_entry_new(fips_node_addr addr, uint64_t sequence, uint64_t timestamp);
FIPS_CORE_API fips_coord_entry fips_coord_entry_addr_only(fips_node_addr addr);
FIPS_CORE_API bool fips_coord_entry_equal(const fips_coord_entry *a, const fips_coord_entry *b);
/* Entry wire format for tree gossip: addr(16), sequence(u64 LE), timestamp(u64 LE).
 * Address-only coordinate fields below intentionally discard metadata. */
FIPS_CORE_API fips_status fips_coord_entry_encode(const fips_coord_entry *entry, fips_writer *w, fips_proto_error *error);
FIPS_CORE_API fips_status fips_coord_entry_decode(fips_reader *r, fips_coord_entry *out, fips_proto_error *error);
/* Empty constructors: BadCoord(EmptyCoordinate). Excess count: MessageTooLong
 * {max: FIPS_COORD_MAX_ENTRIES, got: count}; both return FIPS_ERR_RANGE. */
FIPS_CORE_API fips_status fips_tree_coordinate_new(fips_tree_coordinate *out, const fips_coord_entry *entries, size_t count, fips_proto_error *error);
FIPS_CORE_API fips_status fips_tree_coordinate_from_addrs(fips_tree_coordinate *out, const fips_node_addr *addrs, size_t count, fips_proto_error *error);
FIPS_CORE_API void fips_tree_coordinate_root(fips_tree_coordinate *out, fips_node_addr addr);
FIPS_CORE_API void fips_tree_coordinate_root_with_meta(fips_tree_coordinate *out, fips_node_addr addr, uint64_t sequence, uint64_t timestamp);
FIPS_CORE_API bool fips_tree_coordinate_equal(const fips_tree_coordinate *a, const fips_tree_coordinate *b);
FIPS_CORE_API const fips_node_addr *fips_tree_coordinate_node_addr(const fips_tree_coordinate *c);
FIPS_CORE_API const fips_node_addr *fips_tree_coordinate_root_id(const fips_tree_coordinate *c);
FIPS_CORE_API const fips_node_addr *fips_tree_coordinate_parent_id(const fips_tree_coordinate *c);
FIPS_CORE_API size_t fips_tree_coordinate_depth(const fips_tree_coordinate *c);
FIPS_CORE_API bool fips_tree_coordinate_is_root(const fips_tree_coordinate *c);
FIPS_CORE_API bool fips_tree_coordinate_contains(const fips_tree_coordinate *c, const fips_node_addr *addr);
FIPS_CORE_API bool fips_tree_coordinate_has_ancestor(const fips_tree_coordinate *c, const fips_node_addr *addr);
FIPS_CORE_API const fips_node_addr *fips_tree_coordinate_ancestor_at(const fips_tree_coordinate *c, size_t depth);
/* Routing compares addresses only. Common ancestry is a suffix, not a prefix
 * of the self-to-root array (a prefix when walking root-to-self). */
FIPS_CORE_API size_t fips_tree_coordinate_common_suffix_len(const fips_tree_coordinate *a, const fips_tree_coordinate *b);
FIPS_CORE_API size_t fips_tree_coordinate_lca_depth(const fips_tree_coordinate *a, const fips_tree_coordinate *b);
FIPS_CORE_API const fips_node_addr *fips_tree_coordinate_lca(const fips_tree_coordinate *a, const fips_tree_coordinate *b);
FIPS_CORE_API size_t fips_tree_coordinate_distance_to(const fips_tree_coordinate *a, const fips_tree_coordinate *b);
FIPS_CORE_API size_t fips_coords_wire_size(const fips_tree_coordinate *c);
FIPS_CORE_API fips_status fips_encode_coords(const fips_tree_coordinate *c, fips_writer *w, fips_proto_error *error);
FIPS_CORE_API fips_status fips_decode_coords(fips_reader *r, fips_tree_coordinate *out, fips_proto_error *error);
FIPS_CORE_API fips_status fips_decode_optional_coords(fips_reader *r, fips_tree_coordinate *out, bool *present, fips_proto_error *error);
FIPS_CORE_API fips_status fips_encode_empty_coords(fips_writer *w, fips_proto_error *error);
/* Exact Rust Debug, UTF-8 arrows, lowercase first two address bytes, NUL.
 * RANGE on insufficient capacity, output untouched. size includes NUL. */
FIPS_CORE_API size_t fips_tree_coordinate_debug_size(const fips_tree_coordinate *c);
FIPS_CORE_API fips_status fips_tree_coordinate_debug(const fips_tree_coordinate *c, char *out, size_t capacity);
#ifdef __cplusplus
}
#endif
#endif
