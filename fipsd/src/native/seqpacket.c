#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "fiber_errno.h"
#include "native/seqpacket.h"
#include <libgo/fiber.h>
#include <sodium.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

int fipsd_native_set_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0 ? -1 : 0;
}
static int pair_of(int type, int fds[2])
{
#ifdef SOCK_CLOEXEC
    if (socketpair(AF_UNIX, type | SOCK_CLOEXEC, 0, fds) != 0) return -1;
#else
    if (socketpair(AF_UNIX, type, 0, fds) != 0) return -1;
    if (fipsd_native_set_cloexec(fds[0]) != 0 || fipsd_native_set_cloexec(fds[1]) != 0) {
        int e = errno; close(fds[0]); close(fds[1]); errno = e; return -1;
    }
#endif
#ifdef SO_NOSIGPIPE
    int one = 1;
    if (setsockopt(fds[0], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one) != 0 ||
        setsockopt(fds[1], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one) != 0) {
        int e = errno; close(fds[0]); close(fds[1]); errno = e; return -1;
    }
#endif
    return 0;
}
int fipsd_native_pair(int fds[2]) { return pair_of(FIPSD_NATIVE_SOCK_TYPE, fds); }
int fipsd_native_pair_framed(int fds[2]) { return pair_of(SOCK_STREAM, fds); }
static int set_buf(int fd, int option, size_t bytes)
{
    if (bytes > INT32_MAX) { errno = EINVAL; return -1; }
    int size = (int)bytes;
    return setsockopt(fd, SOL_SOCKET, option, &size, sizeof size);
}
int fipsd_native_set_sndbuf(int fd, size_t bytes) { return set_buf(fd, SO_SNDBUF, bytes); }
int fipsd_native_set_rcvbuf(int fd, size_t bytes) { return set_buf(fd, SO_RCVBUF, bytes); }
int fipsd_native_set_nonblocking(int fd, bool nonblocking)
{
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0) return -1;
    int wanted = nonblocking ? flags | O_NONBLOCK : flags & ~O_NONBLOCK;
    return fcntl(fd, F_SETFL, wanted) < 0 ? -1 : 0;
}
int fipsd_native_set_timeout(int fd, int option, int64_t ms)
{
    struct timeval tv = {0, 0};
    if (ms == 0) { errno = EINVAL; return -1; }
    if (ms > 0) {
        int64_t secs = ms / 1000;
        tv.tv_sec = secs > (int64_t)INT32_MAX ? (time_t)INT32_MAX : (time_t)secs;
        tv.tv_usec = (suseconds_t)((ms % 1000) * 1000);
    }
    return setsockopt(fd, SOL_SOCKET, option, &tv, sizeof tv);
}
int fipsd_native_timeout(int fd, int option, int64_t *ms)
{
    struct timeval tv = {0, 0};
    socklen_t len = sizeof tv;
    if (getsockopt(fd, SOL_SOCKET, option, &tv, &len) != 0) return -1;
    *ms = tv.tv_sec == 0 && tv.tv_usec == 0 ? -1 : (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    return 0;
}
bool fipsd_native_peer_hung_up(int fd)
{
    struct pollfd p = {.fd = fd, .events = POLLIN};
    int rc = poll(&p, 1, 0);
    return rc > 0 && (p.revents & POLLHUP);
}
size_t fipsd_native_bytes_queued(int fd)
{
    int queued = 0;
    if (ioctl(fd, FIONREAD, &queued) < 0 || queued < 0) return 0;
    return (size_t)queued;
}
int fipsd_native_peer_gone_as_epipe(int error)
{
    return error == ECONNRESET || error == EDESTADDRREQ ? EPIPE : error;
}
static fipsd_native_received classify(int fd, ssize_t n, size_t *len)
{
    if (n < 0) {
        if (fipsd_io_errno() == ECONNRESET) return FIPSD_NATIVE_RECEIVED_EOF;
        if (fipsd_io_errno() == EAGAIN || fipsd_io_errno() == EWOULDBLOCK) return FIPSD_NATIVE_RECEIVED_AGAIN;
        return FIPSD_NATIVE_RECEIVED_ERROR;
    }
    if (n == 0 && fipsd_native_peer_hung_up(fd) && fipsd_native_bytes_queued(fd) == 0) return FIPSD_NATIVE_RECEIVED_EOF;
    *len = (size_t)n;
    return FIPSD_NATIVE_RECEIVED_DATAGRAM;
}
fipsd_native_received fipsd_native_recv_once(int fd, void *buf, size_t cap, size_t *len)
{
    ssize_t n;
    do n = recv(fd, buf, cap, MSG_DONTWAIT); while (n < 0 && errno == EINTR);
    return classify(fd, n, len);
}
ssize_t fipsd_native_send_once(int fd, const void *buf, size_t len)
{
    ssize_t n;
    do n = send(fd, buf, len, FIPSD_NATIVE_MSG_NOSIGNAL | MSG_DONTWAIT); while (n < 0 && errno == EINTR);
    return n;
}
fipsd_native_received fipsd_native_recv_fiber(int fd, void *buf, size_t cap, size_t *len)
{
    for (;;) {
        struct iovec iov = {.iov_base = buf, .iov_len = cap};
        struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1};
        ssize_t n = gof_recvmsg(fd, &msg, 0, FIPSD_NATIVE_CLOSE_RETRY_MS);
        if (n < 0 && fipsd_io_errno() == ETIMEDOUT && FIPSD_NATIVE_CLOSE_RETRY_MS > 0) continue;
        if (n < 0 && fipsd_io_errno() == EINTR) continue;
        fipsd_native_received r = classify(fd, n, len);
        if (r == FIPSD_NATIVE_RECEIVED_AGAIN) continue;
        return r;
    }
}
int fipsd_native_send_fiber(int fd, const void *buf, size_t len, int timeout_ms)
{
    struct iovec iov = {.iov_base = (void *)buf, .iov_len = len};
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1};
    ssize_t n;
    do n = gof_sendmsg(fd, &msg, FIPSD_NATIVE_MSG_NOSIGNAL, timeout_ms); while (n < 0 && fipsd_io_errno() == EINTR);
    return n < 0 ? -1 : 0;
}

