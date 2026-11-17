#ifndef FIPSD_BLE_BACKEND_H
#define FIPSD_BLE_BACKEND_H
/* The BLE radio backend contract (upstream transport/ble/io.rs `BleIo`,
 * `BleStream`, `BleAcceptor`, `BleScanner`). This is the whole surface a
 * platform backend implements: BlueZ (nostrc-gfi8h.6.2), CoreBluetooth behind
 * the macOS LaunchAgent (nostrc-gfi8h.6.3) and the simulated radio (radio.c).
 * Everything above it (discovery, PSM learning, pubkey exchange, arbitration,
 * pool, framing, MTU, presence, backoff, stats) is the platform-independent
 * core in ble.c and never lives in a backend.
 *
 * Idiom: the fipsd libgo one. A vtable of operations that park only the
 * calling fiber, each bounded by an absolute deadline, plus one sink of typed
 * events the backend pushes into. No callback ever runs transport code on a
 * backend thread: a backend that lives on foreign threads (a D-Bus loop, a
 * CoreBluetooth dispatch queue, the agent socket reader) hands results to the
 * waiting fiber through a channel and events through the sink, both of which
 * are nsync-backed and safe from any thread.
 *
 * Time: deadlines are absolute milliseconds on fipsd_transport_now_ms()
 * (CLOCK_MONOTONIC). FIPSD_BLE_NO_DEADLINE parks until the outcome or stop.
 *
 * Errors: 0 on success or a negative errno.
 *   -ETIMEDOUT     the deadline passed (peer out of reach; NOT a refusal)
 *   -ECANCELED     stop() ran, or the stream was cancelled
 *   -ECONNREFUSED  the peer answered and refused (nothing listens on the PSM)
 *   -ENODEV        the adapter is absent or powered off
 *   -ENOTSUP       the backend cannot perform this operation
 *   other -errno   any other failure, counted as a connect/IO error
 * The core treats ETIMEDOUT and ECONNREFUSED differently (upstream
 * forget_psm: only a refusal forgets a learned PSM), so a backend must not
 * report one as the other.
 *
 * Owner-directed cancellation: every operation that can wait returns promptly
 * with -ECANCELED once stop() has run, whatever its deadline. A stream's
 * cancel() does the same for that stream's read/write. The core joins every
 * caller before it calls stream close() or backend destroy().
 *
 * ------------------------------------------------------------------------
 * Mapping: upstream io.rs -> this contract -> platform
 * ------------------------------------------------------------------------
 * io.rs                          | here             | BlueZ (6.2, bluer over D-Bus)             | CoreBluetooth (6.3, LaunchAgent)
 * -------------------------------+------------------+-------------------------------------------+-------------------------------------------
 * BluerIo::new, start_daemon_    | start            | Session + Adapter, set_powered(true);     | connect the agent socket; the agent owns
 *   radio (construction)         |                  | watch Powered / adapter removal and       | CBCentral/CBPeripheralManager and relays
 *                                |                  | report it with fipsd_ble_sink_adapter     | CBManagerState (no agent: present=false)
 * (task drop on stop_async)      | stop             | drop discovery and advert handles, close  | tell the agent to stop; fail every parked
 *                                |                  | the listener, fail parked D-Bus calls     | request with -ECANCELED
 * (adapter power, slot install,  | adapter,         | Adapter1.Powered, InterfacesRemoved       | poweredOn / poweredOff / unsupported; an
 *   radio replaced)              | sink_adapter     |                                           | agent reconnect is a replaced radio
 * BleIo::listen(psm) -> (a, psm) | listen           | l2cap SEQPACKET StreamListener bind(psm); | publishL2CAPChannel: OS-assigned PSM,
 *                                |                  | reports psm unchanged                     | reported back and served over GATT
 * BleAcceptor::accept            | INBOUND_STREAM   | listener.accept() -> fd stream            | didOpenL2CAPChannel -> agent hands over
 *                                |                  |                                           | the channel (fd, or a bridged stream)
 * BleIo::connect(addr, psm)      | connect          | l2cap Stream::connect(LePublic, psm);     | connect(peripheral) then
 *                                |                  | keep ETIMEDOUT and ECONNREFUSED distinct  | openL2CAPChannel(psm)
 * (io_macos DialPlan GATT steps) | read_psm         | -ENOTSUP at first (GattCharacteristic1    | discoverServices(FIPS) ->
 *                                |                  | ReadValue may be adopted later)           | discoverCharacteristics(PSM) -> read
 * start_advertising(psm)         | advertise        | LEAdvertisement1: FIPS UUID, ServiceData  | startAdvertising(FIPS UUID only) and set
 *                                |                  | {0x9C90: psm LE}, 400-600 ms interval     | the PSM characteristic to psm LE
 * stop_advertising               | advertise_stop   | drop the AdvertisementHandle              | stopAdvertising
 * start_scanning, Scanner::next  | scan_start,      | discover_devices(): DeviceAdded with the  | scanForPeripherals(FIPS UUID, duplicates
 *   -> ScanAdvert{addr,psm,rssi} | PEER_DISCOVERED  | FIPS UUID -> sink_discovered(service      | throttled to 1 s) -> sink_discovered;
 *                                |                  | data, rssi); DeviceRemoved -> sink_lost   | peer = fipsd_ble_peer_from_identifier()
 * stop_scanning                  | scan_stop        | drop the discovery stream                 | stopScan
 * local_addr, adapter_name       | local_addr,      | Adapter1.Address, "hciN"                  | hidden (false), "ble0"
 *                                | adapter_name     |                                           |
 * BleStream::send / recv         | stream write /   | the fd fast path: SEQPACKET send/recv     | NSStream via the agent: fd path when the
 *                                | read, or fd      | through libgo                             | agent passes a descriptor, else read/write
 * BleStream::send_mtu / recv_mtu | stream mtu       | BT_SNDMTU / BT_RCVMTU                     | channel MTU from the agent (0: unknown)
 * BleStream::remote_addr         | stream->peer     | BleAddr::from_bluer                       | fipsd_ble_peer_from_identifier(uuid)
 * (stream drop)                  | cancel + close   | core gof_io_cancel_fd, backend close(fd)  | agent close_channel(ch_id)
 * (channel died, nobody reading) | STREAM_CLOSED    | HUP seen by the backend                   | didClose / stream error
 *
 * The macOS agent handshake (socket, version, Bluetooth permission prompt)
 * is backend-internal. Its only outcome the core sees is adapter presence: no
 * agent is present=false, an agent coming back is present=true, and the core
 * then listens, advertises and scans afresh.
 */
