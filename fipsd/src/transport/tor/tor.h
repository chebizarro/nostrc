#ifndef FIPSD_TOR_H
#define FIPSD_TOR_H
#include "transport/transport.h"
#include "config/config.h"
#include <jansson.h>
/* Upstream transport/tor: a stream owner over the TCP owner, dialing through
 * Tor's SOCKS5 proxy (username "fips", password = the peer address, so
 * IsolateSOCKSAuth gives each destination its own circuit) and, in directory
 * mode, accepting the daemon's HiddenServicePort forwards on a local
 * listener. Modes: socks5 (outbound only), control_port (outbound plus a
 * control-port monitor, no onion service), directory (outbound plus the
 * Tor-managed onion service; the monitor only when control_addr is set).
 * Upstream never creates onion services over the control port (ADD_ONION),
 * which keeps Tor's Sandbox 1 usable; neither does this port. */
struct fipsd_watcher_source;
typedef struct fipsd_tor_config {
    const char *mode;              /* socks5 | control_port | directory */
    const char *socks5_addr;       /* host:port of Tor's SOCKS listener */
    uint32_t connect_timeout_ms;   /* SOCKS5 connect + handshake deadline */
    uint16_t mtu;
    const char *control_addr;      /* unix socket path or host:port; NULL: no monitor */
    const char *control_auth;      /* cookie | cookie:<path> | password:<secret> */
    const char *cookie_path;       /* for control_auth "cookie" */
    size_t max_inbound_connections;
    const char *hostname_file, *bind_addr; /* directory_service */
    uint16_t advertised_port;
    /* Host bounds, not YAML keys. */
    size_t max_outbound_connections, send_queue_depth;
    uint64_t first_frame_ms, idle_ms;
    uint32_t control_backoff_ms;   /* first reconnect delay; doubles up to 30 s */
    uint32_t control_stall_ms;     /* bootstrap-stall warning after this long (upstream 60 s) */
    const struct fipsd_watcher_source *watcher_source;
} fipsd_tor_config;
/* Upstream control.rs TorMonitoringInfo. */
typedef struct fipsd_tor_monitoring {
    uint8_t bootstrap;
    bool circuit_established, dormant;
    uint64_t traffic_read, traffic_written;
    char network_liveness[16], version[64];
} fipsd_tor_monitoring;
void fipsd_tor_config_default(fipsd_tor_config *);
fipsd_transport *fipsd_tor_create(fips_transport_id, const char *name, const fipsd_tor_config *);
void fipsd_tor_destroy(fipsd_transport *); /* after runtime join */
bool fipsd_tor_is(const fipsd_transport *);
const char *fipsd_tor_mode(const fipsd_transport *);
/* NULL until a directory-mode start read the hostname file. */
const char *fipsd_tor_onion_address(const fipsd_transport *);
/* The onion for this node's Nostr advert: NULL until the hostname is read
 * and, when a control monitor runs, Tor reported the service published
 * (HS_DESC UPLOADED for this address, or a current descriptor already held
 * when the monitor subscribed). Without a monitor, or when the monitor never
 * completes its first session, the hostname alone suffices (upstream). Set
 * once; fipsd_tor_advert_updates then carries one notification. */
const char *fipsd_tor_advertised_onion(const fipsd_transport *);
uint16_t fipsd_tor_advertised_port(const fipsd_transport *);
GoChannel *fipsd_tor_advert_updates(fipsd_transport *); /* borrowed, capacity one */
/* False until the control monitor took its first snapshot; the last one is
 * kept across control reconnects, as upstream keeps its cache. */
bool fipsd_tor_monitoring_snapshot(const fipsd_transport *, fipsd_tor_monitoring *);
json_t *fipsd_tor_monitoring_json(const fipsd_tor_monitoring *); /* owned */
GoChannel *fipsd_tor_bound(fipsd_transport *);              /* borrowed: inner listener/dialer readiness */
GoChannel *fipsd_tor_monitoring_updates(fipsd_transport *); /* borrowed capacity-one notification per snapshot */
bool fipsd_tor_listener_address(fipsd_transport *, fips_transport_addr *); /* the HiddenServicePort target */
size_t fipsd_tor_connected_count(fipsd_transport *);
fipsd_transport *fipsd_tor_stream(fipsd_transport *); /* the inner TCP owner (tests) */
int fipsd_tor_register_config(fipsd_transport_registry *, const fipsd_config *,
    fipsd_transport **owned, size_t *count);
#endif
