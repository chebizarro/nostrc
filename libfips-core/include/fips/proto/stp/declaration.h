#ifndef FIPS_STP_DECLARATION_H
#define FIPS_STP_DECLARATION_H
#include <fips/proto/codec.h>
#include <fips/identity/node_addr.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Public values, not crypto objects. The host signs signing_bytes and verifies
 * the signature and authenticated sender before admitting an announce. */
typedef struct fips_parent_declaration {
    fips_node_addr node_addr, parent_id;
    uint64_t sequence, timestamp;
    bool is_signed;
    uint8_t signature[64];
} fips_parent_declaration;
#define FIPS_PARENT_DECLARATION_SIGNING_SIZE 48u
FIPS_CORE_API fips_parent_declaration fips_parent_declaration_new(fips_node_addr node,
                                                                  fips_node_addr parent,
                                                                  uint64_t sequence,
                                                                  uint64_t timestamp);
FIPS_CORE_API fips_parent_declaration fips_parent_declaration_self_root(fips_node_addr node,
                                                                        uint64_t sequence,
                                                                        uint64_t timestamp);
FIPS_CORE_API fips_parent_declaration fips_parent_declaration_with_signature(
    fips_node_addr node, fips_node_addr parent, uint64_t sequence, uint64_t timestamp,
    const uint8_t signature[64]);
FIPS_CORE_API void fips_parent_declaration_set_signature(fips_parent_declaration *d,
                                                         const uint8_t signature[64]);
FIPS_CORE_API bool fips_parent_declaration_is_root(const fips_parent_declaration *d);
FIPS_CORE_API bool fips_parent_declaration_equal(const fips_parent_declaration *a,
                                                 const fips_parent_declaration *b);
FIPS_CORE_API bool fips_parent_declaration_is_fresher_than(const fips_parent_declaration *a,
                                                           const fips_parent_declaration *b);
/* This is the 48-byte signing preimage, NOT a standalone link message. Rust
 * defines no ParentDeclaration wire message. Signature is carried by TreeAnnounce.
 * decode reads an unsigned preimage. Both calls preserve output/cursor on error. */
FIPS_CORE_API fips_status fips_parent_declaration_signing_bytes(const fips_parent_declaration *d,
                                                                fips_writer *w,
                                                                fips_proto_error *error);
FIPS_CORE_API fips_status fips_parent_declaration_decode(fips_reader *r,
                                                         fips_parent_declaration *out,
                                                         fips_proto_error *error);
#ifdef __cplusplus
}
#endif
#endif
