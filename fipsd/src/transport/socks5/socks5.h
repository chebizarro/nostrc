#ifndef FIPSD_SOCKS5_H
#define FIPSD_SOCKS5_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* RFC 1928 CONNECT client for the proxied stream owners (upstream
 * transport/socks5/dialer.rs over tokio-socks). The codec is pure and byte
 * compatible with that crate: a no-auth greeting offers method 0 only, a
 * username/password greeting offers 0 and 2 (RFC 1929, both lengths <= 255),
 * requests carry IPv4/IPv6/domain targets with big-endian ports. The
 * handshake runs on a libgo fiber over a connected nonblocking socket under
 * one absolute deadline; it records no counters and logs nothing, callers
 * classify the result (tor's refused-versus-error split). */
enum {
    FIPSD_SOCKS5_VERSION = 0x05, FIPSD_SOCKS5_METHOD_NONE = 0x00, FIPSD_SOCKS5_METHOD_USERPASS = 0x02,
    FIPSD_SOCKS5_METHOD_REJECTED = 0xff, FIPSD_SOCKS5_CMD_CONNECT = 0x01, FIPSD_SOCKS5_ATYP_IPV4 = 0x01,
    FIPSD_SOCKS5_ATYP_DOMAIN = 0x03, FIPSD_SOCKS5_ATYP_IPV6 = 0x04, FIPSD_SOCKS5_USERPASS_VERSION = 0x01,
    FIPSD_SOCKS5_REP_SUCCESS = 0x00, FIPSD_SOCKS5_REP_REFUSED = 0x05
};
#define FIPSD_SOCKS5_MAX_REQUEST 262u /* 4 + 1 + 255 + 2 */
typedef enum fipsd_socks5_target_kind {
    FIPSD_SOCKS5_TARGET_IPV4, FIPSD_SOCKS5_TARGET_IPV6, FIPSD_SOCKS5_TARGET_DOMAIN
} fipsd_socks5_target_kind;
typedef struct fipsd_socks5_target {
    fipsd_socks5_target_kind kind;
    uint8_t ip[16];       /* network order, 4 or 16 bytes */
    char domain[256];     /* NUL terminated, domain_len bytes */
    uint8_t domain_len;
    uint16_t port;
} fipsd_socks5_target;
typedef struct fipsd_socks5_auth { const char *username, *password; } fipsd_socks5_auth; /* NULL username: no auth */
/* Pure encoders: bytes written, 0 when the arguments or capacity do not fit. */
size_t fipsd_socks5_greeting(uint8_t *out, size_t cap, bool userpass);
size_t fipsd_socks5_userpass(uint8_t *out, size_t cap, const char *username, const char *password);
size_t fipsd_socks5_request(uint8_t *out, size_t cap, const fipsd_socks5_target *);
/* Length of a complete CONNECT reply whose first bytes are `in`: > 0 once
 * enough bytes decide it (4, or 5 for a domain BND.ADDR), 0 when more are
 * needed, -1 when malformed (version, reserved byte or address type). */
int fipsd_socks5_reply_length(const uint8_t *in, size_t len);
/* The parse upstream tor/nym do before dialing: numeric ip:port (bracketed
 * IPv6) is an IP target; anything else is host:port resolved by the proxy
 * (".onion" names included). require_dot is tor's fully-qualified rule. */
bool fipsd_socks5_target_parse(const char *text, size_t len, bool require_dot, fipsd_socks5_target *out);
/* tokio-socks error classes: REFUSED is REP 0x05 (Error::ConnectionRefused,
 * the only one tor counts as connect_refused); REJECTED any other non-zero
 * REP (rep_out carries it); NO_METHOD the 0xff method reply
 * (NoAcceptableAuthMethods); AUTH_FAILED a non-zero RFC 1929 status
 * (PasswordAuthFailure); PROTOCOL a bad version, reserved byte, address type
 * or unoffered method, or the proxy closing mid-handshake; IO a socket
 * error; TIMEOUT the deadline; CANCELLED the owner's gof_io_cancel_fd latch or
 * scheduler stop. */
typedef enum fipsd_socks5_result {
    FIPSD_SOCKS5_OK, FIPSD_SOCKS5_TIMEOUT, FIPSD_SOCKS5_REFUSED, FIPSD_SOCKS5_REJECTED,
    FIPSD_SOCKS5_NO_METHOD, FIPSD_SOCKS5_AUTH_FAILED, FIPSD_SOCKS5_PROTOCOL, FIPSD_SOCKS5_IO,
    FIPSD_SOCKS5_CANCELLED
} fipsd_socks5_result;
/* Greeting, optional RFC 1929 sub-negotiation and CONNECT on `fd` (connected,
 * nonblocking, reset for fiber I/O), checked in tokio-socks order (version,
 * reserved byte, REP, then BND address type). deadline_ms is absolute on the
 * fipsd_transport_now_ms clock and bounds every read and write. */
fipsd_socks5_result fipsd_socks5_handshake(int fd, const fipsd_socks5_auth *, const fipsd_socks5_target *,
    uint64_t deadline_ms, uint8_t *rep_out);
int fipsd_socks5_errno(fipsd_socks5_result);
const char *fipsd_socks5_result_name(fipsd_socks5_result);
#endif
