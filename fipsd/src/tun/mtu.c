/* Pure configuration arithmetic, built on every platform (including those
 * without a TUN backend): the smallest configured transport MTU. */
#include "tun/lifecycle.h"
uint32_t fipsd_tun_transport_mtu(const fipsd_config *c)
{
    uint32_t mtu=UINT32_MAX;
    const fipsd_config_value *transports=fipsd_config_get(c,"transports");
    for (size_t i=0;i<fipsd_config_count(transports);++i) {
        const fipsd_config_value *type=fipsd_config_member(transports,fipsd_config_key(transports,i));
        const fipsd_config_value *value=fipsd_config_member(type,"mtu");
        if (value) { uint64_t n=fipsd_config_uint(value); if (n && n<mtu) mtu=(uint32_t)n; }
        else for (size_t j=0;j<fipsd_config_count(type);++j) {
            const fipsd_config_value *instance=fipsd_config_member(type,fipsd_config_key(type,j));
            uint64_t n=fipsd_config_uint(fipsd_config_member(instance,"mtu"));
            if (n && n<mtu) mtu=(uint32_t)n;
        }
    }
    return mtu==UINT32_MAX?1357:mtu;
}
