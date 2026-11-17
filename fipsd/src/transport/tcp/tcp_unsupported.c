/* The TCP stream transport is not yet ported to this platform (libgo's
 * ConnectEx/AcceptEx path exists; the listener/pool owners still use POSIX
 * descriptors). A configured transports.tcp fails start-up. */
#include "transport/tcp/tcp.h"
#include "log.h"
void fipsd_tcp_destroy(fipsd_transport *t) { (void)t; }
int fipsd_tcp_register_config(fipsd_transport_registry *r, const fipsd_config *config,
    fipsd_transport **owned, size_t *count)
{
    (void)r; (void)owned; (void)count;
    if (!fipsd_config_get(config, "transports.tcp")) return 0;
    FIPSD_ERROR("tcp", "transports.tcp: the TCP transport is not available on this platform yet");
    return -1;
}
