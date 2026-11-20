#ifndef FIPS_ROUTING_WIRE_H
#define FIPS_ROUTING_WIRE_H
#include <fips/proto/coord.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum fips_routing_signal_type {
    FIPS_ROUTING_CoordsRequired = 0x20,
    FIPS_ROUTING_PathBroken = 0x21,
    FIPS_ROUTING_MtuExceeded = 0x22
} fips_routing_signal_type;
#define FIPS_COORDS_REQUIRED_SIZE 34u
#define FIPS_MTU_EXCEEDED_SIZE 36u
#define FIPS_ROUTING_MAX_WIRE_SIZE (38u + FIPS_COORD_MAX_WIRE_SIZE)
typedef struct fips_coords_required { fips_node_addr dest_addr, reporter; } fips_coords_required;
typedef struct fips_path_broken {
    fips_node_addr dest_addr, reporter;
    const fips_tree_coordinate *last_known_coords; /* NULL = None; borrowed */
} fips_path_broken;
typedef struct fips_mtu_exceeded { fips_node_addr dest_addr, reporter; uint16_t mtu; } fips_mtu_exceeded;
/* from_byte None: false, out unchanged. */
FIPS_CORE_API bool fips_routing_signal_type_from_byte(uint8_t byte, fips_routing_signal_type *out);
FIPS_CORE_API uint8_t fips_routing_signal_type_to_byte(fips_routing_signal_type type);
FIPS_CORE_API const char *fips_routing_signal_type_name(fips_routing_signal_type type);
FIPS_CORE_API fips_coords_required fips_coords_required_new(fips_node_addr dest, fips_node_addr reporter);
FIPS_CORE_API fips_path_broken fips_path_broken_new(fips_node_addr dest, fips_node_addr reporter);
FIPS_CORE_API void fips_path_broken_with_last_coords(fips_path_broken *p, const fips_tree_coordinate *coords);
FIPS_CORE_API fips_mtu_exceeded fips_mtu_exceeded_new(fips_node_addr dest, fips_node_addr reporter, uint16_t mtu);
/* Encode includes the 4-byte FSP cleartext prefix AND msg_type. Decode takes
 * only the body AFTER that prefix and type, exactly as Rust. Reserved flags
 * and trailing bytes are errors. Cursor/buffers/outputs unchanged on failure;
 * error unchanged on success. Required pointers valid, byte ranges disjoint.
 * PathBroken decode borrows caller coords_storage (large: heap/static), which
 * must outlive out. It is untouched for None or failure. Address-only coords
 * use the shared codec, not tree-announcement metadata. The prefix's u16
 * length cast mirrors upstream even for coordinate lists exceeding 65535 wire
 * bytes; the host must enforce its enclosing frame/transport size budget. */
FIPS_CORE_API size_t fips_path_broken_encoded_len(const fips_path_broken *p);
FIPS_CORE_API fips_status fips_coords_required_encode(const fips_coords_required *p, fips_writer *w, fips_proto_error *error);
FIPS_CORE_API fips_status fips_path_broken_encode(const fips_path_broken *p, fips_writer *w, fips_proto_error *error);
FIPS_CORE_API fips_status fips_mtu_exceeded_encode(const fips_mtu_exceeded *p, fips_writer *w, fips_proto_error *error);
FIPS_CORE_API fips_status fips_coords_required_decode(fips_reader *r, fips_coords_required *out, fips_proto_error *error);
FIPS_CORE_API fips_status fips_path_broken_decode(fips_reader *r, fips_path_broken *out, fips_tree_coordinate *coords_storage, fips_proto_error *error);
FIPS_CORE_API fips_status fips_mtu_exceeded_decode(fips_reader *r, fips_mtu_exceeded *out, fips_proto_error *error);
#ifdef __cplusplus
}
#endif
#endif
