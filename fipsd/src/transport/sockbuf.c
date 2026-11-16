#include "transport/sockbuf.h"
#include "log.h"
#include <errno.h>
#include <limits.h>
#include <sys/socket.h>
bool fipsd_sockbuf_clamped(const fipsd_buffer_granted *g) { return g->actual < g->requested; }
int fipsd_sockbuf_set(int fd, fipsd_socket_buffer which, size_t size, fipsd_buffer_granted *out)
{
    if (!out || (which != FIPSD_SOCKET_RECV && which != FIPSD_SOCKET_SEND)) { errno = EINVAL; return -1; }
    int value = size > INT_MAX ? INT_MAX : (int)size;
    int option = which == FIPSD_SOCKET_RECV ? SO_RCVBUF : SO_SNDBUF;
    int rc = -1;
#ifdef __linux__
    int force = which == FIPSD_SOCKET_RECV ? SO_RCVBUFFORCE : SO_SNDBUFFORCE;
    rc = setsockopt(fd, SOL_SOCKET, force, &value, sizeof value);
#endif
    if (rc < 0) rc = setsockopt(fd, SOL_SOCKET, option, &value, sizeof value);
    if (rc < 0) return -1;
    int actual = 0; socklen_t len = sizeof actual;
    if (getsockopt(fd, SOL_SOCKET, option, &actual, &len) < 0) return -1;
    *out = (fipsd_buffer_granted){size, actual > 0 ? (size_t)actual : 0};
    if (fipsd_sockbuf_clamped(out)) {
#ifdef __linux__
        FIPSD_WARN("sockbuf", "%s buffer clamped: requested=%zu actual=%zu; grant CAP_NET_ADMIN or raise net.core.rmem_max/net.core.wmem_max to 4194304 (docs/how-to/tune-udp-buffers.md)", which == FIPSD_SOCKET_RECV ? "receive" : "send", size, out->actual);
#else
        FIPSD_WARN("sockbuf", "%s buffer clamped: requested=%zu actual=%zu; inspect kern.ipc.maxsockbuf and net.inet.udp.recvspace on macOS; see docs/how-to/tune-udp-buffers.md", which == FIPSD_SOCKET_RECV ? "receive" : "send", size, out->actual);
#endif
    }
    return 0;
}
