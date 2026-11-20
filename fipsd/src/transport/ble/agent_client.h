#ifndef FIPSD_BLE_AGENT_CLIENT_H
#define FIPSD_BLE_AGENT_CLIENT_H
/* fipsd's CoreBluetooth backend: backend.h over the fips-ble-agent socket
 * (upstream transport/ble/io_macos/agent_server.rs plus the io_radio bridge it
 * drives). fipsd is the client of the agent's radio; on the socket it is the
 * listener, as upstream: the root daemon cannot own the user's Bluetooth, so
 * it binds FIPSD_BLE_AGENT_SOCKET_PATH (group `fips`, mode 0770, the control
 * socket policy) and the agent in a login session connects, says HELLO with
 * its listener PSM and lends the radio for as long as it stays connected.
 *
 *   start      bind and accept; adapter absent until an agent says hello
 *   adapter    present and powered while an agent holds the radio; one at a
 *              time, a second is told BUSY and dropped; a reconnect is a
 *              replaced radio (the core listens, advertises, scans afresh)
 *   listen     the PSM from the hello (OS-assigned by publishL2CAPChannel);
 *              an agent without a listener (PSM 0) reports `requested`, as
 *              upstream RadioIo::listen, and the advert asks it to publish
 *   advertise  START_ADVERTISING; the agent advertises the UUID alone and
 *              serves the PSM from the GATT characteristic
 *   scan       START/STOP_SCANNING; SCAN frames become PEER_DISCOVERED
 *   read_psm   none: the agent reads a UUID-only peer's GATT PSM inside the
 *              dial (upstream DialPlan), so the core dials its fallback PSM
 *   connect    CONNECT / CONNECT_RESULT; ok=false carries no reason and is
 *              -EIO, an unanswered dial is -ETIMEDOUT at the deadline
 *   streams    a framed relay, as upstream: no descriptor crosses the
 *              socket. RECV bytes queue per channel (the reader waits for
 *              room rather than drop: they are a byte stream); SEND spends
 *              the channel's credit and parks for CREDIT; CLOSED is EOF after
 *              the queued bytes; close() sends CLOSE. An agent that leaves
 *              resets every channel (STREAM_CLOSED) and fails parked dials
 *              with -ENODEV.
 * Fibers only (accept, one reader and one writer per connection), all
 * spawned through the runtime; no lock is held across a park. */
#include "transport/ble/backend.h"
#include "transport/ble/agent_proto.h"

#define FIPSD_BLE_AGENT_SOCKET_ENV "FIPSD_BLE_AGENT_SOCKET" /* overrides the socket path */
#define FIPSD_BLE_AGENT_HELLO_TIMEOUT_MS 5000u /* upstream HELLO_TIMEOUT: a connection's whole budget to say hello */
#define FIPSD_BLE_AGENT_BUSY_WRITE_MS 1000u    /* upstream BUSY_WRITE_TIMEOUT */
#define FIPSD_BLE_AGENT_PENDING_MAX 8u         /* connections not yet past their hello */
#define FIPSD_BLE_AGENT_RX_DEPTH 256u          /* RECV chunks queued per channel */

typedef struct fipsd_ble_agent_config {
    const char *socket_path;   /* NULL: $FIPSD_BLE_AGENT_SOCKET, else FIPSD_BLE_AGENT_SOCKET_PATH */
    const char *group;         /* socket group: NULL "fips"; "" the daemon's own group */
    uint32_t hello_timeout_ms; /* 0: FIPSD_BLE_AGENT_HELLO_TIMEOUT_MS */
} fipsd_ble_agent_config;
/* The backend; nothing is bound until start. Resolves the socket group, so
 * call it on the starting thread (config registration), not a worker. */
bool fipsd_ble_agent_backend(const fipsd_ble_agent_config *, fipsd_ble_backend *out);

/* Observation for tests and logs, on the backend's `self`. */
typedef struct fipsd_ble_agent_stats {
    uint64_t sessions;      /* agents that took the radio */
    uint64_t refused_busy;  /* told BUSY: another agent held the radio */
    uint64_t refused_hello; /* no valid hello in time, or another protocol version */
    size_t channels;        /* channels registered with the live agent */
    bool live;              /* an agent holds the radio */
    uint16_t psm;           /* its hello PSM */
} fipsd_ble_agent_stats;
fipsd_ble_agent_stats fipsd_ble_agent_stats_read(void *self);
/* One-slot wake on every change the stats show: an event to wait on, never to poll. */
GoChannel *fipsd_ble_agent_changed(void *self);
const char *fipsd_ble_agent_socket(void *self);
#endif
