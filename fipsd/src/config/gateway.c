/* Schema at libfips-core/UPSTREAM.md pin; defaults verified against
 * docs/reference/configuration.md and src/config. No parser in the core. */
#include "internal.h"

static const cfg_field schema_gateway_dns[] = {
    S("listen","\"[::1]:5365\""),
    S("upstream","\"[::1]:5354\""),
    U32("ttl","60"),
    END
};
static const cfg_field schema_gateway_conntrack[] = {
    U("tcp_established","432000"),
    U("udp_timeout","30"),
    U("udp_assured","180"),
    U("icmp_timeout","30"),
    END
};
static const cfg_field schema_gateway_port_forwards[] = {
    U16("listen_port","!"),
    E("proto","!","tcp|udp"),
    S("target","!"),
    END
};
const cfg_field cfg_gateway[] = {
    B("enabled","false"),
    S("pool","!"),
    S("lan_interface","!"),
    B("global_prefix","false"),
    U("pool_grace_period","60"),
    M("dns",schema_gateway_dns),
    M("conntrack",schema_gateway_conntrack),
    A("port_forwards",schema_gateway_port_forwards),
    END
};
