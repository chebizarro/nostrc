#define _DARWIN_C_SOURCE 1
#include "tun/internal.h"
#include <errno.h>
#include <net/if.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/kern_control.h>
#include <sys/sys_domain.h>
static int open_tun(fipsd_tun *t)
{
    const fipsd_tun_ops *o=&t->ops; void *u=o->user;
    int fd=o->socket(u,PF_SYSTEM,SOCK_DGRAM,SYSPROTO_CONTROL); if (fd<0) return -1;
    struct ctl_info info={0}; memcpy(info.ctl_name,"com.apple.net.utun_control",26);
    if (o->ioctl(u,fd,CTLIOCGINFO,&info)<0) return fipsd_tun_close_fd(t,fd);
    struct sockaddr_ctl address={0}; address.sc_len=sizeof(address);
    address.sc_family=AF_SYSTEM; address.ss_sysaddr=AF_SYS_CONTROL;
    address.sc_id=info.ctl_id; /* unit=0: kernel chooses a fresh utun */
    if (o->connect(u,fd,(struct sockaddr *)&address,sizeof(address))<0) return fipsd_tun_close_fd(t,fd);
    socklen_t len=sizeof(t->name);
    if (o->getsockopt(u,fd,SYSPROTO_CONTROL,2,t->name,&len)<0) return fipsd_tun_close_fd(t,fd);
    if (!len || len>sizeof(t->name) || !memchr(t->name,0,len)) { errno=EPROTO; return fipsd_tun_close_fd(t,fd); }
    if (o->prepare(u,fd)<0) return fipsd_tun_close_fd(t,fd);
    return fd;
}
const fipsd_tun_platform *fipsd_tun_native_platform(void)
{
    static const fipsd_tun_platform p={.os=FIPSD_TUN_MACOS,.family_header=30,.open=open_tun,
        .address=fipsd_tun_bsd_address,.mtu=fipsd_tun_bsd_mtu,.up=fipsd_tun_bsd_up,
        .route=fipsd_tun_bsd_route,.destroy=fipsd_tun_no_destroy}; return &p;
}