#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "channel.h"

#define FIPSD_BLE_NO_DEADLINE UINT64_MAX
#define FIPSD_BLE_ADAPTER_MAX 16u      /* adapter name capacity incl. NUL ("hci0", "ble0") */
#define FIPSD_BLE_SERVICE_DATA_MAX 16u /* PSM service data kept per advert; extra bytes ignored */
#define FIPSD_BLE_GATT_VALUE_MAX 16u   /* PSM characteristic read capacity */
/* FIPS primary service UUID (advert and GATT service) and the GATT PSM
 * characteristic (Apple's CBUUIDL2CAPPSMCharacteristicString). */
#define FIPSD_BLE_SERVICE_UUID "9c90b790-2cc5-42c0-9f87-c9cc40648f4c"
#define FIPSD_BLE_PSM_CHARACTERISTIC_UUID "ABDD3056-28FA-441D-A470-55A75A52553A"
#define FIPSD_BLE_PSM_SERVICE_DATA_UUID16 0x9C90u /* Service Data - 16-bit UUID key */
#define FIPSD_BLE_DEFAULT_PSM 0x0085u
#define FIPSD_BLE_RADIO_ADAPTER "ble0" /* adapter name for radios that hide link addresses */

/* A link address: adapter name and 6-byte device address, rendered as the
 * transport address "adapter/AA:BB:CC:DD:EE:FF" (upstream addr.rs BleAddr). */
typedef struct fipsd_ble_peer {
    char adapter[FIPSD_BLE_ADAPTER_MAX];
    uint8_t device[6];
} fipsd_ble_peer;

typedef struct fipsd_ble_adapter {
    bool present; /* the adapter (or the agent standing in for it) exists */
    bool powered; /* and is powered on: listen/advertise/scan/connect can run */
} fipsd_ble_adapter;

