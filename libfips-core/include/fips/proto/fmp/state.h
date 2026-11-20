#ifndef FIPS_PROTO_FMP_STATE_H
#define FIPS_PROTO_FMP_STATE_H
#include <fips/proto/fmp/core.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct fips_fmp_leg_stamp { uint64_t wall_ms; fips_instant interval; } fips_fmp_leg_stamp;
/* Pure bookkeeping only: handshake phase/crypto and index allocator ownership
 * remain with the host. All public fields are plain values, except active_at's
 * borrowed clock ops. The host may update identity/epoch/transport/stats fields.
 * Index set/take return the previous value for explicit host release/transfer;
 * this is the slot for the future rekey IndexLease, not an owning C copy. */
typedef struct fips_fmp_connection_state {
    fips_link_id link_id;
    fips_link_direction direction;
    bool has_expected_identity;
    fips_peer_identity expected_identity;
    uint64_t started_at, last_activity;
    fips_instant active_at;
    fips_link_stats link_stats;
    bool has_our_index, has_their_index, has_transport_id, has_source_addr;
    fips_session_index our_index, their_index;
    fips_transport_id transport_id;
    fips_transport_addr source_addr;
    fips_fmp_epoch remote_epoch;
    bool has_handshake_msg1, has_handshake_msg2, has_msg1_digest;
    uint8_t handshake_msg1[FIPS_FRAMING_FMP_MSG1_WIRE_SIZE];
    uint8_t handshake_msg2[FIPS_FRAMING_FMP_MSG2_WIRE_SIZE];
    size_t handshake_msg1_len, handshake_msg2_len;
    fips_fmp_msg1_digest msg1_digest;
    uint32_t resend_count;
} fips_fmp_connection_state;
FIPS_CORE_API void fips_fmp_connection_inbound(fips_fmp_connection_state *s, fips_link_id link, fips_fmp_leg_stamp at);
FIPS_CORE_API void fips_fmp_connection_outbound(fips_fmp_connection_state *s, fips_link_id link, fips_peer_identity identity, fips_fmp_leg_stamp at);
FIPS_CORE_API void fips_fmp_connection_inbound_with_transport(fips_fmp_connection_state *s, fips_link_id link, fips_transport_id transport, fips_transport_addr source, fips_fmp_leg_stamp at);
FIPS_CORE_API bool fips_fmp_connection_set_our_index(fips_fmp_connection_state *s, fips_session_index index, fips_session_index *previous);
FIPS_CORE_API bool fips_fmp_connection_take_our_index(fips_fmp_connection_state *s, fips_session_index *out);
FIPS_CORE_API fips_status fips_fmp_connection_set_handshake_msg1(fips_fmp_connection_state *s, const uint8_t *bytes, size_t len);
FIPS_CORE_API fips_status fips_fmp_connection_set_handshake_msg2(fips_fmp_connection_state *s, const uint8_t *bytes, size_t len);
FIPS_CORE_API void fips_fmp_connection_record_resend(fips_fmp_connection_state *s);
FIPS_CORE_API void fips_fmp_connection_touch(fips_fmp_connection_state *s, fips_fmp_leg_stamp at);
FIPS_CORE_API uint64_t fips_fmp_connection_duration(const fips_fmp_connection_state *s, uint64_t wall_ms);
FIPS_CORE_API uint64_t fips_fmp_connection_idle_time(const fips_fmp_connection_state *s, fips_instant now);
FIPS_CORE_API bool fips_fmp_connection_is_timed_out(const fips_fmp_connection_state *s, fips_instant now, uint64_t timeout_ms);
#ifdef __cplusplus
}
#endif
#endif
