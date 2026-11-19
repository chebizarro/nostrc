#ifndef FIPSD_BLE_INTERNAL_H
#define FIPSD_BLE_INTERNAL_H
#include "transport/ble/ble.h"
#include "transport/stream.h"
#include "libgo/fiber.h"
#include "select.h"

/* Upstream constants (mod.rs, pool.rs, recall.rs, stream_read.rs). */
#define BLE_MIN_RECV_CHUNK 64u          /* MIN_RECV_CHUNK */
#define BLE_PUBKEY_PREFIX 0x00u         /* PUBKEY_EXCHANGE_PREFIX */
#define BLE_PUBKEY_SIZE 33u             /* PUBKEY_EXCHANGE_SIZE: [0x00][x-only key] */
#define BLE_INBOUND_INFLIGHT 8u         /* INBOUND_HANDSHAKE_INFLIGHT */
#define BLE_MAX_PENDING_PROBES 32u      /* MAX_PENDING_PROBES */
#define BLE_MAX_PROBE_BACKOFF_SHIFT 5u  /* MAX_PROBE_BACKOFF_SHIFT */
#define BLE_MAX_RECALLED 64u            /* MAX_RECALLED_ADDRS */
#define BLE_RECALL_MAX_AGE_MS 600000u   /* RECALLED_ADDR_MAX_AGE */
#define BLE_BOUND_LOG_HOLD_MS 60000u    /* pool.rs BOUND_LOG_HOLD */
#define BLE_MAX_CHALLENGERS 2u          /* not upstream: pending challengers beside one verified link */
#define BLE_WRITER_DRAIN_MS 5000u       /* stream.rs WRITER_DRAIN_TIMEOUT */
#define BLE_MIN_VERIFY_GRACE_MS 10000u  /* DEFAULT_BLE_CONNECT_TIMEOUT_MS */
#define BLE_PROBE_QUEUE 64u             /* the scanner fan-in to the probe loop */
#define BLE_EVENT_QUEUE 256u            /* io_radio CHANNEL_CAP */

/* ---- pool.c: the link pool, arbitration, probe book and address memory.
 * Pure state over caller-supplied time; the transport holds its mutex. ---- */
typedef struct ble_link {
    fips_transport_addr addr;   /* pool key: the rendered link address */
    fipsd_ble_peer peer;
    uint64_t id;                /* stream.rs ConnId: remove_own guard */
    bool outbound, has_node, verified, proven;
    /* Not upstream: a newcomer that won the tie-break against a verified link
     * at its own device address. It waits beside the link under its own
     * address (ble_challenger_addr), so the node's Noise runs on it apart from
     * the verified link's traffic; it replaces the link only once the node
     * authenticates it as the claimed node (ble_pool_prove). */
    bool challenger;
    fips_node_addr node;        /* the node the pubkey exchange claimed */
    uint64_t established_ms, grace_from_ms, held_from_ms;
    uint16_t send_mtu, recv_mtu;
} ble_link;
uint16_t ble_link_mtu(const ble_link *); /* effective: min(send, recv) */
typedef enum ble_insert_kind { BLE_INSERT_ADMITTED, BLE_INSERT_REFUSED, BLE_INSERT_OCCUPIED } ble_insert_kind;
typedef struct ble_insert {
    ble_insert_kind kind;
    ble_link *evicted;  /* ADMITTED: the unverified link removed to make room */
    bool first;         /* REFUSED: first refusal of a run (warn once) */
    bool has_ended;     /* a refusal run this outcome closed, with its count */
    uint64_t ended;
} ble_insert;
typedef enum ble_verify { BLE_VERIFY_MARKED, BLE_VERIFY_MISMATCH, BLE_VERIFY_ABSENT } ble_verify;
typedef struct ble_pool {
    ble_link **items;
    size_t count, max;
    uint64_t verify_grace_ms, refusals, last_refusal_ms;
    bool has_refusal;
} ble_pool;
int ble_pool_init(ble_pool *, size_t max, uint64_t verify_grace_ms);
void ble_pool_free(ble_pool *);
ble_link *ble_pool_get(const ble_pool *, const fips_transport_addr *);
ble_insert ble_pool_insert(ble_pool *, ble_link *, uint64_t now);
bool ble_pool_remove(ble_pool *, const ble_link *);
ble_link *ble_pool_remove_own(ble_pool *, const fips_transport_addr *, uint64_t id);
ble_link *ble_pool_find_verified(const ble_pool *, const fips_node_addr *);
bool ble_pool_claims(const ble_pool *, const fips_node_addr *);
ble_verify ble_pool_mark_verified(ble_pool *, const fips_transport_addr *, const fips_node_addr *, size_t *demoted);
bool ble_pool_clear_verified(ble_pool *, const fips_transport_addr *, const fips_node_addr *, bool regrace, uint64_t now);
void ble_pool_hold(ble_pool *, const fips_transport_addr *, uint64_t now);
size_t ble_pool_unproven(const ble_pool *, uint64_t now, uint64_t timeout_ms, ble_link **out, size_t capacity);
ble_link *ble_pool_victim(const ble_pool *, uint64_t now);
/* Whether any link, challengers included, is at the device address. */
bool ble_pool_peer_pooled(const ble_pool *, const fipsd_ble_peer *);
size_t ble_pool_challengers(const ble_pool *, const fipsd_ble_peer *);
ble_link *ble_pool_challenger_at(const ble_pool *, const fipsd_ble_peer *); /* any one, or NULL */
/* The node authenticated `challenger` as `n`: PROMOTED (it is an ordinary
 * verified link now and replaces what shares its device address: drain
 * ble_pool_displaced), or MISMATCH (a different identity: the caller drops it). */
