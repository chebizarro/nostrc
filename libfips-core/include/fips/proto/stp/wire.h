#ifndef FIPS_STP_WIRE_H
#define FIPS_STP_WIRE_H
#include <fips/proto/stp/declaration.h>
#include <fips/proto/coord.h>
#ifdef __cplusplus
extern "C" {
#endif
#define FIPS_TREE_ANNOUNCE_VERSION_1 1u
#define FIPS_TREE_ANNOUNCE_MIN_PAYLOAD_SIZE 99u
#define FIPS_TREE_ANNOUNCE_MAX_WIRE_SIZE (100u + 32u * FIPS_COORD_MAX_ENTRIES)
typedef struct fips_tree_announce {
    fips_parent_declaration declaration;
    fips_tree_coordinate ancestry;
} fips_tree_announce;
typedef enum fips_tree_error_variant {
    FIPS_TREE_EmptyCoordinate,
    FIPS_TREE_AncestryNotToRoot,
    FIPS_TREE_RootDeclarationMismatch,
    FIPS_TREE_AncestryTooShort,
    FIPS_TREE_AncestryNodeMismatch,
    FIPS_TREE_AncestryParentMismatch,
    FIPS_TREE_AncestryRootNotMinimum,
    FIPS_TREE_InvalidSignature,
    FIPS_TREE_SequenceRegression,
    FIPS_TREE_ParentNotPeer,
    FIPS_TREE_Identity
} fips_tree_error_variant;
typedef struct fips_tree_error {
    fips_tree_error_variant variant;
    fips_node_addr declared, ancestry; /* mismatch; advertised/minimum for root */
    uint64_t got, expected;
} fips_tree_error;
/* Allocate large coordinate-containing values in caller heap/static storage.
 * No crypto verification: Rust Schnorr parsing accepts every 64-byte string.
 * encode includes 0x10; decode starts AFTER it, consumes exactly one message,
 * allows trailing bytes. Atomic on failure, error unchanged on success. */
FIPS_CORE_API size_t fips_tree_announce_encoded_len(const fips_tree_announce *a);
FIPS_CORE_API fips_status fips_tree_announce_encode(const fips_tree_announce *a, fips_writer *w,
                                                    fips_proto_error *error);
FIPS_CORE_API fips_status fips_tree_announce_decode(fips_reader *r, fips_tree_announce *out,
                                                    fips_proto_error *error);
FIPS_CORE_API fips_status fips_tree_announce_validate_semantics(const fips_tree_announce *a,
                                                                fips_tree_error *error);
#ifdef __cplusplus
}
#endif
#endif
