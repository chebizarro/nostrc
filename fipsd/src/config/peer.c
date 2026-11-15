/* Schema at libfips-core/UPSTREAM.md pin; defaults verified against
 * docs/reference/configuration.md and src/config. No parser in the core. */
#include "internal.h"

static const cfg_field schema_peers_addresses[] = {
    S("transport","!"),
    S("addr","!"),
    U8("priority","100"),
    END
};
const cfg_field cfg_peers[] = {
    S("npub","!"),
    S("alias",NULL),
    A("addresses",schema_peers_addresses),
    E("connect_policy","\"auto_connect\"","auto_connect|on_demand|manual"),
    B("auto_reconnect","true"),
    B("via_nostr","false"),
    END
};
