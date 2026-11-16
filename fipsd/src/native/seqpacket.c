#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "native/seqpacket.h"
#include <libgo/fiber.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

int fipsd_native_set_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0 ? -1 : 0;
}
int fipsd_native_pair(int fds[2])
{
#ifdef SOCK_CLOEXEC
    if (socketpair(AF_UNIX, FIPSD_NATIVE_SOCK_TYPE | SOCK_CLOEXEC, 0, fds) != 0) return -1;
#else
    if (socketpair(AF_UNIX, FIPSD_NATIVE_SOCK_TYPE, 0, fds) != 0) return -1;
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
        if (errno == ECONNRESET) return FIPSD_NATIVE_RECEIVED_EOF;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return FIPSD_NATIVE_RECEIVED_AGAIN;
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
        if (n < 0 && errno == ETIMEDOUT && FIPSD_NATIVE_CLOSE_RETRY_MS > 0) continue;
        if (n < 0 && errno == EINTR) continue;
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
    do n = gof_sendmsg(fd, &msg, FIPSD_NATIVE_MSG_NOSIGNAL, timeout_ms); while (n < 0 && errno == EINTR);
    return n < 0 ? -1 : 0;
}
