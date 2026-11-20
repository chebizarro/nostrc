#ifndef FIPS_PROTO_LOOKUP_WIRE_H
#define FIPS_PROTO_LOOKUP_WIRE_H
#include <fips/proto/coord.h>
#include <fips/identity/local.h>
#include <fips/identity/peer.h>
#include <fips/node/rate_limit.h>
#ifdef __cplusplus
extern "C" {
#endif
#define FIPS_LOOKUP_REQUEST_MAX_WIRE_SIZE (46u + 16u * FIPS_COORD_MAX_ENTRIES)
#define FIPS_LOOKUP_RESPONSE_MAX_WIRE_SIZE (93u + 16u * FIPS_COORD_MAX_ENTRIES)
typedef struct fips_lookup_request {
    uint64_t request_id;
    fips_node_addr target, origin;
    fips_tree_coordinate origin_coords;
    uint8_t ttl;
    uint16_t min_mtu;
} fips_lookup_request;
typedef struct fips_lookup_response {
    uint64_t request_id;
    fips_node_addr target;
    uint16_t path_mtu;
    fips_tree_coordinate target_coords;
    uint8_t proof[64];
} fips_lookup_response;
/* Large inline coordinates: allocate messages on caller heap/static storage.
 * Reader decodes AFTER msg_type; Writer emits msg_type. Trailing data is left
 * unread. Failure preserves output/cursor; error is optional, untouched on
 * success. Required pointers and nonoverlapping storage are preconditions. */
FIPS_CORE_API bool fips_lookup_request_can_forward(const fips_lookup_request *r);
FIPS_CORE_API bool fips_lookup_request_forward(fips_lookup_request *r);
FIPS_CORE_API size_t fips_lookup_request_encoded_len(const fips_lookup_request *r);
FIPS_CORE_API size_t fips_lookup_response_encoded_len(const fips_lookup_response *r);
FIPS_CORE_API fips_status fips_lookup_request_encode(const fips_lookup_request *r, fips_writer *w, fips_proto_error *e);
FIPS_CORE_API fips_status fips_lookup_request_decode(fips_reader *r, fips_lookup_request *out, fips_proto_error *e);
FIPS_CORE_API fips_status fips_lookup_response_encode(const fips_lookup_response *r, fips_writer *w, fips_proto_error *e);
FIPS_CORE_API fips_status fips_lookup_response_decode(fips_reader *r, fips_lookup_response *out, fips_proto_error *e);
/* request_id LE || target || address-only coord encoding; excludes path_mtu. */
FIPS_CORE_API fips_status fips_lookup_response_proof_bytes(uint64_t request_id, const fips_node_addr *target,
    const fips_tree_coordinate *coords, fips_writer *w, fips_proto_error *e);
FIPS_CORE_API void fips_lookup_response_proof_digest(const fips_lookup_response *r, uint8_t out[8]);
/* Scratch is borrowed for this call, at least 24 + coords_wire_size bytes.
 * Verify also binds peer identity to target. No implicit identity resolution. */
FIPS_CORE_API fips_status fips_lookup_response_verify(const fips_lookup_response *r, const fips_secp *secp,
    const fips_peer_identity *peer, uint8_t *scratch, size_t capacity, fips_proto_error *e);
/* Sign only as target, charged to the authenticated inbound link, never origin.
 * Rate refusal is OK with signed_out=false; proof and path_mtu are unchanged.
 * Success sets path_mtu=UINT16_MAX. Identity entropy supplies Schnorr aux bytes. */
FIPS_CORE_API fips_status fips_lookup_response_sign(fips_lookup_response *r, const fips_identity *identity,
    fips_lookup_sign_limiter *limiter, const fips_node_addr *from, fips_instant now,
    uint8_t *scratch, size_t capacity, bool *signed_out, fips_proto_error *e);
#ifdef __cplusplus
}
#endif
#endif
