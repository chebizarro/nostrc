#ifndef FIPSD_TUN_ROUTE_H
#define FIPSD_TUN_ROUTE_H
#include "tun/tun.h"
/* Explicit little-endian host wire layouts: Linux UAPI netlink and the
 * 64-bit XNU/FreeBSD routing ABI. No struct casts/alignment assumptions. */
typedef enum fipsd_tun_netlink_kind {
    FIPSD_TUN_NL_ADDRESS_ADD, FIPSD_TUN_NL_ADDRESS_DEL,
    FIPSD_TUN_NL_MTU, FIPSD_TUN_NL_UP, FIPSD_TUN_NL_DOWN,
    FIPSD_TUN_NL_ROUTE_ADD, FIPSD_TUN_NL_ROUTE_DEL
} fipsd_tun_netlink_kind;
size_t fipsd_tun_netlink_message(uint8_t *, size_t, fipsd_tun_netlink_kind,
    uint32_t seq, uint32_t index, const uint8_t address[16], uint16_t mtu);
size_t fipsd_tun_bsd_message(uint8_t *, size_t, fipsd_tun_os, uint8_t type,
    uint32_t seq, uint32_t pid, uint32_t index);
/* 0: unrelated; 1: matching success; -1: matching error or malformed message.
 * ESRCH/ENOENT is an error, never mistaken for another request's absence. */
int fipsd_tun_route_reply(const uint8_t *, size_t, fipsd_tun_os,
    uint8_t type, uint32_t seq, uint32_t pid, uint32_t *index);
uint32_t fipsd_tun_route_sequence(void);
int fipsd_tun_route_exchange(fipsd_tun *, const uint8_t *, size_t,
    uint8_t type, uint32_t seq, uint32_t *index);
#endif