/* What the core asks to be advertised: the FIPS service UUID plus, where the
 * platform can, Service Data keyed on FIPSD_BLE_PSM_SERVICE_DATA_UUID16
 * carrying `service_data` (fipsd_ble_encode_psm(psm), 2 bytes LE). A backend
 * that cannot carry service data (CoreBluetooth) advertises the UUID alone and
 * serves the same bytes from the GATT PSM characteristic. */
typedef struct fipsd_ble_advert {
    uint16_t psm;
    uint8_t service_data[2];
} fipsd_ble_advert;

/* ---- streams: one L2CAP CoC channel -------------------------------------- */
typedef struct fipsd_ble_stream fipsd_ble_stream;
typedef struct fipsd_ble_stream_ops {
    /* Parks the calling fiber until bytes arrive, EOF, cancel or the deadline.
     * Returns the byte count (> 0), 0 for a clean peer close, or -errno. The
     * bytes may be a fragment of an SDU or several SDUs coalesced: the core
     * recovers frame boundaries from the FMP prefix, so a byte-stream backend
     * (NSStream) and a boundary-preserving one (SEQPACKET) both fit. */
    ssize_t (*read)(fipsd_ble_stream *, uint8_t *buf, size_t capacity, uint64_t deadline_ms);
    /* Sends all `len` bytes (one SDU on a boundary-preserving channel) or
     * fails. Parks for flow control until the deadline; never drops. */
    int (*write)(fipsd_ble_stream *, const uint8_t *buf, size_t len, uint64_t deadline_ms);
    /* Prompt, idempotent, any thread, never parks: latches -ECANCELED for the
     * pending and every later read/write on this stream. */
    void (*cancel)(fipsd_ble_stream *);
    /* Exactly once, after cancel and after every read/write caller returned:
     * releases the channel (and its descriptor) and the stream object. */
    void (*close)(fipsd_ble_stream *);
    /* Negotiated L2CAP MTUs, constant; 0 means unknown and the core uses
     * transports.ble mtu (upstream io_radio DEFAULT_BLE_MTU fallback). */
    void (*mtu)(const fipsd_ble_stream *, uint16_t *send_mtu, uint16_t *recv_mtu);
    /* A kernel descriptor carrying the channel, or -1. With one the core never
     * calls read/write: it owns readiness through libgo (gof_io_reset_fd at
     * adoption, gof_recvmsg/gof_sendmsg with deadlines, gof_io_cancel_fd in
     * place of cancel, reset again before close). The backend keeps the
     * descriptor and closes it in close(). */
    int (*fd)(const fipsd_ble_stream *);
} fipsd_ble_stream_ops;
struct fipsd_ble_stream {
    const fipsd_ble_stream_ops *ops;
    fipsd_ble_peer peer; /* the remote end (upstream remote_addr) */
    uint64_t id;         /* backend-unique; names the channel in STREAM_CLOSED */
};

/* ---- events: backend -> core --------------------------------------------- */
typedef enum fipsd_ble_event_kind {
    FIPSD_BLE_PEER_DISCOVERED, /* an advert of the FIPS service UUID was observed */
    FIPSD_BLE_PEER_LOST,       /* the platform forgot an advertiser (BlueZ DeviceRemoved) */
    FIPSD_BLE_INBOUND_STREAM,  /* an accepted L2CAP channel; owns `stream` */
    FIPSD_BLE_STREAM_CLOSED    /* channel `stream_id` died, whether or not a reader is parked */
} fipsd_ble_event_kind;
typedef struct fipsd_ble_event {
    fipsd_ble_event_kind kind;
    fipsd_ble_peer peer;
    bool has_rssi;
    int16_t rssi;             /* dBm */
    uint8_t service_data_len; /* 0: a legacy UUID-only advert, no PSM */
    uint8_t service_data[FIPSD_BLE_SERVICE_DATA_MAX]; /* raw 0x9C90 service data; the core decodes */
    fipsd_ble_stream *stream;
    uint64_t stream_id;
} fipsd_ble_event;

