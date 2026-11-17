/* The .fips DNS responder is not yet ported to this platform. dns.enabled
 * defaults to true, so the subsystem registers and reports, once, that it
 * serves nothing rather than refusing every default configuration. */
#include "dns/service.h"
#include "log.h"
#include <stdlib.h>
struct fipsd_dns_service { fipsd_subsystem subsystem; };
static int start(fipsd_subsystem *self, fipsd_runtime *rt)
{
    (void)self; (void)rt;
    FIPSD_WARN("dns", "the .fips DNS responder is not available on this platform yet; dns.enabled has no effect");
    return 0;
}
static void stop(fipsd_subsystem *self, fipsd_runtime *rt) { (void)self; (void)rt; }
fipsd_dns_service *fipsd_dns_service_create(GoSwap *configs, fipsd_node_handlers *handlers, fipsd_tun *tun)
{
    (void)configs; (void)handlers; (void)tun;
    fipsd_dns_service *s = calloc(1, sizeof *s);
    if (s) s->subsystem = (fipsd_subsystem){.name = "dns", .start = start, .stop = stop, .user = s};
    return s;
}
fipsd_subsystem *fipsd_dns_service_subsystem(fipsd_dns_service *s) { return s ? &s->subsystem : NULL; }
void fipsd_dns_service_destroy(fipsd_dns_service *s) { free(s); }
