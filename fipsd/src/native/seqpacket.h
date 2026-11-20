#ifndef FIPSD_NATIVE_SEQPACKET_H
#define FIPSD_NATIVE_SEQPACKET_H
/* A connected socket pair behind a flow and behind a listener (upstream
 * src/native/seqpacket.rs). SOCK_SEQPACKET where AF_UNIX implements it as an
 * atomic-record socket (Linux); SOCK_DGRAM on macOS, which has no AF_UNIX
 * SOCK_SEQPACKET, and FreeBSD, whose SOCK_SEQPACKET coalesces records. The
 * two differ at end of file: Linux reports a closed peer as a zero-byte read
 * with POLLHUP latched, Darwin as ECONNRESET and with no readiness event at
 * all, so the daemon's reads there are bounded by FIPSD_NATIVE_CLOSE_RETRY_MS
 * and retried. One rule accepts either signal; see fipsd_native_recv_once. */
#include "native/frame.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>

#if defined(__APPLE__) || defined(__FreeBSD__)
#define FIPSD_NATIVE_SOCK_TYPE SOCK_DGRAM
/* A full client receive buffer refuses a datagram write with ENOBUFS, and
 * plain writability stays raised on any residual space, so a datagram flow
 * cannot wait on it: FreeBSD drops (and counts), macOS waits on a low-water
 * mark sized to the datagram instead (FIPSD_NATIVE_DGRAM_PARK below). A
 * client that asks for `"framing":"stream"` (libfipsnative does) gets a
 * framed SOCK_STREAM flow on both, whose writer the kernel wakes when the
 * reader drains: lossless, see native/frame.h. */
#define FIPSD_NATIVE_STREAM_FRAMING 1
/* Detection latency for a closed peer the reactor cannot see: a bounded wait
 * per idle datagram flow, not a poll interval for data. A stream flow sees
 * its peer's close as a readiness event and needs none. */
#define FIPSD_NATIVE_CLOSE_RETRY_MS 250
#else
#define FIPSD_NATIVE_SOCK_TYPE SOCK_SEQPACKET
/* SOCK_SEQPACKET already parks a writer on a full client: the framing
 * request is accepted and answered with the usual pair. */
#define FIPSD_NATIVE_STREAM_FRAMING 0
#define FIPSD_NATIVE_CLOSE_RETRY_MS (-1)
#endif
/* nostrc-e3ps5: on macOS a datagram flow's feeder, refused with ENOBUFS,
 * parks until the client's buffer has room for that datagram instead of
 * dropping it. XNU evaluates EVFILT_WRITE on a connected AF_UNIX socket
 * against the peer's receive space and honours NOTE_LOWAT there
 * (filt_sowrite_common), and every recv on a unix datagram socket wakes its
 * connected peer's writers (PR_WANTRCVD -> uipc_rcvd -> sowakeup), so
 * gof_wait_writable with a low-water mark of the datagram's cost fires
 * exactly when it fits (characterised by fipsd_native_dgram_kqueue_test).
 * FreeBSD keeps the counted drop: its unix datagram write filter reads the
 * sender's own send buffer, which a connected send never fills, and its
 * receive wakes no sender. FIPSD_NATIVE_DGRAM_PARK=0 in the environment
 * turns it off at runtime (read when the native API is created). */
#if defined(__APPLE__)
#define FIPSD_NATIVE_DGRAM_PARK 1
#else
#define FIPSD_NATIVE_DGRAM_PARK 0
#endif
/* What one datagram from the daemon's half costs the client's receive buffer
 * beyond its bytes on XNU: the sender's address stored with it, the 16-byte
 * sun_noname of an unbound socket-pair half (sbconcat_mbufs). */
#define FIPSD_NATIVE_DGRAM_ADDR_COST 16u
#ifndef MSG_NOSIGNAL
#define FIPSD_NATIVE_MSG_NOSIGNAL 0
#else
#define FIPSD_NATIVE_MSG_NOSIGNAL MSG_NOSIGNAL
#endif

/* Both halves close-on-exec, neither non-blocking, SO_NOSIGPIPE where the
 * platform has it. Returns 0 or -1 with errno. */
int fipsd_native_pair(int fds[2]);
/* The same for a stream flow: AF_UNIX SOCK_STREAM carrying native/frame.h
 * frames. Used where FIPSD_NATIVE_STREAM_FRAMING is set. */
