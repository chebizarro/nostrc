/* The control socket is an AF_UNIX stream server guarded by the `fips`
 * group; on Windows it becomes TCP 127.0.0.1:21210 (upstream; W3). Until
 * then node.control.enabled, on by default, starts a subsystem that reports
 * once that no control endpoint is served. The protocol, queries and render
 * modules are built; only the listener is absent. */
#include "control/control.h"
#include "log.h"
#include <string.h>
static int start(fipsd_subsystem *self, fipsd_runtime *rt)
{
    (void)self; (void)rt;
    FIPSD_WARN("control", "no control socket on this platform yet (Windows: TCP 127.0.0.1:21210 is pending); fipsctl cannot attach");
    return 0;
}
static void stop(fipsd_subsystem *self, fipsd_runtime *rt) { (void)self; (void)rt; }
void fipsd_control_service_init(fipsd_control_service *service, GoSwap *configs,
                               const fipsd_control_provider *provider)
{
    memset(service, 0, sizeof *service);
    service->configs = configs;
    if (provider) service->provider = *provider;
    service->listener = service->cancel_socket = -1;
    service->subsystem = (fipsd_subsystem){.name = "control", .start = start, .stop = stop, .user = service};
}
void fipsd_control_service_clear(fipsd_control_service *service) { (void)service; }
