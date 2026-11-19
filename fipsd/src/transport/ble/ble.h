#ifndef FIPSD_BLE_H
#define FIPSD_BLE_H
/* BLE L2CAP CoC transport (upstream transport/ble/mod.rs): the
 * platform-independent core over the backend contract in backend.h. A stream
 * transport like TCP: the shared FMP framing recovers packet boundaries, the
 * shared stats block carries the counters, and links report medium Ble. */
#include "transport/transport.h"
#include "transport/ble/backend.h"
#include "config/config.h"
#include <fips/identity/mod.h>

#define FIPSD_BLE_SEND_DEPTH 16u /* upstream pool.rs SEND_QUEUE_DEPTH */
typedef struct fipsd_ble_config {
    /* transports.ble.* (upstream config BleConfig, same names and defaults) */
    const char *adapter;          /* "hci0" */
    uint16_t psm, mtu;            /* 0x0085, 2048 */
    size_t max_connections;       /* 7 */
    uint32_t connect_timeout_ms;  /* 10000 */
    bool advertise, scan, auto_connect, accept_connections; /* true, true, false, true */
    uint64_t probe_cooldown_ms;   /* probe_cooldown_secs (30) in ms */
    /* Host bounds and test hooks, not YAML keys. */
    uint32_t pubkey_exchange_ms;  /* 5000, upstream PUBKEY_EXCHANGE_TIMEOUT_SECS */
    size_t send_queue_depth;      /* FIPSD_BLE_SEND_DEPTH */
    /* Without a node context (standalone tests): the local key for the
     * pre-handshake exchange and the secp context that validates the peer's.
     * NULL identity: no exchange, as upstream without set_local_pubkey. */
    const fips_xonly_pubkey *identity;
    const fips_secp *secp;
} fipsd_ble_config;
void fipsd_ble_config_default(fipsd_ble_config *);
/* Upstream verify_grace: connect_timeout_ms, never less than 10 s. */
uint64_t fipsd_ble_verify_grace_ms(const fipsd_ble_config *);
/* Takes ownership of the backend (destroyed with the transport, also on
 * failure). */
fipsd_transport *fipsd_ble_create(fips_transport_id, const char *name, const fipsd_ble_config *,
    fipsd_ble_backend backend);
void fipsd_ble_destroy(fipsd_transport *); /* after runtime join */
bool fipsd_ble_is(const fipsd_transport *);
/* transports.ble from config over this build's platform backend. Without one
 * the instance is ignored with a warning, as upstream ble_blocker. */
int fipsd_ble_register_config(fipsd_transport_registry *, const fipsd_config *,
    fipsd_transport **owned, size_t *count);
/* This build's platform backend for `adapter`, or false and a reason. The
 * BlueZ (gfi8h.6.2) and CoreBluetooth (gfi8h.6.3) beads fill this in. */
bool fipsd_ble_platform_backend(const fipsd_ble_config *, fipsd_ble_backend *out, const char **reason);

typedef fipsd_transport_stats_view fipsd_ble_stats_view;
fipsd_ble_stats_view fipsd_ble_stats_snapshot(const fipsd_transport *);
/* Borrowed one-slot notification, sent each time the transport becomes bound
 * (adapter present and powered, listener and advertiser up). */
GoChannel *fipsd_ble_bound(fipsd_transport *);
size_t fipsd_ble_connected_count(fipsd_transport *); /* pooled links */
uint16_t fipsd_ble_listener_psm(fipsd_transport *);  /* 0 while unbound */
uint64_t fipsd_ble_event_drops(fipsd_transport *);  /* backend events the sink dropped */
/* Upstream mark_verified / clear_verified / close_unproven, also reachable
 * through the node's optional transport ops. */
void fipsd_ble_mark_verified(fipsd_transport *, const fips_transport_addr *, const fips_node_addr *);
void fipsd_ble_clear_verified(fipsd_transport *, const fips_transport_addr *, const fips_node_addr *, bool removed);
size_t fipsd_ble_close_unproven(fipsd_transport *, uint64_t timeout_ms,
    bool (*held)(void *, const fips_transport_addr *), void *);
int fipsd_ble_is_verified(fipsd_transport *, const fips_transport_addr *); /* -1 not pooled */

/* Link addresses ("adapter/AA:BB:CC:DD:EE:FF", upstream addr.rs). */
bool fipsd_ble_peer_parse(const char *, size_t, fipsd_ble_peer *);
size_t fipsd_ble_peer_format(const fipsd_ble_peer *, char *out, size_t capacity);
bool fipsd_ble_peer_to_addr(const fipsd_ble_peer *, fips_transport_addr *);
bool fipsd_ble_peer_from_addr(const fips_transport_addr *, fipsd_ble_peer *);
bool fipsd_ble_peer_equal(const fipsd_ble_peer *, const fipsd_ble_peer *);
/* A stable, locally administered unicast address under "ble0" for a radio
 * that names peers by a 16-byte identifier (CoreBluetooth; upstream
 * io_macos::peer_addr, 64-bit FNV-1a). */
fipsd_ble_peer fipsd_ble_peer_from_identifier(const uint8_t identifier[16]);

/* The PSM wire format (upstream psm.rs): service data and GATT value are the
 * PSM as two bytes little-endian; a reader ignores trailing bytes. */
