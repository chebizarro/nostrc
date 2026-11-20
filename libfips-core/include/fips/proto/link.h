#ifndef FIPS_PROTO_LINK_H
#define FIPS_PROTO_LINK_H

#include <fips/identity/mod.h>
#include <fips/proto/codec.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Link-layer message catalog: core/src/proto/link.rs LinkMessageType at the
 * pin. Enumerator suffixes are the Rust variant names; values are the wire
 * bytes. Gaps (0x03-0x0F, 0x11-0x1F, ...) are invalid message types. */
typedef enum fips_link_message_type {
    FIPS_LINK_SessionDatagram = 0x00,
    FIPS_LINK_SenderReport = 0x01,
    FIPS_LINK_ReceiverReport = 0x02,
    FIPS_LINK_TreeAnnounce = 0x10,
    FIPS_LINK_FilterAnnounce = 0x20,
    FIPS_LINK_LookupRequest = 0x30,
    FIPS_LINK_LookupResponse = 0x31,
    FIPS_LINK_Disconnect = 0x50,
    FIPS_LINK_Heartbeat = 0x51,
    FIPS_LINK_PathProbe = 0x52,
    FIPS_LINK_PathAck = 0x53,
    FIPS_LINK_PathClose = 0x54
} fips_link_message_type;
#define FIPS_LINK_MESSAGE_TYPE_COUNT 12

/* Session-layer inner-header msg_type catalog: core/src/proto/fsp/wire.rs
 * SessionMessageType at the pin. FSP handshake messages are dispatched by the
 * phase nibble, not a type byte, so 0x00-0x0F is unallocated here. The FSP
 * frame codec itself is nostrc-82g9n.8; only the catalog lives here. */
typedef enum fips_session_message_type {
    FIPS_SESSION_DataPacket = 0x10,
    FIPS_SESSION_SenderReport = 0x11,
    FIPS_SESSION_ReceiverReport = 0x12,
    FIPS_SESSION_PathMtuNotification = 0x13,
    FIPS_SESSION_CoordsWarmup = 0x14
} fips_session_message_type;
#define FIPS_SESSION_MESSAGE_TYPE_COUNT 5

/** Rust from_byte: None becomes FIPS_ERR_RANGE with InvalidMessageType{byte};
 * out is unchanged. Success leaves error untouched. */
FIPS_CORE_API fips_status fips_link_message_type_from_u8(uint8_t byte,
    fips_link_message_type *out, fips_proto_error *error);
FIPS_CORE_API fips_status fips_session_message_type_from_u8(uint8_t byte,
    fips_session_message_type *out, fips_proto_error *error);
/** Static Rust Display/Debug name; "Unknown" for a value outside the catalog. */
FIPS_CORE_API const char *fips_link_message_type_name(fips_link_message_type type);
FIPS_CORE_API const char *fips_session_message_type_name(fips_session_message_type type);

/* SessionDatagram (link.rs): msg_type(1) ttl(1) path_mtu(2 LE) src(16) dest(16)
 * then the opaque session payload. decode() consumes the body after the
 * msg_type byte, as upstream does, so it requires BODY_HEADER_SIZE bytes. */
#define FIPS_SESSION_DATAGRAM_HEADER_SIZE 36
#define FIPS_SESSION_DATAGRAM_BODY_HEADER_SIZE 35
#define FIPS_SESSION_DATAGRAM_DEFAULT_TTL 64
#define FIPS_SESSION_DATAGRAM_DEFAULT_PATH_MTU UINT16_MAX

/** Rust SessionDatagramRef: the payload is borrowed from the decoded input's
 * backing buffer or from the caller, never copied or owned (no Vec equivalent).
 * payload must reference payload_len readable bytes and may be NULL only when
 * payload_len is 0; it must not overlap an encode destination. */
typedef struct fips_session_datagram {
    fips_node_addr src_addr, dest_addr;
    uint8_t ttl;
    uint16_t path_mtu;
    const uint8_t *payload;
    size_t payload_len;
} fips_session_datagram;

/** SessionDatagram::new: ttl 64, path_mtu u16::MAX, borrowed payload. */
FIPS_CORE_API void fips_session_datagram_init(fips_session_datagram *d,
    fips_node_addr src_addr, fips_node_addr dest_addr,
    const uint8_t *payload, size_t payload_len);
/** ttl_after_hop: decrement first with saturation; false (out unchanged) when
 * the datagram would leave with zero, i.e. for arrivals at TTL 0 and 1. */
FIPS_CORE_API bool fips_ttl_after_hop(uint8_t ttl, uint8_t *out);
/** decrement_ttl: saturating decrement in place; true if still transmittable.
 * Governs forwarding only; local delivery is never TTL-gated. */
FIPS_CORE_API bool fips_session_datagram_decrement_ttl(fips_session_datagram *d);
/** can_forward: true only at TTL 2 or more. */
FIPS_CORE_API bool fips_session_datagram_can_forward(const fips_session_datagram *d);
/** HEADER_SIZE + payload_len, saturating at SIZE_MAX. */
FIPS_CORE_API size_t fips_session_datagram_encoded_len(const fips_session_datagram *d);
/** SessionDatagram::encode into a caller-sized writer: the msg_type byte (which
 * decode does NOT consume), the 35-byte body header and the payload. Capacity is checked up front, so a
 * MessageTooLong {max: capacity, got: len + encoded_len} failure writes
 * nothing (got saturates at SIZE_MAX). */
FIPS_CORE_API fips_status fips_session_datagram_encode(const fips_session_datagram *d,
    fips_writer *w, fips_proto_error *error);
/** SessionDatagramRef::decode of the body after the msg_type byte. Requires 35
 * bytes, then takes the whole remaining input as the borrowed payload and
 * leaves the reader at its end, so FMP/FSP callers must bound the reader to one
 * datagram rather than a concatenation. A reader positioned at the body start reports
 * Rust's MessageTooShort {expected: 35, got: len}; per the codec convention a
 * later position adds itself to expected. Failure leaves reader and out
 * unchanged. */
FIPS_CORE_API fips_status fips_session_datagram_decode(fips_reader *r,
    fips_session_datagram *out, fips_proto_error *error);

#ifdef __cplusplus
}
#endif
#endif /* FIPS_PROTO_LINK_H */
