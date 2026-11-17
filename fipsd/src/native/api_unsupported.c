/* The native datagram API is an AF_UNIX SOCK_SEQPACKET server handing out
 * descriptors with SCM_RIGHTS, neither of which this platform has;
 * node.native_api.enabled fails start-up. */
#include "native/api.h"
#include "log.h"
#include <errno.h>
fipsd_native_api *fipsd_native_api_create(GoSwap *configs, fipsd_node *node, fipsd_node_handlers *handlers)
{
    (void)configs; (void)node; (void)handlers;
    FIPSD_ERROR("native", "node.native_api.enabled: the native datagram API is not available on this platform");
    errno = ENOTSUP;
    return NULL;
}
fipsd_subsystem *fipsd_native_api_subsystem(fipsd_native_api *api) { (void)api; return NULL; }
fipsd_control_provider fipsd_native_api_provider(fipsd_native_api *api, fipsd_control_provider base)
{ (void)api; return base; }
void fipsd_native_api_destroy(fipsd_native_api *api) { (void)api; }
