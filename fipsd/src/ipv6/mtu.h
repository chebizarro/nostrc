#ifndef FIPSD_IPV6_MTU_H
#define FIPSD_IPV6_MTU_H
/* The IPv6 shim's MTU arithmetic (upstream ipv6tun/mtu.rs): how large an IPv6
 * packet, and how large a TCP segment, a FIPS path carries. Two inputs, one
 * set of constants: the transport form starts from a transport MTU, the
 * payload-limit form from the FSP payload limit the node publishes. They agree
 * from a transport MTU of 110 up; below it the transport form saturates to
 * zero while the payload-limit form gives 33. Neither admits a 40-byte header. */
#include <stdint.h>
#include "tun/tun.h"

#define FIPSD_IPV6_HEADER_SIZE 40u
#define FIPSD_IPV6_SHIM_RESIDUAL_SIZE 6u
/* How many bytes shorter a packet is once compressed: the 40-byte header
 * becomes the format byte and the six residual bytes. */
#define FIPSD_IPV6_HEADER_SAVING (FIPSD_IPV6_HEADER_SIZE - 1u - FIPSD_IPV6_SHIM_RESIDUAL_SIZE)
/* FIPS_OVERHEAD(106) + FSP port header(4) - HEADER_SAVING(33): the shim's
 * 77 bytes, NOT core FIPS_OVERHEAD (106/110). mtu.c asserts both identities. */
#define FIPSD_IPV6_OVERHEAD 77u
#define FIPSD_IPV6_MIN_MTU 1280u
#define FIPSD_IPV6_TCP_HEADER 20u

uint16_t fipsd_ipv6_effective_mtu(uint16_t transport_mtu);   /* transport - 77, saturating */
uint16_t fipsd_ipv6_mss_ceiling(uint16_t transport_mtu);     /* effective - 40 - 20, saturating */
uint16_t fipsd_ipv6_mtu_of_limit(uint16_t payload_limit);    /* limit + 33, saturating */
uint16_t fipsd_ipv6_tcp_mss(uint16_t payload_limit);         /* mtu_of_limit - 40 - 20, saturating */
#endif
