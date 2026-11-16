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
} fipsd_tun_service;
void fipsd_tun_service_init(fipsd_tun_service *,fipsd_tun *,fipsd_node *);
/* Minimum configured transport payload MTU; no path learning in device owner. */
uint32_t fipsd_tun_transport_mtu(const fipsd_config *);
#endif
