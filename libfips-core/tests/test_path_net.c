#include "path_machine_test.h"
/* Public-interface shell from upstream tests/shell.rs, not the two-party
 * simulator. A probe gets a scripted answer; time advances to an answer or
 * next_wake, never by a sleep or a guessed settling delay. */
typedef struct pending { bool used; uint64_t at; fips_path_input in; } pending;
typedef struct change { uint64_t at; unsigned t; fips_path_state state; } change;
typedef struct net {
    fixture x;
    pending queue[128];
    uint64_t lead_window,lead_horizon;
    uint64_t now, cut_from[16],cut_until[16],loaded_at[16];
    unsigned delay[16],loaded_delay[16];
    size_t ceiling[16];
    bool drop[16],remote[16],connect_ok;
    uint64_t probes[16],first_probe[16],last_probe[16],connections[16];
    size_t last_len[16],not_capable,capable,releases,proven,refused;
    uint64_t not_at,switched_at[64]; unsigned switched_to[64]; size_t switches;
    change states[256]; size_t nstates;
} net;
static void net_init(net *n,fips_path_config cfg)
{
    memset(n,0,sizeof(*n)); n->lead_window=n->lead_horizon=UINT64_MAX; init(&n->x,cfg.enabled); n->connect_ok=true;
    for(size_t i=0;i<16;++i) { n->delay[i]=10; n->ceiling[i]=SIZE_MAX; n->cut_from[i]=n->loaded_at[i]=UINT64_MAX; }
    fips_path_params p; CHECK(fips_path_params_new(&p,cfg,NULL)==FIPS_OK);
    fips_path_bundle_new(&n->x.b,AT(0),T(1),address(1),p,counter_entropy(&n->x.seq),&n->x.env);
}
static void enqueue(net *n,uint64_t at,fips_path_input in)
{
    for(size_t i=0;i<128;++i) if(!n->queue[i].used) { n->queue[i]=(pending){true,at,in}; return; }
    CHECK(false);
}
static void observe(net *n,uint64_t now)
{
    fixture *x=&n->x; fips_path_bundle_view view; fips_path_bundle_view_get(&x->b,AT(now),&view);
    CHECK(view.len<=8&&x->out.len<=FIPS_PATH_OUTPUTS_MAX);
    const fips_path_view *p=NULL,*q=NULL;
    for(size_t i=0;i<view.len;++i) { if(view.paths[i].to.transport.value==1) p=&view.paths[i]; if(view.paths[i].to.transport.value==2) q=&view.paths[i]; }
    if(p&&q&&p->samples>=2&&q->samples>=2&&p->has_score&&q->has_score) {
        if(n->lead_window==UINT64_MAX&&p->score>q->score*x->b.params.cfg.switch_margin) n->lead_window=now;
        fips_transport_addr a=address(1); size_t pi=fips_path_set_find(&x->b.set,T(1),&a); double steady;
        if(n->lead_horizon==UINT64_MAX&&fips_path_select_steady(&fips_path_set_get(&x->b.set,pi)->live,&steady)&&steady>q->score*x->b.params.cfg.switch_margin) n->lead_horizon=now;
    }

    fips_instant wake=fips_path_bundle_next_wake(&x->b); CHECK(!wake.ops||wake.opaque[0]>now);
    for(size_t i=0;i<x->out.len;++i) {
        const fips_path_output *o=&x->actions[i]; unsigned t=(unsigned)o->to.transport.value; CHECK(t<16);
        switch(o->kind) {
        case FIPS_PATH_OUTPUT_Send: {
            uint8_t bytes[1500]; fips_writer w; fips_writer_init(&w,bytes,sizeof(bytes));
            CHECK(fips_path_output_encode(o,&w,NULL)==FIPS_OK&&bytes[0]==o->as.send.type);
            if(!view.enabled) CHECK(false);
            if(o->as.send.type!=FIPS_LINK_PathProbe) break;
            const fips_path_probe *p=&o->as.send.as.probe;
            CHECK(p->len==w.pos&&p->len>=27&&p->len+36<=x->facts.mtu[t]&&!(p->flags&FIPS_PATH_EXT)&&p->caps==0);
            if(!view.capable) CHECK(t==1&&p->len==27&&p->path_id==x->b.original&&p->flags==FIPS_PATH_ACTIVE);
            if(!n->probes[t]) n->first_probe[t]=now;
            ++n->probes[t]; n->last_probe[t]=now; n->last_len[t]=p->len;
            if(n->drop[t]||(now>=n->cut_from[t]&&now<n->cut_until[t])||p->len>n->ceiling[t]) break;
            fips_path_input in={.kind=FIPS_PATH_INPUT_Ack,.to=o->to};
            in.as.ack=(fips_path_ack){.probe_id=p->probe_id,.path_id=100+t,.echo_path_id=p->path_id,.len=p->len,.flags=n->remote[t]?FIPS_PATH_ACTIVE:0};
            enqueue(n,now+(now>=n->loaded_at[t]?n->loaded_delay[t]:n->delay[t]),in); break;
        }
        case FIPS_PATH_OUTPUT_Connect:
            CHECK(view.enabled&&view.capable&&x->facts.stream[t]); ++n->connections[t];
            { fips_path_input in={.kind=FIPS_PATH_INPUT_Connected,.to=o->to}; in.as.ok=n->connect_ok; enqueue(n,now+1,in); }
            break;
        case FIPS_PATH_OUTPUT_State:
            CHECK(n->nstates<256); n->states[n->nstates++]=(change){now,t,o->as.state.next}; break;
        case FIPS_PATH_OUTPUT_Capable: CHECK(++n->capable==1); break;
        case FIPS_PATH_OUTPUT_NotCapable: CHECK(!view.capable&&++n->not_capable==1); n->not_at=now; break;
        case FIPS_PATH_OUTPUT_Refused: ++n->refused; break;
        case FIPS_PATH_OUTPUT_Released: ++n->releases; break;
        case FIPS_PATH_OUTPUT_Proven: ++n->proven; CHECK(o->as.mtu>=63&&o->as.mtu<=x->facts.mtu[t]); break;
        case FIPS_PATH_OUTPUT_Switched:
            CHECK(n->switches<64); n->switched_at[n->switches]=now; n->switched_to[n->switches++]=t;
            { bool active=false; for(size_t j=0;j<view.len;++j) if(view.paths[j].active) { CHECK(view.paths[j].acked); active=true; } CHECK(active); }
            break;
        default: break;
        }
    }
}
static void net_step(net *n,uint64_t now,fips_path_input in)
{
    CHECK(now>=n->now); n->now=now;
    double before=fips_path_bundle_cost(&n->x.b,AT(now),n->x.env.cost); size_t switches=n->switches;
    if(in.kind==FIPS_PATH_INPUT_Ack) fips_path_bundle_account(&n->x.b,AT(now),in.to.transport,&in.to.addr,in.as.ack.len+36,FIPS_LINK_PathAck);
    if(in.kind==FIPS_PATH_INPUT_Probe) fips_path_bundle_account(&n->x.b,AT(now),in.to.transport,&in.to.addr,in.as.probe.len+36,FIPS_LINK_PathProbe);
    step(&n->x,now,in); observe(n,now);
    if(n->switches!=switches) CHECK(fips_path_bundle_cost(&n->x.b,AT(now),n->x.env.cost)==before);
}
static void run(net *n,uint64_t until)
{
    size_t events=0;
    for(;;) {
        CHECK(++events<100000);
        size_t slot=128; uint64_t at=UINT64_MAX;
        for(size_t i=0;i<128;++i) if(n->queue[i].used&&n->queue[i].at<at) { at=n->queue[i].at; slot=i; }
        fips_instant wake=fips_path_bundle_next_wake(&n->x.b);
        if(wake.ops&&wake.opaque[0]<at) { at=wake.opaque[0]; slot=128; }
        if(at>until) break;
        fips_path_input in=input(FIPS_PATH_INPUT_Poll,0,0);
        if(slot<128) { in=n->queue[slot].in; n->queue[slot].used=false; }
        net_step(n,at,in);
    }
    /* Same-facts early polls are inert, including the hold after its reports. */
    fips_path_bundle before=n->x.b;
    net_step(n,until,input(FIPS_PATH_INPUT_Poll,0,0));
    CHECK(!n->x.out.len&&!memcmp(&before,&n->x.b,sizeof(before)));
}
static void heard(net *n)
{
    fips_path_input in=input(FIPS_PATH_INPUT_Close,1,1); in.as.close=(fips_path_close){0,FIPS_PATH_CarrierLost}; net_step(n,0,in);
}
static void offer(net *n,uint64_t now,unsigned t)
{ fips_path_input in=input(FIPS_PATH_INPUT_Candidate,t,t); in.as.source=FIPS_PATH_Config; net_step(n,now,in); }
static fips_path_view path(const net *n,unsigned t)
{
    fips_path_bundle_view v; fips_path_bundle_view_get(&n->x.b,AT(n->now),&v);
    for(size_t i=0;i<v.len;++i) if(v.paths[i].to.transport.value==t) return v.paths[i];
    return (fips_path_view){.state=FIPS_PATH_NoState};
}
static uint64_t changed(const net *n,unsigned t,fips_path_state state)
{ for(size_t i=0;i<n->nstates;++i) if(n->states[i].t==t&&n->states[i].state==state) return n->states[i].at; return UINT64_MAX; }
static void capability_gates(void)
{
    net n; fips_path_config cfg=fips_path_config_draft(); net_init(&n,cfg); n.drop[1]=true;
    run(&n,7200000); CHECK(!n.probes[1]&&!n.not_capable&&!n.capable);
    net_init(&n,cfg); n.drop[1]=true; offer(&n,0,2); run(&n,3600000-1);
    CHECK(n.probes[1]==12&&n.not_capable==1&&n.not_at==20400&&path(&n,1).state==FIPS_PATH_Live&&path(&n,1).misses==0);
    run(&n,7200000-1); CHECK(n.probes[1]==18&&!n.probes[2]&&n.not_capable==1);
    net_init(&n,cfg); n.x.facts.dial[2]=false; offer(&n,0,2); run(&n,10000); CHECK(!n.probes[1]);
    n.x.facts.dial[2]=true; net_step(&n,10001,input(FIPS_PATH_INPUT_Poll,0,0)); CHECK(n.probes[1]==1);
    run(&n,10030); CHECK(n.capable==1&&path(&n,2).state==FIPS_PATH_Live);
    net_init(&n,cfg); n.drop[1]=true; offer(&n,0,2); run(&n,400);
    fips_path_input in=input(FIPS_PATH_INPUT_Unlisted,2,2); in.as.source=FIPS_PATH_Config; net_step(&n,401,in);
    run(&n,60000); CHECK(n.probes[1]==1&&!fips_path_bundle_next_wake(&n.x.b).ops);
    offer(&n,60001,2); CHECK(n.probes[1]==2); /* resume record, not a new round */
    net_init(&n,cfg); n.drop[1]=true; n.x.facts.dial[2]=false; offer(&n,0,2);
    in=input(FIPS_PATH_INPUT_Present,2,0); in.as.present=false; net_step(&n,1,in);
    n.x.facts.dial[2]=true; net_step(&n,2,input(FIPS_PATH_INPUT_Poll,0,0)); CHECK(!n.probes[1]);
    in.as.present=true; net_step(&n,3,in); CHECK(n.probes[1]==1);
}
static void cadences_and_failures(void)
{
    net n; fips_path_config cfg=fips_path_config_draft(); cfg.switch_margin=1000;
    net_init(&n,cfg); heard(&n); offer(&n,0,2); run(&n,9999);
    /* The first post-answer standby probe keeps the Probing schedule (200ms). */
    CHECK(n.probes[1]==50&&n.probes[2]==11&&n.last_probe[2]==9200&&path(&n,2).state==FIPS_PATH_Live);
    net_init(&n,cfg); n.remote[2]=true; heard(&n); offer(&n,0,2); run(&n,9999);
    CHECK(n.probes[1]==50&&n.probes[2]==50); /* first answer shortens standby cadence */
    net_init(&n,cfg); n.cut_from[1]=1000; n.cut_until[1]=UINT64_MAX; heard(&n); offer(&n,0,2); run(&n,20000);
    CHECK(changed(&n,1,FIPS_PATH_Suspect)==1800&&n.switches==1&&n.switched_to[0]==2&&n.switched_at[0]==1800);
    CHECK(path(&n,1).state==FIPS_PATH_Dead&&path(&n,2).active);
    n.cut_until[1]=20001; run(&n,21500); CHECK(path(&n,1).state==FIPS_PATH_Live);
    net_init(&n,cfg); n.drop[2]=true; heard(&n); offer(&n,0,2); run(&n,120000);
    CHECK(n.probes[2]==8&&changed(&n,2,FIPS_PATH_Suspect)==UINT64_MAX&&path(&n,2).state==FIPS_PATH_Dead);
    uint64_t dead=changed(&n,2,FIPS_PATH_Dead); run(&n,dead+600000); CHECK(path(&n,2).state==FIPS_PATH_NoState&&n.releases==1);
    offer(&n,dead+600001,2); CHECK(n.probes[2]==8); /* repeated source is held */
    fips_path_input fresh=input(FIPS_PATH_INPUT_Candidate,2,2); fresh.as.source=FIPS_PATH_Request;
    net_step(&n,dead+600002,fresh); CHECK(n.probes[2]==9);
    net_init(&n,cfg); heard(&n); offer(&n,0,2); run(&n,1000);
    fips_path_input close=input(FIPS_PATH_INPUT_Close,1,1); close.as.close=(fips_path_close){path(&n,2).id,FIPS_PATH_CarrierLost};
    net_step(&n,1001,close); CHECK(path(&n,2).state==FIPS_PATH_Dead);
    fips_path_input from=input(FIPS_PATH_INPUT_Probe,2,2); from.as.probe=fips_path_probe_new(90,102,0,0);
    net_step(&n,1002,from); CHECK(path(&n,2).state==FIPS_PATH_Probing); run(&n,1012); CHECK(path(&n,2).state==FIPS_PATH_Live);
}
static void verdicts(void)
{
    const uint8_t types[]={FIPS_LINK_SessionDatagram,FIPS_LINK_Heartbeat,FIPS_LINK_SenderReport,FIPS_LINK_PathProbe};
    for(size_t k=0;k<4;++k) {
        net n; fips_path_config cfg=fips_path_config_draft(); cfg.switch_margin=1000;
        net_init(&n,cfg); n.cut_from[1]=1000; n.cut_until[1]=UINT64_MAX; heard(&n); offer(&n,0,2);
        for(uint64_t at=50;at<=4500;at+=50) {
            run(&n,at); fips_transport_addr a=address(1);
            fips_path_bundle_account(&n.x.b,AT(at),T(1),&a,100,types[k]);
            if(types[k]==FIPS_LINK_PathProbe) { fips_path_input in=input(FIPS_PATH_INPUT_Probe,1,1); in.as.probe=fips_path_probe_new((uint32_t)at,101,0,0); net_step(&n,at,in); }
        }
        CHECK(changed(&n,1,FIPS_PATH_Suspect)==(k==0?4400:1800));
    }
}
static void mtu_search_and_connections(void)
{
    net n; fips_path_config cfg=fips_path_config_draft(); cfg.switch_margin=1000;
    net_init(&n,cfg); n.ceiling[2]=900; heard(&n); offer(&n,0,2); run(&n,120000);
    fips_path_view p=path(&n,2); CHECK(p.state==FIPS_PATH_Live&&p.has_mtu&&p.mtu<=900&&p.mtu>868&&p.misses==0);
    CHECK(n.proven>=1&&changed(&n,2,FIPS_PATH_Suspect)==UINT64_MAX);
    n.ceiling[2]=1200; run(&n,800000); CHECK(path(&n,2).mtu==1200);
    net_init(&n,cfg); n.x.facts.stream[2]=true; heard(&n); offer(&n,0,2); CHECK(!n.probes[2]&&n.connections[2]==1);
    run(&n,10000); CHECK(n.connections[2]==1&&n.probes[2]==11&&path(&n,2).state==FIPS_PATH_Live);
    net_init(&n,cfg); n.x.facts.stream[2]=true; n.connect_ok=false; heard(&n); offer(&n,0,2); run(&n,120000);
    CHECK(n.connections[2]==8&&!n.probes[2]&&path(&n,2).state==FIPS_PATH_Dead);
    net_init(&n,cfg); n.x.facts.stream[2]=true; heard(&n);
    fips_path_input in=input(FIPS_PATH_INPUT_Probe,2,2); in.as.probe=fips_path_probe_new(1,102,0,0); net_step(&n,0,in);
    run(&n,1000); CHECK(!n.connections[2]&&path(&n,2).state==FIPS_PATH_Live);
}
static void selection_over_time(void)
{
    for(unsigned role_case=0;role_case<4;++role_case) {
        net n; net_init(&n,fips_path_config_draft()); n.delay[1]=200; n.delay[2]=10;
        if(role_case==1) n.x.facts.role[2]=FIPS_PATH_Backup;
        if(role_case==2) n.x.facts.role[1]=FIPS_PATH_Backup;
        if(role_case==3) { n.delay[1]=10; n.delay[2]=10; n.x.facts.role[2]=FIPS_PATH_Preferred; }
        heard(&n); offer(&n,0,2); run(&n,6000);
        CHECK(path(&n,role_case==1?1:2).active);
        if(role_case==1) CHECK(!n.switches);
        /* Score comparison needs both paths sampled twice. A higher tier does not. */
        else CHECK(n.switches==1&&n.switched_at[0]==(role_case==0?2400:role_case==2?10:2210));
    }
    net n; net_init(&n,fips_path_config_draft()); n.delay[1]=5; n.delay[2]=25;
    n.cut_from[1]=40000; n.cut_until[1]=42000; heard(&n); offer(&n,0,2); run(&n,50000);
    CHECK(n.switches==2&&n.switched_to[0]==2&&n.switched_to[1]==1&&n.switched_at[1]>=44000);
    for(unsigned fast=0;fast<2;++fast) {
        net_init(&n,fips_path_config_draft()); n.delay[1]=10; n.delay[2]=fast?5:25;
        n.loaded_at[1]=40000; n.loaded_delay[1]=200; heard(&n); offer(&n,0,2); run(&n,60000);
        if(fast) CHECK(n.switches==1&&n.lead_window<50000&&n.lead_horizon==UINT64_MAX&&n.switched_at[0]==n.lead_window+2000);
        else CHECK(!n.switches&&n.lead_window<50000);
        run(&n,120000);
        CHECK(n.switches==1&&n.switched_to[0]==2);
        if(!fast) CHECK(n.lead_horizon>=100000&&n.switched_at[0]==n.lead_horizon+2000);
    }
}
void test_path_net(void)
{
    capability_gates(); cadences_and_failures(); verdicts(); mtu_search_and_connections(); selection_over_time();
    puts("PASS path public-interface capability/cadence/verdict/MTU/selection deadlines");
}
