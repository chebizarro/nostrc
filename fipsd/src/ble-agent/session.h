#ifndef FIPS_BLE_AGENT_SESSION_H
#define FIPS_BLE_AGENT_SESSION_H
/* The fips BLE agent's end of the agent socket (upstream
 * transport/ble/io_macos/agent.rs): it lends a radio to the daemon listening
 * on the socket, reconnecting whenever the daemon goes away, and proxies
 * between them in the agent protocol (transport/ble/agent_proto.h): commands
 * in; adverts, channels and channel bytes out.
 *
 * Written against a radio vtable, not CoreBluetooth: radio.m implements it
 * over CBCentralManager/CBPeripheralManager, and the fipsd tests over a mock
 * radio, which is how this is tested end to end on any host. Plain POSIX
 * threads and blocking sockets; no fipsd or libgo dependency.
 *
 * The radio is the agent's for its whole life; a session owns its channels,
 * which end with it, and lasts only while the radio is up on the PSM its
 * hello reported. The session thread never waits on a channel: outbound
 * bytes queue per channel, bounded by the send credit granted, so a peer that
 * stops taking bytes stalls only its own channel. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct fba_channel fba_channel; /* a radio's L2CAP channel; opaque here */
typedef struct fba_radio_ops {
    /* Commands, from the session thread with no lock held; each returns at
     * once. A dial is answered exactly once, with its token, through
     * fba_agent_dialled (the radio bounds it with its own timeout); tokens
     * are agent-wide, so a late answer never lands on a later session. */
    void (*connect)(void *radio, int64_t token, const uint8_t device[6], uint16_t psm);
    void (*start_advertising)(void *radio, uint16_t psm);
    void (*stop_advertising)(void *radio);
    void (*start_scanning)(void *radio);
    void (*stop_scanning)(void *radio);
    /* Channel bytes, blocking, each on the channel's own pump thread: read
     * returns > 0 bytes, 0 at the end, < 0 on error; write returns how many
     * bytes it took (> 0) or < 0. */
    ssize_t (*read)(fba_channel *, uint8_t *buf, size_t cap);
    ssize_t (*write)(fba_channel *, const uint8_t *buf, size_t len);
    /* Any thread (also inside a report below), idempotent, never blocks: ends
     * the channel on the radio, unblocking a read or write in progress. */
    void (*close)(fba_channel *);
    /* Once, after close and after both pumps returned (any thread). */
    void (*release)(fba_channel *);
} fba_radio_ops;

enum { FBA_LOG_DEBUG, FBA_LOG_INFO, FBA_LOG_WARN, FBA_LOG_ERROR };
typedef struct fba_agent_config {
    const char *socket_path;                            /* the daemon's socket */
    bool check_uid;                                     /* serve only a socket answered by daemon_uid */
    uid_t daemon_uid;
    uint32_t reconnect_ms, max_reconnect_ms, stable_ms; /* 0: upstream's 2000, 60000, 30000 */
    void (*log)(void *ctx, int level, const char *message);
    void *log_ctx;
} fba_agent_config;
typedef struct fba_agent fba_agent;
fba_agent *fba_agent_create(const fba_agent_config *, const fba_radio_ops *, void *radio);
/* Serves the daemon on the calling thread until fba_agent_stop: waits for the
 * radio to be up, connects, says hello, serves, and reconnects with backoff
 * (longer while the daemon turns it away: BUSY, another protocol version). */
void fba_agent_run(fba_agent *);
void fba_agent_stop(fba_agent *); /* any thread; run returns */
void fba_agent_destroy(fba_agent *); /* after run returned: waits for the channel pumps */

/* Radio reports: any thread, never block. Ownership of a channel passes; one
 * that no session wants is closed and released. */
void fba_agent_radio(fba_agent *, bool up, uint16_t psm); /* up: the listener's PSM, 0 with none */
void fba_agent_inbound(fba_agent *, fba_channel *, const uint8_t device[6], uint16_t mtu);
void fba_agent_dialled(fba_agent *, int64_t token, const uint8_t device[6], fba_channel *, uint16_t mtu); /* NULL: failed */
void fba_agent_advert(fba_agent *, const uint8_t device[6], uint16_t psm, int16_t rssi); /* psm 0 / INT16_MIN rssi: none */

typedef struct fba_agent_stats {
    uint64_t sessions, busy, overruns, channels_opened;
    size_t channels; /* open in the live session */
    bool connected;
} fba_agent_stats;
fba_agent_stats fba_agent_stats_read(fba_agent *);
#endif
