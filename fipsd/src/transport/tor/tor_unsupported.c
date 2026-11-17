/* The Tor transport rides on the TCP stream owners and a control-port
 * monitor, neither ported to this platform yet. A configured transports.tor
 * fails start-up; the control projection sees no Tor transports. */
#include "transport/tor/tor.h"
#include "log.h"
void fipsd_tor_destroy(fipsd_transport *t) { (void)t; }
bool fipsd_tor_is(const fipsd_transport *t) { (void)t; return false; }
const char *fipsd_tor_mode(const fipsd_transport *t) { (void)t; return NULL; }
const char *fipsd_tor_onion_address(const fipsd_transport *t) { (void)t; return NULL; }
bool fipsd_tor_monitoring_snapshot(const fipsd_transport *t, fipsd_tor_monitoring *out)
{ (void)t; (void)out; return false; }
json_t *fipsd_tor_monitoring_json(const fipsd_tor_monitoring *m) { (void)m; return NULL; }
int fipsd_tor_register_config(fipsd_transport_registry *r, const fipsd_config *config,
    fipsd_transport **owned, size_t *count)
{
    (void)r; (void)owned; (void)count;
    if (!fipsd_config_get(config, "transports.tor")) return 0;
    FIPSD_ERROR("tor", "transports.tor: the Tor transport is not available on this platform yet");
    return -1;
}
