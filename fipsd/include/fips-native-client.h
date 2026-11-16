/* libfipsnative: a C client for the fipsd native datagram API.
 *
 * The native datagram API lets a program send and receive datagrams over the
 * mesh addressed by public key and FSP port, with no IPv6 emulation and no
 * TUN device. A program opens a flow to a peer, receives a socket descriptor,
 * and uses ordinary socket calls on it. This library is the C counterpart of
 * upstream's `fips::native::client` (docs/reference/native-api.md): it speaks
 * the daemon's line protocol so a caller never sees it, and it depends on
 * nothing but POSIX, so any program can link it.
 *
 * Status: experimental, Unix only. The daemon must run with
 * `node.native_api.enabled: true`; its socket is mode 0770, group `fips`, and
 * any process that can open it can send as the node's identity.
 *
 * The Berkeley mapping. The descriptor the daemon hands back is a real
 * socket (SOCK_SEQPACKET on Linux, SOCK_DGRAM on macOS and FreeBSD), so
 * `poll`, `select`, `epoll`, `kqueue`, `setsockopt` and `close` on it are the
 * genuine syscalls:
 *
 *   socket()    none: connect and listen each return one already bound.
 *   bind()      fips_native_listen (port 0 asks the daemon to choose).
 *   listen()    folded into fips_native_listen; the backlog is node config.
 *   connect()   fips_native_connect, a local registration contacting no peer.
 *   accept()    fips_native_accept: one recvmsg on the listener's descriptor.
 *   send()      fips_native_send: whole or not at all; EMSGSIZE above the limit.
 *   recv()      fips_native_recv: 0 is an empty datagram and only that.
 *   close()     fips_native_stream_close / fips_native_listener_close: this
 *               is how a flow is released and a port unbound.
 *   getsockname fips_native_stream_local_addr / fips_native_listener_local_addr
 *   getpeername fips_native_stream_peer_addr
 *   SO_RCVTIMEO fips_native_stream_set_read_timeout (and the write side).
 *
 * Errors. Every function that can fail returns 0 on success and a negative
 * errno on failure (fips_native_recv returns the length or a negative errno).
 * Compare against the errno constants, never against literals:
 *
 *   -EADDRINUSE     the local port is held, or that exact flow already exists
 *   -EADDRNOTAVAIL  a port in a reserved tier (0-1023), either end, or the
 *                   ephemeral range is exhausted
 *   -EMFILE         the node is at max_flows, or a socket pair could not be made
 *   -EINVAL         an address that does not parse, or a malformed command
 *   -EMSGSIZE       a send above max_payload, refused before any syscall
 *   -EPIPE          the daemon went away; never a peer finishing
 *   -ETIMEDOUT      the five-second setup deadline, and nothing else
 *   -ECONNREFUSED   the node is shutting down, a debug command is disabled,
 *                   nothing is accepting on the socket path, or the reply
 *                   carried an errno name this client has no row for
 *   -ENOENT         no daemon runs at the socket path (or the API is disabled)
 *   -EACCES         the socket's permissions refuse this user
 *   -EPROTO         the daemon is not speaking the protocol (a reply or
 *                   arrival that is not JSON, a missing field, a reply with
 *                   no descriptor, a descriptor on a read that completed no
 *                   line, a partial line past 64 KiB, a truncated control
 *                   message). Never worth retrying.
 *   -EAGAIN         a deadline expired or the descriptor is non-blocking
 *
 * Where data disappears. The API is datagram-unreliable: nothing confirms
 * delivery, and a full per-flow queue, a listener that does not accept fast
 * enough, a datagram sent before a session exists and an MTU that has fallen
 * all lose data with nothing reported here. Nothing peer-driven ever ends a
 * flow: the program decides when a flow is over (a bounded exchange, or an
 * idle deadline), or nothing does. Do not give a zero-length datagram a
 * meaning of its own: on Linux one sent just before a close is read as the
 * close. */
#ifndef FIPS_NATIVE_CLIENT_H
#define FIPS_NATIVE_CLIENT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif

#define FIPS_NATIVE_KEY_SIZE 32
/* An npub is 63 characters; both capacities include the terminating NUL. */
#define FIPS_NATIVE_NPUB_CAPACITY 64
#define FIPS_NATIVE_ADDR_CAPACITY 72
/* The one wait on this surface: how long a setup call waits for its answer. */
#define FIPS_NATIVE_SETUP_TIMEOUT_MS 5000
#define FIPS_NATIVE_PORT_EPHEMERAL_MIN 49152

/* Where a packaged daemon puts its socket: /run/fips/api.sock on Linux,
 * /var/run/fips/api.sock on macOS and FreeBSD. A development daemon usually
 * falls through to $XDG_RUNTIME_DIR or /tmp; see fips_native_socket_path. */
extern const char *const fips_native_socket;
/* The daemon's own resolution order: /run/fips when it exists, /var/run/fips
 * on macOS and FreeBSD, $XDG_RUNTIME_DIR/fips when that names a directory,
 * else /tmp/fips-api.sock. Selection is by the directory's existence, not by
 * whether this process may write to it. Returns 0 or -ENAMETOOLONG. */
int fips_native_socket_path(char *out, size_t cap);

/* One end of a flow: an x-only secp256k1 public key and a port. The npub is
 * the key written down; converting between them is bech32 and nothing else. */
