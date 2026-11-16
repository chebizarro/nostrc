#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "native/fdpass.h"
#include "native/seqpacket.h"
#include <libgo/fiber.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

ssize_t fipsd_native_sendmsg_fd(int sock, const void *data, size_t len, int fd, int timeout_ms)
{
    struct iovec iov = {.iov_base = (void *)data, .iov_len = len};
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1};
    union { struct cmsghdr header; unsigned char bytes[CMSG_SPACE(sizeof(int))]; } control;
    if (fd >= 0) {
        memset(&control, 0, sizeof control);
        msg.msg_control = control.bytes; msg.msg_controllen = CMSG_SPACE(sizeof(int));
        struct cmsghdr *h = CMSG_FIRSTHDR(&msg);
        h->cmsg_level = SOL_SOCKET; h->cmsg_type = SCM_RIGHTS; h->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(h), &fd, sizeof fd);
    }
    ssize_t n;
    do n = gof_sendmsg(sock, &msg, FIPSD_NATIVE_MSG_NOSIGNAL, timeout_ms); while (n < 0 && errno == EINTR);
    return n;
}
int fipsd_native_recvmsg_fd(int sock, void *buf, size_t cap, fipsd_native_chunk *out, int timeout_ms)
{
    *out = (fipsd_native_chunk){.fd = -1};
    struct iovec iov = {.iov_base = buf, .iov_len = cap};
    union { struct cmsghdr header; unsigned char bytes[CMSG_SPACE(sizeof(int) * 8)]; } control;
    memset(&control, 0, sizeof control);
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control.bytes, .msg_controllen = sizeof control.bytes};
    int flags = 0;
#ifdef MSG_CMSG_CLOEXEC
    flags |= MSG_CMSG_CLOEXEC;
#endif
    ssize_t n;
    do n = gof_recvmsg(sock, &msg, flags, timeout_ms); while (n < 0 && errno == EINTR);
    if (n < 0) {
        if (errno == ECONNRESET) { out->len = 0; return 0; }
        return -1;
    }
    for (struct cmsghdr *h = CMSG_FIRSTHDR(&msg); h; h = CMSG_NXTHDR(&msg, h)) {
        if (h->cmsg_level != SOL_SOCKET || h->cmsg_type != SCM_RIGHTS) continue;
        size_t payload = h->cmsg_len - CMSG_LEN(0);
        for (size_t i = 0; i + sizeof(int) <= payload; i += sizeof(int)) {
            int got; memcpy(&got, CMSG_DATA(h) + i, sizeof got);
            if (out->fd < 0) { out->fd = got; (void)fipsd_native_set_cloexec(got); }
            else close(got);
        }
    }
    out->len = (size_t)n;
    out->truncated = (msg.msg_flags & MSG_CTRUNC) != 0;
    return 0;
}
