#ifndef FIPS_TRANSPORT_TYPES_H
#define FIPS_TRANSPORT_TYPES_H

#include <fips/host.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fips_transport_id { uint32_t value; } fips_transport_id;
typedef struct fips_link_id { uint64_t value; } fips_link_id;

typedef enum fips_link_direction {
    FIPS_LINK_OUTBOUND,
    FIPS_LINK_INBOUND
} fips_link_direction;

/* Upstream TransportAddr is opaque bytes plus a MAC display hint, NOT a
 * transport-kind enum. Upstream is unbounded; this C port deliberately caps
 * addresses at 256 bytes to keep assignment a cheap, allocation-free POD copy.
 * Oversized constructors/parsers return FIPS_ERR_RANGE without changing out.
 * UDP/TCP/Tor/Nym/BLE text remains opaque; resolution/validation belongs to the
 * host transport. There is no extra wire envelope: as_bytes is the encoding.
 * Only the active bytes participate in equality/hash, never mac or padding.
 * Construct via the functions below; len <= capacity and mac implies len == 6.
 */
#define FIPS_TRANSPORT_ADDR_CAPACITY 256
#define FIPS_TRANSPORT_HASH_KEY_BYTES 16

typedef struct fips_transport_addr {
    uint8_t bytes[FIPS_TRANSPORT_ADDR_CAPACITY];
    uint16_t len;
    bool mac;
} fips_transport_addr;

typedef enum fips_transport_addr_parse_mode {
    FIPS_TRANSPORT_ADDR_PARSE_TEXT,
    FIPS_TRANSPORT_ADDR_PARSE_MAC,
    FIPS_TRANSPORT_ADDR_PARSE_HEX
} fips_transport_addr_parse_mode;

/* All format/debug functions write NUL-terminated text and its byte length
 * (excluding NUL); capacity must include NUL. Embedded UTF-8 NULs are preserved:
 * the returned length, not strlen, is authoritative. Every fallible operation
 * leaves all outputs/buffers unchanged on failure. Malformed text, invalid
 * encodings and insufficient capacity use FIPS_ERR_RANGE, as in host.h.
 * Required pointers are caller preconditions; zero-length inputs may be NULL.
 */
FIPS_CORE_API fips_transport_id fips_transport_id_new(uint32_t id);
FIPS_CORE_API uint32_t fips_transport_id_as_u32(fips_transport_id id);
FIPS_CORE_API bool fips_transport_id_equal(fips_transport_id a, fips_transport_id b);
FIPS_CORE_API fips_status fips_transport_id_format(fips_transport_id id,
    char *buf, size_t capacity, size_t *len);
FIPS_CORE_API fips_link_id fips_link_id_new(uint64_t id);
FIPS_CORE_API uint64_t fips_link_id_as_u64(fips_link_id id);
FIPS_CORE_API bool fips_link_id_equal(fips_link_id a, fips_link_id b);
FIPS_CORE_API fips_status fips_link_id_format(fips_link_id id,
    char *buf, size_t capacity, size_t *len);
FIPS_CORE_API fips_status fips_link_direction_format(fips_link_direction direction,
    char *buf, size_t capacity, size_t *len);

FIPS_CORE_API fips_status fips_transport_addr_new(const uint8_t *bytes,
    size_t len, fips_transport_addr *out);
FIPS_CORE_API fips_status fips_transport_addr_from_bytes(const uint8_t *bytes,
    size_t len, fips_transport_addr *out);
FIPS_CORE_API fips_status fips_transport_addr_from_string(const char *text,
    size_t len, fips_transport_addr *out);
FIPS_CORE_API fips_transport_addr fips_transport_addr_from_mac(const uint8_t mac[6]);
/* Numeric IP octets in network order: ip_len must be 4 or 16. IPv6 scope_id is
 * rendered as a decimal %scope (zero omitted); IPv4 ignores scope_id. No OS,
 * DNS or platform socket types are used. Matches Rust SocketAddr Display,
 * including zero compression and dotted IPv4-mapped IPv6 addresses.
 */
FIPS_CORE_API fips_status fips_transport_addr_from_socket_addr(const uint8_t *ip,
    size_t ip_len, uint16_t port, uint32_t scope_id, fips_transport_addr *out);
