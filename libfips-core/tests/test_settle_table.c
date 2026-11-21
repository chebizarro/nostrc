#include "fakes/settle_table.h"
#include "fakes/host.h"
#include <fips/proto/link.h>
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"table:%d: %s\n",__LINE__,#x); abort(); } } while (0)
static fips_node_addr peer(unsigned n) { fips_node_addr p; memset(p.bytes,(int)n,16); return p; }
static fips_settle_pair pair(unsigned n)
{ fips_settle_pair p={.transport={1}}; uint8_t b=(uint8_t)n; CHECK(fips_transport_addr_new(&b,1,&p.addr)==FIPS_OK); return p; }
static void step(fips_settle_table *t,fips_settle_input *in,uint64_t now)
{ CHECK(fips_settle_table_step(t,in,fips_test_instant(now))==FIPS_OK); }
void fips_test_settle_table_boundaries(void)
{
    fips_settle_params p=fips_settle_params_default(); fips_test_settle_table *f=fips_test_settle_table_create(peer(1),&p,2,4,32,32); fips_settle_table *t=&f->table;
    fips_settle_input in={.kind=FIPS_SETTLE_IN_CONNECTING,.data.connecting={.peer=peer(2),.to=pair(1),.has_cap=true,.cap=1}};
    step(t,&in,0); fips_settle_outputs out=fips_settle_table_outputs(t); CHECK(out.count==1 && out.items[0].kind==FIPS_SETTLE_OUT_OPENED); uint64_t link=out.items[0].link;
    uint8_t msg[]={1,2,3}; fips_rekey_index_lease lease={.index=1,.live=true};
    in=(fips_settle_input){.kind=FIPS_SETTLE_IN_DIAL,.data.dial={.peer=peer(3),.to=pair(9),.msg1={msg,3},.lease=&lease,.has_link=true,.link=link,.has_cap=true,.cap=5}};
    step(t,&in,0); CHECK(!lease.live);
    const fips_settle_leg *leg=fips_settle_leg_get(fips_settle_table_legs(t),link);
    CHECK(leg && !memcmp(leg->peer.bytes,peer(2).bytes,16) && leg->resends==1);
    lease=(fips_rekey_index_lease){.index=2,.live=true}; in.data.dial.has_link=false; step(t,&in,0); CHECK(!lease.live);
    lease=(fips_rekey_index_lease){.index=3,.live=true};
    unsigned active=t->active; const fips_settle_legs *before=fips_settle_table_legs(t); out=fips_settle_table_outputs(t); fips_settle_output saved=out.items[0];
    CHECK(fips_settle_table_step(t,&in,fips_test_instant(0))==FIPS_ERR_RANGE && lease.live && t->active==active);
    CHECK(fips_settle_table_legs(t)==before && fips_settle_table_outputs(t).items==out.items && !memcmp(&saved,out.items,sizeof(saved)));
    CHECK(fips_settle_leg_get(before,link)->lease.live && !memcmp(fips_settle_leg_get(before,link)->msg1,msg,3));
    uint32_t index; CHECK(fips_rekey_index_lease_release(&lease,&index) && index==3);
    in=(fips_settle_input){.kind=FIPS_SETTLE_IN_MSG2,.data.msg2={.link=link,.from=pair(1)}}; memset(in.data.msg2.epoch,1,8); step(t,&in,1);
    out=fips_settle_table_outputs(t); CHECK(out.count==2 && out.items[0].kind==FIPS_SETTLE_OUT_INSTALL && out.items[0].lease.live); active=t->active;
    CHECK(fips_settle_table_poll(t,fips_test_instant(2))==FIPS_ERR_RANGE && t->active==active && out.items[0].lease.live);
    in=(fips_settle_input){.kind=FIPS_SETTLE_IN_HEARD,.data.heard={.peer=peer(2),.from=pair(1),.kind=FIPS_LINK_Heartbeat}};
    CHECK(fips_settle_table_step(t,&in,fips_test_instant(2))==FIPS_ERR_RANGE);
    CHECK(!fips_settle_table_held(t,&in.data.heard.peer)->has_last && out.items[0].lease.live);
    CHECK(fips_rekey_index_lease_release(&out.items[0].lease,&index) && index==1);
    step(t,&in,2);
    const fips_settle_held *h=fips_settle_table_held(t,&in.data.heard.peer);
    CHECK(h && h->has_last && h->last.opaque[0]==2 && !fips_settle_table_outputs(t).count);
    CHECK(fips_settle_table_poll(t,fips_test_instant(2))==FIPS_OK);
    lease=(fips_rekey_index_lease){.index=4,.live=true}; in=(fips_settle_input){.kind=FIPS_SETTLE_IN_HELLO,.data.hello={.link=999,.msg2={msg,3},.lease=&lease}}; step(t,&in,3);
    out=fips_settle_table_outputs(t); CHECK(!lease.live && out.count==1 && out.items[0].kind==FIPS_SETTLE_OUT_FREE && out.items[0].lease.index==4); fips_test_settle_drain(t);
    CHECK(fips_settle_table_close(t,fips_test_instant(4))==FIPS_OK); fips_test_settle_drain(t); CHECK(!fips_settle_table_held(t,&(fips_node_addr){{2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2}}));
    CHECK(fips_settle_table_close(t,fips_test_instant(5))==FIPS_OK && !fips_settle_table_outputs(t).count);
    fips_test_settle_table_destroy(f,fips_test_instant(5));

    /* A reap can fill scratch outputs before its close arena fails. Neither
     * the live leg/lease nor the previous live output may be touched. */
    f=fips_test_settle_table_create(peer(1),&p,1,0,8,0); t=&f->table; lease=(fips_rekey_index_lease){.index=7,.live=true};
    in=(fips_settle_input){.kind=FIPS_SETTLE_IN_DIAL,.data.dial={.peer=peer(2),.to=pair(1),.msg1={msg,3},.lease=&lease}}; step(t,&in,0);
    out=fips_settle_table_outputs(t); saved=out.items[0]; active=t->active; link=out.items[0].link;
    CHECK(fips_settle_table_poll(t,fips_test_instant(30000))==FIPS_ERR_RANGE && t->active==active);
    CHECK(!memcmp(&saved,out.items,sizeof(saved)) && fips_settle_leg_get(fips_settle_table_legs(t),link)->lease.live);
    bool found=false; CHECK(fips_settle_table_drop_leg(t,link,&found,&lease)==FIPS_OK && found); CHECK(fips_rekey_index_lease_release(&lease,&index) && index==7);
    fips_test_settle_table_destroy(f,fips_test_instant(30000));
    puts("settle table: arena rollback, input lease moves, connect facts, output ownership and close checks passed");
}
