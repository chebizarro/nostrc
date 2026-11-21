#include <fips/proto/settle/settle.h>
#include "fakes/host.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); abort(); } } while (0)
static fips_settle_pair pair(unsigned n)
{
    fips_settle_pair p={.transport={n/8+1}};
    uint8_t b=(uint8_t)(n%8);
    CHECK(fips_transport_addr_new(&b,1,&p.addr)==FIPS_OK);
    return p;
}
static fips_settle_holder holder(unsigned n)
{
    fips_settle_holder h={.peer=n!=0}; memset(h.identity.bytes,(int)n,16); return h;
}
static void release(fips_rekey_index_lease *l, unsigned expected)
{
    uint32_t n=0; CHECK(fips_rekey_index_lease_release(l,&n)); CHECK(n==expected); CHECK(!l->live);
}
static uint64_t open_link(fips_settle_links *s, unsigned p, unsigned h)
{
    uint64_t n; fips_settle_pair addr=pair(p);
    CHECK(fips_settle_link_open(s,&addr,FIPS_LINK_OUTBOUND,holder(h),&n)==FIPS_OK); return n;
}
static bool expected(void *ctx, uint64_t link, fips_node_addr *p)
{
    (void)link; if (!ctx) return false; memset(p->bytes,3,16); return true;
}
static void links_test(void)
{
    fips_settle_links s; fips_settle_link slots[4]; fips_settle_path paths[16];
    CHECK(fips_settle_links_init(&s,slots,4,paths,4)==FIPS_OK);
    uint64_t a=open_link(&s,1,2), b=open_link(&s,1,0), c=open_link(&s,3,3);
    fips_settle_pair out[8], p=pair(1), q=pair(2), r=pair(3); size_t count=77;
    CHECK(fips_settle_link_retire(&s,a,out,8,&count)==FIPS_OK && count==0); /* shared leg */
    CHECK(fips_settle_link_retire(&s,b,out,0,&count)==FIPS_ERR_RANGE && count==0);
    CHECK(fips_settle_link_get(&s,b));
    CHECK(fips_settle_link_retire(&s,b,out,8,&count)==FIPS_OK && count==1);
    CHECK(fips_settle_pair_equal(&out[0],&p));
    CHECK(!fips_settle_link_indexed(&s,b,&p));
    CHECK(fips_settle_link_path(&s,c,&q,FIPS_SETTLE_ACKED)==FIPS_OK);
    a=open_link(&s,2,4);
    CHECK(fips_settle_link_retire(&s,a,out,8,&count)==FIPS_OK && count==0); /* held proven path */
    CHECK(fips_settle_link_path(&s,c,&q,FIPS_SETTLE_PROBING)==FIPS_OK);
    a=open_link(&s,2,4);
    CHECK(fips_settle_link_retire(&s,a,out,8,&count)==FIPS_OK && count==1);
    CHECK(fips_settle_link_path(&s,c,&q,FIPS_SETTLE_LIVE)==FIPS_OK);
    CHECK(fips_settle_link_release(&s,c,&q,true,out,8,&count)==FIPS_OK && count==0);
    CHECK(!fips_settle_link_indexed(&s,c,&q));
    CHECK(fips_settle_link_path(&s,c,&r,FIPS_SETTLE_LIVE)==FIPS_OK);
    CHECK(fips_settle_link_moved(&s,c,&r,&q)==FIPS_OK);
    CHECK(fips_settle_link_indexed(&s,c,&r)); /* roamed setup remains */
    fips_settle_placement place=fips_settle_place(&s,&r,c,expected,&s);
    CHECK(place.established && place.has_expected && place.expected.bytes[0]==3 && place.on_link);
    fips_settle_link_capable(&s,c);
    place=fips_settle_place(&s,&r,c,expected,&s); CHECK(!place.on_link);
    CHECK(fips_settle_link_release(&s,c,&r,true,out,8,&count)==FIPS_OK && count==0);
    CHECK(!fips_settle_link_indexed(&s,c,&r));
    /* Preflight never mutates a full path arena or insufficient close output. */
    for (unsigned i=4; i<7; ++i) { p=pair(i); CHECK(fips_settle_link_path(&s,c,&p,FIPS_SETTLE_LIVE)==FIPS_OK); }
    fips_settle_link before=*fips_settle_link_get(&s,c); fips_settle_path saved[4]; memcpy(saved,before.paths,sizeof saved);
    p=pair(7);
    CHECK(fips_settle_link_path(&s,c,&p,FIPS_SETTLE_LIVE)==FIPS_ERR_RANGE);
    CHECK(fips_settle_link_moved(&s,c,&r,&p)==FIPS_ERR_RANGE);
    CHECK(!memcmp(saved,before.paths,sizeof saved));
    count=77; CHECK(fips_settle_link_release(&s,c,&q,false,out,0,&count)==FIPS_ERR_RANGE && count==77);
    CHECK(!memcmp(saved,before.paths,sizeof saved));
    CHECK(fips_settle_link_retire(&s,c,out,8,&count)==FIPS_OK);
    CHECK(!fips_settle_link_get(&s,c));
    /* Newest named leg without expected identity falls back to peer path. */
    a=open_link(&s,1,4); b=open_link(&s,2,0); p=pair(2);
    CHECK(fips_settle_link_path(&s,a,&p,FIPS_SETTLE_ACKED)==FIPS_OK);
    place=fips_settle_place(&s,&p,a,expected,NULL);
    CHECK(place.has_expected && place.expected.bytes[0]==4 && place.on_link);
    fips_settle_link_hold(&s,b,holder(5));
    place=fips_settle_place(&s,&p,a,expected,NULL); CHECK(place.expected.bytes[0]==5);
    /* Storage and ID exhaustion leave outputs alone. */
    s.next=UINT64_MAX; uint64_t number=77;
    CHECK(fips_settle_link_open(&s,&p,FIPS_LINK_OUTBOUND,holder(0),&number)==FIPS_ERR_RANGE && number==77);
}
static void legs_test(void)
{
    fips_settle_links links; fips_settle_link records[3]; fips_settle_path paths[6];
    fips_settle_legs legs; fips_settle_leg slots[3], old, taken;
    fips_settle_legs_init(&legs,slots,3);
    CHECK(fips_settle_links_init(&links,records,3,paths,2)==FIPS_OK);
    fips_settle_params p=fips_settle_params_default();
    fips_rekey_index_lease lease={.index=42,.live=true};
    uint64_t link=open_link(&links,1,0);
    const uint8_t msg1[]={1,2,3}, msg2[]={4,5};
    fips_settle_start start={.link=link,.has_peer=true,.peer={{2}},.has_held=true,.held=99,
        .msg1=msg1,.msg1_len=3,.lease=&lease};
    CHECK(fips_settle_leg_open(&legs,&start,false,NULL,0,fips_test_instant(0),&p,&old)==FIPS_OK);
    CHECK(!lease.live && !old.active);
    fips_instant wake; CHECK(fips_settle_legs_next_wake(&legs,&wake) && wake.opaque[0]==1000);
    fips_settle_leg_out outs[3]; fips_settle_pair closes[9]; size_t n=77, cn=77;
    CHECK(fips_settle_legs_poll(&legs,&links,fips_test_instant(999),false,outs,3,&n,closes,9,&cn)==FIPS_OK && n==0);
    CHECK(fips_settle_legs_poll(&legs,&links,fips_test_instant(1000),false,outs,0,&n,closes,9,&cn)==FIPS_ERR_RANGE);
    CHECK(fips_settle_legs_poll(&legs,&links,fips_test_instant(1000),false,outs,3,&n,closes,9,&cn)==FIPS_OK && n==1);
    CHECK(!outs[0].reaped && outs[0].length==3 && !memcmp(outs[0].bytes,msg1,3));
    fips_settle_leg_unsent(&legs,link,fips_test_instant(1000),&p);
    CHECK(fips_settle_legs_next_wake(&legs,&wake) && wake.opaque[0]==2000);
    CHECK(fips_settle_legs_poll(&legs,&links,fips_test_instant(1001),false,outs,3,&n,closes,9,&cn)==FIPS_OK && n==1);
    fips_settle_leg_resent(&legs,link,fips_test_instant(1001),&p);
    CHECK(fips_settle_legs_next_wake(&legs,&wake) && wake.opaque[0]==3001);
    /* Take moves the lease, leaves no hidden owning copy. */
    CHECK(fips_settle_leg_take(&legs,link,&taken)); CHECK(taken.lease.live && taken.held==99);
    CHECK(!fips_settle_leg_get(&legs,link)); release(&taken.lease,42);
    lease=(fips_rekey_index_lease){.index=43,.live=true};
    CHECK(fips_settle_leg_open(&legs,&start,true,msg2,2,fips_test_instant(0),&p,&old)==FIPS_OK);
    size_t len=99; const uint8_t *stored=fips_settle_leg_stored(&legs,link,msg1,3,&len);
    CHECK(stored && len==2 && !memcmp(stored,msg2,2));
    CHECK(!fips_settle_leg_stored(&legs,link,msg2,2,&len));
    CHECK(fips_settle_legs_answering(&legs)==1);
    fips_settle_leg_unsent(&legs,link,fips_test_instant(0),&p);
    CHECK(fips_settle_legs_next_wake(&legs,&wake) && wake.opaque[0]==30000);
    lease=(fips_rekey_index_lease){.index=44,.live=true};
    start.msg1_len=FIPS_SETTLE_MESSAGE_CAPACITY+1;
    CHECK(fips_settle_leg_open(&legs,&start,false,NULL,0,fips_test_instant(0),&p,&old)==FIPS_ERR_RANGE && lease.live);
    start.msg1_len=3; start.has_cap=true; start.cap=0;
    CHECK(fips_settle_leg_open(&legs,&start,false,NULL,0,fips_test_instant(0),&p,&old)==FIPS_OK);
    CHECK(old.active && old.answer); release(&old.lease,43);
    fips_settle_leg_unsent(&legs,link,fips_test_instant(1),&p);
    CHECK(!fips_settle_leg_get(&legs,link)->has_next);
    n=77; cn=88;
    CHECK(fips_settle_legs_poll(&legs,&links,fips_test_instant(30000),false,outs,3,&n,closes,0,&cn)==FIPS_ERR_RANGE);
    CHECK(n==77 && cn==88 && fips_settle_leg_get(&legs,link)->lease.live);
    CHECK(fips_settle_legs_poll(&legs,&links,fips_test_instant(30000),false,outs,3,&n,closes,9,&cn)==FIPS_OK);
    CHECK(n==1 && cn==1 && outs[0].reaped && outs[0].dialed); release(&outs[0].lease,44);
    CHECK(!fips_settle_legs_next_wake(&legs,&wake));
    /* Closing unsorted slots returns every lease in ascending link order. */
    for (unsigned i=0; i<3; ++i) {
        start.link=open_link(&links,i+2,0); start.lease=&lease;
        lease=(fips_rekey_index_lease){.index=i+10,.live=true};
        CHECK(fips_settle_leg_open(&legs,&start,false,NULL,0,fips_test_instant(0),&p,&old)==FIPS_OK);
    }
    uint64_t first=slots[0].link;
    CHECK(fips_settle_leg_take(&legs,first,&taken)); release(&taken.lease,10);
    CHECK(fips_settle_link_retire(&links,first,closes,9,&cn)==FIPS_OK);
    start.link=open_link(&links,7,0);
    lease=(fips_rekey_index_lease){.index=13,.live=true};
    CHECK(fips_settle_leg_open(&legs,&start,false,NULL,0,fips_test_instant(0),&p,&old)==FIPS_OK);
    CHECK(slots[0].link>slots[1].link && slots[1].link<slots[2].link);
    CHECK(fips_settle_legs_poll(&legs,&links,fips_test_instant(0),true,outs,3,&n,closes,9,&cn)==FIPS_OK && n==3);
    for (unsigned i=0; i<3; ++i) { release(&outs[i].lease,i+11); if (i) CHECK(outs[i].link>outs[i-1].link); }
}
static fips_status short_add(fips_instant a, fips_duration d, fips_instant *out)
{
    if (d.secs>65 || d.secs*1000+d.nanos/1000000>65535-a.opaque[0]) return FIPS_ERR_RANGE;
    a.opaque[0]+=d.secs*1000+d.nanos/1000000; *out=a; return FIPS_OK;
}
static fips_status short_sub(fips_instant a, fips_duration d, fips_instant *out)
{
    if (d.secs>65 || d.secs*1000+d.nanos/1000000>a.opaque[0]) return FIPS_ERR_RANGE;
    a.opaque[0]-=d.secs*1000+d.nanos/1000000; *out=a; return FIPS_OK;
}
static void short_clock_test(void)
{
    fips_instant at=fips_test_instant(60000);
    fips_instant_ops ops=*at.ops; ops.checked_add=short_add; ops.checked_sub=short_sub; at.ops=&ops;
    fips_settle_legs legs; fips_settle_leg slots[1], old;
    fips_settle_links links; fips_settle_link records[1];
    fips_settle_legs_init(&legs,slots,1);
    CHECK(fips_settle_links_init(&links,records,1,NULL,0)==FIPS_OK);
    fips_settle_params p=fips_settle_params_default();
    fips_rekey_index_lease lease={.index=9,.live=true};
    fips_settle_start start={.link=open_link(&links,1,0),.lease=&lease};
    CHECK(fips_settle_leg_open(&legs,&start,false,NULL,0,at,&p,&old)==FIPS_OK);
    const fips_settle_leg *leg=fips_settle_leg_get(&legs,start.link);
    CHECK(leg && leg->deadline.opaque[0]>60000 && leg->deadline.opaque[0]<=65535);
    fips_settle_leg_out out[1]; fips_settle_pair closes[1]; size_t n, cn;
    at.opaque[0]=65535;
    CHECK(fips_settle_legs_poll(&legs,&links,at,false,out,1,&n,closes,1,&cn)==FIPS_OK);
    CHECK(n==1 && out[0].reaped); release(&out[0].lease,9);
}
static void timing_test(void)
{
    fips_settle_params p=fips_settle_params_default();
    CHECK(p.interval.secs==15 && p.pending==1000 && p.peers==128 && p.replays==8 && p.counted==8);
    const double factors[]={1.3333,1.5,2.0,2.5005};
    const uint64_t waits[][4]={{1333,1777,2370,3160},{1500,2250,3375,5062},{2000,4000,8000,16000},{2500,6252,15634,39093}};
    for (size_t j=0; j<4; ++j) {
        memcpy(&p.backoff,&factors[j],8);
        for (uint32_t i=1; i<=4; ++i) {
            fips_duration d=fips_settle_wait(&p,i);
            CHECK(d.secs*1000+d.nanos/1000000==waits[j][i-1]);
        }
    }
    const uint64_t special[]={UINT64_C(0x7ff8000000000000),UINT64_C(0xbff0000000000000),UINT64_C(0x7ff0000000000000)};
    for (size_t i=0; i<3; ++i) {
        p.backoff=special[i]; fips_duration d=fips_settle_wait(&p,1);
        uint64_t want=i==2 ? UINT64_MAX : 0;
        CHECK(d.secs==want/1000 && d.nanos==(want%1000)*1000000);
    }
    fips_instant end=fips_test_instant(UINT64_MAX-1000);
    fips_instant d=fips_deadline(end,p.handshake);
    CHECK(d.ops->compare(d,end)>=0); /* no overflow, halves until representable */
}
static void *independent(void *ctx)
{
    (void)ctx; for (unsigned i=0; i<16; ++i) { links_test(); legs_test(); timing_test(); short_clock_test(); } return NULL;
}
void fips_test_settle_gates(void);
int main(void)
{
    fips_test_settle_gates();
    links_test(); legs_test(); timing_test(); short_clock_test();
    pthread_t threads[4];
    for (unsigned i=0; i<4; ++i) CHECK(pthread_create(&threads[i],NULL,independent,NULL)==0);
    for (unsigned i=0; i<4; ++i) CHECK(pthread_join(threads[i],NULL)==0);
    puts("settle: links/legs/timing + four independent concurrent owners passed"); return 0;
}
