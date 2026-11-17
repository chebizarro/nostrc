/* Built without -DFIPSD_ENABLE_BLE: a configured transports.ble is refused
 * loudly rather than ignored, as a build without Nym refuses transports.nym. */
#include "transport/ble/ble.h"
#include "log.h"
int fipsd_ble_register_config(fipsd_transport_registry *r, const fipsd_config *config,
    fipsd_transport **owned, size_t *count)
{
    (void)r; (void)owned; (void)count;
    if (!fipsd_config_get(config, "transports.ble")) return 0;
    FIPSD_ERROR("ble", "transports.ble is configured but this fipsd was built without the BLE transport (configure with -DFIPSD_ENABLE_BLE=ON)");
    return -1;
}
void fipsd_ble_destroy(fipsd_transport *t) { (void)t; } /* nothing is ever created */
