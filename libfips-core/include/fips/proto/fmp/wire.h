#ifndef FIPS_PROTO_FMP_WIRE_H
#define FIPS_PROTO_FMP_WIRE_H
#include <fips/proto/framing.h>
#include <fips/proto/link.h>
#include <fips/utils/index.h>
#ifdef __cplusplus
extern "C" {
#endif
#define FIPS_FMP_VERSION 0
#define FIPS_FMP_PHASE_ESTABLISHED 0
#define FIPS_FMP_PHASE_MSG1 1
#define FIPS_FMP_PHASE_MSG2 2
#define FIPS_FMP_FLAG_KEY_EPOCH 0x01
#define FIPS_FMP_FLAG_CE 0x02
#define FIPS_FMP_FLAG_SP 0x04

typedef enum fips_fmp_handshake_message_type {
    FIPS_FMP_NoiseIKMsg1 = 1, FIPS_FMP_NoiseIKMsg2 = 2
} fips_fmp_handshake_message_type;
typedef enum fips_fmp_disconnect_reason {
    FIPS_FMP_Shutdown, FIPS_FMP_Restart, FIPS_FMP_ProtocolError,
    FIPS_FMP_TransportFailure, FIPS_FMP_ResourceExhaustion,
    FIPS_FMP_SecurityViolation, FIPS_FMP_ConfigurationChange, FIPS_FMP_Timeout,
    FIPS_FMP_Other = 255
} fips_fmp_disconnect_reason;
typedef struct fips_fmp_disconnect { fips_fmp_disconnect_reason reason; } fips_fmp_disconnect;
typedef struct fips_fmp_common_prefix {
    uint8_t version, phase, flags;
    uint16_t payload_len;
} fips_fmp_common_prefix;
typedef struct fips_fmp_msg1_header {
    fips_session_index sender_idx;
    size_t noise_msg1_offset;
} fips_fmp_msg1_header;
typedef struct fips_fmp_msg2_header {
    fips_session_index sender_idx, receiver_idx;
    size_t noise_msg2_offset;
} fips_fmp_msg2_header;
typedef struct fips_fmp_encrypted_header {
    uint8_t flags;
    uint16_t payload_len;
    fips_session_index receiver_idx;
    uint64_t counter;
    uint8_t header_bytes[FIPS_FRAMING_FMP_ESTABLISHED_HEADER_SIZE];
} fips_fmp_encrypted_header;

/* Required pointers, nonoverlapping input/output, NULL input only at length 0.
 * Fallible calls preserve all outputs/cursors on failure (except optional error).
 * parse mirrors Rust's Option parsers: payload_len validation is separate.
 * validate_frame is the ingress gate, including the pending-session path: it
 * checks size, version, phase, flags and declared length before any dispatch. */
FIPS_CORE_API bool fips_fmp_handshake_message_type_from_byte(uint8_t byte, fips_fmp_handshake_message_type *out);
FIPS_CORE_API bool fips_fmp_is_handshake(uint8_t byte);
FIPS_CORE_API const char *fips_fmp_handshake_message_type_name(fips_fmp_handshake_message_type type);
FIPS_CORE_API bool fips_fmp_disconnect_reason_from_byte(uint8_t byte, fips_fmp_disconnect_reason *out);
FIPS_CORE_API const char *fips_fmp_disconnect_reason_name(fips_fmp_disconnect_reason reason);
FIPS_CORE_API fips_status fips_fmp_disconnect_encode(const fips_fmp_disconnect *d, fips_writer *w, fips_proto_error *error);
/* Body after the type byte; unknown reasons become Other; trailing bytes ignored. */
FIPS_CORE_API fips_status fips_fmp_disconnect_decode(fips_reader *r, fips_fmp_disconnect *out, fips_proto_error *error);
FIPS_CORE_API bool fips_fmp_common_prefix_parse(const uint8_t *data, size_t len, fips_fmp_common_prefix *out);
FIPS_CORE_API bool fips_fmp_msg1_header_parse(const uint8_t *data, size_t len, fips_fmp_msg1_header *out);
FIPS_CORE_API bool fips_fmp_msg2_header_parse(const uint8_t *data, size_t len, fips_fmp_msg2_header *out);
FIPS_CORE_API bool fips_fmp_encrypted_header_parse(const uint8_t *data, size_t len, fips_fmp_encrypted_header *out);
FIPS_CORE_API bool fips_fmp_expected_payload_len(uint8_t phase, size_t total, uint16_t *out);
FIPS_CORE_API fips_status fips_fmp_validate_frame(const uint8_t *data, size_t len, fips_proto_error *error);
FIPS_CORE_API fips_status fips_fmp_build_msg1(fips_session_index sender, const uint8_t *noise, size_t len, fips_writer *w, fips_proto_error *error);
FIPS_CORE_API fips_status fips_fmp_build_msg2(fips_session_index sender, fips_session_index receiver, const uint8_t *noise, size_t len, fips_writer *w, fips_proto_error *error);
FIPS_CORE_API void fips_fmp_build_established_header(fips_session_index receiver, uint64_t counter, uint8_t flags, uint16_t payload_len, uint8_t out[FIPS_FRAMING_FMP_ESTABLISHED_HEADER_SIZE]);
/* Assembly only, like Rust: ciphertext (including tag) is opaque. Validates the
 * resulting envelope, not authentication. AAD is exactly the supplied header. */
FIPS_CORE_API fips_status fips_fmp_build_encrypted(const uint8_t header[FIPS_FRAMING_FMP_ESTABLISHED_HEADER_SIZE], const uint8_t *ciphertext, size_t len, fips_writer *w, fips_proto_error *error);
FIPS_CORE_API fips_status fips_fmp_prepend_inner_header(uint32_t timestamp_ms, const uint8_t *plaintext, size_t len, fips_writer *w, fips_proto_error *error);
FIPS_CORE_API bool fips_fmp_strip_inner_header(const uint8_t *plaintext, size_t len, uint32_t *timestamp_ms, const uint8_t **payload, size_t *payload_len);
#ifdef __cplusplus
}
#endif
#endif
