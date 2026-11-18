/* MIB_* rows for tun/winnet.h's plain rows (Windows). Includes IP Helper, so
 * only tun/winnet_win32.c and its row test include it: <iphlpapi.h> and
 * <netioapi.h> pull in ole2.h, whose macros collide with fipsd and libgo
 * names (see node/ifinfo_win32.h). */
#ifndef FIPSD_TUN_WINNET_ROWS_H
#define FIPSD_TUN_WINNET_ROWS_H
#if defined(_WIN32)
#include "tun/winnet.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
/* InitializeUnicastIpAddressEntry, then the row's address, interface,
 * on-link prefix length and manual origins (winnet.rs address_row). */
void fipsd_winnet_address_mib(const fipsd_winnet_address *row, MIB_UNICASTIPADDRESS_ROW *out);
/* InitializeIpForwardEntry, then interface, destination prefix, the
 * unspecified next hop (on-link), metric and protocol (winnet.rs route_row). */
void fipsd_winnet_route_mib(const fipsd_winnet_route *row, MIB_IPFORWARD_ROW2 *out);
#endif
#endif
