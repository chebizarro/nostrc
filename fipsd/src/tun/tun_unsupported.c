/* TUN device owner on platforms without a backend (Windows: wintun is W2).
 * Replaces tun.c, route.c, lifecycle.c and status.c there: tun.enabled fails
 * start-up visibly instead of running without the device it asked for. */
#include "tun/lifecycle.h"
#include "tun/status.h"
#include "log.h"
#include <errno.h>
fipsd_tun *fipsd_tun_create(const fipsd_tun_config *cfg, const fipsd_tun_ops *ops,
                            const fipsd_tun_platform *platform)
{
    (void)cfg; (void)ops; (void)platform;
    FIPSD_ERROR("tun", "tun.enabled: no TUN device backend on this platform yet");
    errno = ENOTSUP;
    return NULL;
}
void fipsd_tun_destroy(fipsd_tun *t) { (void)t; }
const fipsd_tun_platform *fipsd_tun_native_platform(void) { return NULL; }
fipsd_control_provider fipsd_tun_provider(fipsd_tun *t, fipsd_control_provider base)
{ (void)t; return base; }
void fipsd_tun_service_init(fipsd_tun_service *s, fipsd_tun *t, fipsd_node *node)
{ *s = (fipsd_tun_service){.device = t, .node = node}; }
void fipsd_tun_service_set_shim(fipsd_tun_service *s, struct fipsd_ipv6_shim *shim) { s->shim = shim; }