void fipsd_ble_encode_psm(uint16_t psm, uint8_t out[2]);
bool fipsd_ble_decode_psm(const uint8_t *, size_t, uint16_t *psm);
bool fipsd_ble_decode_gatt_psm(const uint8_t *, size_t, uint16_t *psm); /* also rejects 0 */
/* Legacy advertising PDU budget: flags 3 + 128-bit UUID list 18 + 16-bit
 * keyed service data 6 = 27 <= 31 (asserted at compile time in psm.c). */
enum { FIPSD_BLE_LEGACY_ADV_BYTES = 31, FIPSD_BLE_ADV_FLAGS_BYTES = 3, FIPSD_BLE_ADV_UUID128_BYTES = 18,
    FIPSD_BLE_ADV_PSM_BYTES = 6 };

/* ---- the simulated radio (radio.c, port of io_radio.rs) ------------------
 * In-process radios sharing an "air". Each radio is a full backend whose
 * control plane runs on its own foreign thread, exactly like a CoreBluetooth
 * dispatch queue or a D-Bus loop: fiber-side operations hand a request to that
 * thread and park on the reply with their deadline, and the thread reports
 * events through the sink. Bytes never cross the control thread: they move
 * through bounded per-direction channels (or a socketpair in fd mode). Used by
 * the tests and as the reference bridge for foreign-thread backends. */
typedef struct fipsd_ble_air fipsd_ble_air;
typedef struct fipsd_ble_radio fipsd_ble_radio;
typedef enum fipsd_ble_radio_mode {
    FIPSD_BLE_RADIO_SDU,   /* one write is one read (boundary-preserving, like SEQPACKET) */
    FIPSD_BLE_RADIO_BYTES, /* a byte stream: writes fragment and coalesce (like NSStream) */
    FIPSD_BLE_RADIO_FD     /* a real descriptor: socketpair, the core's fd fast path */
} fipsd_ble_radio_mode;
typedef struct fipsd_ble_radio_config {
    const char *adapter;     /* NULL: FIPSD_BLE_RADIO_ADAPTER */
    uint8_t device[6];
    uint16_t assigned_psm;   /* 0: bind what is asked (BlueZ); else OS-assigned (CoreBluetooth) */
    bool legacy_advert;      /* advertise the UUID only, no PSM service data (CoreBluetooth) */
    bool serve_gatt_psm;     /* serve the PSM characteristic (CoreBluetooth) */
    bool read_gatt;          /* implement read_psm (else -ENOTSUP) */
    uint16_t send_mtu, recv_mtu; /* 0: unknown */
    fipsd_ble_radio_mode mode;
    size_t chunk;            /* BYTES mode: largest delivery; 0 = 7 */
    size_t queue;            /* per-direction depth; 0 = 32 (io_radio SEND_QUEUE_CAP) */
    bool absent, unpowered;  /* initial adapter state (default present and powered) */
} fipsd_ble_radio_config;
fipsd_ble_air *fipsd_ble_air_create(void);
void fipsd_ble_air_destroy(fipsd_ble_air *); /* after its radios */
fipsd_ble_radio *fipsd_ble_radio_create(fipsd_ble_air *, const fipsd_ble_radio_config *);
/* The backend view; the transport owns it once created over it, so
 * destroying the transport destroys the radio. */
fipsd_ble_backend fipsd_ble_radio_backend(fipsd_ble_radio *);
/* Controls, any thread. Powering off or removing the adapter fails parked
 * operations with -ENODEV and kills the radio's live channels. */
void fipsd_ble_radio_set_adapter(fipsd_ble_radio *, bool present, bool powered);
void fipsd_ble_radio_set_stall(fipsd_ble_radio *, bool stall); /* dials never answer */
void fipsd_ble_radio_set_refuse(fipsd_ble_radio *, bool refuse); /* inbound dials refused */
void fipsd_ble_radio_set_assigned_psm(fipsd_ble_radio *, uint16_t); /* next listen */
size_t fipsd_ble_radio_drop_links(fipsd_ble_radio *); /* break every live channel */
/* Return false to drop a write this radio makes on any channel (it reads as
 * sent and never arrives), so tests can lose a frame deterministically. SDU
 * and byte modes only: an FD stream writes its descriptor directly. Set before
 * the transport starts; it runs on the writer, under no lock. */
typedef bool (*fipsd_ble_radio_filter)(void *user, const uint8_t *bytes, size_t len);
void fipsd_ble_radio_set_filter(fipsd_ble_radio *, fipsd_ble_radio_filter, void *user);
/* An advert from an arbitrary (possibly nonexistent) advertiser. */
void fipsd_ble_radio_inject_advert(fipsd_ble_radio *, const fipsd_ble_peer *, const uint8_t *service_data, size_t len);
typedef struct fipsd_ble_radio_stats {
    uint64_t listens, connects, refused, advertise_calls, advertise_restarts, scan_starts, scan_stops, gatt_reads, channels;
    uint16_t advertised_psm, bound_psm;
    bool advertising, scanning;
} fipsd_ble_radio_stats;
fipsd_ble_radio_stats fipsd_ble_radio_stats_read(fipsd_ble_radio *);
/* Channels not yet closed by both ends, and a one-slot wake on every channel
 * opened or freed in the air: an event to wait on, never to poll. */
size_t fipsd_ble_radio_channels(fipsd_ble_radio *);
GoChannel *fipsd_ble_air_changed(fipsd_ble_air *);
#endif
