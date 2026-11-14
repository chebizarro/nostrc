#ifndef FIPS_PROTO_FSP_CORE_H
#define FIPS_PROTO_FSP_CORE_H
#include <fips/proto/fsp/wire.h>
#include <fips/proto/fsp/quorum.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Fsp is a stateless namespace upstream; session table/keys remain host-owned. */
typedef enum fips_fsp_action_kind {
    FIPS_FSP_InvalidateCoords, FIPS_FSP_DemoteCoords,
    FIPS_FSP_TightenPathMtuLookup, FIPS_FSP_InitiateLookup,
    /* C ingress adapters, NOT proof of a completed Noise handshake. */
    FIPS_FSP_ReceiveSetup, FIPS_FSP_ReceiveAck, FIPS_FSP_ReceiveMsg3,
    FIPS_FSP_ReceiveEncrypted, FIPS_FSP_ReceiveRoutingSignal,
    FIPS_FSP_DropFrame, FIPS_FSP_DeliverSessionMessage
} fips_fsp_action_kind;
typedef struct fips_fsp_action {
    fips_fsp_action_kind kind;
    fips_node_addr dest; /* cache key / lookup dest / SessionDatagram src */
    fips_ipv6_address fips_addr; /* TightenPathMtuLookup only */
    uint16_t mtu;
    fips_fsp_bytes bytes; /* ingress: full frame; deliver: inner payload */
    fips_fsp_inner_header inner; /* DeliverSessionMessage only */
    fips_proto_error error; /* DropFrame only */
} fips_fsp_action;
/* Append atomically; NULL items only at capacity zero, len <= capacity.
 * RANGE preserves all actions; borrowed bytes must outlive action execution.
 * No I/O, crypto, wall clock, allocator or implicit retry inside the core. */
typedef struct fips_fsp_action_list { fips_fsp_action *items; size_t len, capacity; } fips_fsp_action_list;
FIPS_CORE_API fips_status fips_fsp_plan_coords_required_lookup(fips_node_addr dest, bool has_cached_identity, fips_fsp_action_list *actions);
FIPS_CORE_API fips_status fips_fsp_plan_path_broken(fips_node_addr dest, bool verified, fips_fsp_quorum_verdict quorum, fips_fsp_action_list *actions);
FIPS_CORE_API bool fips_fsp_should_apply_path_mtu(bool has_existing, uint16_t existing, uint16_t candidate);
FIPS_CORE_API fips_status fips_fsp_plan_path_mtu_tighten(fips_ipv6_address addr, bool has_existing, uint16_t existing, uint16_t candidate, fips_fsp_action_list *actions);
/* Borrowed pending packets, oldest first, caller storage. Positive capacity:
 * drop oldest at capacity, as push_bounded_pending; zero rejects unchanged
 * instead of Rust's surprising per_dest=0 queue of one. No hidden allocation. */
typedef struct fips_fsp_pending_queue { fips_fsp_bytes *items; size_t len, capacity; } fips_fsp_pending_queue;
FIPS_CORE_API fips_status fips_fsp_push_bounded_pending(fips_fsp_pending_queue *q, fips_fsp_bytes packet);
/* Endpoint-only ingress. Parses/checks envelopes before handing them to a
 * host. Transit forwarding must use the lenient wire decoders, not this gate.
 * RoutingSignal still needs its own strict routing-body decoder after dispatch.
 * Invalid peer bytes emit DropFrame; capacity refusal is a local RANGE. */
FIPS_CORE_API fips_status fips_fsp_receive(fips_node_addr src, const uint8_t *frame, size_t len, fips_fsp_action_list *actions);
/* Post-AEAD event: ONLY the host/rekey profile may call this after successful
 * authentication and replay admission, for current/previous/pending keys alike.
 * No short/reserved/unknown plaintext is delivered. DataPacket service dispatch
 * must additionally decode its port header. */
FIPS_CORE_API fips_status fips_fsp_receive_plaintext(fips_node_addr src, const uint8_t *plaintext, size_t len, fips_fsp_action_list *actions);
/* Named attachment slots:
 * - XK handshake (nostrc-pwo3h.5): ReceiveSetup/Ack/Msg3 carry opaque payloads;
 *   decode the strict envelope then pass handshake_payload to Noise XK.
 * - FspRekeyProfile (nostrc-f12v2.2): ReceiveEncrypted carries the checked full
 *   frame, including K, CP and raw 12-byte AAD. That profile owns keys, counters,
 *   replay commits, first-handshake/rekey resends, crossing-handshake policy,
 *   cutover/drain and authenticated effects. It must call receive_plaintext on
 *   ALL key paths. There is no fake successful rekey or handshake here.
 * FSP has no receiver_idx: dispatch by SessionDatagram src address; the shared
 * SessionIndex/IndexAllocator belongs to FMP and is not replicated here. */
#ifdef __cplusplus
}
#endif
#endif