typedef enum ble_proof { BLE_PROOF_PROMOTED, BLE_PROOF_MISMATCH, BLE_PROOF_NOT_CHALLENGER } ble_proof;
ble_proof ble_pool_prove(ble_pool *, ble_link *challenger, const fips_node_addr *n);
/* Another link at the winner's device address, or NULL. */
ble_link *ble_pool_displaced(const ble_pool *, const ble_link *winner);
/* A challenger's node-facing address: the device address and the link id
 * ("hci0/AA:BB:CC:DD:EE:FF#7"), which fipsd_ble_peer_parse refuses, so no
 * dial or advert can ever name it. */
bool ble_challenger_addr(const fipsd_ble_peer *, uint64_t id, fips_transport_addr *out);

/* BLE_CHALLENGE: the newcomer wins the tie-break against a verified link, which
 * stays authoritative (existing); admit the newcomer as its challenger. */
typedef enum ble_admission_kind { BLE_ADMIT, BLE_DECLINE, BLE_REPLACE, BLE_OCCUPIED, BLE_CHALLENGE } ble_admission_kind;
typedef struct ble_admission { ble_admission_kind kind; ble_link *existing; } ble_admission;
bool ble_newcomer_wins(bool local_is_smaller, bool newcomer_outbound, bool incumbent_outbound);
ble_admission ble_arbitrate(const ble_pool *, const fips_node_addr *local, const fips_node_addr *peer,
    const fips_transport_addr *ta, bool newcomer_outbound, uint64_t tie_window_ms, uint64_t now);

typedef struct ble_probe { fipsd_ble_peer peer; uint32_t failures; uint64_t next_attempt_ms; } ble_probe;
typedef struct ble_probes { ble_probe entries[BLE_MAX_PENDING_PROBES]; size_t count; uint64_t cooldown_ms; } ble_probes;
typedef bool (*ble_peer_pred)(void *, const fipsd_ble_peer *);
void ble_probes_init(ble_probes *, uint64_t cooldown_ms);
bool ble_probes_contains(const ble_probes *, const fipsd_ble_peer *);
void ble_probes_observe(ble_probes *, const fipsd_ble_peer *, uint64_t now);
bool ble_probes_is_due(const ble_probes *, const fipsd_ble_peer *, uint64_t now);
void ble_probes_mark_attempt(ble_probes *, const fipsd_ble_peer *, uint64_t now);
uint32_t ble_probes_record_failure(ble_probes *, const fipsd_ble_peer *, uint64_t now);
void ble_probes_resolve(ble_probes *, const fipsd_ble_peer *);
void ble_probes_drop(ble_probes *, ble_peer_pred, void *);
bool ble_probes_next_due(ble_probes *, uint64_t now, fipsd_ble_peer *out);

typedef struct ble_recall_entry {
    bool used;
    fipsd_ble_peer peer;
    uint64_t seen_ms, seq;
    uint16_t psm;
    fips_node_addr node;
} ble_recall_entry;
typedef struct ble_recall { ble_recall_entry *entries; size_t capacity; uint64_t next_seq; } ble_recall;
int ble_recall_init(ble_recall *, size_t capacity);
void ble_recall_free(ble_recall *);
const ble_recall_entry *ble_recall_get(const ble_recall *, const fipsd_ble_peer *);
/* Insert or refresh; evicts the oldest non-live entries beyond
 * BLE_MAX_RECALLED and returns how many (their addresses into `evicted`). */
