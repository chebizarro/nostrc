#ifndef FIPS_PROTO_FSP_WIRE_H
#define FIPS_PROTO_FSP_WIRE_H
#include <fips/proto/framing.h>
#include <fips/proto/link.h>
#ifdef __cplusplus
extern "C" {
#endif
#define FIPS_FSP_VERSION 0
#define FIPS_FSP_PHASE_ESTABLISHED 0
#define FIPS_FSP_PHASE_MSG1 1
#define FIPS_FSP_PHASE_MSG2 2
#define FIPS_FSP_PHASE_MSG3 3
#define FIPS_FSP_PORT_IPV6_SHIM 256
#define FIPS_FSP_FLAG_CP 0x01
#define FIPS_FSP_FLAG_K 0x02
#define FIPS_FSP_FLAG_U 0x04
#define FIPS_FSP_FLAGS_RESERVED 0xf8
#define FIPS_FSP_INNER_FLAG_SP 0x01
#define FIPS_FSP_INNER_FLAGS_RESERVED 0xfe
/* SessionMessageType is the single catalog in proto/link.h. */
typedef struct fips_fsp_session_flags { uint8_t bits; } fips_fsp_session_flags;
typedef struct fips_fsp_flags { bool coords_present, key_epoch, unencrypted; } fips_fsp_flags;
typedef struct fips_fsp_inner_flags { bool spin_bit; } fips_fsp_inner_flags;
FIPS_CORE_API fips_fsp_session_flags fips_fsp_session_flags_new(void);
FIPS_CORE_API uint8_t fips_fsp_session_flags_to_byte(fips_fsp_session_flags flags);
FIPS_CORE_API fips_fsp_session_flags fips_fsp_session_flags_from_byte(uint8_t byte);
FIPS_CORE_API uint8_t fips_fsp_flags_to_byte(fips_fsp_flags flags);
FIPS_CORE_API fips_fsp_flags fips_fsp_flags_from_byte(uint8_t byte);
FIPS_CORE_API uint8_t fips_fsp_inner_flags_to_byte(fips_fsp_inner_flags flags);
FIPS_CORE_API fips_fsp_inner_flags fips_fsp_inner_flags_from_byte(uint8_t byte);
/* Address-only TreeCoordinate wire view: count consecutive 16-byte NodeAddrs,
 * self -> root. No metadata, allocation or guessed depth cap. Input count may
 * be zero only for optional encrypted coords; setup/ack require nonempty paths.
 * Views/Noise/ciphertext borrow caller bytes and must outlive their use. */
typedef struct fips_fsp_coords { const uint8_t *addrs; size_t count; } fips_fsp_coords;
typedef struct fips_fsp_bytes { const uint8_t *data; size_t len; } fips_fsp_bytes;
typedef struct fips_fsp_session_setup {
    fips_fsp_coords src_coords, dest_coords;
    fips_fsp_session_flags flags;
    fips_fsp_bytes handshake_payload;
} fips_fsp_session_setup;
typedef struct fips_fsp_session_ack {
    fips_fsp_coords src_coords, dest_coords;
    uint8_t flags;
    fips_fsp_bytes handshake_payload;
} fips_fsp_session_ack;
typedef struct fips_fsp_session_msg3 { uint8_t flags; fips_fsp_bytes handshake_payload; } fips_fsp_session_msg3;
typedef struct fips_fsp_common_prefix { uint8_t version, phase, flags; uint16_t payload_len; } fips_fsp_common_prefix;
typedef struct fips_fsp_encrypted_header {
    uint8_t flags;
    uint16_t payload_len;
    uint64_t counter;
    uint8_t header_bytes[FIPS_FRAMING_FSP_HEADER_SIZE];
} fips_fsp_encrypted_header;
typedef struct fips_fsp_inner_header { uint32_t timestamp_ms; uint8_t msg_type, inner_flags; } fips_fsp_inner_header;
typedef struct fips_fsp_port_header { uint16_t src_port, dst_port; } fips_fsp_port_header;
/* Required pointers valid, NULL bytes only at length zero; no overlapping
 * input/output. Fallible calls preserve outputs/cursors on failure; optional
 * error is unchanged on success. Decode consumes a BODY after the prefix.
 * Lenient transit decoders preserve reserved flags and leave trailing bytes.
 * Strict endpoint decoders require flags=0 and consume the entire body.
 * Encode includes the prefix and rejects u16 length truncation. */
FIPS_CORE_API fips_status fips_fsp_session_setup_encode(const fips_fsp_session_setup *m, fips_writer *w, fips_proto_error *e);
FIPS_CORE_API fips_status fips_fsp_session_setup_decode(fips_reader *r, fips_fsp_session_setup *out, fips_proto_error *e);
FIPS_CORE_API fips_status fips_fsp_session_setup_decode_strict(fips_reader *r, fips_fsp_session_setup *out, fips_proto_error *e);
FIPS_CORE_API fips_status fips_fsp_session_ack_encode(const fips_fsp_session_ack *m, fips_writer *w, fips_proto_error *e);
FIPS_CORE_API fips_status fips_fsp_session_ack_decode(fips_reader *r, fips_fsp_session_ack *out, fips_proto_error *e);
FIPS_CORE_API fips_status fips_fsp_session_ack_decode_strict(fips_reader *r, fips_fsp_session_ack *out, fips_proto_error *e);
FIPS_CORE_API fips_status fips_fsp_session_msg3_encode(const fips_fsp_session_msg3 *m, fips_writer *w, fips_proto_error *e);
FIPS_CORE_API fips_status fips_fsp_session_msg3_decode(fips_reader *r, fips_fsp_session_msg3 *out, fips_proto_error *e);
FIPS_CORE_API fips_status fips_fsp_session_msg3_decode_strict(fips_reader *r, fips_fsp_session_msg3 *out, fips_proto_error *e);
FIPS_CORE_API bool fips_fsp_common_prefix_parse(const uint8_t *data, size_t len, fips_fsp_common_prefix *out);
FIPS_CORE_API bool fips_fsp_common_prefix_is_unencrypted(const fips_fsp_common_prefix *p);
FIPS_CORE_API bool fips_fsp_common_prefix_has_coords(const fips_fsp_common_prefix *p);
FIPS_CORE_API bool fips_fsp_encrypted_header_parse(const uint8_t *data, size_t len, fips_fsp_encrypted_header *out);
FIPS_CORE_API bool fips_fsp_encrypted_header_has_coords(const fips_fsp_encrypted_header *h);
FIPS_CORE_API size_t fips_fsp_encrypted_header_data_offset(const fips_fsp_encrypted_header *h);
FIPS_CORE_API void fips_fsp_build_header(uint64_t counter, uint8_t flags, uint16_t payload_len, uint8_t out[FIPS_FRAMING_FSP_HEADER_SIZE]);
/* Assembly only, exactly as Rust: ciphertext+tag (and CP coords, if present)
 * are opaque. Endpoint validation is separate, never implicit authentication. */
FIPS_CORE_API fips_status fips_fsp_build_encrypted(const uint8_t header[FIPS_FRAMING_FSP_HEADER_SIZE], const uint8_t *ciphertext, size_t len, fips_writer *w, fips_proto_error *e);
FIPS_CORE_API void fips_fsp_build_handshake_prefix(uint8_t phase, uint16_t payload_len, uint8_t out[FIPS_FRAMING_FSP_COMMON_PREFIX_SIZE]);
FIPS_CORE_API void fips_fsp_build_error_prefix(uint16_t payload_len, uint8_t out[FIPS_FRAMING_FSP_COMMON_PREFIX_SIZE]);
FIPS_CORE_API fips_status fips_fsp_prepend_inner_header(uint32_t timestamp_ms, uint8_t msg_type, uint8_t inner_flags, const uint8_t *payload, size_t len, fips_writer *w, fips_proto_error *e);
FIPS_CORE_API bool fips_fsp_strip_inner_header(const uint8_t *data, size_t len, fips_fsp_inner_header *out, fips_fsp_bytes *payload);
/* r begins AFTER the 12-byte cleartext header. Zero count means absent. */
FIPS_CORE_API fips_status fips_fsp_parse_encrypted_coords(fips_reader *r, fips_fsp_coords *src, fips_fsp_coords *dest, fips_proto_error *e);
FIPS_CORE_API fips_status fips_fsp_check_prefix(const fips_fsp_common_prefix *p, size_t rest, fips_proto_error *e);
FIPS_CORE_API fips_status fips_fsp_check_header(const fips_fsp_encrypted_header *h, fips_proto_error *e);
FIPS_CORE_API fips_status fips_fsp_check_sealed(const fips_fsp_encrypted_header *h, size_t sealed, fips_proto_error *e);
FIPS_CORE_API fips_status fips_fsp_check_inner(uint8_t inner_flags, fips_proto_error *e);
/* Service header in a DataPacket payload, not in any other message. */
FIPS_CORE_API fips_status fips_fsp_port_header_encode(fips_fsp_port_header ports, fips_writer *w, fips_proto_error *e);
FIPS_CORE_API fips_status fips_fsp_port_header_decode(fips_reader *r, fips_fsp_port_header *out, fips_proto_error *e);
#ifdef __cplusplus
}
#endif
#endif
