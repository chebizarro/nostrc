#define _DEFAULT_SOURCE 1
#include "tun/internal.h"
#include "tun/route.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <string.h>
#include <sys/ioctl.h>
static int open_tun(fipsd_tun *t)
{
    const fipsd_tun_ops *o=&t->ops; void *u=o->user;
    int fd=o->open(u,"/dev/net/tun",O_RDWR|O_CLOEXEC|O_NONBLOCK); if (fd<0) return -1;
    struct ifreq req={0}; memcpy(req.ifr_name,t->configured,strlen(t->configured)+1);
    req.ifr_flags=IFF_TUN|IFF_NO_PI;
    if (o->ioctl(u,fd,TUNSETIFF,&req)<0) return fipsd_tun_close_fd(t,fd);
    memcpy(t->name,req.ifr_name,sizeof(t->name)); t->name[sizeof(t->name)-1]=0;
    if (o->prepare(u,fd)<0) return fipsd_tun_close_fd(t,fd);
    return fd;
}
static int change(fipsd_tun *t,fipsd_tun_netlink_kind kind)
{
    uint32_t seq=fipsd_tun_route_sequence(); uint8_t bytes[80];
    size_t n=fipsd_tun_netlink_message(bytes,sizeof(bytes),kind,seq,t->index,t->address.bytes,t->mtu);
    if (!n) { errno=EINVAL; return -1; }
    return fipsd_tun_route_exchange(t,bytes,n,2,seq,NULL);
}
static int address(fipsd_tun *t,bool add) { return change(t,add?FIPSD_TUN_NL_ADDRESS_ADD:FIPSD_TUN_NL_ADDRESS_DEL); }
static int mtu(fipsd_tun *t) { return change(t,FIPSD_TUN_NL_MTU); }
static int up(fipsd_tun *t,bool raise) { return change(t,raise?FIPSD_TUN_NL_UP:FIPSD_TUN_NL_DOWN); }
static int route(fipsd_tun *t,bool add)
{
    int rc=change(t,add?FIPSD_TUN_NL_ROUTE_ADD:FIPSD_TUN_NL_ROUTE_DEL);
    if (!rc && add) t->routed=true;
    return rc;
}
const fipsd_tun_platform *fipsd_tun_native_platform(void)
{
    static const fipsd_tun_platform p={.os=FIPSD_TUN_LINUX,.open=open_tun,.address=address,
        .mtu=mtu,.up=up,.route=route,.destroy=fipsd_tun_no_destroy}; return &p;
}