typedef struct fips_native_addr {
    uint8_t key[FIPS_NATIVE_KEY_SIZE];
    uint16_t port;
} fips_native_addr;
/* bech32 npub <-> key. Decoding refuses anything that is not an npub of the
 * right length with a valid checksum (an nsec included) with -EINVAL; the
 * daemon additionally refuses a key that is not on the curve at setup. */
int fips_native_npub_encode(const uint8_t key[FIPS_NATIVE_KEY_SIZE], char *out, size_t cap);
int fips_native_npub_decode(const char *npub, uint8_t key[FIPS_NATIVE_KEY_SIZE]);
int fips_native_addr_from_npub(const char *npub, uint16_t port, fips_native_addr *out);
/* "npub1...:4242", split on the last colon; -EINVAL for anything else. */
int fips_native_addr_parse(const char *text, fips_native_addr *out);
/* The exact inverse of fips_native_addr_parse. Returns 0 or -ENAMETOOLONG. */
int fips_native_addr_format(const fips_native_addr *addr, char *out, size_t cap);
bool fips_native_addr_eq(const fips_native_addr *a, const fips_native_addr *b);

typedef struct fips_native_stream fips_native_stream;
typedef struct fips_native_listener fips_native_listener;

/* Open a flow to `peer` from `local_port`, or from an ephemeral port when it
 * is 0, at `socket_path` (NULL: fips_native_socket). Contacts no peer. The
 * connection to the daemon is closed before this returns: what comes back
 * holds the flow's descriptor and plain copies of what the reply said. */
int fips_native_connect(const char *socket_path, uint16_t local_port, const fips_native_addr *peer,
                        fips_native_stream **out);
/* Hold `local_port`, or an ephemeral one when it is 0. */
int fips_native_listen(const char *socket_path, uint16_t local_port, fips_native_listener **out);
/* Take the next arriving flow, blocking until one arrives: one recvmsg on
 * the listener's descriptor. Whatever the peer sent before this returned is
 * already queued on the stream. Refusing a flow is closing the stream; a
 * refused flow that was never sent on holds its port and its slot until the
 * listener closes. `peer` may be NULL. -EPIPE when the daemon closed its half. */
int fips_native_accept(fips_native_listener *listener, fips_native_stream **out, fips_native_addr *peer);
/* Send one datagram, whole or not at all. -EMSGSIZE above
 * fips_native_stream_max_payload, before any syscall; -EPIPE when the daemon
 * has gone. 0 means only that the local socket pair accepted the bytes. */
int fips_native_send(fips_native_stream *stream, const void *buf, size_t len);
/* Receive one datagram and return its length. Blocks by default; -EAGAIN on
 * a deadline or in non-blocking mode. 0 is an empty datagram, never end of
 * file; -EPIPE means the daemon went away. A datagram longer than `cap` is
 * truncated and the rest discarded, so size `cap` at max_payload. */
ssize_t fips_native_recv(fips_native_stream *stream, void *buf, size_t cap);
/* Close the descriptor, which releases the flow and its port at the daemon.
 * An accepted flow never sent on is released when its listener closes. */
void fips_native_stream_close(fips_native_stream *stream);
/* Close the descriptor, which unbinds the port. Flows already accepted and
 * still held are untouched; those still pending go with it. */
void fips_native_listener_close(fips_native_listener *listener);

/* Field reads of what setup reported; they cannot fail. local_addr carries
 * the node's own key, which on an accepted stream comes from the arrival. */
fips_native_addr fips_native_stream_peer_addr(const fips_native_stream *stream);
fips_native_addr fips_native_stream_local_addr(const fips_native_stream *stream);
/* The largest datagram this flow carries, a snapshot taken at setup. */
size_t fips_native_stream_max_payload(const fips_native_stream *stream);
/* The flow's descriptor for a poll loop or setsockopt. The stream keeps
 * ownership: never close it or hand it to anything that would. */
int fips_native_stream_fd(const fips_native_stream *stream);
/* Bound one recv or one send: ms > 0 sets the deadline (an expiry reports
 * -EAGAIN), ms < 0 clears it, ms == 0 is -EINVAL because the kernel would
 * read it as no deadline. The two directions are separate. */
int fips_native_stream_set_read_timeout(fips_native_stream *stream, int64_t ms);
int fips_native_stream_set_write_timeout(fips_native_stream *stream, int64_t ms);
/* Read a deadline back: *ms is -1 when none is set. */
int fips_native_stream_read_timeout(const fips_native_stream *stream, int64_t *ms);
int fips_native_stream_write_timeout(const fips_native_stream *stream, int64_t *ms);
/* Read-modify-write of O_NONBLOCK, so a flag the caller set survives. A flow
 * from accept is blocking however its listener was set. */
int fips_native_stream_set_nonblocking(fips_native_stream *stream, bool nonblocking);

fips_native_addr fips_native_listener_local_addr(const fips_native_listener *listener);
/* Readable exactly when fips_native_accept would not block. */
int fips_native_listener_fd(const fips_native_listener *listener);
int fips_native_listener_set_nonblocking(fips_native_listener *listener, bool nonblocking);

/* Text for a negative errno this library returned (strerror, plus a row for
 * -EPROTO). Static storage. */
const char *fips_native_strerror(int error);
#ifdef __cplusplus
}
#endif
#endif /* FIPS_NATIVE_CLIENT_H */