/* ---- stream flows (native/frame.h) ----------------------------------------- */
static size_t advance(struct iovec *iov, size_t count, size_t sent)
{
    size_t first = 0;
    while (first < count && sent >= iov[first].iov_len) { sent -= iov[first].iov_len; iov[first].iov_len = 0; ++first; }
    if (first < count) { iov[first].iov_base = (uint8_t *)iov[first].iov_base + sent; iov[first].iov_len -= sent; }
    return first;
}
int fipsd_native_frame_send_fiber(int fd, const void *buf, size_t len)
{
    if (len > FIPSD_NATIVE_FRAME_MAX) { fipsd_io_set_errno(EMSGSIZE); return -1; }
    uint8_t header[FIPSD_NATIVE_FRAME_HEADER];
    fipsd_native_frame_header(header, (uint32_t)len);
    struct iovec iov[2] = {{.iov_base = header, .iov_len = sizeof header}, {.iov_base = (void *)buf, .iov_len = len}};
    size_t first = 0, left = sizeof header + len;
    while (left) {
        struct msghdr msg = {.msg_iov = iov + first};
        msg.msg_iovlen = first ? 1 : 2;
        /* Parks on writability: the kernel raises it when the client reads. */
        ssize_t n = gof_sendmsg(fd, &msg, FIPSD_NATIVE_MSG_NOSIGNAL, -1);
        if (n < 0) { if (fipsd_io_errno() == EINTR) continue; return -1; }
        left -= (size_t)n;
        first = advance(iov, 2, (size_t)n);
    }
    return 0;
}
int fipsd_native_frame_send_once(int fd, const void *buf, size_t len)
{
    if (len > FIPSD_NATIVE_FRAME_MAX) { errno = EMSGSIZE; return -1; }
    uint8_t header[FIPSD_NATIVE_FRAME_HEADER];
    fipsd_native_frame_header(header, (uint32_t)len);
    struct iovec iov[2] = {{.iov_base = header, .iov_len = sizeof header}, {.iov_base = (void *)buf, .iov_len = len}};
    struct msghdr msg = {.msg_iov = iov, .msg_iovlen = 2};
    ssize_t n;
    do n = sendmsg(fd, &msg, FIPSD_NATIVE_MSG_NOSIGNAL | MSG_DONTWAIT); while (n < 0 && errno == EINTR);
    if (n < 0) return -1;
    if ((size_t)n != sizeof header + len) { errno = ENOBUFS; return -1; }
    return 0;
}
int fipsd_native_frame_reader_init(fipsd_native_frame_reader *r)
{
    memset(r, 0, sizeof *r);
    r->buf = malloc(FIPSD_NATIVE_FRAME_READER_CAPACITY);
    return r->buf ? 0 : -1;
}
void fipsd_native_frame_reader_clear(fipsd_native_frame_reader *r)
{
    if (r->buf) { sodium_memzero(r->buf, FIPSD_NATIVE_FRAME_READER_CAPACITY); free(r->buf); }
    memset(r, 0, sizeof *r);
}
fipsd_native_received fipsd_native_frame_recv_fiber(fipsd_native_frame_reader *r, int fd, const uint8_t **payload,
                                                    size_t *len)
{
    for (;;) {
        uint32_t n = 0;
        fipsd_native_frame_status st = fipsd_native_frame_take(r->buf + r->start, r->have - r->start, &n);
        if (st == FIPSD_NATIVE_FRAME_INVALID) { fipsd_io_set_errno(EPROTO); return FIPSD_NATIVE_RECEIVED_ERROR; }
        if (st == FIPSD_NATIVE_FRAME_WHOLE) {
            *payload = r->buf + r->start + FIPSD_NATIVE_FRAME_HEADER;
            *len = n;
            r->start += FIPSD_NATIVE_FRAME_HEADER + n;
            if (r->start == r->have) r->start = r->have = 0;
            return FIPSD_NATIVE_RECEIVED_DATAGRAM;
        }
        /* A partial frame moves to the front once per read, so the space
         * behind it always holds at least one more maximum frame. */
        if (r->start) {
            memmove(r->buf, r->buf + r->start, r->have - r->start);
            r->have -= r->start; r->start = 0;
        }
        struct iovec iov = {.iov_base = r->buf + r->have, .iov_len = FIPSD_NATIVE_FRAME_READER_CAPACITY - r->have};
        struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1};
        /* No deadline: a stream's close is a readiness event. */
        ssize_t got = gof_recvmsg(fd, &msg, 0, -1);
        if (got < 0) {
            if (fipsd_io_errno() == EINTR) continue;
            if (fipsd_io_errno() == ECONNRESET) return FIPSD_NATIVE_RECEIVED_EOF;
            return FIPSD_NATIVE_RECEIVED_ERROR;
        }
        if (got == 0) return FIPSD_NATIVE_RECEIVED_EOF;
        r->have += (size_t)got;
    }
}
