/* The IPv6 packet adapter rides on a TUN device; without a TUN backend
 * (fipsd_tun_create fails first) there is nothing to adapt. */
#include "ipv6/shim.h"
#include <errno.h>
fipsd_ipv6_shim *fipsd_ipv6_shim_create(const fipsd_ipv6_shim_config *config)
{ (void)config; errno = ENOTSUP; return NULL; }
void fipsd_ipv6_shim_destroy(fipsd_ipv6_shim *shim) { (void)shim; }
