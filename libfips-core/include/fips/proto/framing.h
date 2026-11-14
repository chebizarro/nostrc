#ifndef FIPS_PROTO_FRAMING_H
#define FIPS_PROTO_FRAMING_H

#include <fips/host.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Encapsulation overhead spanning the FMP and FSP layers (core/src/proto/
 * framing.rs). The sizes below are defined by value with their upstream source
 * at the pin; the FMP/FSP codecs (nostrc-82g9n.7/.8) and the Noise port must
 * agree with them, and framing.c pins every derived figure with static asserts.
 *
 *   FMP outer header (cleartext AAD)              16   ESTABLISHED_HEADER_SIZE
 *   FMP AEAD ciphertext:
 *     timestamp (4) + msg_type (1)                 5   FMP INNER_HEADER_SIZE
 *     ttl (1) + path_mtu (2) + src (16) + dst (16) 35  SessionDatagram body
 *     FSP header (4 prefix + 8 counter)            12  FSP_HEADER_SIZE
 *     FSP AEAD ciphertext:
 *       timestamp (4) + msg_type (1) + flags (1)    6  FSP_INNER_HEADER_SIZE
 *       <application data>
 *       Poly1305 tag                               16  FSP TAG_SIZE
 *   FMP Poly1305 tag                              16   noise TAG_SIZE
 *                                                ────
 *                                                 106  FIPS_OVERHEAD
 *
 * The SessionDatagram msg_type byte is the last byte of the FMP inner header
 * and is counted once. FIPS_OVERHEAD excludes the 4-byte DataPacket port
 * header (110 with it). The 77-byte FIPS_IPV6_OVERHEAD (110 minus 33 bytes of
 * IPv6 header compression) belongs to the host IPv6 shim (src/ipv6tun/mtu.rs),
 * not to fips-core, and is deliberately not defined here. */

/* core/src/noise/mod.rs */
#define FIPS_FRAMING_NOISE_TAG_SIZE 16
#define FIPS_FRAMING_NOISE_PUBKEY_SIZE 33
#define FIPS_FRAMING_NOISE_EPOCH_SIZE 8
#define FIPS_FRAMING_NOISE_EPOCH_ENCRYPTED_SIZE 24  /* EPOCH_SIZE + TAG_SIZE */
#define FIPS_FRAMING_NOISE_HANDSHAKE_MSG1_SIZE 106  /* 33 + 33 + 16 + 24 */
#define FIPS_FRAMING_NOISE_HANDSHAKE_MSG2_SIZE 57   /* 33 + 24 */
/* core/src/proto/fmp/wire.rs */
#define FIPS_FRAMING_FMP_COMMON_PREFIX_SIZE 4
#define FIPS_FRAMING_FMP_ESTABLISHED_HEADER_SIZE 16
#define FIPS_FRAMING_FMP_INNER_HEADER_SIZE 5
#define FIPS_FRAMING_FMP_MSG1_WIRE_SIZE 114   /* prefix + sender_idx + msg1 */
#define FIPS_FRAMING_FMP_MSG2_WIRE_SIZE 69    /* prefix + sender_idx + receiver_idx + msg2 */
#define FIPS_FRAMING_FMP_ENCRYPTED_MIN_SIZE 32 /* established header + tag */
/** Bytes an established frame adds around a link message counted from its type
 * byte: header + inner timestamp (inner header less the shared type byte) + tag. */
#define FIPS_FRAMING_FMP_FRAME_OVERHEAD 36
/* core/src/proto/fsp/wire.rs */
#define FIPS_FRAMING_FSP_COMMON_PREFIX_SIZE 4
#define FIPS_FRAMING_FSP_HEADER_SIZE 12
#define FIPS_FRAMING_FSP_INNER_HEADER_SIZE 6
#define FIPS_FRAMING_FSP_TAG_SIZE 16
#define FIPS_FRAMING_FSP_ENCRYPTED_MIN_SIZE 28 /* header + tag */
#define FIPS_FRAMING_FSP_PORT_HEADER_SIZE 4    /* src_port + dst_port */

/** core/src/proto/framing.rs FIPS_OVERHEAD (u16): the fixed cost of a
 * SessionDatagram carrying an FSP DataPacket, excluding the port header. */
#define FIPS_OVERHEAD ((uint16_t)106)

/** framing::payload_limit: the largest FSP payload a path MTU admits once FMP
 * and FSP framing and the port header are paid, as
 * mtu.saturating_sub(FIPS_OVERHEAD).saturating_sub(FSP_PORT_HEADER_SIZE):
 * zero for every MTU up to 110, mtu - 110 above it (1280 -> 1170). */
FIPS_CORE_API uint16_t fips_framing_payload_limit(uint16_t path_mtu);

#ifdef __cplusplus
}
#endif
#endif /* FIPS_PROTO_FRAMING_H */
