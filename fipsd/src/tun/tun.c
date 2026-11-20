#define _DEFAULT_SOURCE 1
#define _DARWIN_C_SOURCE 1
#include "fiber_errno.h"
#include "tun/internal.h"
#include "node/netmon.h"
#include <libgo/fiber.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32) /* tun_windows.c supplies the Windows system ops */
#include <fcntl.h>
#include <net/if.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
static int sys_open(void *u,const char *p,int f) { (void)u; return open(p,f); }
static int sys_socket(void *u,int d,int t,int p) { (void)u; return socket(d,t,p); }
static int sys_ioctl(void *u,int f,unsigned long q,void *v) { (void)u; return ioctl(f,q,v); }
static int sys_connect(void *u,int f,const struct sockaddr *a,socklen_t n) { (void)u; return connect(f,a,n); }
static int sys_getsockopt(void *u,int f,int l,int q,void *v,socklen_t *n) { (void)u; return getsockopt(f,l,q,v,n); }
static int sys_prepare(void *u,int fd) { (void)u; return fipsd_network_fd_prepare(fd); }
static int sys_route_socket(void *u) { (void)u; return fipsd_network_socket(false); }
static int sys_close(void *u,int f) { (void)u; return close(f); }
static unsigned sys_index(void *u,const char *n) { (void)u; return if_nametoindex(n); }
static int sys_name(void *u,int f,char *n,size_t z)
{
    (void)u;
#ifdef __FreeBSD__
    return fdevname_r(f,n,(int)z)?0:-1;
#else
    (void)f; (void)n; (void)z; errno=ENOTSUP; return -1;
#endif
}
static int sys_pid(void *u) { (void)u; return (int)getpid(); }
static int sys_wait(void *u,int f,int ms) { (void)u; struct pollfd p={.fd=f,.events=POLLIN}; return poll(&p,1,ms); }
static ssize_t sys_read(void *u,int f,void *b,size_t n) { (void)u; return read(f,b,n); }
static ssize_t sys_write(void *u,int f,const void *b,size_t n) { (void)u; return write(f,b,n); }
static ssize_t fiber_read(void *u,int f,void *b,size_t n) { (void)u; return gof_read(f,b,n); }
static ssize_t fiber_write(void *u,int f,const void *b,size_t n) { (void)u; return gof_write(f,b,n); }
const fipsd_tun_ops *fipsd_tun_system_ops(void)
{
    static const fipsd_tun_ops o={.open=sys_open,.socket=sys_socket,.ioctl=sys_ioctl,
        .connect=sys_connect,.getsockopt=sys_getsockopt,.prepare=sys_prepare,.route_socket=sys_route_socket,
        .close=sys_close,.ifindex=sys_index,.device_name=sys_name,.pid=sys_pid,.wait_read=sys_wait,
        .read=sys_read,.write=sys_write,.frame_read=fiber_read,.frame_write=fiber_write};
    return &o;
}
#endif
int fipsd_tun_close_fd(fipsd_tun *t,int fd)
{
    int e=errno; (void)t->ops.close(t->ops.user,fd); errno=e; return -1;
}
int fipsd_tun_no_destroy(fipsd_tun *t) { (void)t; return 0; }
uint16_t fipsd_tun_mtu(uint32_t transport,uint16_t configured)
{
    uint32_t mtu=transport>FIPSD_TUN_IPV6_OVERHEAD?transport-FIPSD_TUN_IPV6_OVERHEAD:0;
    if (mtu>UINT16_MAX) mtu=UINT16_MAX;
    if (configured && mtu>configured) mtu=configured;
    if (mtu<FIPSD_TUN_IPV6_MIN_MTU) mtu=FIPSD_TUN_IPV6_MIN_MTU;
    return (uint16_t)mtu;
}
fipsd_tun *fipsd_tun_create(const fipsd_tun_config *cfg,const fipsd_tun_ops *ops,const fipsd_tun_platform *platform)
{
    if (!cfg || !cfg->name || !*cfg->name || strlen(cfg->name)>=FIPSD_TUN_NAME_SIZE) { errno=EINVAL; return NULL; }
    fipsd_tun *t=calloc(1,sizeof(*t)); if (!t) return NULL;
    t->config=*cfg; memcpy(t->configured,cfg->name,strlen(cfg->name)+1); t->config.name=t->configured;
    t->ops=ops?*ops:*fipsd_tun_system_ops(); t->platform=platform?platform:fipsd_tun_native_platform();
    if (!t->ops.wintun) t->ops.wintun=fipsd_tun_system_ops()->wintun;
    t->fd=-1; t->mtu=fipsd_tun_mtu(cfg->transport_mtu,cfg->mtu);
    if (!t->ops.open || !t->ops.socket || !t->ops.ioctl || !t->ops.connect || !t->ops.getsockopt ||
        !t->ops.prepare || !t->ops.route_socket || !t->ops.close || !t->ops.ifindex || !t->ops.device_name ||
        !t->ops.pid || !t->ops.wait_read || !t->ops.read || !t->ops.write || !t->ops.frame_read || !t->ops.frame_write ||
        !t->platform || !t->platform->open || !t->platform->address || !t->platform->mtu || !t->platform->up ||
        !t->platform->route || !t->platform->destroy) { free(t); errno=EINVAL; return NULL; }
    fipsd_tun_publish(t,FIPSD_TUN_WAITING,0); return t;
}
void fipsd_tun_destroy(fipsd_tun *t) { if (t) { (void)fipsd_tun_stop(t); free(t); } }
int fipsd_tun_fd(const fipsd_tun *t) { return t ? t->fd : -1; }
const char *fipsd_tun_name(const fipsd_tun *t) { return t && t->name[0] ? t->name : t ? t->configured : "-"; }
fips_ipv6_address fipsd_tun_address(const fipsd_tun *t) { return t ? t->address : (fips_ipv6_address){{0}}; }
uint16_t fipsd_tun_effective_mtu(const fipsd_tun *t) { return t ? t->mtu : 0; }
void fipsd_tun_set_host_drops(fipsd_tun *t, const _Atomic uint64_t *counter)
{
    if (!t) return;
    nsync_mu_lock(&t->status_mu); t->host_drops = counter; nsync_mu_unlock(&t->status_mu);
}
int fipsd_tun_header_prepend(uint8_t family,const uint8_t *p,size_t n,uint8_t *out,size_t cap)
{
    if (!family || n>65535 || cap<n+4 || !p || !out) { fipsd_io_set_errno(EMSGSIZE); return -1; }
    memmove(out+4,p,n); out[0]=out[1]=out[2]=0; out[3]=family; return (int)(n+4);
}
int fipsd_tun_header_strip(uint8_t family,uint8_t *b,size_t n)
{
    if (!b || n<4 || n>65539) { fipsd_io_set_errno(EPROTO); return -1; }
    if (b[0] || b[1] || b[2] || b[3]!=family) { fipsd_io_set_errno(EPROTONOSUPPORT); return -1; }
    memmove(b,b+4,n-4); return (int)(n-4);
}
static ssize_t frame_read(fipsd_tun *t,uint8_t *b,size_t n)
{
    return t->platform->frame_read?t->platform->frame_read(t,b,n):t->ops.frame_read(t->ops.user,t->fd,b,n);
}
static ssize_t frame_write(fipsd_tun *t,const uint8_t *b,size_t n)
{
    return t->platform->frame_write?t->platform->frame_write(t,b,n):t->ops.frame_write(t->ops.user,t->fd,b,n);
}
ssize_t fipsd_tun_read(fipsd_tun *t,uint8_t *b,size_t cap)
{
    if (!t || t->fd<0) { fipsd_io_set_errno(ENODEV); return -1; }
    if (!b || !cap || cap>65535) { fipsd_io_set_errno(EINVAL); return -1; }
    uint8_t family=t->platform->family_header;
    if (!family) return frame_read(t,b,cap);
    uint8_t *packet=malloc(cap+4); if (!packet) return -1;
    ssize_t n=frame_read(t,packet,cap+4);
    if (n>=0) {
        int stripped=fipsd_tun_header_strip(family,packet,(size_t)n);
        n=stripped;
        if (stripped>=0) memcpy(b,packet,(size_t)stripped);
    }
    int error=n<0?fipsd_io_errno():0; free(packet); if (n<0) fipsd_io_set_errno(error); return n;
}
ssize_t fipsd_tun_write(fipsd_tun *t,const uint8_t *b,size_t n)
{
    if (!t || t->fd<0) { fipsd_io_set_errno(ENODEV); return -1; }
    if (!b || !n || n>t->mtu) { fipsd_io_set_errno(EMSGSIZE); return -1; }
    uint8_t family=t->platform->family_header; uint8_t *packet=NULL; const uint8_t *data=b; size_t len=n;
    if (family) {
        packet=malloc(n+4); if (!packet) return -1;
        if (fipsd_tun_header_prepend(family,b,n,packet,n+4)<0) { free(packet); return -1; }
        data=packet; len=n+4;
    }
    ssize_t wrote=frame_write(t,data,len);
    int error=wrote<0?fipsd_io_errno():0; free(packet);
    if (wrote>=0 && wrote!=(ssize_t)len) { fipsd_io_set_errno(EIO); return -1; }
    if (wrote<0) fipsd_io_set_errno(error);
    return wrote<0?-1:(ssize_t)n;
}
