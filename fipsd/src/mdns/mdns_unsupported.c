/* mDNS/DNS-SD LAN rendezvous needs multicast interface enumeration that is
 * not yet ported to this platform; node.rendezvous.lan.enabled fails start-up. */
#include "mdns/mdns.h"
#include "log.h"
#include <errno.h>
fipsd_mdns_service *fipsd_mdns_service_create(GoSwap *configs, fipsd_node *node, fipsd_node_handlers *handlers)
{
    (void)configs; (void)node; (void)handlers;
    FIPSD_ERROR("mdns", "node.rendezvous.lan.enabled: mDNS LAN rendezvous is not available on this platform yet");
    errno = ENOTSUP;
    return NULL;
}
fipsd_subsystem *fipsd_mdns_service_subsystem(fipsd_mdns_service *s) { (void)s; return NULL; }
void fipsd_mdns_service_destroy(fipsd_mdns_service *s) { (void)s; }
