/* Schema at libfips-core/UPSTREAM.md pin; defaults verified against
 * docs/reference/configuration.md and src/config. No parser in the core. */
#include "internal.h"

static const cfg_field schema_transports_udp[] = {
    S("bind_addr","\"0.0.0.0:2121\""),
    U16("mtu","1357"),
    Z("recv_buf_size","2097152"),
    Z("send_buf_size","2097152"),
    B("advertise_on_nostr","false"),
    B("public","false"),
    S("external_addr",NULL),
    S("interface",NULL),
    B("outbound_only","false"),
    B("accept_connections","true"),
    E("role","\"normal\"","never|backup|normal|preferred"),
    E("kind",NULL,"local|internet"),
    END
};
static const cfg_field schema_transports_ethernet[] = {
    S("interface","!"),
    U16("ethertype","0x2121"),
    U16("mtu",NULL),
    Z("recv_buf_size","2097152"),
    Z("send_buf_size","2097152"),
    B("listen","true"),
    B("announce","false"),
    B("auto_connect","false"),
    B("accept_connections","false"),
    U("beacon_interval_secs","30"),
    B("optional","false"),
    E("role","\"normal\"","never|backup|normal|preferred"),
    END
};
static const cfg_field schema_transports_tcp[] = {
    S("bind_addr",NULL),
    U16("mtu","1400"),
    U("connect_timeout_ms","5000"),
    B("nodelay","true"),
    U("keepalive_secs","30"),
    Z("recv_buf_size","2097152"),
    Z("send_buf_size","2097152"),
    Z("max_inbound_connections",NULL),
    Z("max_inbound_per_source","8"),
    B("advertise_on_nostr","false"),
    S("external_addr",NULL),
    E("role","\"normal\"","never|backup|normal|preferred"),
    E("kind",NULL,"local|internet"),
    END
};
static const cfg_field schema_transports_tor_directory_service[] = {
    S("hostname_file","\"/var/lib/tor/fips_onion_service/hostname\""),
    S("bind_addr","\"127.0.0.1:8443\""),
    END
};
static const cfg_field schema_transports_tor[] = {
    S("mode","\"socks5\""),
    U("connect_timeout_ms","120000"),
    U16("mtu","1400"),
    S("control_addr","\"/run/tor/control\""),
    S("control_auth","\"cookie\""),
    S("cookie_path","\"/var/run/tor/control.authcookie\""),
    Z("max_inbound_connections","64"),
    O("directory_service",schema_transports_tor_directory_service),
    B("advertise_on_nostr","false"),
    U16("advertised_port","443"),
    E("role","\"normal\"","never|backup|normal|preferred"),
    END
};
static const cfg_field schema_transports_nym[] = {
    U("connect_timeout_ms","300000"),
    U16("mtu","1400"),
    U("startup_timeout_secs","120"),
    E("role","\"normal\"","never|backup|normal|preferred"),
    END
};
static const cfg_field schema_transports_ble[] = {
    S("adapter","\"hci0\""),
    U16("psm","0x0085"),
    U16("mtu","2048"),
    Z("max_connections","7"),
    U("connect_timeout_ms","10000"),
    B("advertise","true"),
    B("scan","true"),
    B("auto_connect","false"),
    B("accept_connections","true"),
    U("probe_cooldown_secs","30"),
    E("role","\"normal\"","never|backup|normal|preferred"),
    END
};
const cfg_field cfg_transports[] = {
    {"udp",CFG_INSTANCES,NULL,0,schema_transports_udp,NULL},
    {"ethernet",CFG_INSTANCES,NULL,0,schema_transports_ethernet,NULL},
    {"tcp",CFG_INSTANCES,NULL,0,schema_transports_tcp,NULL},
    {"tor",CFG_INSTANCES,NULL,0,schema_transports_tor,NULL},
    {"nym",CFG_INSTANCES,NULL,0,schema_transports_nym,NULL},
    {"ble",CFG_INSTANCES,NULL,0,schema_transports_ble,NULL},
    END
};
