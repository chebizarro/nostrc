/* Relay rendezvous with STUN and NAT traversal is not yet ported to this
 * platform (interface enumeration, TCP dials); the event contracts compile,
 * node.rendezvous.nostr.enabled fails start-up. */
#include "nostr/runtime.h"
#include "nostr/driver.h"
#include "log.h"
#include <errno.h>
fipsd_rendezvous_driver fipsd_nostr_node_driver_init(fipsd_nostr_node_driver *d, const fipsd_node_context *node,
    fipsd_node_handlers *handlers, const fipsd_transport_registry *transports)
{
    if (d) *d = (fipsd_nostr_node_driver){.node = node, .handlers = handlers, .transports = transports};
    return (fipsd_rendezvous_driver){.user = d};
}
fipsd_nostr_runtime *fipsd_nostr_runtime_create(const fipsd_config *config, const fips_identity *identity,
    const fipsd_node_context *node, fipsd_rendezvous_driver driver)
{
    (void)config; (void)identity; (void)node; (void)driver;
    FIPSD_ERROR("nostr", "node.rendezvous.nostr.enabled: relay rendezvous is not available on this platform yet");
    errno = ENOTSUP;
    return NULL;
}
fipsd_subsystem *fipsd_nostr_runtime_subsystem(fipsd_nostr_runtime *r) { (void)r; return NULL; }
void fipsd_nostr_runtime_destroy(fipsd_nostr_runtime *r) { (void)r; }