size_t ble_recall_note(ble_recall *, const fipsd_ble_peer *, uint16_t psm, const fips_node_addr *node,
    uint64_t now, ble_peer_pred live, void *);
bool ble_recall_remove(ble_recall *, const fipsd_ble_peer *);
void ble_recall_prune(ble_recall *, uint64_t now, ble_peer_pred live, void *);
size_t ble_recall_len(const ble_recall *);

/* ---- stream_read.c: stream I/O over either the ops or the fd fast path,
 * and the one reader a connection keeps across the pubkey exchange and the
 * receive loop (upstream BleStreamRead). ---- */
ssize_t ble_io_read(fipsd_ble_stream *, int fd, uint8_t *, size_t, uint64_t deadline);
int ble_io_write(fipsd_ble_stream *, int fd, const uint8_t *, size_t, uint64_t deadline);
void ble_io_cancel(fipsd_ble_stream *, int fd);
void ble_io_close(fipsd_ble_stream *, int fd); /* after every caller joined */
int ble_io_adopt(fipsd_ble_stream *, int *fd);
void ble_stream_mtu(const fipsd_ble_stream *, uint16_t fallback, uint16_t *send, uint16_t *recv);
typedef struct ble_reader {
    fipsd_ble_stream *stream;
    int fd;
    uint8_t *chunk;
    size_t capacity, pos, len;
    bool eof;
} ble_reader;
int ble_reader_init(ble_reader *, fipsd_ble_stream *, int fd, uint16_t recv_mtu);
void ble_reader_free(ble_reader *);
size_t ble_reader_buffered(const ble_reader *);
/* 0, or -EPIPE for EOF before n bytes, or the I/O error. */
int ble_reader_read_exact(ble_reader *, uint8_t *, size_t, uint64_t deadline);
/* One FMP frame into the framer's buffer: 1 complete (*size), 0 clean EOF at
 * a frame boundary, or -errno (-EPROTO malformed, oversize or truncated). */
int ble_reader_frame(ble_reader *, fipsd_stream *, uint64_t deadline, size_t *size);

/* ---- stats.c ---- */
#define BLE_STAT(u, name) atomic_fetch_add_explicit(&(u)->shared.stats.name, 1, memory_order_relaxed)

/* ---- ble.c: the transport ---- */
typedef struct ble_instance ble_instance;
typedef struct ble_frame { size_t len; uint8_t bytes[]; } ble_frame;
typedef struct ble_conn {
    ble_link link; /* first: the pool holds &conn->link */
    ble_instance *owner;
    struct ble_conn *next;
    fipsd_ble_stream *stream;
    uint64_t stream_id; /* names the channel in STREAM_CLOSED */
    int fd;
    ble_reader reader;
    bool pooled, closing, deliberate, notifying; /* owner.mu */
    /* stop: teardown; close: deliberate close (drain the writer); notify_stop
     * cancels a parked DISCONNECTED; notified closes once it was published. */
    GoChannel *stop, *close, *notify_stop, *notified, *reader_done, *writer_done, *queue;
} ble_conn;
/* A stream between its arrival and the pool (pubkey exchange, arbitration):
 * listed so stop, adapter loss and the inbound budget can cancel it. */
typedef struct ble_exchange {
    struct ble_exchange *next;
    fipsd_ble_stream *stream;
    int fd;
    bool inbound, aborted;
} ble_exchange;
typedef struct ble_dial { struct ble_dial *next; fips_transport_addr addr; bool abandoned; } ble_dial;
typedef struct ble_advert { bool lost, has_psm; uint16_t psm; fipsd_ble_peer peer; } ble_advert;
struct ble_instance {
    fipsd_transport transport;
    fipsd_transport_shared shared;
    fipsd_ble_config config;
    char name[128], adapter[FIPSD_BLE_ADAPTER_MAX];
    fipsd_ble_backend backend;
    fipsd_ble_sink sink;
    fipsd_runtime *runtime;
    GoContext *root;
    GoChannel *events, *stop, *bound, *changed, *adverts;
    bool has_identity;
    fips_xonly_pubkey local_key;
    fips_node_addr local_node;
    const fips_secp *secp;
    nsync_mu mu; /* guards everything below; never held across a park */
    ble_pool pool;
    ble_conn *conns;
    ble_exchange *exchanges;
    ble_dial *dials;
    ble_recall learned_psm, known_node;
    ble_probes probes;
    size_t inbound_exchanges;
    bool started;
    _Atomic bool present; /* written under mu, read anywhere */
    uint16_t listener_psm;
};
uint64_t ble_now(void);
#endif
