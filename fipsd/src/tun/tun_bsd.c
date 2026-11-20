#define _DARWIN_C_SOURCE 1
#define __BSD_VISIBLE 1
#include "tun/internal.h"
#include "tun/route.h"
#include <errno.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/route.h>
#include <stddef.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#if defined(__FreeBSD__)
#include <netinet6/nd6.h>
#include <netinet6/in6_var.h>
#endif
_Static_assert(sizeof(struct sockaddr_in6)==28,"BSD sockaddr_in6 ABI");
_Static_assert(offsetof(struct rt_msghdr,rtm_seq)==20,"BSD route sequence offset");
_Static_assert(offsetof(struct rt_msghdr,rtm_pid)==16,"BSD route pid offset");
_Static_assert(offsetof(struct rt_msghdr,rtm_errno)==24,"BSD route errno offset");
#ifdef __APPLE__
_Static_assert(sizeof(struct rt_msghdr)==92 && sizeof(struct sockaddr_dl)==20,"XNU route ABI");
#else
_Static_assert(sizeof(struct rt_msghdr)==152 && sizeof(struct sockaddr_dl)==54,"FreeBSD64 route ABI");
#endif
/* XNU's public SDK omits in6_aliasreq. These are the repr(C) fields from
 * upstream ipv6tun/bsdnet.rs, verified by its size/ioctl assertions. */
struct tun_lifetime { time_t expire, preferred; uint32_t valid, preferred_valid; };
struct tun_alias {
    char name[IFNAMSIZ];
    struct sockaddr_in6 address, destination, mask;
    int flags;
    struct tun_lifetime lifetime;
#ifdef __FreeBSD__
    int vhid;
#endif
};
#ifdef __APPLE__
_Static_assert(sizeof(struct tun_alias)==128,"XNU in6_aliasreq ABI");
#define TUN_ADD_ADDRESS 0x8080691aUL
#else
_Static_assert(sizeof(struct tun_alias)==136,"FreeBSD in6_aliasreq ABI");
#define TUN_ADD_ADDRESS 0x8088691bUL
#endif
static int control_ioctl(fipsd_tun *t,unsigned long request,void *arg)
{
    int fd=t->ops.socket(t->ops.user,AF_INET6,SOCK_DGRAM,0); if (fd<0) return -1;
    if (t->ops.ioctl(t->ops.user,fd,request,arg)<0) return fipsd_tun_close_fd(t,fd);
    return t->ops.close(t->ops.user,fd);
}
int fipsd_tun_bsd_address(fipsd_tun *t,bool add)
{
    struct tun_alias req={0}; memcpy(req.name,t->name,sizeof(t->name));
    req.address.sin6_len=sizeof(req.address); req.address.sin6_family=AF_INET6;
    memcpy(&req.address.sin6_addr,t->address.bytes,16);
    req.mask.sin6_len=sizeof(req.mask); req.mask.sin6_family=AF_INET6;
    memset(&req.mask.sin6_addr,255,16);
    req.lifetime.valid=req.lifetime.preferred_valid=UINT32_MAX;
    req.flags=0x0020; /* IN6_IFF_NODAD: usable immediately (point-to-point). */
    if (!add) {
        /* in6_ifreq starts with name + address, like aliasreq; ioctl only
         * reads those fields. Command encodes sizeof(in6_ifreq)=288. */
        uint8_t remove[288]={0}; memcpy(remove,req.name,16);
        memcpy(remove+16,&req.address,sizeof(req.address));
        return control_ioctl(t,0x81206919UL,remove); /* SIOCDIFADDR_IN6 */
    }
    return control_ioctl(t,TUN_ADD_ADDRESS,&req);
}
int fipsd_tun_bsd_mtu(fipsd_tun *t)
{
    struct ifreq req={0}; memcpy(req.ifr_name,t->name,sizeof(t->name)); req.ifr_mtu=t->mtu;
    return control_ioctl(t,SIOCSIFMTU,&req);
}
int fipsd_tun_bsd_up(fipsd_tun *t,bool up)
{
#ifdef __FreeBSD__
    if (up) {
        struct in6_ndireq nd={0}; memcpy(nd.ifname,t->name,sizeof(t->name));
        if (control_ioctl(t,SIOCGIFINFO_IN6,&nd)<0) return -1;
        nd.ndi.flags &= ~ND6_IFF_IFDISABLED;
        if (control_ioctl(t,SIOCSIFINFO_IN6,&nd)<0) return -1;
    }
#endif
    struct ifreq req={0}; memcpy(req.ifr_name,t->name,sizeof(t->name));
    if (control_ioctl(t,SIOCGIFFLAGS,&req)<0) return -1;
    if (up) req.ifr_flags |= IFF_UP; else req.ifr_flags &= (short)~IFF_UP;
    return control_ioctl(t,SIOCSIFFLAGS,&req);
}
static int request(fipsd_tun *t,uint8_t type,uint32_t gateway,uint32_t *index)
{
    uint8_t bytes[288]; uint32_t seq=fipsd_tun_route_sequence();
    size_t n=fipsd_tun_bsd_message(bytes,sizeof(bytes),t->platform->os,type,seq,
        (uint32_t)t->ops.pid(t->ops.user),gateway);
    if (!n) { errno=EINVAL; return -1; }
    return fipsd_tun_route_exchange(t,bytes,n,type,seq,index);
}
int fipsd_tun_bsd_route(fipsd_tun *t,bool add)
{
    if (!add) {
        uint32_t index=0;
        if (request(t,4,0,&index)<0) return errno==ESRCH?0:-1;
        if (index!=t->index) return 0; /* replacement is not ours */
        return request(t,2,t->index,NULL);
    }
    if (!request(t,1,t->index,NULL)) { t->routed=true; return 0; }
    if (errno!=EEXIST) return -1;
    uint32_t index=0;
    if (!request(t,4,0,&index)) {
        if (index==t->index) return 0; /* borrowed, never delete on unwind */
        errno=EEXIST; return -1;
    }
    /* Route disappeared between add and lookup: try once, as upstream does. */
    if (errno==ESRCH && !request(t,1,t->index,NULL)) { t->routed=true; return 0; }
    return -1;
}
