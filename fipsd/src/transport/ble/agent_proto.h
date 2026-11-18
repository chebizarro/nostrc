#ifndef FIPSD_BLE_AGENT_PROTO_H
#define FIPSD_BLE_AGENT_PROTO_H
/* The BLE agent socket protocol (upstream transport/ble/io_macos/agent_proto.rs,
 * version 2), byte for byte. macOS lends CoreBluetooth only to processes in a
 * login session, never to a root launchd daemon, so the radio lives in a
 * per-user agent (fips-ble-agent) and the transport in fipsd. The daemon binds
 * the socket and the agent connects to it:
 *   daemon -> agent: radio commands, and outbound channel bytes (SEND);
 *   agent -> daemon: HELLO once, then accepted and dialled channels, adverts,
 *     inbound bytes (RECV), send credit (CREDIT) and closures (CLOSED).
 * Channels are named by the agent's identifier, allocated when the channel
 * opens. Outbound bytes are credit-based per channel: the agent grants
 * FIPSD_BLE_AGENT_CHANNEL_WINDOW when a channel opens and returns credit for
 * each packet its radio took; the daemon sends while it holds credit (a packet
 * goes whole once any is left) and an overrun closes the channel.
 *
 * Framing: `len: u32 BE | tag: u8 | payload`, len counting tag and payload.
 * Integers are big-endian; a link address is its six device bytes under the
 * shared "ble0" adapter label. Decoding ignores trailing bytes, as upstream.
 *
 * Plain C with no fipsd dependency: compiled into fipsd and fips-ble-agent. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FIPSD_BLE_AGENT_VERSION 2u
#define FIPSD_BLE_AGENT_HEADER 4u                    /* the u32 length */
#define FIPSD_BLE_AGENT_MAX_FRAME (128u * 1024u)     /* largest body (tag included) accepted */
#define FIPSD_BLE_AGENT_MAX_HELLO_FRAME 16u          /* largest first body from a new connection */
#define FIPSD_BLE_AGENT_CHANNEL_WINDOW (32u * 1024u) /* upstream agent.rs CHANNEL_WINDOW */
#define FIPSD_BLE_AGENT_NO_PSM 0u                    /* SCAN: the advert carried no PSM */
#define FIPSD_BLE_AGENT_NO_RSSI INT16_MIN            /* SCAN: no RSSI reading */
#define FIPSD_BLE_AGENT_SOCKET_PATH "/var/run/fips/ble-agent.sock" /* upstream AGENT_SOCKET_PATH */
/* The most data a sender puts in one SEND or RECV: with the channel id it
 * stays well under FIPSD_BLE_AGENT_MAX_FRAME. */
#define FIPSD_BLE_AGENT_MAX_DATA (64u * 1024u)

typedef enum fipsd_ble_agent_tag {
    /* daemon -> agent */
    FIPSD_BLE_AGENT_CONNECT = 1,           /* id, addr, psm: answered by CONNECT_RESULT */
    FIPSD_BLE_AGENT_START_ADVERTISING = 2, /* psm */
    FIPSD_BLE_AGENT_STOP_ADVERTISING = 3,
    FIPSD_BLE_AGENT_START_SCANNING = 4,
    FIPSD_BLE_AGENT_STOP_SCANNING = 5,
    FIPSD_BLE_AGENT_CLOSE = 6,             /* ch: the daemon dropped the channel */
    FIPSD_BLE_AGENT_SEND = 7,              /* ch, data: spends len of the credit */
    FIPSD_BLE_AGENT_BUSY = 8,              /* another agent lends the radio */
    /* agent -> daemon */
    FIPSD_BLE_AGENT_HELLO = 16,            /* version, psm (the listener's; 0: none) */
    FIPSD_BLE_AGENT_INBOUND = 17,          /* ch, addr, mtu */
    FIPSD_BLE_AGENT_CONNECT_RESULT = 18,   /* id, ok, ch, addr, mtu (ch/mtu meaningful when ok) */
    FIPSD_BLE_AGENT_SCAN = 19,             /* addr, psm (0: none), rssi (INT16_MIN: none) */
    FIPSD_BLE_AGENT_RECV = 20,             /* ch, data */
    FIPSD_BLE_AGENT_CLOSED = 21,           /* ch ended on the radio side */
    FIPSD_BLE_AGENT_CREDIT = 22            /* ch, bytes */
} fipsd_ble_agent_tag;

/* One decoded or to-be-encoded frame; only the tag's fields are meaningful. */
typedef struct fipsd_ble_agent_frame {
    fipsd_ble_agent_tag tag;
    int64_t id;
    uint32_t ch, bytes;
    uint16_t psm, mtu;
    int16_t rssi;
    uint8_t version;
    bool ok;
    uint8_t addr[6];
    const uint8_t *data; /* SEND/RECV; decode borrows the body */
    size_t len;
} fipsd_ble_agent_frame;

/* Encoded size of `f` including the header, or 0 for an unknown tag or a body
 * beyond FIPSD_BLE_AGENT_MAX_FRAME. */
size_t fipsd_ble_agent_encoded_len(const fipsd_ble_agent_frame *f);
/* Encode `f` into `out`; returns the bytes written, or 0 (also when `cap` is short). */
size_t fipsd_ble_agent_encode(const fipsd_ble_agent_frame *f, uint8_t *out, size_t cap);
/* The body length a header announces: true with *len, false for an empty
 * frame or one longer than `max` (checked before anything is allocated). */
bool fipsd_ble_agent_frame_len(const uint8_t header[FIPSD_BLE_AGENT_HEADER], size_t max, size_t *len);
/* Decode a body (tag and payload): false when empty, truncated or of an
 * unknown tag. SEND/RECV data points into `body`. */
bool fipsd_ble_agent_decode(const uint8_t *body, size_t len, fipsd_ble_agent_frame *out);
/* The link address a CoreBluetooth peer identifier maps to (upstream
 * io_macos::peer_addr, 64-bit FNV-1a, locally administered unicast); the same
 * mapping as fipsd_ble_peer_from_identifier. */
void fipsd_ble_agent_device_from_identifier(const uint8_t identifier[16], uint8_t out[6]);
#endif
