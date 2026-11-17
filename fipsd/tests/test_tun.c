#define _DARWIN_C_SOURCE 1
#define _DEFAULT_SOURCE 1
#include "tun/internal.h"
#include "tun/route.h"
#include "tun/lifecycle.h"
#include "host_os.h"
#include "control/protocol.h"
#include <fips/identity/address.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <net/if.h>
#ifdef __APPLE__
#include <sys/kern_control.h>
#include <sys/sys_domain.h>
#endif
#ifdef __FreeBSD__
#include <net/if_tun.h>
#endif
#ifdef __linux__
#include <linux/if_tun.h>
#endif

static fips_identity *identity(fips_secp **secp)
{
    uint8_t key[32]={0}; key[31]=1;
    assert(fips_identity_init_secp(fipsd_host_os_entropy(),secp)==FIPS_IDENTITY_OK);
    fips_identity *id=NULL;
    assert(fips_identity_from_secret_bytes(*secp,fipsd_host_os_entropy(),key,&id)==FIPS_IDENTITY_OK);
    return id;
}
static uint32_t read32(const uint8_t *p) { return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static void write32(uint8_t *p,uint32_t n) { for (unsigned i=0;i<4;++i) p[i]=(uint8_t)(n>>(i*8)); }
static void codecs(void)
{
    /* Hand-expanded Linux UAPI nlmsghdr + rtmsg + aligned rtattrs:
     * RTM_NEWROUTE, REQUEST|ACK|CREATE|EXCL, seq=7, fd00::/8,
     * main/static/link/unicast, output interface=3. */
    static const uint8_t linux_route[56]={
        56,0,0,0,24,0,5,6,7,0,0,0,0,0,0,0,
        10,8,0,0,254,4,253,1,0,0,0,0,
        20,0,1,0,253,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        8,0,4,0,3,0,0,0};
    uint8_t b[512];
    size_t n=fipsd_tun_netlink_message(b,sizeof(b),FIPSD_TUN_NL_ROUTE_ADD,7,3,NULL,1280);
    assert(n==sizeof(linux_route) && !memcmp(b,linux_route,n));
    /* Upstream bsdnet.rs tests: dst, sockaddr_dl gateway, mask in bit
     * order; XNU 92/28/20/28, FreeBSD64 152/32/56/32 (54-byte dl). */
    static const uint8_t mac_route[168]={
        [0]=168,[2]=5,[3]=1,[8]=1,[9]=8,[12]=7,[20]=7,
        [92]=28,[93]=30,[100]=253,
        [120]=20,[121]=18,[122]=3,
        [140]=28,[141]=30,[148]=255};
    static const uint8_t bsd_route[272]={
        [0]=16,[1]=1,[2]=5,[3]=1,[8]=1,[9]=8,[12]=7,[20]=7,
        [152]=28,[153]=28,[160]=253,
        [184]=54,[185]=18,[186]=3,
        [240]=28,[241]=28,[248]=255};
    n=fipsd_tun_bsd_message(b,sizeof(b),FIPSD_TUN_MACOS,1,7,0,3);
    assert(n==sizeof(mac_route) && !memcmp(b,mac_route,n));
    n=fipsd_tun_bsd_message(b,sizeof(b),FIPSD_TUN_FREEBSD,1,7,0,3);
    assert(n==sizeof(bsd_route) && !memcmp(b,bsd_route,n));
    assert(!fipsd_tun_bsd_message(b,sizeof(b),FIPSD_TUN_MACOS,1,7,0,65536));
    assert(!fipsd_tun_bsd_message(b,10,FIPSD_TUN_MACOS,1,7,0,3));
    const uint8_t addr[16]={0xfd,1,2,3};
    static const uint8_t addr_header[]={44,0,0,0,20,0,5,6,9,0,0,0,0,0,0,0,10,128,2,0,3,0,0,0,20,0,1,0};
    n=fipsd_tun_netlink_message(b,sizeof(b),FIPSD_TUN_NL_ADDRESS_ADD,9,3,addr,1280);
    assert(n==44 && !memcmp(b,addr_header,sizeof(addr_header)) && !memcmp(b+28,addr,16));
    n=fipsd_tun_netlink_message(b,sizeof(b),FIPSD_TUN_NL_MTU,9,3,NULL,1395);
    assert(n==40 && read32(b+32)==0x40008 && read32(b+36)==1395);
    n=fipsd_tun_netlink_message(b,sizeof(b),FIPSD_TUN_NL_UP,9,3,NULL,1280);
    assert(n==32 && read32(b+24)==1 && read32(b+28)==1);
    /* Full-byte family prefixes, including short/wrong-family rejection. */
    uint8_t payload[]={0x60,1,2,3,4},packet[9];
    assert(fipsd_tun_header_prepend(30,payload,5,packet,sizeof(packet))==9);
    const uint8_t expected[]={0,0,0,30,0x60,1,2,3,4}; assert(!memcmp(packet,expected,9));
    assert(fipsd_tun_header_strip(28,packet,9)<0 && errno==EPROTONOSUPPORT);
    assert(fipsd_tun_header_strip(30,packet,3)<0 && errno==EPROTO);
    assert(fipsd_tun_header_strip(30,packet,9)==5 && !memcmp(packet,payload,5));
    assert(fipsd_tun_header_prepend(28,payload,5,packet,sizeof(packet))==9 && packet[3]==28);
    assert(fipsd_tun_header_strip(28,packet,9)==5);
    struct {uint32_t transport; uint16_t ceiling,expected;} cases[]={
        {0,0,1280},{76,0,1280},{77,0,1280},{1280,0,1280},{1357,0,1280},
        {1472,0,1395},{1500,0,1423},{1500,1280,1280},{1500,1300,1300},
        {1500,42,1280},{65535,0,65458},{UINT32_MAX,0,65535},{UINT32_MAX,9000,9000}};
    for (size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i)
        assert(fipsd_tun_mtu(cases[i].transport,cases[i].ceiling)==cases[i].expected);
}
static void reply_matching(void)
{
    for (unsigned os=FIPSD_TUN_MACOS;os<=FIPSD_TUN_FREEBSD;++os) {
        uint8_t b[512]; uint32_t index=0;
        size_t n=fipsd_tun_bsd_message(b,sizeof(b),(fipsd_tun_os)os,4,7,123,0);
        b[4]=3;
        assert(!fipsd_tun_route_reply(b,n,(fipsd_tun_os)os,4,8,123,&index));
        write32(b+24,ESRCH); /* delayed absence from the PREVIOUS request */
        assert(!fipsd_tun_route_reply(b,n,(fipsd_tun_os)os,4,8,123,&index));
        write32(b+20,8); write32(b+24,0);
        assert(!fipsd_tun_route_reply(b,n,(fipsd_tun_os)os,4,8,124,&index));
        assert(fipsd_tun_route_reply(b,n,(fipsd_tun_os)os,4,8,123,&index)==1 && index==3);
        write32(b+24,EPERM);
        assert(fipsd_tun_route_reply(b,n,(fipsd_tun_os)os,4,8,123,&index)<0 && errno==EPERM);
        b[0]=1; b[1]=0; assert(fipsd_tun_route_reply(b,n,(fipsd_tun_os)os,4,8,123,&index)<0);
    }
    uint8_t ack[36]={36,0,0,0,2}; write32(ack+8,42);
    assert(!fipsd_tun_route_reply(ack,sizeof(ack),FIPSD_TUN_LINUX,2,41,0,NULL));
    assert(fipsd_tun_route_reply(ack,sizeof(ack),FIPSD_TUN_LINUX,2,42,0,NULL)==1);
    write32(ack+16,(uint32_t)-EPERM);
    assert(fipsd_tun_route_reply(ack,sizeof(ack),FIPSD_TUN_LINUX,2,42,0,NULL)<0 && errno==EPERM);
    assert(fipsd_tun_route_reply(ack,19,FIPSD_TUN_LINUX,2,42,0,NULL)<0 && errno==EPROTO);
    uint32_t previous=fipsd_tun_route_sequence();
    for (unsigned i=0;i<1000;++i) {uint32_t next=fipsd_tun_route_sequence(); assert(next && next!=previous); previous=next;}
}

typedef struct fake {
    char trace[256]; size_t at;
    char fail; int error; unsigned transient;
    uint8_t request[512]; size_t request_len;
    unsigned reads, requests, closes;
    bool stale;
} fake;
static void trace(fake *f,char c) { assert(f->at+1<sizeof(f->trace)); f->trace[f->at++]=c; f->trace[f->at]=0; }
static int action(fipsd_tun *t,char c)
{
    fake *f=t->ops.user; trace(f,c);
    if (f->fail==c) {errno=f->error; return -1;} return 0;
}
static int fake_open(fipsd_tun *t)
{
    fake *f=t->ops.user; trace(f,'O');
    if (f->transient) { --f->transient; errno=f->error; return -1; }
    if (f->fail=='O') {errno=f->error;return -1;}
    memcpy(t->name,"fake0",6); return 10;
}
static int fake_addr(fipsd_tun *t,bool add) { return action(t,add?'A':'a'); }
static int fake_mtu(fipsd_tun *t) { return action(t,'M'); }
static int fake_up(fipsd_tun *t,bool up) { return action(t,up?'U':'u'); }
static int fake_route(fipsd_tun *t,bool add)
{ int rc=action(t,add?'R':'r'); if (!rc && add) t->routed=true; return rc; }
static int fake_destroy(fipsd_tun *t) {return action(t,'D');}
static int close_fd(void *u,int fd) { fake *f=u; ++f->closes; if (fd==10) trace(f,'C'); return 0; }
static unsigned index_of(void *u,const char *name) { (void)u; assert(*name); return 3; }
static const fipsd_tun_platform platform={.os=FIPSD_TUN_MACOS,.family_header=30,
    .open=fake_open,.address=fake_addr,.mtu=fake_mtu,.up=fake_up,.route=fake_route,.destroy=fake_destroy};
static fipsd_tun *device(fake *f,const fipsd_tun_platform *p)
{
    fipsd_tun_ops ops=*fipsd_tun_system_ops(); ops.user=f; ops.close=close_fd; ops.ifindex=index_of;
    fipsd_tun_config cfg={.name="fips0",.mtu=1280,.transport_mtu=1357};
    fipsd_tun *t=fipsd_tun_create(&cfg,&ops,p); assert(t); return t;
}
static GoRefPtr base_acquire(void *arg) {GoRefPtr ref=*(GoRefPtr *)arg; go_refptr_retain(&ref); return ref;}
static void lifecycle(void)
{
    fips_secp *secp=NULL; fips_identity *id=identity(&secp);
    const char fail[]="OAMUR";
    const char *const traces[]={"O","OACD","OAMaCD","OAMUaCD","OAMURuaCD"};
    for (size_t i=0;i<sizeof(fail)-1;++i) {
        fake f={.fail=fail[i],.error=EPERM}; fipsd_tun *t=device(&f,&platform);
        assert(fipsd_tun_start(t,id)<0 && errno==EPERM);
        assert(!strcmp(f.trace,traces[i]));
        fipsd_tun_status s=fipsd_tun_status_read(t); assert(s.state==FIPSD_TUN_FAILED && s.error==EPERM);
        fipsd_tun_destroy(t);
    }
    const int retry_errors[]={EBUSY,ENOENT,ENODEV};
    for (size_t i=0;i<3;++i) {
        fake f={.transient=2,.error=retry_errors[i]}; fipsd_tun *t=device(&f,&platform);
        assert(!fipsd_tun_start(t,id) && !strcmp(f.trace,"OOOAMUR"));
        assert(fipsd_tun_status_read(t).attempts==3);
        assert(!fipsd_tun_stop(t) && !strcmp(f.trace,"OOOAMURruaCD"));
        assert(!fipsd_tun_stop(t) && !strcmp(f.trace,"OOOAMURruaCD"));
        fipsd_tun_destroy(t);
    }
    fake exhausted={.transient=4,.error=EBUSY}; fipsd_tun *t=device(&exhausted,&platform);
    assert(fipsd_tun_start(t,id)<0 && errno==EBUSY && !strcmp(exhausted.trace,"OOO")); fipsd_tun_destroy(t);
    fake pending={0}; t=device(&pending,&platform); assert(!fipsd_tun_start(t,id));
    pending.fail='D'; pending.error=EBUSY;
    assert(fipsd_tun_stop(t)<0 && t->destroy_pending && t->fd==-1);
    assert(!strcmp(t->name,"fake0"));
    assert(fipsd_tun_start(t,id)<0 && pending.at==11 && t->destroy_pending);
    pending.fail=0;
    assert(!fipsd_tun_start(t,id)); /* retries destroy BEFORE opening another clone */
    assert(!strcmp(pending.trace,"OAMURruaCDDDOAMUR"));
    assert(!fipsd_tun_stop(t) && !t->destroy_pending); fipsd_tun_destroy(t);
    fake f={0}; t=device(&f,&platform); assert(!fipsd_tun_start(t,id));
    fips_node_addr node=fips_identity_node_addr(id);
    assert(t->address.bytes[0]==0xfd && !memcmp(t->address.bytes+1,node.bytes,15));
    fipsd_tun_status s=fipsd_tun_status_read(t); assert(s.state==FIPSD_TUN_UP && s.mtu==1280 && s.index==3 && *s.address);
    json_t *rows=fipsd_tun_status_rows(&s); assert(rows);
    assert(!strcmp(json_string_value(json_object_get(rows,"tun_state")),"active"));
    assert(!strcmp(json_string_value(json_object_get(rows,"tun_name")),"fake0"));
    assert(json_integer_value(json_object_get(rows,"effective_ipv6_mtu"))==1280); json_decref(rows);
    fipsd_control_snapshot *snapshot=calloc(1,sizeof(*snapshot)); assert(snapshot);
    snapshot->npub="test"; snapshot->node_addr="00"; snapshot->ipv6_addr=s.address;
    GoRefPtr base=make_go_refptr(snapshot,free);
    fipsd_control_provider provider=fipsd_tun_provider(t,(fipsd_control_provider){.user=&base,.acquire=base_acquire});
    GoRefPtr retained=provider.acquire(provider.user);
    assert(!fipsd_tun_stop(t)); /* retained status is still up */
    json_t *response=fipsd_control_query(retained.ptr,"show_status",NULL);
    assert(!strcmp(json_string_value(json_object_get(json_object_get(response,"data"),"tun_state")),"active"));
    json_decref(response); go_refptr_release(&retained);
    json_t *request=json_pack("{s:s}","command","show_status");
    response=fipsd_control_dispatch(&provider,request); json_decref(request);
    assert(!strcmp(json_string_value(json_object_get(json_object_get(response,"data"),"tun_state")),"disabled"));
    json_decref(response); go_refptr_release(&base); fipsd_tun_destroy(t);
    fips_identity_destroy(id); fips_identity_destroy_secp(secp);
}

/* Low-level fake kernel: production native backend is exercised, not a second
 * implementation of device configuration. Every fd stays synthetic. */
static int sys_socket(void *u,int domain,int type,int proto)
{
    (void)type; (void)proto; fake *f=u;
#ifdef __APPLE__
    if (domain==PF_SYSTEM) {trace(f,'O'); return 10;}
#else
    (void)domain; (void)f;
#endif
    return 11;
}
static int sys_open(void *u,const char *path,int flags)
{(void)flags; fake *f=u; assert(strstr(path,"tun")); trace(f,'O'); return 10;}
static int sys_ioctl(void *u,int fd,unsigned long req,void *arg)
{
    (void)fd; fake *f=u;
#ifdef __APPLE__
    if (req==CTLIOCGINFO) {struct ctl_info *info=arg; assert(!strcmp(info->ctl_name,"com.apple.net.utun_control")); info->ctl_id=23; return 0;}
#endif
#ifdef __linux__
    if (req==TUNSETIFF) {struct ifreq *ifr=arg; assert(ifr->ifr_flags==(IFF_TUN|IFF_NO_PI)); return 0;}
#endif
#ifdef __FreeBSD__
    if (req==TUNSIFHEAD) {assert(*(int *)arg==1);return 0;}
#endif
    if (req==SIOCGIFFLAGS) ((struct ifreq *)arg)->ifr_flags=IFF_POINTOPOINT;
    if (req==SIOCSIFMTU) {assert(((struct ifreq *)arg)->ifr_mtu==1280); trace(f,'M');}
    return 0;
}
static int sys_connect(void *u,int fd,const struct sockaddr *sa,socklen_t len)
{
    (void)u; (void)fd; (void)len;
#ifdef __APPLE__
    const struct sockaddr_ctl *ctl=(const struct sockaddr_ctl *)sa;
    assert(ctl->sc_id==23 && ctl->sc_unit==0 && ctl->sc_family==AF_SYSTEM);
#else
    (void)sa;
#endif
    return 0;
}
static int sys_getopt(void *u,int fd,int level,int opt,void *v,socklen_t *len)
{(void)u;(void)fd;(void)level;assert(opt==2 && *len>=6);memcpy(v,"utun9",6);*len=6;return 0;}
static int sys_name(void *u,int fd,char *name,size_t cap)
{(void)u;(void)fd;assert(cap>=5);memcpy(name,"tun9",5);return 0;}
static int sys_prepare(void *u,int fd) {(void)u; assert(fd==10); return 0;}
static int sys_route(void *u) {(void)u;return 12;}
static int sys_pid(void *u) {(void)u;return 123;}
static int sys_wait(void *u,int fd,int ms) {(void)u;(void)fd;assert(ms==1000);return 1;}
static ssize_t sys_write(void *u,int fd,const void *v,size_t len)
{
    fake *f=u; assert(fd==12 && len<=sizeof(f->request));
    memcpy(f->request,v,len); f->request_len=len; ++f->requests; f->reads=0;return (ssize_t)len;
}
static ssize_t sys_read(void *u,int fd,void *v,size_t cap)
{
    fake *f=u; assert(fd==12); uint8_t *b=v; memset(b,0,cap);
    fipsd_tun_os os=fipsd_tun_native_platform()->os;
    if (os==FIPSD_TUN_LINUX) {
        b[0]=36;b[4]=2;write32(b+8,read32(f->request+8));return 36;
    }
    assert(cap>=f->request_len); memcpy(b,f->request,f->request_len); b[4]=3;
    if (f->stale && !f->reads++) {write32(b+20,read32(b+20)-1);write32(b+24,ESRCH);}
    return (ssize_t)f->request_len;
}
static void native_backend(void)
{
    fake f={.stale=true}; fipsd_tun_ops ops=*fipsd_tun_system_ops();
    ops.user=&f; ops.open=sys_open; ops.socket=sys_socket; ops.ioctl=sys_ioctl;
    ops.connect=sys_connect; ops.getsockopt=sys_getopt; ops.prepare=sys_prepare; ops.device_name=sys_name;
    ops.route_socket=sys_route; ops.close=close_fd; ops.pid=sys_pid; ops.ifindex=index_of;
    ops.read=sys_read; ops.write=sys_write; ops.wait_read=sys_wait;
    fipsd_tun_config config={.name="fips0",.transport_mtu=1357,.mtu=1280};
    fipsd_tun *t=fipsd_tun_create(&config,&ops,NULL); assert(t);
    fips_secp *secp=NULL; fips_identity *id=identity(&secp);
    assert(!fipsd_tun_start(t,id)); assert(fipsd_tun_status_read(t).state==FIPSD_TUN_UP);
    assert(!fipsd_tun_stop(t)); assert(f.requests>=3 && f.closes>=4);
    fipsd_tun_destroy(t); fips_identity_destroy(id); fips_identity_destroy_secp(secp);
}
int main(void)
{
    codecs(); reply_matching(); lifecycle(); native_backend();
    puts("TUN: MTU/identity, native fake syscalls, lifecycle rollback/retry, route bytes/sequence, headers/status: ok");
    return 0;
}
