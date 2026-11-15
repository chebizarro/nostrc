/* Schema at libfips-core/UPSTREAM.md pin; defaults verified against
 * docs/reference/configuration.md and src/config. No parser in the core. */
#include "internal.h"

static const cfg_field schema_node_control[] = {
    B("enabled","true"),
    S("socket_path","\"__control_socket__\""),
    END
};
static const cfg_field schema_node_identity[] = {
    S("nsec",NULL),
    B("persistent","false"),
    END
};
static const cfg_field schema_node_limits[] = {
    Z("max_connections","256"),
    Z("max_peers","128"),
    Z("max_links","256"),
    Z("max_pending_inbound","1000"),
    Z("max_sessions","1024"),
    END
};
static const cfg_field schema_node_rate_limit[] = {
    U32("handshake_burst","100"),
    F("handshake_rate","10.0"),
    U("handshake_timeout_secs","30"),
    U("handshake_resend_interval_ms","1000"),
    F("handshake_resend_backoff","2.0"),
    U32("handshake_max_resends","5"),
    U32("established_handshake_burst",NULL),
    F("established_handshake_rate",NULL),
    U32("session_setup_burst","64"),
    F("session_setup_rate","16.0"),
    END
};
static const cfg_field schema_node_retry[] = {
    U32("max_retries","5"),
    U("base_interval_secs","5"),
    U("max_backoff_secs","300"),
    END
};
static const cfg_field schema_node_netmon[] = {
    B("enabled","true"),
    U("poll_interval_secs","5"),
    U("debounce_ms","250"),
    END
};
static const cfg_field schema_node_cache[] = {
    Z("coord_size","50000"),
    U("coord_ttl_secs","300"),
    Z("identity_size","10000"),
    U("test_coord_ttl_secs",NULL),
    END
};
static const cfg_field schema_node_lookup_attempt_timeouts_secs_element[] = {{"",CFG_UINT,NULL,UINT64_MAX,NULL,NULL}, END};
static const cfg_field schema_node_lookup[] = {
    U8("ttl","64"),
    {"attempt_timeouts_secs",CFG_LIST,"[1, 2, 4, 8]",0,schema_node_lookup_attempt_timeouts_secs_element,NULL},
    U("recent_expiry_secs","10"),
    U("backoff_base_secs","0"),
    U("backoff_max_secs","0"),
    U("forward_min_interval_secs","2"),
    END
};
static const cfg_field schema_node_rendezvous_nostr_advert_relays_element[] = {{"",CFG_STRING,NULL,UINT64_MAX,NULL,NULL}, END};
static const cfg_field schema_node_rendezvous_nostr_dm_relays_element[] = {{"",CFG_STRING,NULL,UINT64_MAX,NULL,NULL}, END};
static const cfg_field schema_node_rendezvous_nostr_stun_servers_element[] = {{"",CFG_STRING,NULL,UINT64_MAX,NULL,NULL}, END};
static const cfg_field schema_node_rendezvous_nostr[] = {
    B("enabled","false"),
    E("policy","\"configured_only\"","disabled|configured_only|open"),
    Z("open_discovery_max_pending","64"),
    Z("max_concurrent_incoming_offers","16"),
    Z("max_concurrent_offers_per_npub","4"),
    Z("advert_cache_max_entries","2048"),
    Z("seen_sessions_max_entries","4096"),
    B("advertise","true"),
    {"advert_relays",CFG_LIST,"[\"wss://nos.lol\", \"wss://offchain.pub\"]",0,schema_node_rendezvous_nostr_advert_relays_element,NULL},
    {"dm_relays",CFG_LIST,"[\"wss://nos.lol\", \"wss://offchain.pub\"]",0,schema_node_rendezvous_nostr_dm_relays_element,NULL},
    {"stun_servers",CFG_LIST,"[\"stun:stun.l.google.com:19302\", \"stun:stun.cloudflare.com:3478\", \"stun:global.stun.twilio.com:3478\"]",0,schema_node_rendezvous_nostr_stun_servers_element,NULL},
    B("share_local_candidates","false"),
    S("app","\"fips-overlay-v1\""),
    U("signal_ttl_secs","120"),
    U("attempt_timeout_secs","10"),
    U("replay_window_secs","300"),
    U("punch_start_delay_ms","2000"),
    U("punch_interval_ms","200"),
    U("punch_duration_ms","10000"),
    U("advert_ttl_secs","3600"),
    U("advert_refresh_secs","1800"),
    U("startup_sweep_delay_secs","5"),
    U("startup_sweep_max_age_secs","3600"),
    U32("failure_streak_threshold","5"),
    U("extended_cooldown_secs","1800"),
    U("warn_log_interval_secs","300"),
    Z("failure_state_max_entries","4096"),
    U("protocol_mismatch_cooldown_secs","86400"),
    END
};
static const cfg_field schema_node_rendezvous_lan[] = {
    B("enabled","false"),
    S("service_type","\"_fips._udp.local.\""),
    S("scope",NULL),
    END
};
static const cfg_field schema_node_rendezvous[] = {
    M("nostr",schema_node_rendezvous_nostr),
    M("lan",schema_node_rendezvous_lan),
    END
};
static const cfg_field schema_node_tree[] = {
    U("announce_min_interval_ms","500"),
    F("parent_hysteresis","0.2"),
    U("hold_down_secs","30"),
    U("reeval_interval_secs","60"),
    U32("flap_threshold","4"),
    U("flap_window_secs","60"),
    U("flap_dampening_secs","120"),
    END
};
static const cfg_field schema_node_bloom[] = {
    U("update_debounce_ms","500"),
    F("max_inbound_fpr","0.20"),
    F("child_echo_threshold","0.8"),
    END
};
static const cfg_field schema_node_ecn[] = {
    B("enabled","true"),
    F("loss_threshold","0.05"),
    F("etx_threshold","3.0"),
    END
};
static const cfg_field schema_node_rekey[] = {
    U("after_secs","120"),
    U("after_messages","65536"),
    END
};
static const cfg_field schema_node_path_switch[] = {
    F("margin","1.3"),
    U("dwell_secs","2"),
    END
};
static const cfg_field schema_node_path_probe[] = {
    U("active_ms","200"),
    U("standby_ms","1000"),
    END
};
static const cfg_field schema_node_path[] = {
    B("enabled","false"),
    U32("min_samples","2"),
    B("mix_kinds","true"),
    M("switch",schema_node_path_switch),
    M("probe",schema_node_path_probe),
    END
};
static const cfg_field schema_node_session[] = {
    U8("default_ttl","64"),
    Z("pending_packets_per_dest","16"),
    Z("pending_max_destinations","256"),
    U("idle_timeout_secs","90"),
    U8("coords_warmup_packets","5"),
    U("coords_response_interval_ms","2000"),
    END
};
static const cfg_field schema_node_mmp[] = {
    E("mode","\"full\"","full|lightweight|minimal"),
    U("log_interval_secs","30"),
    Z("owd_window_size","32"),
    END
};
static const cfg_field schema_node_session_mmp[] = {
    E("mode","\"full\"","full|lightweight|minimal"),
    U("log_interval_secs","30"),
    Z("owd_window_size","32"),
    END
};
static const cfg_field schema_node_buffers[] = {
    Z("packet_channel","1024"),
    Z("tun_channel","1024"),
    Z("dns_channel","64"),
    Z("host_channel","4096"),
    END
};
static const cfg_field schema_node_native_api_grants_ports_element[] = {{"",CFG_UINT,NULL,UINT16_MAX,NULL,NULL}, END};
static const cfg_field schema_node_native_api_grants[] = {
    U32("uid",NULL),
    U32("gid",NULL),
    {"ports",CFG_LIST,"[]",0,schema_node_native_api_grants_ports_element,NULL},
    B("keys","false"),
    B("learn","false"),
    END
};
static const cfg_field schema_node_native_api[] = {
    B("enabled","false"),
    S("socket_path","\"__api_socket__\""),
    Z("pending_per_flow","16"),
    Z("backlog","16"),
    Z("max_flows","256"),
    B("debug_commands","false"),
    A("grants",schema_node_native_api_grants),
    END
};
const cfg_field cfg_node[] = {
    M("control",schema_node_control),
    M("identity",schema_node_identity),
    B("leaf_only","false"),
    U("base_rtt_ms","100"),
    U("heartbeat_interval_secs","10"),
    U("link_dead_timeout_secs","30"),
    U("drain_timeout_secs","2"),
    S("log_level","\"info\""),
    S("log_file",NULL),
    U("log_max_size_mb","10"),
    U32("log_max_files","4"),
    M("limits",schema_node_limits),
    M("rate_limit",schema_node_rate_limit),
    M("retry",schema_node_retry),
    M("netmon",schema_node_netmon),
    M("cache",schema_node_cache),
    M("lookup",schema_node_lookup),
    M("rendezvous",schema_node_rendezvous),
    M("tree",schema_node_tree),
    M("bloom",schema_node_bloom),
    M("ecn",schema_node_ecn),
    M("rekey",schema_node_rekey),
    M("path",schema_node_path),
    M("session",schema_node_session),
    M("mmp",schema_node_mmp),
    M("session_mmp",schema_node_session_mmp),
    M("buffers",schema_node_buffers),
    M("native_api",schema_node_native_api),
    END
};
