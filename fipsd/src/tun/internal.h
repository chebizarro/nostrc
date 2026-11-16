#ifndef FIPSD_TUN_INTERNAL_H
#define FIPSD_TUN_INTERNAL_H
#include "tun/status.h"
#include <nsync.h>
struct fipsd_tun {
    fipsd_tun_config config;
    char configured[FIPSD_TUN_NAME_SIZE], name[FIPSD_TUN_NAME_SIZE];
    fipsd_tun_ops ops;
    const fipsd_tun_platform *platform;
    fips_ipv6_address address;
    int fd;
    uint32_t index;
    uint16_t mtu;
    bool addressed, raised, routed, destroy_pending;
    nsync_mu status_mu;
    fipsd_tun_status status;
    fipsd_control_provider base;
};
void fipsd_tun_publish(fipsd_tun *, fipsd_tun_state, int);
/* Callbacks shared by the two BSD backends. */
int fipsd_tun_bsd_address(fipsd_tun *, bool);
int fipsd_tun_bsd_mtu(fipsd_tun *);
int fipsd_tun_bsd_up(fipsd_tun *, bool);
int fipsd_tun_bsd_route(fipsd_tun *, bool);
int fipsd_tun_no_destroy(fipsd_tun *);
int fipsd_tun_close_fd(fipsd_tun *, int);
#endif
