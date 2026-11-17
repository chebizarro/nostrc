/* Upstream transport/ble/stream_read.rs: one reader per connection, threaded
 * through the pre-handshake pubkey exchange and the receive loop, so bytes the
 * peer coalesced behind the exchange are not lost at the hand-off and a
 * fragmented exchange or frame is reassembled. Boundaries come from the FMP
 * prefix (the shared framer), never from the backend's socket type, which
 * makes it a pass-through on a boundary-preserving backend. Also the stream
 * I/O seam: the backend's read/write ops, or the fd fast path through libgo. */
#define _POSIX_C_SOURCE 200809L
#include "transport/ble/internal.h"
#include "transport/presence.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
uint64_t ble_now(void) { return fipsd_transport_now_ms(); }
/* Reacquire TLS errno after a migrating fiber resumes (tcp_io_errno). */
__attribute__((noinline)) static int io_errno(void) { return errno; }
/* gof timeout for an absolute deadline: -1 none, -2 already passed. */
static int remaining(uint64_t deadline)
{
    if (deadline == FIPSD_BLE_NO_DEADLINE) return -1;
    uint64_t now = ble_now();
    if (now >= deadline) return -2;
    return deadline - now > INT_MAX ? INT_MAX : (int)(deadline - now);
}
ssize_t ble_io_read(fipsd_ble_stream *s, int fd, uint8_t *buf, size_t cap, uint64_t deadline)
{
    if (fd < 0) return s->ops->read(s, buf, cap, deadline);
    for (;;) {
        int t = remaining(deadline);
        if (t == -2) return -ETIMEDOUT;
        struct iovec v = {.iov_base = buf, .iov_len = cap};
        struct msghdr m = {.msg_iov = &v, .msg_iovlen = 1};
        ssize_t n = gof_recvmsg(fd, &m, 0, t);
        if (n >= 0) return n;
        int e = io_errno();
        if (e != EINTR) return -e;
    }
}
int ble_io_write(fipsd_ble_stream *s, int fd, const uint8_t *p, size_t len, uint64_t deadline)
{
    if (fd < 0) return s->ops->write(s, p, len, deadline);
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags = MSG_NOSIGNAL; /* Apple descriptors carry SO_NOSIGPIPE from the backend */
#endif
    size_t sent = 0;
    while (sent < len) {
        int t = remaining(deadline);
        if (t == -2) return -ETIMEDOUT;
        struct iovec v = {.iov_base = (void *)(p + sent), .iov_len = len - sent};
        struct msghdr m = {.msg_iov = &v, .msg_iovlen = 1};
        ssize_t n = gof_sendmsg(fd, &m, flags, t);
        if (n < 0) { int e = io_errno(); if (e == EINTR) continue; return -e; }
        if (!n) return -EPIPE;
        sent += (size_t)n;
    }
    return 0;
}
void ble_io_cancel(fipsd_ble_stream *s, int fd)
{ if (fd >= 0) (void)gof_io_cancel_fd(fd); else s->ops->cancel(s); }
void ble_io_close(fipsd_ble_stream *s, int fd)
{ if (fd >= 0) (void)gof_io_reset_fd(fd); s->ops->close(s); }
int ble_io_adopt(fipsd_ble_stream *s, int *fd)
{
    *fd = s->ops->fd ? s->ops->fd(s) : -1;
    return *fd >= 0 && gof_io_reset_fd(*fd) ? -1 : 0;
}
void ble_stream_mtu(const fipsd_ble_stream *s, uint16_t fallback, uint16_t *send, uint16_t *recv)
{
    uint16_t a = 0, b = 0;
    if (s->ops->mtu) s->ops->mtu(s, &a, &b);
    *send = a ? a : fallback; *recv = b ? b : fallback;
}

int ble_reader_init(ble_reader *r, fipsd_ble_stream *s, int fd, uint16_t recv_mtu)
{
    *r = (ble_reader){.stream = s, .fd = fd, .capacity = recv_mtu > BLE_MIN_RECV_CHUNK ? recv_mtu : BLE_MIN_RECV_CHUNK};
    r->chunk = malloc(r->capacity);
    return r->chunk ? 0 : -1;
}
void ble_reader_free(ble_reader *r) { free(r->chunk); r->chunk = NULL; r->pos = r->len = 0; }
size_t ble_reader_buffered(const ble_reader *r) { return r->len - r->pos; }
/* One backend read into an empty chunk: 1 data, 0 EOF (latched), or -errno.
 * A closed connection stays closed rather than being re-read forever. */
static int fill(ble_reader *r, uint64_t deadline)
{
    if (r->eof) return 0;
    ssize_t n = ble_io_read(r->stream, r->fd, r->chunk, r->capacity, deadline);
    if (n < 0) return (int)n;
    if (!n) { r->eof = true; return 0; }
    r->pos = 0; r->len = (size_t)n;
    return 1;
}
int ble_reader_read_exact(ble_reader *r, uint8_t *out, size_t n, uint64_t deadline)
{
    size_t got = 0;
    while (got < n) {
        if (r->pos < r->len) {
            size_t take = r->len - r->pos < n - got ? r->len - r->pos : n - got;
            memcpy(out + got, r->chunk + r->pos, take); r->pos += take; got += take;
            continue;
        }
        int rc = fill(r, deadline);
        if (rc < 0) return rc;
        if (!rc) return -EPIPE; /* UnexpectedEof */
    }
    return 0;
}
int ble_reader_frame(ble_reader *r, fipsd_stream *f, uint64_t deadline, size_t *size)
{
    fipsd_stream_reset(f);
    for (;;) {
        if (r->pos < r->len) {
            size_t used = 0;
            int rc = fipsd_stream_feed(f, r->chunk + r->pos, r->len - r->pos, &used);
            r->pos += used;
            if (rc < 0) return -EPROTO;
            if (rc == 1) { *size = f->used; return 1; }
            continue;
        }
        int rc = fill(r, deadline);
        if (rc < 0) return rc;
        if (!rc) return fipsd_stream_eof(f) ? 0 : -EPROTO;
    }
}
