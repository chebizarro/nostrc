#define _POSIX_C_SOURCE 200809L
#include "tun/internal.h"
#include "tun/lifecycle.h"
#include "transport/loopback.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct fixture { fipsd_tun *tun; fipsd_node *node; int pair[2]; unsigned opened,closed; GoChannel *done; } fixture;
static int device_open(fipsd_tun *t)
{
    fixture *f=t->ops.user;
    GoRefPtr ref=fipsd_node_status_load(fipsd_node_status_slot(f->node));
    assert(ref.ptr && ((fipsd_node_status *)ref.ptr)->ready); go_refptr_release(&ref);
    assert(fipsd_node_identity(fipsd_node_get_context(f->node)));
    ++f->opened; memcpy(t->name,"fake0",6); return f->pair[0];
}
static int address(fipsd_tun *t,bool up) {(void)t;(void)up;return 0;}
static int mtu(fipsd_tun *t) {(void)t;return 0;}
static int route(fipsd_tun *t,bool up) {t->routed=up;return 0;}
static int close_fd(void *arg,int fd) {fixture *f=arg;assert(fd==f->pair[0]);++f->closed;return close(fd);}
static unsigned index_of(void *arg,const char *name) {(void)arg;assert(!strcmp(name,"fake0"));return 3;}
static const fipsd_tun_platform platform={.os=FIPSD_TUN_MACOS,.family_header=30,.open=device_open,
    .address=address,.mtu=mtu,.up=address,.route=route,.destroy=fipsd_tun_no_destroy};
static fipsd_network_snapshot *network(void *arg) {(void)arg;return calloc(1,sizeof(fipsd_network_snapshot));}
/* Match tun.c's post-resume errno access: a compiler-cached TLS pointer may
 * still name the previous worker after a parked read migrates this fiber. */
__attribute__((noinline)) static int frame_error(void)
{ return __atomic_load_n(&errno, __ATOMIC_RELAXED); }
static void read_frame(void *arg)
{
    fixture *f=arg; uint8_t packet[1280];
    assert(fipsd_tun_read(f->tun,packet,sizeof(packet))==40 && packet[0]==0x60 && packet[39]==7);
    assert(fipsd_tun_read(f->tun,packet,sizeof(packet))<0 && frame_error()==EPROTONOSUPPORT);
    assert(go_channel_send(f->done,f)==0);
}
static void write_frame(void *arg)
{
    fixture *f=arg; uint8_t packet[40]={0x60};packet[39]=9;
    assert(fipsd_tun_write(f->tun,packet,sizeof(packet))==40);
    assert(fipsd_tun_write(f->tun,packet,1281)<0 && frame_error()==EMSGSIZE);
    assert(go_channel_send(f->done,f)==0);
}
/* Read the immutable status concurrently with I/O on migrating workers. */
static void status_reader(void *arg)
{
    fixture *f=arg;
    for (unsigned i=0;i<1000;++i) {
        fipsd_tun_status s=fipsd_tun_status_read(f->tun);
        assert(s.state==FIPSD_TUN_UP && s.index==3 && s.mtu==1280);
    }
    assert(go_channel_send(f->done,f)==0);
}
int main(int argc,char **argv)
{
    bool ready=argc==1; (void)argv;
    const char *yaml="node: {identity: {persistent: false}}\ntransports: {udp: {mtu: 1472}}\ntun: {enabled: true, mtu: 1500}\n";
    fipsd_config *config=fipsd_config_parse(yaml,strlen(yaml),NULL); assert(config);
    assert(fipsd_tun_transport_mtu(config)==1472);
    GoSwap *configs=go_swap_create(config,fipsd_config_free); assert(configs);
    fipsd_netmon_source source={.injected=true,.read=network};
    fipsd_node_options options={.netmon_source=&source,.workers=2};
    fixture f={0}; f.node=fipsd_node_create(configs,&options); assert(f.node);
    fipsd_loopback_network *net=fipsd_loopback_network_create(); assert(net);
    fipsd_transport *transport=fipsd_loopback_transport_create(net,(fips_transport_id){7},"test","loop:a",1357); assert(transport);
    if (ready) assert(!fipsd_transport_register(fipsd_node_transports(f.node),transport));
    assert(!socketpair(AF_UNIX,SOCK_DGRAM,0,f.pair));
    fipsd_tun_ops ops=*fipsd_tun_system_ops();ops.user=&f;ops.close=close_fd;ops.ifindex=index_of;
    fipsd_tun_config cfg={.name="fips0",.transport_mtu=1357,.mtu=1280};
    f.tun=fipsd_tun_create(&cfg,&ops,&platform);assert(f.tun);
    fipsd_tun_service service;fipsd_tun_service_init(&service,f.tun,f.node);
    fipsd_runtime_config rc;fipsd_runtime_config_default(&rc);rc.workers=2;rc.handle_signals=false;
    fipsd_runtime *rt=fipsd_runtime_create(&rc);assert(rt);
    assert(!fipsd_runtime_register(rt,fipsd_node_subsystem(f.node)));
    assert(!fipsd_runtime_register(rt,&service.subsystem)); assert(!fipsd_runtime_start(rt));
    f.done=go_channel_create(3);assert(f.done);
    if (ready) {
        assert(f.opened==1);
        assert(!fipsd_runtime_spawn(rt,read_frame,&f));
        assert(!fipsd_runtime_spawn(rt,write_frame,&f));
        assert(!fipsd_runtime_spawn(rt,status_reader,&f));
        uint8_t incoming[44]={0,0,0,30,0x60}; incoming[43]=7;
        assert(write(f.pair[1],incoming,sizeof(incoming))==(ssize_t)sizeof(incoming));
        incoming[3]=2; /* non-IPv6 frame, error after a potentially parked read */
        assert(write(f.pair[1],incoming,sizeof(incoming))==(ssize_t)sizeof(incoming));
        uint8_t outgoing[44];assert(read(f.pair[1],outgoing,sizeof(outgoing))==44);
        assert(outgoing[3]==30 && outgoing[4]==0x60 && outgoing[43]==9);
        for (unsigned i=0;i<3;++i) {void *value;assert(!go_channel_receive(f.done,&value) && value==&f);}
    } else assert(!f.opened && fipsd_tun_status_read(f.tun).state==FIPSD_TUN_WAITING);
    fipsd_runtime_shutdown(rt);assert(fipsd_runtime_run(rt)==FIPSD_RUN_OK);
    assert(f.closed==f.opened);
    fipsd_runtime_destroy(rt);fipsd_tun_destroy(f.tun);fipsd_node_destroy(f.node);
    fipsd_loopback_transport_destroy(transport);fipsd_loopback_network_destroy(net);go_swap_free(configs);
    if (!ready) assert(!close(f.pair[0]));
    assert(!close(f.pair[1]));go_channel_unref(f.done);
    puts("TUN runtime: node-ready gating, fiber-parked frame I/O, immutable status and ordered shutdown: ok");return 0;
}
