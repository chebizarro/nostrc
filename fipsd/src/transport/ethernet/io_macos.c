#define _DARWIN_C_SOURCE 1
#include "transport/ethernet/internal.h"
#include "transport/presence.h"
#include "libgo/fiber.h"
#include "fiber_errno.h"
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
/* Explicit pinned Darwin ABI, matching upstream io_macos.rs repr(C). */
typedef struct bpf_insn_abi { uint16_t code; uint8_t jt, jf; uint32_t k; } bpf_insn_abi;
typedef struct bpf_program_abi { uint32_t len; const bpf_insn_abi *insns; } bpf_program_abi;
typedef struct bpf_stat_abi { uint32_t recv, drop; } bpf_stat_abi;
_Static_assert(sizeof(bpf_insn_abi) == 8, "BPF instruction ABI");
_Static_assert(sizeof(bpf_stat_abi) == 8, "BPF statistics ABI");
_Static_assert(sizeof(bpf_program_abi) == 16, "BPF program ABI");
int fipsd_eth_io_open(eth_socket *s, const fipsd_ethernet_config *c)
{
    int fd = -1;
    for (unsigned i = 0; i < 256; ++i) {
        char path[32]; snprintf(path, sizeof path, "/dev/bpf%u", i);
        fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0 || errno != EBUSY) break;
    }
    if (fd < 0) return -1;
    unsigned size = 512 * 1024, one = 1;
    uint8_t req[32] = {0}; memcpy(req, c->interface_name, strlen(c->interface_name));
    bpf_insn_abi insns[] = {{0x28,0,0,12}, {0x15,0,1,c->ethertype}, {0x06,0,0,65535}, {0x06,0,0,0}};
    bpf_program_abi program = {4, insns};
    if (ioctl(fd, 0xc0044266UL, &size) || ioctl(fd, 0x8020426cUL, req) ||
        ioctl(fd, 0x80044270UL, &one) || ioctl(fd, 0x80044275UL, &one) ||
        ioctl(fd, 0x80104267UL, &program) || ioctl(fd, 0x40044266UL, &size)) goto fail;
    fipsd_presence_facts facts = fipsd_presence_read(c->interface_name);
    if (!facts.valid || !facts.exists || !facts.has_mac) { errno = ENODEV; goto fail; }
    s->index = (int)facts.index; memcpy(s->mac, facts.mac, 6);
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) goto fail;
    struct ifreq mtu = {0}; strcpy(mtu.ifr_name, c->interface_name);
    int rc = ioctl(sock, SIOCGIFMTU, &mtu); int error = errno; close(sock); errno = error;
    if (rc || mtu.ifr_mtu <= 3 || mtu.ifr_mtu > UINT16_MAX) goto fail;
    s->mtu = (uint16_t)(mtu.ifr_mtu - 3);
    if (c->mtu && c->mtu < s->mtu) s->mtu = c->mtu;
    s->batch = malloc(size); if (!s->batch) goto fail;
    s->batch_capacity = size; s->fd = fd; return 0;
fail: { int saved = errno; close(fd); errno = saved; return -1; }
}
int fipsd_eth_io_bound_index(eth_socket *s) { return s->index; }
ssize_t fipsd_eth_io_recv(eth_socket *s, uint8_t *buf, size_t cap, uint8_t mac[6])
{
    for (;;) {
        const uint8_t *p; size_t n;
        if (fipsd_eth_bpf_next(s->batch, s->batch_len, &s->batch_at, s->ethertype, &p, &n, mac)) {
            if (n > cap) { fipsd_io_set_errno(EMSGSIZE); return -1; }
            memcpy(buf, p, n); return (ssize_t)n;
        }
        ssize_t got = gof_read(s->fd, s->batch, s->batch_capacity);
        if (got <= 0) return got;
        s->batch_len = (size_t)got; s->batch_at = 0;
    }
}
ssize_t fipsd_eth_io_send(eth_socket *s, const uint8_t mac[6], const uint8_t *buf, size_t len)
{
    uint8_t *frame = malloc(len + 14); if (!frame) return -1;
    memcpy(frame, mac, 6); memcpy(frame + 6, s->mac, 6);
    frame[12] = (uint8_t)(s->ethertype >> 8); frame[13] = (uint8_t)s->ethertype; memcpy(frame + 14, buf, len);
    ssize_t n = gof_write(s->fd, frame, len + 14); free(frame);
    return n == (ssize_t)(len + 14) ? (ssize_t)len : -1;
}
uint32_t fipsd_eth_io_drops(eth_socket *s)
{
    bpf_stat_abi st = {0}; if (ioctl(s->fd, 0x4008426fUL, &st)) return 0;
    uint32_t delta = st.drop - s->drops_seen; s->drops_seen = st.drop; return delta;
}
