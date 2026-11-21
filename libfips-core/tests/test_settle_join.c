#include <fips/proto/settle/join.h>
#include "fakes/rekey.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"join:%d: %s\n",__LINE__,#x); abort(); } } while (0)
static fips_node_addr peer(void) { fips_node_addr a; memset(a.bytes,2,16); return a; }
static fips_path_ref path(unsigned port)
{ fips_path_ref p={.transport={1}}; uint8_t a=(uint8_t)port; CHECK(fips_transport_addr_new(&a,1,&p.addr)==FIPS_OK); return p; }
static fips_settle_proven read_path(void *ctx,const fips_path_ref *p)
{ (void)ctx; return p->addr.bytes[0]==2 ? FIPS_SETTLE_ACKED : FIPS_SETTLE_LIVE; }
static void begin(fips_settle_joined *j,fips_settle_join_event e)
{ CHECK(fips_settle_join(j,peer(),&e)==FIPS_OK); }
static fips_settle_join_step next(fips_settle_joined *j,fips_settle_join_step_kind kind)
{ fips_settle_join_step s; CHECK(fips_settle_join_next(j,&s) && s.kind==kind); return s; }
static void finish(fips_settle_joined *j)
{ fips_settle_join_step s; CHECK(!fips_settle_join_next(j,&s)); fips_settle_join_clear(j); }
static void each_rekey_output_routes_to_the_core_or_the_shell(fips_settle_joined *j)
{
    fips_rekey_outputs o={0};
    for (unsigned kind=FIPS_REKEY_OUT_Send;kind<=FIPS_REKEY_OUT_RedialRequested;++kind) {
        o.items[0]=(fips_rekey_output){.kind=(fips_rekey_output_kind)kind,.tag=3,.has_tag=true,.index=4}; o.count=1;
        memset(o.items[0].epoch,6,8); fips_rekey_output before=o.items[0];
        begin(j,(fips_settle_join_event){.kind=FIPS_SETTLE_JOIN_REKEY,.rekey_out=&o});
        fips_settle_join_step s; CHECK(fips_settle_join_next(j,&s));
        if (kind==FIPS_REKEY_OUT_PeerRestarted) CHECK(s.kind==FIPS_SETTLE_STEP_CORE && s.input.kind==FIPS_SETTLE_IN_RESTARTED && !memcmp(s.input.data.epoch.epoch,before.epoch,8) && !memcmp(s.input.data.epoch.peer.bytes,peer().bytes,16));
        else if (kind==FIPS_REKEY_OUT_Teardown) CHECK(s.kind==FIPS_SETTLE_STEP_CORE && s.input.kind==FIPS_SETTLE_IN_ENDED && !memcmp(s.input.data.peer.bytes,peer().bytes,16));
        else { CHECK(s.rekey==&o.items[0] && !memcmp(s.rekey,&before,sizeof(before))); CHECK(s.kind==(kind==FIPS_REKEY_OUT_ReplaceRequested ? FIPS_SETTLE_STEP_REPLACE : kind==FIPS_REKEY_OUT_RedialRequested ? FIPS_SETTLE_STEP_REDIAL : FIPS_SETTLE_STEP_REKEY)); }
        finish(j);
    }
    fips_rekey_outputs_clear(&o);
}
static size_t bundle_outputs(fips_path_output *o)
{
    fips_path_output values[]={
        {.kind=FIPS_PATH_OUTPUT_Send,.to=path(1)},
        {.kind=FIPS_PATH_OUTPUT_Moved,.to=path(3),.as.moved={.from=path(1),.driver_slot=(void *)1}},
        {.kind=FIPS_PATH_OUTPUT_Released,.to=path(4),.as.released={.driver_slot=(void *)2,.keep=true}},
        {.kind=FIPS_PATH_OUTPUT_State,.to=path(2),.as.state={.next=FIPS_PATH_Live}},
        {.kind=FIPS_PATH_OUTPUT_State,.to=path(5),.as.state={.next=FIPS_PATH_Live}},
        {.kind=FIPS_PATH_OUTPUT_State,.to=path(6),.as.state={.next=FIPS_PATH_NoState}},
        {.kind=FIPS_PATH_OUTPUT_Capable},{.kind=FIPS_PATH_OUTPUT_NotCapable},
        {.kind=FIPS_PATH_OUTPUT_Refused,.to=path(1)},
        {.kind=FIPS_PATH_OUTPUT_Switched,.to=path(5),.as.switched={.has_from=true,.from=path(1)}},
        {.kind=FIPS_PATH_OUTPUT_Switched,.to=path(2)},
        {.kind=FIPS_PATH_OUTPUT_Orphaned},{.kind=FIPS_PATH_OUTPUT_Connect,.to=path(1)},
        {.kind=FIPS_PATH_OUTPUT_Proven,.to=path(1),.as.mtu=1280}};
    memcpy(o,values,sizeof(values)); return sizeof(values)/sizeof(*values);
}
static void each_bundle_output_mirrors_to_its_input(fips_settle_joined *j,bool held)
{
    fips_path_output items[14]; size_t n=bundle_outputs(items);
    const int want[]={-1,FIPS_SETTLE_IN_MOVED,FIPS_SETTLE_IN_RELEASED,FIPS_SETTLE_IN_STATE,FIPS_SETTLE_IN_STATE,-1,FIPS_SETTLE_IN_CAPABLE,-1,-1,FIPS_SETTLE_IN_SWITCHED,FIPS_SETTLE_IN_SWITCHED,FIPS_SETTLE_IN_ORPHANED,-1,-1};
    for (size_t i=0;i<n;++i) {
        fips_path_outputs p={.items=&items[i],.capacity=1,.len=1}; fips_path_output saved=items[i];
        begin(j,(fips_settle_join_event){.kind=FIPS_SETTLE_JOIN_PATHS,.path_out=&p,.read=read_path,.has_link=held,.link=7});
        fips_settle_join_step s=next(j,FIPS_SETTLE_STEP_PATH); CHECK(s.path==&items[i] && !memcmp(s.path,&saved,sizeof(saved)));
        if (held && want[i]>=0) {
            s=next(j,FIPS_SETTLE_STEP_CORE); CHECK((int)s.input.kind==want[i]);
            switch(s.input.kind) {
            case FIPS_SETTLE_IN_MOVED: CHECK(s.input.data.moved.link==7 && !s.input.data.moved.roam && s.input.data.moved.from.addr.bytes[0]==1 && s.input.data.moved.to.addr.bytes[0]==3); break;
            case FIPS_SETTLE_IN_RELEASED: CHECK(s.input.data.released.link==7 && s.input.data.released.keep && s.input.data.released.pair.addr.bytes[0]==4); break;
            case FIPS_SETTLE_IN_STATE: CHECK(s.input.data.state.link==7 && s.input.data.state.proven==(i==3 ? FIPS_SETTLE_ACKED : FIPS_SETTLE_LIVE)); break;
            case FIPS_SETTLE_IN_SWITCHED: CHECK(s.input.data.switched.link==7 && s.input.data.switched.proven==(i==10 ? FIPS_SETTLE_ACKED : FIPS_SETTLE_LIVE)); break;
            case FIPS_SETTLE_IN_CAPABLE: CHECK(s.input.data.link==7); break;
            case FIPS_SETTLE_IN_ORPHANED: CHECK(!memcmp(s.input.data.peer.bytes,peer().bytes,16)); break;
            default: CHECK(false);
            }
        }
        finish(j);
    }
}
static void a_released_path_goes_to_the_link_moved_off(fips_settle_joined *j)
{
    fips_path_output items[2]={{.kind=FIPS_PATH_OUTPUT_Released,.to=path(4)}, {.kind=FIPS_PATH_OUTPUT_State,.to=path(5),.as.state={.next=FIPS_PATH_Live}}};
    fips_path_outputs p={items,2,2};
    for (unsigned held=0;held<2;++held) {
        begin(j,(fips_settle_join_event){.kind=FIPS_SETTLE_JOIN_PATHS,.path_out=&p,.read=read_path,.has_link=held!=0,.link=7,.has_released=true,.released=3});
        (void)next(j,FIPS_SETTLE_STEP_PATH); fips_settle_join_step s=next(j,FIPS_SETTLE_STEP_CORE); CHECK(s.input.kind==FIPS_SETTLE_IN_RELEASED && s.input.data.released.link==3 && !s.input.data.released.keep);
        (void)next(j,FIPS_SETTLE_STEP_PATH); if (held) { s=next(j,FIPS_SETTLE_STEP_CORE); CHECK(s.input.kind==FIPS_SETTLE_IN_STATE && s.input.data.state.link==7); } finish(j);
    }
}
static void a_roam_is_moved_and_a_bind_is_a_live_path(fips_settle_joined *j)
{
    for (unsigned from=0;from<2;++from) for (unsigned standby=0;standby<2;++standby) {
        begin(j,(fips_settle_join_event){.kind=FIPS_SETTLE_JOIN_ROAM,.link=7,.has_from=from!=0,.from=fips_settle_path_pair(&(fips_path_ref){.transport={1},.addr=path(1).addr}),.to=fips_settle_path_pair(&(fips_path_ref){.transport={1},.addr=path(2).addr}),.standby=standby!=0});
        fips_settle_join_step s=next(j,FIPS_SETTLE_STEP_CORE);
        if (from) CHECK(s.input.kind==FIPS_SETTLE_IN_MOVED && s.input.data.moved.link==7 && s.input.data.moved.roam==!standby && s.input.data.moved.from.addr.bytes[0]==1 && s.input.data.moved.to.addr.bytes[0]==2);
        else CHECK(s.input.kind==FIPS_SETTLE_IN_STATE && s.input.data.state.link==7 && s.input.data.state.proven==FIPS_SETTLE_LIVE && s.input.data.state.pair.addr.bytes[0]==2);
        finish(j);
    }
}
static void a_steps_outputs_come_back_in_order(fips_settle_joined *j)
{
    fips_rekey_outputs r={.items={{.kind=FIPS_REKEY_OUT_Reject},{.kind=FIPS_REKEY_OUT_Teardown},{.kind=FIPS_REKEY_OUT_Retired},{.kind=FIPS_REKEY_OUT_Abandoned}},.count=4};
    begin(j,(fips_settle_join_event){.kind=FIPS_SETTLE_JOIN_REKEY,.rekey_out=&r});
    CHECK(next(j,FIPS_SETTLE_STEP_REKEY).rekey==&r.items[0]); CHECK(next(j,FIPS_SETTLE_STEP_CORE).input.kind==FIPS_SETTLE_IN_ENDED); CHECK(next(j,FIPS_SETTLE_STEP_REKEY).rekey==&r.items[2]); CHECK(next(j,FIPS_SETTLE_STEP_REKEY).rekey==&r.items[3]); finish(j);
    fips_path_output items[]={{.kind=FIPS_PATH_OUTPUT_Moved,.to=path(3),.as.moved={.from=path(1)}},{.kind=FIPS_PATH_OUTPUT_Capable},{.kind=FIPS_PATH_OUTPUT_Orphaned}};
    fips_path_outputs p={items,3,3}; begin(j,(fips_settle_join_event){.kind=FIPS_SETTLE_JOIN_PATHS,.path_out=&p,.read=read_path,.has_link=true,.link=7});
    const fips_settle_input_kind want[]={FIPS_SETTLE_IN_MOVED,FIPS_SETTLE_IN_CAPABLE,FIPS_SETTLE_IN_ORPHANED};
    for (size_t i=0;i<3;++i) { CHECK(next(j,FIPS_SETTLE_STEP_PATH).path==&items[i]); CHECK(next(j,FIPS_SETTLE_STEP_CORE).input.kind==want[i]); } finish(j);
}
typedef struct read_snapshot { unsigned calls; fips_settle_proven value; } read_snapshot;
static fips_settle_proven snapshot(void *ctx,const fips_path_ref *p)
{ (void)p; read_snapshot *s=ctx; ++s->calls; return s->value; }
static void snapshot_and_bounds(fips_settle_joined *j)
{
    fips_path_output items[2]={{.kind=FIPS_PATH_OUTPUT_State,.to=path(1),.as.state={.next=FIPS_PATH_Live}},{.kind=FIPS_PATH_OUTPUT_State,.to=path(2),.as.state={.next=FIPS_PATH_Live}}};
    fips_path_outputs p={items,2,2}; read_snapshot read={0,FIPS_SETTLE_LIVE};
    fips_settle_join_event e={.kind=FIPS_SETTLE_JOIN_PATHS,.path_out=&p,.read=snapshot,.read_ctx=&read,.has_link=true,.link=7};
    begin(j,e); CHECK(read.calls==2); read.value=FIPS_SETTLE_PROBING;
    for (size_t i=0;i<2;++i) { (void)next(j,FIPS_SETTLE_STEP_PATH); CHECK(next(j,FIPS_SETTLE_STEP_CORE).input.data.state.proven==FIPS_SETTLE_LIVE); }
    CHECK(read.calls==2); finish(j);
    fips_settle_joined *saved=malloc(sizeof(*saved)); CHECK(saved); memcpy(saved,j,sizeof(*saved));
    p.len=p.capacity=FIPS_PATH_OUTPUTS_MAX+1;
    CHECK(fips_settle_join(j,peer(),&e)==FIPS_ERR_RANGE && read.calls==2 && !memcmp(saved,j,sizeof(*j))); free(saved);
}
static size_t mtu(void *c,fips_transport_id t,const fips_transport_addr *a) { (void)c;(void)t;(void)a;return 1280; }
static fips_path_medium medium(void *c,fips_transport_id t,const fips_transport_addr *a) { (void)c;(void)t;(void)a;return FIPS_PATH_Unknown; }
static bool no(void *c,fips_transport_id t) { (void)c;(void)t;return false; }
static bool yes(void *c,fips_transport_id t) { (void)c;(void)t;return true; }
static fips_path_class clear(void *c,fips_transport_id t) { (void)c;(void)t;return FIPS_PATH_Clear; }
static fips_path_kind unfixed(void *c,fips_transport_id t) { (void)c;(void)t;return FIPS_PATH_Unfixed; }
static fips_path_role normal(void *c,fips_transport_id t) { (void)c;(void)t;return FIPS_PATH_Normal; }
static void setup(rk_test_node *me,rk_test_node *other,uint32_t index,fips_settle_setup *s,fips_rekey_setup_facts *facts)
{
    fips_rekey_hs hs={0}; fips_rekey_wire msg={0}; fips_rekey_read read={0};
    CHECK(me->profile.initiate(&me->local,&other->local.key,(fips_rekey_index){index,true},&hs,&msg));
    rk_test_packet m={.handshake=true,.kind=FIPS_REKEY_MSG_Msg1,.len=msg.len}; memcpy(m.bytes,msg.bytes,msg.len);
    fips_rekey_msg1_read token; CHECK(rk_test_token(&m,&token)); rk_test_packet answer=rk_test_answer(&other->local,&m,9);
    CHECK(me->profile.reply(&hs,&me->local,answer.bytes,answer.len,&read));
    *s=(fips_settle_setup){.done=read.done,.lease=fips_rekey_index_lease_for_test(index),.theirs={9,true},.transport=1};
    memset(&read.done,0,sizeof(read.done)); fips_rekey_hs_clear(&hs);
    *facts=(fips_rekey_setup_facts){.has_peer_epoch=true,.link_since=fips_test_instant(0),.started=fips_test_instant(0)};
    memcpy(facts->peer_epoch,s->done.epoch,8); fips_rekey_digest digest,ephemeral; memcpy(digest.bytes,token.digest,32); memcpy(ephemeral.bytes,token.ephemeral,32); fips_rekey_record_setup(&facts->record,digest,ephemeral,NULL);
}
static void drain(fips_settle_joined *j)
{
    fips_settle_join_step s; while (fips_settle_join_next(j,&s)) if (s.rekey && s.rekey->lease.live) { uint32_t index; CHECK(fips_rekey_index_lease_release(&s.rekey->lease,&index)); }
}
static void lifecycle(fips_settle_joined *j)
{
    rk_test_node *a=calloc(1,sizeof(*a)),*b=calloc(1,sizeof(*b)),*c=calloc(1,sizeof(*c)); CHECK(a && b && c);
    rk_test_node_init(a,1,&rk_test_ik); rk_test_node_init(b,2,&rk_test_ik); rk_test_node_init(c,3,&rk_test_ik);
    fips_path_facts facts={NULL,mtu,medium,no,clear,unfixed,normal,yes,yes}; fips_path_env env={&facts,1.0};
    fips_path_config config=fips_path_config_draft(); fips_path_params params; CHECK(fips_path_params_new(&params,config,NULL)==FIPS_OK);
    fips_settle_caps caps={.profile=&a->profile,.params=fips_rekey_params_fmp(),.me=rk_test_addr(a->local.key),.now=fips_test_instant(0),.env=&a->env};
    fips_seeded_entropy entropy={42};
    fips_path_bundle *bundle=calloc(1,sizeof(*bundle)); CHECK(bundle); bool has_r=false,has_p=false;
    for (unsigned link=0;link<2;++link) {
        fips_settle_setup s; fips_rekey_setup_facts initial; setup(a,b,1,&s,&initial);
        fips_settle_join_event e={.kind=FIPS_SETTLE_JOIN_START,.rekey=&a->machine,.has_rekey=&has_r,.paths=bundle,.has_paths=&has_p,.setup=&s,.facts=&initial,.key=b->local.key,.caps=&caps,.to={.transport={1},.addr=path(1).addr},.path_params=params,.entropy=fips_seeded_entropy_adapter(&entropy),.path_env=&env,.has_link=link!=0,.link=7};
        /* A foreign key refuses atomically, including lease and setup secrets. */
        e.key=c->local.key; CHECK(fips_settle_join(j,peer(),&e)==FIPS_ERR_RANGE && s.lease.live && !has_r && !has_p); e.key=b->local.key;
        begin(j,e); CHECK(j->built && has_r && has_p && !s.lease.live); CHECK(fips_settle_path_proven(bundle,&(fips_path_ref){.transport={1},.addr=path(1).addr})==FIPS_SETTLE_LIVE);
        fips_settle_join_step step_; bool saw_core=false; while(fips_settle_join_next(j,&step_)) { if (step_.kind==FIPS_SETTLE_STEP_CORE) { CHECK(step_.input.kind==FIPS_SETTLE_IN_STATE && step_.input.data.state.link==7 && step_.input.data.state.proven==FIPS_SETTLE_LIVE); saw_core=true; } else CHECK(!saw_core); } CHECK(saw_core==(link!=0)); fips_settle_join_clear(j);
        /* Another authenticated key cannot alter either live machine. */
        fips_rekey *before=malloc(sizeof(*before)); CHECK(before); memcpy(before,&a->machine,sizeof(*before)); fips_path_bundle saved=*bundle;
        fips_settle_setup other; fips_rekey_setup_facts unused; setup(a,c,2,&other,&unused); caps.now=fips_test_instant(5000);
        e.kind=FIPS_SETTLE_JOIN_SHIFT; e.setup=&other; e.to.addr=path(2).addr; begin(j,e); CHECK(!j->built && !j->has_carry); step_=next(j,FIPS_SETTLE_STEP_REKEY); CHECK(step_.rekey->kind==FIPS_REKEY_OUT_Release && step_.rekey->lease.index==2); uint32_t index; CHECK(fips_rekey_index_lease_release(&step_.rekey->lease,&index)); finish(j);
        CHECK(!memcmp(before,&a->machine,sizeof(*before)) && !memcmp(&saved,bundle,sizeof(saved))); free(before);
        if (link) {
            /* Successful move carries the link age and replay record. */
            fips_settle_setup moved; setup(a,b,3,&moved,&unused); e.setup=&moved; begin(j,e); CHECK(j->built && has_r && has_p); drain(j); fips_settle_join_clear(j); CHECK(a->machine.mem.link_since.opaque[0]==0 && fips_rekey_record_len(&a->machine.record)==1); CHECK(fips_settle_path_proven(bundle,&(fips_path_ref){.transport={1},.addr=path(2).addr})==FIPS_SETTLE_LIVE);
        }
        e.kind=FIPS_SETTLE_JOIN_STOP; e.why=FIPS_REKEY_CLOSE_Admin; begin(j,e); CHECK(!has_r && !has_p && j->has_carry && j->carry.has_peer_epoch && !memcmp(j->carry.peer_epoch,b->local.epoch,8) && j->carry.link_since.opaque[0]==0 && fips_rekey_record_len(&j->carry.record)==1);
        bool rekey_seen=false,path_seen=false; while(fips_settle_join_next(j,&step_)) { if(step_.kind==FIPS_SETTLE_STEP_PATH) { CHECK(!rekey_seen); path_seen=true; } else { CHECK(step_.kind==FIPS_SETTLE_STEP_REKEY); rekey_seen=true; if(step_.rekey->lease.live) CHECK(fips_rekey_index_lease_release(&step_.rekey->lease,&index)); } } CHECK(path_seen && rekey_seen); fips_settle_join_clear(j); caps.now=fips_test_instant(0);
    }
    free(bundle); free(a); free(b); free(c);
}
void fips_test_settle_join(void)
{
    size_t leaked=fips_rekey_leaked(); fips_settle_joined *j=calloc(1,sizeof(*j)); CHECK(j);
    each_rekey_output_routes_to_the_core_or_the_shell(j); each_bundle_output_mirrors_to_its_input(j,true); each_bundle_output_mirrors_to_its_input(j,false);
    a_released_path_goes_to_the_link_moved_off(j); a_roam_is_moved_and_a_bind_is_a_live_path(j); a_steps_outputs_come_back_in_order(j); snapshot_and_bounds(j); lifecycle(j);
    CHECK(fips_rekey_leaked()==leaked); free(j); puts("settle join: routing, ordering, slots, refused/accepted moves and carry passed");
}
