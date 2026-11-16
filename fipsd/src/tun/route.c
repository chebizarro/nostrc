#include "tun/route.h"
#include "tun/internal.h"
#include <errno.h>
#include <stdatomic.h>
#include <string.h>
_Static_assert(sizeof(void *) == 8, "TUN route codecs require a 64-bit host ABI");
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "TUN route codecs currently support little-endian host routing ABIs"
#endif
static void u16(uint8_t *p, uint16_t n) { p[0]=(uint8_t)n; p[1]=(uint8_t)(n>>8); }
static void u32(uint8_t *p, uint32_t n) { u16(p,(uint16_t)n); u16(p+2,(uint16_t)(n>>16)); }
static uint16_t r16(const uint8_t *p) { return (uint16_t)((unsigned)p[0] | (unsigned)p[1]<<8); }
static uint32_t r32(const uint8_t *p) { return (uint32_t)r16(p) | (uint32_t)r16(p+2)<<16; }
static _Atomic uint32_t sequence = 1;
uint32_t fipsd_tun_route_sequence(void)
{
    uint32_t n;
    do { n = atomic_fetch_add_explicit(&sequence, 1, memory_order_relaxed); } while (!n);
    return n;
}
static size_t attr(uint8_t *p, uint16_t type, const void *v, size_t n)
{
    u16(p,(uint16_t)(n+4)); u16(p+2,type); memcpy(p+4,v,n);
    return (n+7)&~(size_t)3;
}
size_t fipsd_tun_netlink_message(uint8_t *b, size_t cap, fipsd_tun_netlink_kind kind,
    uint32_t seq, uint32_t index, const uint8_t address[16], uint16_t mtu)
{
    if (cap < 80 || !index) return 0;
    memset(b,0,cap);
    uint16_t type, flags=5; size_t n;
    uint8_t value[4];
    switch (kind) {
    case FIPSD_TUN_NL_ADDRESS_ADD: case FIPSD_TUN_NL_ADDRESS_DEL:
        if (!address) return 0;
        type=kind==FIPSD_TUN_NL_ADDRESS_ADD?20:21;
        if (kind==FIPSD_TUN_NL_ADDRESS_ADD) flags|=0x600;
        b[16]=10; b[17]=128; b[18]=2; /* AF_INET6, /128, IFA_F_NODAD */
        u32(b+20,index); n=24; n+=attr(b+n,1,address,16); break;
    case FIPSD_TUN_NL_MTU: case FIPSD_TUN_NL_UP: case FIPSD_TUN_NL_DOWN:
        type=16; u32(b+20,index); n=32;
        if (kind==FIPSD_TUN_NL_MTU) { u32(value,mtu); n+=attr(b+n,4,value,4); }
        else { u32(b+24,kind==FIPSD_TUN_NL_UP?1:0); u32(b+28,1); }
        break;
    case FIPSD_TUN_NL_ROUTE_ADD: case FIPSD_TUN_NL_ROUTE_DEL:
        type=kind==FIPSD_TUN_NL_ROUTE_ADD?24:25;
        if (kind==FIPSD_TUN_NL_ROUTE_ADD) flags|=0x600;
        b[16]=10; b[17]=8; b[20]=254; b[21]=4; b[22]=253; b[23]=1;
        n=28; { uint8_t dst[16]={0xfd}; n+=attr(b+n,1,dst,16); }
        u32(value,index); n+=attr(b+n,4,value,4); break;
    default: return 0;
    }
    u32(b,(uint32_t)n); u16(b+4,type); u16(b+6,flags); u32(b+8,seq);
    return n;
}
size_t fipsd_tun_bsd_message(uint8_t *b, size_t cap, fipsd_tun_os os, uint8_t type,
    uint32_t seq, uint32_t pid, uint32_t index)
{
    if ((os!=FIPSD_TUN_MACOS && os!=FIPSD_TUN_FREEBSD) || index>65535) return 0;
    size_t h=os==FIPSD_TUN_MACOS?92:152;
    size_t in6=os==FIPSD_TUN_MACOS?28:32;
    /* sizeof(sockaddr_dl): XNU 20, FreeBSD 54; padding 4 / sizeof(long). */
    size_t dl=os==FIPSD_TUN_MACOS?20:56;
    size_t n=h+in6*2+(index?dl:0);
    if (cap<n) return 0;
    memset(b,0,n); u16(b,(uint16_t)n); b[2]=5; b[3]=type;
    u32(b+8,0x801); u32(b+12,index?7:5); u32(b+16,pid); u32(b+20,seq);
    size_t at=h;
    b[at]=28; b[at+1]=os==FIPSD_TUN_MACOS?30:28; b[at+8]=0xfd; at+=in6;
    if (index) {
        b[at]=os==FIPSD_TUN_MACOS?20:54; b[at+1]=18;
        u16(b+at+2,(uint16_t)index); at+=dl;
    }
    b[at]=28; b[at+1]=os==FIPSD_TUN_MACOS?30:28; b[at+8]=0xff;
    return n;
}
int fipsd_tun_route_reply(const uint8_t *b, size_t n, fipsd_tun_os os,
    uint8_t type, uint32_t seq, uint32_t pid, uint32_t *index)
{
    if (os==FIPSD_TUN_LINUX) {
        for (size_t at=0; at<n;) {
            if (n-at<16) { errno=EPROTO; return -1; }
            uint32_t len=r32(b+at);
            if (len<16 || len>n-at) { errno=EPROTO; return -1; }
            if (r32(b+at+8)==seq && r16(b+at+4)==2) {
                if (len<20) { errno=EPROTO; return -1; }
                int32_t error=(int32_t)r32(b+at+16);
                if (error>0 || error==INT32_MIN) { errno=EPROTO; return -1; }
                if (error) { errno=-error; return -1; }
                return 1;
            }
            at+=(len+3u)&~(size_t)3;
        }
        return 0;
    }
    size_t h=os==FIPSD_TUN_MACOS?92:152;
    if (n<h) return 0;
    if (r16(b)<h || r16(b)>n || b[2]!=5) { errno=EPROTO; return -1; }
    if (b[3]!=type || r32(b+16)!=pid || r32(b+20)!=seq) return 0;
    uint32_t error=r32(b+24);
    if (error) { errno=error>INT32_MAX?EPROTO:(int)error; return -1; }
    if (index) *index=r16(b+4);
    return 1;
}
int fipsd_tun_route_exchange(fipsd_tun *t, const uint8_t *msg, size_t len,
    uint8_t type, uint32_t seq, uint32_t *index)
{
    const fipsd_tun_ops *o=&t->ops; void *u=o->user;
    int fd=o->route_socket(u); if (fd<0) return -1;
    int result=-1, error=0;
    ssize_t written;
    do { written=o->write(u,fd,msg,len); } while (written<0 && errno==EINTR);
    if (written!=(ssize_t)len) { error=written<0?errno:EIO; goto done; }
    for (unsigned i=0;i<64;++i) {
        int ready=o->wait_read(u,fd,1000);
        if (ready<0 && errno==EINTR) { --i; continue; }
        if (ready<=0) { error=ready<0?errno:ETIMEDOUT; goto done; }
        uint8_t bytes[4096]; ssize_t n=o->read(u,fd,bytes,sizeof(bytes));
        if (n<0 && (errno==EINTR || errno==EAGAIN)) continue;
        if (n<=0) { error=n<0?errno:EIO; goto done; }
        int rc=fipsd_tun_route_reply(bytes,(size_t)n,t->platform->os,type,seq,(uint32_t)o->pid(u),index);
        if (rc) { result=rc>0?0:-1; error=rc<0?errno:0; goto done; }
    }
    error=ETIMEDOUT;
done:
    (void)o->close(u,fd); if (error) errno=error; return result;
}
