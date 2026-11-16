#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "native/dgram_probe.h"
#include "native/seqpacket.h"
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

int fipsd_native_probe_dgram_pair(int fds[2]) { return socketpair(AF_UNIX, SOCK_DGRAM, 0, fds); }
ssize_t fipsd_native_probe_recv(int fd, void *buf, size_t cap) { return recv(fd, buf, cap, MSG_DONTWAIT); }
ssize_t fipsd_native_probe_send(int fd, const void *buf, size_t len) { return send(fd, buf, len, FIPSD_NATIVE_MSG_NOSIGNAL); }
bool fipsd_native_probe_hung_up(int fd)
{
    struct pollfd p = {.fd = fd, .events = POLLIN};
    int rc = poll(&p, 1, 0);
    return rc > 0 && (p.revents & POLLHUP);
}
void fipsd_native_probe_provoke_collection(void)
{
    int fds[2];
    if (fipsd_native_probe_dgram_pair(fds) == 0) { close(fds[0]); close(fds[1]); }
    struct timespec wait = {0, 100 * 1000 * 1000};
    while (nanosleep(&wait, &wait) != 0 && errno == EINTR) {}
}
static ssize_t try_send_fd(int sock, const void *data, size_t len, int fd)
{
    struct iovec iov = {.iov_base = (void *)data, .iov_len = len};
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1};
    union { struct cmsghdr header; unsigned char bytes[CMSG_SPACE(sizeof(int))]; } control;
    memset(&control, 0, sizeof control);
    msg.msg_control = control.bytes; msg.msg_controllen = CMSG_SPACE(sizeof(int));
    struct cmsghdr *h = CMSG_FIRSTHDR(&msg);
    h->cmsg_level = SOL_SOCKET; h->cmsg_type = SCM_RIGHTS; h->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(h), &fd, sizeof fd);
    return sendmsg(sock, &msg, FIPSD_NATIVE_MSG_NOSIGNAL);
}
static int recv_fd(int sock, void *buf, size_t cap, ssize_t *n)
{
    struct iovec iov = {.iov_base = buf, .iov_len = cap};
    union { struct cmsghdr header; unsigned char bytes[CMSG_SPACE(sizeof(int))]; } control;
    memset(&control, 0, sizeof control);
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control.bytes, .msg_controllen = sizeof control.bytes};
    *n = recvmsg(sock, &msg, 0);
    if (*n < 0) return -1;
    for (struct cmsghdr *h = CMSG_FIRSTHDR(&msg); h; h = CMSG_NXTHDR(&msg, h))
        if (h->cmsg_level == SOL_SOCKET && h->cmsg_type == SCM_RIGHTS) { int fd; memcpy(&fd, CMSG_DATA(h), sizeof fd); return fd; }
    errno = ENOENT;
    return -1;
}
ssize_t fipsd_native_probe_read_after_collection_in_flight(void)
{
    int flow[2], listener[2];
    if (fipsd_native_pair(flow) != 0) return -1;
    if (fipsd_native_pair(listener) != 0) { close(flow[0]); close(flow[1]); return -1; }
    ssize_t rc = -1;
    if (send(flow[0], "held", 4, FIPSD_NATIVE_MSG_NOSIGNAL) == 4 && try_send_fd(listener[0], "arrival", 7, flow[1]) == 7) {
        /* The message is now the only reference to the flow's client half. */
        close(flow[1]); flow[1] = -1;
        fipsd_native_probe_provoke_collection();
        unsigned char buf[64]; ssize_t n;
        int got = recv_fd(listener[1], buf, sizeof buf, &n);
        if (got >= 0) { rc = fipsd_native_probe_recv(got, buf, sizeof buf); close(got); }
    }
    close(flow[0]); if (flow[1] >= 0) close(flow[1]); close(listener[0]); close(listener[1]);
    return rc;
}
