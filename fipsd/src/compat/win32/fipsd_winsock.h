/* Shared body of the Windows socket-header stand-ins (see README.md). */
#ifndef FIPSD_COMPAT_WINSOCK_H
#define FIPSD_COMPAT_WINSOCK_H
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include "libgo/net_compat.h"
#ifndef SHUT_RD
#define SHUT_RD   SD_RECEIVE
#define SHUT_WR   SD_SEND
#define SHUT_RDWR SD_BOTH
#endif
typedef ADDRESS_FAMILY sa_family_t;
typedef USHORT in_port_t;
typedef ULONG in_addr_t;
#endif
