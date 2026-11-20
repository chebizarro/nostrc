#define __BSD_VISIBLE 1
#include "tun/internal.h"
#include <fcntl.h>
#include <errno.h>
#include <net/if.h>
#include <net/if_tun.h>
#include <string.h>
#include <sys/ioctl.h>
static int open_tun(fipsd_tun *t)
{
    const fipsd_tun_ops *o=&t->ops; void *u=o->user;
    int fd=o->open(u,"/dev/tun",O_RDWR|O_CLOEXEC|O_NONBLOCK); if (fd<0) return -1;
    int head=1;
    /* Discover ownership before configuration so a refused ioctl still
     * destroys only the fresh clone, not a configured/foreign interface. */
    if (o->device_name(u,fd,t->name,sizeof(t->name))<0) return fipsd_tun_close_fd(t,fd);
    t->destroy_pending=true;
    if (o->ioctl(u,fd,TUNSIFHEAD,&head)<0 || o->prepare(u,fd)<0)
        return fipsd_tun_close_fd(t,fd); /* lifecycle retains clone ownership */
    return fd;
}
static int destroy(fipsd_tun *t)
{
    struct ifreq req={0}; memcpy(req.ifr_name,t->name,sizeof(t->name));
    int fd=t->ops.socket(t->ops.user,AF_INET6,SOCK_DGRAM,0); if (fd<0) return -1;
    if (t->ops.ioctl(t->ops.user,fd,SIOCIFDESTROY,&req)<0) {
        int error=errno;
        (void)t->ops.close(t->ops.user,fd);
        /* Some clone configurations disappear on last close already. */
        if (error==ENXIO || error==ENODEV || error==ENOENT) return 0;
        errno=error; return -1;
    }
    return t->ops.close(t->ops.user,fd);
}
const fipsd_tun_platform *fipsd_tun_native_platform(void)
{
    static const fipsd_tun_platform p={.os=FIPSD_TUN_FREEBSD,.family_header=28,.open=open_tun,
        .address=fipsd_tun_bsd_address,.mtu=fipsd_tun_bsd_mtu,.up=fipsd_tun_bsd_up,
        .route=fipsd_tun_bsd_route,.destroy=destroy}; return &p;
}
