#ifndef FIPSD_TUN_LIFECYCLE_H
#define FIPSD_TUN_LIFECYCLE_H
#include "tun/tun.h"
#include "node/context.h"
#include "config/config.h"
/* Register after node, before control. Runtime start has crossed the node
 * startup barrier; a failed/not-ready node never opens a TUN. Failure remains
 * control-visible but does not bring down the host, matching upstream. */
typedef struct fipsd_tun_service {
    fipsd_subsystem subsystem;
    fipsd_tun *device;
    fipsd_node *node;
    struct fipsd_ipv6_shim *shim; /* optional packet adapter, started after the device is up */
} fipsd_tun_service;
void fipsd_tun_service_init(fipsd_tun_service *,fipsd_tun *,fipsd_node *);
/* Before the runtime starts. The shim is started once the device is up and
 * stopped (its I/O fibers cancelled and joined) before the device is torn down. */
void fipsd_tun_service_set_shim(fipsd_tun_service *,struct fipsd_ipv6_shim *);
/* Minimum configured transport payload MTU; no path learning in device owner. */
uint32_t fipsd_tun_transport_mtu(const fipsd_config *);
#endif
