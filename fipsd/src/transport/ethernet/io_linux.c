#define _GNU_SOURCE
#include "transport/ethernet/internal.h"
#include "libgo/fiber.h"
#include "fiber_errno.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <linux/if_packet.h>
#include <sys/ioctl.h>
#include <string.h>
#include <unistd.h>
int fipsd_eth_io_open(eth_socket *s, const fipsd_ethernet_config *c)
{
    int fd = socket(AF_PACKET, SOCK_DGRAM, htons(c->ethertype));
    if (fd < 0) return -1;
    struct ifreq req = {0}; strcpy(req.ifr_name, c->interface_name);
    if (ioctl(fd, SIOCGIFINDEX, &req)) goto fail;
    s->index = req.ifr_ifindex;
    struct sockaddr_ll ll = {.sll_family = AF_PACKET, .sll_protocol = htons(c->ethertype), .sll_ifindex = s->index};
    if (bind(fd, (struct sockaddr *)&ll, sizeof ll) || ioctl(fd, SIOCGIFHWADDR, &req)) goto fail;
    memcpy(s->mac, req.ifr_hwaddr.sa_data, 6);
    if (ioctl(fd, SIOCGIFMTU, &req) || req.ifr_mtu <= 3 || req.ifr_mtu > UINT16_MAX) goto fail;
    s->mtu = (uint16_t)(req.ifr_mtu - 3);
    if (c->mtu && c->mtu < s->mtu) s->mtu = c->mtu;
    fipsd_buffer_granted recv, send;
    if (fipsd_sockbuf_set(fd, FIPSD_SOCKET_RECV, c->recv_buf_size, &recv) ||
        fipsd_sockbuf_set(fd, FIPSD_SOCKET_SEND, c->send_buf_size, &send) ||
        fcntl(fd, F_SETFL, O_NONBLOCK) || fcntl(fd, F_SETFD, FD_CLOEXEC)) goto fail;
    s->fd = fd; return 0;
fail: { int error = errno; close(fd); errno = error; return -1; }
}
int fipsd_eth_io_bound_index(eth_socket *s)
{
    struct sockaddr_ll ll = {0}; socklen_t n = sizeof ll;
    return getsockname(s->fd, (struct sockaddr *)&ll, &n) ? s->index : ll.sll_ifindex;
}
ssize_t fipsd_eth_io_recv(eth_socket *s, uint8_t *buf, size_t cap, uint8_t mac[6])
{
    struct sockaddr_ll ll = {0}; struct iovec v = {buf, cap};
    struct msghdr msg = {.msg_name = &ll, .msg_namelen = sizeof ll, .msg_iov = &v, .msg_iovlen = 1};
    ssize_t n = gof_recvmsg(s->fd, &msg, 0, -1);
    if (n >= 0 && ((msg.msg_flags & MSG_TRUNC) || ll.sll_halen != 6 || msg.msg_namelen < sizeof ll)) { fipsd_io_set_errno(EMSGSIZE); return -1; }
    if (n >= 0) memcpy(mac, ll.sll_addr, 6);
    return n;
}
ssize_t fipsd_eth_io_send(eth_socket *s, const uint8_t mac[6], const uint8_t *buf, size_t len)
{
    struct sockaddr_ll ll = {.sll_family = AF_PACKET, .sll_protocol = htons(s->ethertype), .sll_ifindex = s->index, .sll_halen = 6};
    memcpy(ll.sll_addr, mac, 6); struct iovec v = {(void *)buf, len};
    struct msghdr msg = {.msg_name = &ll, .msg_namelen = sizeof ll, .msg_iov = &v, .msg_iovlen = 1};
    return gof_sendmsg(s->fd, &msg, 0, 0);
}
uint32_t fipsd_eth_io_drops(eth_socket *s)
{
    struct tpacket_stats st = {0}; socklen_t len = sizeof st;
    return getsockopt(s->fd, SOL_PACKET, PACKET_STATISTICS, &st, &len) ? 0 : st.tp_drops;
}
