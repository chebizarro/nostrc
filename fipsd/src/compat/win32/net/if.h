/* Windows stand-in for <net/if.h> (see README.md): name-size constants only.
 * IP Helper itself (if_nametoindex, GetIfEntry2, GetAdaptersAddresses) is
 * confined to node/ifinfo_win32.c, because its headers include ole2.h. */
#ifndef FIPSD_COMPAT_NET_IF_H
#define FIPSD_COMPAT_NET_IF_H
#include "fipsd_winsock.h"
#ifndef IF_NAMESIZE
#define IF_NAMESIZE 256 /* NDIS_IF_MAX_STRING_SIZE */
#endif
#ifndef IFNAMSIZ
#define IFNAMSIZ IF_NAMESIZE
#endif
#endif