/* The sink a backend reports into, owned by the transport and valid from
 * start until destroy returns. Every call is thread-safe and never blocks:
 * events go through a bounded queue by try-send, and an event that does not
 * fit (or arrives after the transport stopped) is dropped and counted in
 * `dropped`; a dropped inbound stream is closed by the sink. Adapter state is
 * not queued: it is coalesced into the latest value plus a one-slot wake, so a
 * power transition is never lost behind a burst of adverts. */
typedef struct fipsd_ble_sink {
    GoChannel *events, *adapter_wake;
    _Atomic bool open;
    _Atomic uint64_t dropped;
    _Atomic unsigned adapter; /* bit 0 present, bit 1 powered, bit 2 reported, bits 3+ up/down transitions */
} fipsd_ble_sink;
void fipsd_ble_sink_adapter(fipsd_ble_sink *, bool present, bool powered);
bool fipsd_ble_sink_discovered(fipsd_ble_sink *, const fipsd_ble_peer *, const int16_t *rssi,
    const uint8_t *service_data, size_t len);
bool fipsd_ble_sink_lost(fipsd_ble_sink *, const fipsd_ble_peer *);
/* Ownership of the stream always passes to the sink, delivered or not. */
bool fipsd_ble_sink_inbound(fipsd_ble_sink *, fipsd_ble_stream *);
bool fipsd_ble_sink_closed(fipsd_ble_sink *, uint64_t stream_id);

/* ---- the backend ---------------------------------------------------------- */
struct fipsd_runtime;
typedef struct fipsd_ble_backend_ops {
    /* On a fiber, nonblocking: launch the backend's tracked fibers (through
     * rt) and/or foreign threads, and return. Must report the adapter state
     * through fipsd_ble_sink_adapter at least once, now or as soon as it is
     * known; until then the core treats the adapter as absent. */
    int (*start)(void *self, struct fipsd_runtime *rt, fipsd_ble_sink *sink);
    /* Prompt and idempotent, never joins: cancels every parked operation
     * (-ECANCELED), stops advertising and scanning, closes the listener and
     * stops reporting. Its fibers exit by runtime join. */
    void (*stop)(void *self);
    /* After runtime join: joins foreign threads and frees the backend. */
    void (*destroy)(void *self);
    /* Current adapter facts, non-blocking (a cached value is fine). */
    fipsd_ble_adapter (*adapter)(void *self);
    /* (Re)open the L2CAP listener asking for `requested`, and report the PSM
     * actually bound: a platform that assigns PSMs (CoreBluetooth, Android)
     * reports its own, which is what gets advertised. Accepted channels arrive
     * as INBOUND_STREAM. A listener does not survive adapter power-off: the
     * core calls listen again on every transition to powered. */
    int (*listen)(void *self, uint16_t requested, uint64_t deadline_ms, uint16_t *bound);
    int (*advertise)(void *self, const fipsd_ble_advert *, uint64_t deadline_ms);
    int (*advertise_stop)(void *self);
    /* Passive scan for the FIPS service UUID; adverts arrive as
     * PEER_DISCOVERED. Neither advertising nor scanning is resumed by the
     * backend after power-off: the core restarts both. */
    int (*scan_start)(void *self, uint64_t deadline_ms);
    int (*scan_stop)(void *self);
    /* Read the GATT PSM characteristic of the peer's FIPS service into
     * `value` (raw bytes: the core decodes and rejects zero). NULL, or
     * -ENOTSUP, when the backend cannot. Only called for a peer whose advert
     * carried no PSM, never cached by the core. */
    int (*read_psm)(void *self, const fipsd_ble_peer *, uint64_t deadline_ms,
        uint8_t *value, size_t capacity, size_t *len);
    /* Dial an L2CAP CoC channel. Ownership of *out passes to the caller. */
    int (*connect)(void *self, const fipsd_ble_peer *, uint16_t psm, uint64_t deadline_ms,
        fipsd_ble_stream **out);
    /* Optional: the adapter's own link address (false when hidden). */
    bool (*local_addr)(void *self, fipsd_ble_peer *out);
    const char *(*adapter_name)(void *self);
} fipsd_ble_backend_ops;
typedef struct fipsd_ble_backend {
    const fipsd_ble_backend_ops *ops;
    void *self;
} fipsd_ble_backend;
bool fipsd_ble_backend_valid(const fipsd_ble_backend *);
#endif
