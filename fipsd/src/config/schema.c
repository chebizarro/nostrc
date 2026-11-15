#include "internal.h"
static const cfg_field schema_tun[] = {
    B("enabled","false"),
    S("name","\"fips0\""),
    U16("mtu","1280"),
    END
};
static const cfg_field schema_dns[] = {
    B("enabled","true"),
    S("bind_addr","\"::1\""),
    U16("port","5354"),
    U32("ttl","300"),
    END
};
const cfg_field cfg_root[] = {
 M("node",cfg_node), M("transports",cfg_transports), A("peers",cfg_peers),
 O("gateway",cfg_gateway), M("tun",schema_tun), M("dns",schema_dns), END
};
