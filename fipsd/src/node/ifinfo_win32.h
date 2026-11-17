/* IP Helper queries for the Windows build. They live in one translation unit
 * (ifinfo_win32.c): <iphlpapi.h>/<netioapi.h> include ole2.h, whose
 * `interface`/`small` macros and wincrypt's CMSG_DATA collide with fipsd and
 * libgo names everywhere else. */
#ifndef FIPSD_NODE_IFINFO_WIN32_H
#define FIPSD_NODE_IFINFO_WIN32_H
#if defined(_WIN32)
#include <stdbool.h>
#include <stdint.h>
struct fipsd_network_snapshot;

/* IANA ifType values the daemon distinguishes. */
#define FIPSD_WIN_IF_ETHERNET 6u
#define FIPSD_WIN_IF_LOOPBACK 24u
#define FIPSD_WIN_IF_IEEE80211 71u

typedef struct fipsd_win_ifrow {
    bool exists, admin_up, connected, hardware, has_mac;
    uint32_t index, type;
    uint8_t mac[6];
} fipsd_win_ifrow;
/* The interface row for an if_nametoindex() name; exists=false if absent. */
void fipsd_win_ifrow_read(const char *name, fipsd_win_ifrow *out);
/* Interfaces (if_indextoname() names) and unicast addresses with their
 * on-link prefix lengths; false (snapshot partially filled) on failure. */
bool fipsd_win_network_read(struct fipsd_network_snapshot *s);
#endif
#endif
