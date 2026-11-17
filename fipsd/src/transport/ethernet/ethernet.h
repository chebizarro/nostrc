#ifndef FIPSD_ETHERNET_H
#define FIPSD_ETHERNET_H
#include "transport/transport.h"
#include "transport/watcher.h"
#include "config/config.h"
#include <fips/identity/peer.h>
#define FIPSD_ETHERTYPE 0x2121u
#define FIPSD_ETH_NEIGHBORS 1024u
#define FIPSD_ETH_BEACON_SIZE 34u
typedef struct fipsd_ethernet_config {
    const char *interface_name;
    uint16_t ethertype, mtu; /* zero mtu: interface MTU minus three */
    size_t recv_buf_size, send_buf_size;
    bool listen, announce, auto_connect, accept_connections, optional;
    uint64_t beacon_interval_secs;
    const fipsd_watcher_source *watcher_source;
} fipsd_ethernet_config;
typedef struct fipsd_eth_neighbor { uint8_t mac[6]; fips_xonly_pubkey key; } fipsd_eth_neighbor;
typedef struct fipsd_eth_neighbors {
    nsync_mu mu;
    size_t count;
    uint64_t dropped;
    fipsd_eth_neighbor peers[FIPSD_ETH_NEIGHBORS];
} fipsd_eth_neighbors;
void fipsd_eth_neighbors_init(fipsd_eth_neighbors *);
bool fipsd_eth_neighbors_add(fipsd_eth_neighbors *, const uint8_t[6], const fips_xonly_pubkey *);
size_t fipsd_eth_neighbors_take(fipsd_eth_neighbors *, fipsd_eth_neighbor *, size_t);
void fipsd_eth_neighbors_clear(fipsd_eth_neighbors *);
bool fipsd_eth_group(const uint8_t[6]);
size_t fipsd_eth_data_encode(uint8_t *, size_t, const uint8_t *, size_t);
bool fipsd_eth_data_decode(const uint8_t *, size_t, const uint8_t **, size_t *);
void fipsd_eth_beacon_encode(uint8_t[34], const fips_xonly_pubkey *);
bool fipsd_eth_beacon_decode(const fips_secp *, const uint8_t *, size_t, fips_xonly_pubkey *);
void fipsd_ethernet_config_default(fipsd_ethernet_config *);
fipsd_transport *fipsd_ethernet_create(fips_transport_id, const char *, const fipsd_ethernet_config *);
void fipsd_ethernet_destroy(fipsd_transport *);
int fipsd_ethernet_register_config(fipsd_transport_registry *, const fipsd_config *, fipsd_transport **, size_t *);
#endif
