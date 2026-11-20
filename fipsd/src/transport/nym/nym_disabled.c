/* Built without -DFIPSD_ENABLE_NYM: a configured transports.nym is refused
 * loudly rather than ignored (the operator asked for a transport that this
 * binary cannot provide). */
#include "transport/nym/nym.h"
int fipsd_nym_register_config(fipsd_transport_registry *r, const fipsd_config *config,
    fipsd_transport **owned, size_t *count)
{
    (void)r; (void)owned; (void)count;
    if (!fipsd_config_get(config, "transports.nym")) return 0;
    FIPSD_ERROR("nym", "transports.nym is configured but this fipsd was built without the Nym transport (configure with -DFIPSD_ENABLE_NYM=ON)");
    return -1;
}
void fipsd_nym_destroy(fipsd_transport *t) { (void)t; } /* nothing is ever created */