FIPS_CORE_API const uint8_t *fips_transport_addr_as_bytes(const fips_transport_addr *addr);
/* Borrowed UTF-8 slice, not necessarily NUL-terminated. MAC hints do not affect
 * as_str; invalid UTF-8 returns FIPS_ERR_RANGE and leaves both outputs alone.
 */
FIPS_CORE_API fips_status fips_transport_addr_as_str(const fips_transport_addr *addr,
    const char **text, size_t *len);
FIPS_CORE_API size_t fips_transport_addr_len(const fips_transport_addr *addr);
FIPS_CORE_API bool fips_transport_addr_is_empty(const fips_transport_addr *addr);
FIPS_CORE_API bool fips_transport_addr_equal(const fips_transport_addr *a,
    const fips_transport_addr *b);
/* Display: explicit MAC -> lowercase xx:xx:xx:xx:xx:xx; otherwise valid UTF-8
 * verbatim, invalid UTF-8 of length 6 as MAC, and other binary as bare hex.
 */
FIPS_CORE_API fips_status fips_transport_addr_format(const fips_transport_addr *addr,
    char *buf, size_t capacity, size_t *len);
FIPS_CORE_API fips_status fips_transport_addr_debug(const fips_transport_addr *addr,
    char *buf, size_t capacity, size_t *len);
/* Display is ambiguous (e.g. text "ff" vs byte 0xff), so parsing NEVER guesses.
 * TEXT accepts any UTF-8; MAC requires six colon-separated hex byte pairs and
 * sets the MAC hint; HEX requires an even count of bare hex digits (empty is
 * valid). MAC/HEX accept either case. Select the original display's mode to
 * round-trip bytes. HEX does not promise to display hex if decoded UTF-8 is valid.
 */
FIPS_CORE_API fips_status fips_transport_addr_parse(const char *text, size_t len,
    fips_transport_addr_parse_mode mode, fips_transport_addr *out);

/* Keyed SipHash-2-4 helpers, with a host-supplied 16-byte key (no RNG/init/I/O).
 * These are C hash-table helpers, NOT a wire format or Rust Hasher output
 * contract. Equal values hash identically with the same key. Use an unpredictable
 * host key for tables indexed by peer input. IDs hash their little-endian value.
 */
FIPS_CORE_API uint64_t fips_transport_id_hash(fips_transport_id id,
    const uint8_t key[FIPS_TRANSPORT_HASH_KEY_BYTES]);
FIPS_CORE_API uint64_t fips_link_id_hash(fips_link_id id,
    const uint8_t key[FIPS_TRANSPORT_HASH_KEY_BYTES]);
FIPS_CORE_API uint64_t fips_transport_addr_hash(const fips_transport_addr *addr,
    const uint8_t key[FIPS_TRANSPORT_HASH_KEY_BYTES]);

typedef struct fips_link_stats {
    uint64_t packets_sent;
    uint64_t packets_recv;
    uint64_t bytes_sent;
    uint64_t bytes_recv;
    uint64_t last_recv_ms;
    uint64_t rtt_estimate; /* Milliseconds; zero means no estimate. */
    float loss_rate;
    uint64_t throughput_estimate;
} fips_link_stats;

FIPS_CORE_API fips_link_stats fips_link_stats_new(void);
/* Counters wrap modulo 2^64, matching unsigned release-mode Rust arithmetic. */
FIPS_CORE_API void fips_link_stats_record_sent(fips_link_stats *stats, size_t bytes);
FIPS_CORE_API void fips_link_stats_record_recv(fips_link_stats *stats, size_t bytes,
    uint64_t timestamp_ms);
/* Option<Duration>: false means unavailable and leaves out unchanged. */
FIPS_CORE_API bool fips_link_stats_rtt_estimate(const fips_link_stats *stats,
    fips_duration *out);
FIPS_CORE_API void fips_link_stats_update_rtt(fips_link_stats *stats, fips_duration rtt);
FIPS_CORE_API uint64_t fips_link_stats_time_since_recv(const fips_link_stats *stats,
    uint64_t current_time_ms);
FIPS_CORE_API void fips_link_stats_reset(fips_link_stats *stats);

#ifdef __cplusplus
}
#endif
#endif /* FIPS_TRANSPORT_TYPES_H */