int fipsd_native_pair_framed(int fds[2]);
int fipsd_native_set_cloexec(int fd);
int fipsd_native_set_sndbuf(int fd, size_t bytes);
int fipsd_native_set_rcvbuf(int fd, size_t bytes);
/* Read-modify-write, so a flag the caller set survives. */
int fipsd_native_set_nonblocking(int fd, bool nonblocking);
/* SO_RCVTIMEO/SO_SNDTIMEO: ms < 0 clears, ms == 0 is EINVAL (the kernel
 * would read it as no deadline), otherwise saturating. */
int fipsd_native_set_timeout(int fd, int option, int64_t ms);
/* Reads a deadline back; -1 when none is set. */
int fipsd_native_timeout(int fd, int option, int64_t *ms);
/* Whether the peer closed its half: POLLHUP, polled for POLLIN so Darwin
 * registers a filter. Non-blocking. */
bool fipsd_native_peer_hung_up(int fd);
/* Bytes the receive queue still holds; non-zero proves a message is waiting. */
size_t fipsd_native_bytes_queued(int fd);
typedef enum fipsd_native_received {
    FIPSD_NATIVE_RECEIVED_DATAGRAM, /* `*len` bytes; zero is an empty datagram */
    FIPSD_NATIVE_RECEIVED_EOF,      /* the peer closed its half */
    FIPSD_NATIVE_RECEIVED_AGAIN,    /* nothing queued and the peer is alive */
    FIPSD_NATIVE_RECEIVED_ERROR     /* errno set */
} fipsd_native_received;
/* One non-waiting receive under the end-of-file rule: ECONNRESET is EOF, a
 * zero-byte read is EOF only with POLLHUP set and nothing queued behind it. */
fipsd_native_received fipsd_native_recv_once(int fd, void *buf, size_t cap, size_t *len);
/* One send, MSG_NOSIGNAL, never waiting on a non-blocking descriptor. */
ssize_t fipsd_native_send_once(int fd, const void *buf, size_t len);
/* The platform's spelling of "the peer is gone" as this API's: ECONNRESET
 * and EDESTADDRREQ (Darwin SOCK_DGRAM) become EPIPE. */
int fipsd_native_peer_gone_as_epipe(int error);

/* Fiber-parked variants for the daemon's halves (call from a fiber). The
 * receive parks until a datagram, end of file, an error or owner-directed
 * cancellation (ERROR with errno ECANCELED); on Darwin it retries every
 * FIPSD_NATIVE_CLOSE_RETRY_MS, the only way its kernel's close is seen. */
fipsd_native_received fipsd_native_recv_fiber(int fd, void *buf, size_t cap, size_t *len);
/* timeout_ms < 0 parks until writable, 0 tries once, > 0 bounds the wait. */
int fipsd_native_send_fiber(int fd, const void *buf, size_t len, int timeout_ms);

/* Consume `sent` bytes from the front of iov[0..count): entries wholly sent
 * become empty, a partly sent one is trimmed. Returns the index of the first
 * entry with bytes left (count when none). */
size_t fipsd_native_iov_advance(struct iovec *iov, size_t count, size_t sent);
/* Stream flows (native/frame.h). Frames onto a flow are written by api.c
 * (one non-waiting write per delivery batch, then the flow's feeder, parked
 * on writability while the client drains); a frame taken in part is
 * finished, never abandoned. */
/* One frame without waiting, onto a pair nobody reads yet: 0 when it was
 * taken whole, else -1 with errno (ENOBUFS when only part fitted, after which
 * the stream is unusable and the caller closes it). */
int fipsd_native_frame_send_once(int fd, const void *buf, size_t len);
/* The receiving half: a buffer of two maximum frames, so whatever one read
 * brings is parsed before the next. */
typedef struct fipsd_native_frame_reader {
    uint8_t *buf;
    size_t start, have; /* unparsed bytes are buf[start..have) */
} fipsd_native_frame_reader;
#define FIPSD_NATIVE_FRAME_READER_CAPACITY (2u * (FIPSD_NATIVE_FRAME_HEADER + FIPSD_NATIVE_FRAME_MAX))
int fipsd_native_frame_reader_init(fipsd_native_frame_reader *r);
void fipsd_native_frame_reader_clear(fipsd_native_frame_reader *r);
/* The next frame, parked until it is whole. DATAGRAM leaves `*payload`
 * pointing into the reader, valid until the next call; EOF is the peer's
 * close (ECONNRESET included, mid-frame included); ERROR sets errno: EPROTO
 * for a length above the maximum, ECANCELED for owner-directed cancellation. */
fipsd_native_received fipsd_native_frame_recv_fiber(fipsd_native_frame_reader *r, int fd, const uint8_t **payload,
                                                    size_t *len);
#endif
