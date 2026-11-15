#include "path_machine_test.h"
static void exchange(void)
{
    fixture x; init(&x,true); CHECK(x.b.set.len==1&&x.b.set.active==0&&x.b.set.paths[0].acked&&!x.b.capable);
    step(&x,0,input(FIPS_PATH_INPUT_Poll,0,0)); CHECK(!x.out.len&&!fips_path_bundle_next_wake(&x.b).ops);
    fips_path_input in=input(FIPS_PATH_INPUT_Candidate,2,2); in.as.source=FIPS_PATH_Config; step(&x,0,in);
    CHECK(x.b.set.len==1&&!x.b.capable&&x.b.record.sent==1); fips_path_probe cap=sent(&x,1); CHECK(cap.len==27&&cap.flags==1&&cap.caps==0&&cap.path_id==x.b.original);
    CHECK(instant_eq(fips_path_bundle_next_wake(&x.b),400)); answer(&x,10,2,77,cap);
    CHECK(x.b.capable&&x.b.set.len==2&&count(&x,FIPS_PATH_OUTPUT_Capable)==1);
    CHECK(x.b.set.paths[0].peer_id==102&&fips_path_liveness_samples(&x.b.set.paths[0].live)==1&&!x.b.record.out.at.ops);
    fips_path_probe second=sent(&x,2); CHECK(second.len==1200);
    answer(&x,30,2,2,second); CHECK(x.b.set.paths[1].state==FIPS_PATH_Live&&x.b.set.paths[1].live.mtu==1200&&count(&x,FIPS_PATH_OUTPUT_Proven)==1);
    answer(&x,31,2,2,second); CHECK(count(&x,FIPS_PATH_OUTPUT_Refused)==1);
    init(&x,true); fips_transport_addr moved=address(9);
    fips_path_account a=fips_path_bundle_account(&x.b,AT(1),T(1),&moved,63,FIPS_LINK_PathProbe); CHECK(a.roam.kind==FIPS_PATH_Unmoved&&a.path==FIPS_PATH_NONE);
    a=fips_path_bundle_account(&x.b,AT(2),T(1),&moved,80,FIPS_LINK_Heartbeat); CHECK(a.roam.kind==FIPS_PATH_Roamed&&a.path==0&&a.sample);
    init(&x,true); capable(&x); CHECK(x.b.set.paths[0].live.frames==1);
    const fips_path_output *ack=NULL; for(size_t i=0;i<x.out.len;++i) if(x.actions[i].kind==FIPS_PATH_OUTPUT_Send&&x.actions[i].as.send.type==FIPS_LINK_PathAck) ack=&x.actions[i];
    CHECK(ack&&ack->as.send.as.ack.rx_frames==1&&ack->as.send.as.ack.rx_bytes==63);
    in=input(FIPS_PATH_INPUT_Probe,2,2); in.as.probe=fips_path_probe_new(1,102,0,0); in.as.probe.len=11;
    step(&x,1,in); CHECK(x.b.set.len==1&&count(&x,FIPS_PATH_OUTPUT_Refused)==1);
    in.as.probe.len=1200; step(&x,2,in); CHECK(x.b.set.len==2&&x.b.set.paths[1].state==FIPS_PATH_Probing&&!x.b.set.paths[1].acked);
    second=sent(&x,2); answer(&x,402,2,2,second); CHECK(x.b.set.paths[1].state==FIPS_PATH_Live);
    init(&x,true); capable(&x); in=input(FIPS_PATH_INPUT_Candidate,2,2); step(&x,1,in); second=sent(&x,2);
    answer(&x,402,2,2,second); CHECK(x.b.set.paths[1].state==FIPS_PATH_Probing&&!x.b.set.paths[1].acked&&x.b.set.paths[1].live.has_rtt);
}
static void disabled_and_rules(void)
{
    fixture x; init(&x,false); uint32_t seq=x.seq;
    const fips_path_input_kind kinds[]={FIPS_PATH_INPUT_Candidate,FIPS_PATH_INPUT_Unlisted,FIPS_PATH_INPUT_Probe,FIPS_PATH_INPUT_Ack,FIPS_PATH_INPUT_Close,FIPS_PATH_INPUT_Connected};
    for(size_t i=0;i<6;++i) { step(&x,i,input(kinds[i],2,2)); CHECK(!x.out.len&&!x.b.capable&&x.b.set.len==1&&x.seq==seq); }
    for(unsigned ms=100;ms<=180000;ms+=100) { step(&x,ms,input(FIPS_PATH_INPUT_Poll,0,0)); CHECK(!x.out.len&&!fips_path_bundle_next_wake(&x.b).ops); }
    fips_transport_addr a=address(9); CHECK(fips_path_bundle_account(&x.b,AT(1),T(1),&a,63,FIPS_LINK_PathProbe).roam.kind==FIPS_PATH_Roamed);
    step(&x,2,input(FIPS_PATH_INPUT_SendFailed,1,9)); CHECK(x.b.set.paths[0].state==FIPS_PATH_Suspect&&count(&x,FIPS_PATH_OUTPUT_Send)==0);
    step(&x,3,input(FIPS_PATH_INPUT_Removed,1,0)); CHECK(x.b.orphaned&&count(&x,FIPS_PATH_OUTPUT_Released)==1&&x.actions[x.out.len-1].kind==FIPS_PATH_OUTPUT_Orphaned);
    CHECK(fips_path_bundle_account(&x.b,AT(4),T(1),&a,100,0).path==FIPS_PATH_NONE&&fips_path_bundle_charge(&x.b,AT(4),T(1),&a)==FIPS_PATH_Nothing);
    init(&x,true); capable(&x); a=address(2); fips_transport_addr wan=text_addr("8.8.8.8:53");
    const fips_path_scope local={FIPS_PATH_Clear,FIPS_PATH_Local};
    CHECK(fips_path_rules_probe(&x.f,local,false,T(2),&a)==FIPS_PATH_RuleNone);
    CHECK(fips_path_rules_probe(&x.f,local,false,T(2),&wan)==FIPS_PATH_Kind);
    CHECK(fips_path_rules_probe(&x.f,local,true,T(2),&wan)==FIPS_PATH_RuleNone);
    x.facts.class_[2]=FIPS_PATH_Tor; CHECK(fips_path_rules_moves(&x.f,local,true,T(2),&a)==FIPS_PATH_Class); x.facts.class_[2]=FIPS_PATH_Clear;
    x.facts.kind[2]=FIPS_PATH_Local; CHECK(fips_path_rules_kind(&x.f,T(2),&wan)==FIPS_PATH_Local);
    x.facts.role[2]=FIPS_PATH_Never; CHECK(fips_path_rules_moves(&x.f,local,false,T(2),&a)==FIPS_PATH_Role); x.facts.role[2]=FIPS_PATH_Normal;
    x.facts.accept[2]=false; CHECK(fips_path_rules_admit(&x.f,local,false,T(2),&a)==FIPS_PATH_Accept&&fips_path_rules_probe(&x.f,local,false,T(2),&a)==FIPS_PATH_RuleNone);
    x.facts.dial[2]=false; CHECK(fips_path_rules_probe(&x.f,local,false,T(2),&a)==FIPS_PATH_Dial&&fips_path_rules_moves(&x.f,local,false,T(2),&a)==FIPS_PATH_RuleNone);
    fips_path_input in=input(FIPS_PATH_INPUT_Candidate,2,2); step(&x,1,in); CHECK(x.b.set.len==1&&count(&x,FIPS_PATH_OUTPUT_Refused)==1);
    step(&x,2,input(FIPS_PATH_INPUT_Poll,0,0)); CHECK(!count(&x,FIPS_PATH_OUTPUT_Refused));
    x.facts.role[2]=FIPS_PATH_Never; step(&x,3,input(FIPS_PATH_INPUT_Poll,0,0)); CHECK(count(&x,FIPS_PATH_OUTPUT_Refused)==1);
    x.facts.role[2]=FIPS_PATH_Normal; x.facts.dial[2]=true; step(&x,4,input(FIPS_PATH_INPUT_Poll,0,0)); CHECK(x.b.set.len==2&&sent(&x,2).len==1200);
}
static void switching(void)
{
    fixture x; init(&x,true); capable(&x); live_path(&x,2,2); score(&x.b.set.paths[0],200); score(&x.b.set.paths[1],10);
    step(&x,20,input(FIPS_PATH_INPUT_Poll,0,0)); CHECK(x.b.set.active==0&&instant_eq(x.b.dwell_at,20));
    /* Deterministic measurements isolate the selection rule from expiry. */
    for(uint64_t now=30;now<=2020;now+=10) {
        score(&x.b.set.paths[0],200); score(&x.b.set.paths[1],10); step(&x,now,input(FIPS_PATH_INPUT_Poll,0,0));
    }
    CHECK(x.b.set.active==1&&count(&x,FIPS_PATH_OUTPUT_Switched)==1&&x.b.hold.at.ops);
    CHECK(fips_path_bundle_cost(&x.b,AT(10000),9)==1.5);
    /* Reports are counted only while held; use a fresh measured state so the
     * cost-hold test has no unrelated expired probes. */
    score(&x.b.set.paths[0],200); score(&x.b.set.paths[1],10);
    step(&x,10001,input(FIPS_PATH_INPUT_Report,0,0)); step(&x,10002,input(FIPS_PATH_INPUT_Report,0,0)); CHECK(fips_path_bundle_cost(&x.b,AT(10002),9)==9);
    init(&x,true); capable(&x); live_path(&x,2,2); score(&x.b.set.paths[0],5); score(&x.b.set.paths[1],25);
    fips_path_input down=input(FIPS_PATH_INPUT_Carrier,1,0); down.as.up=false; step(&x,20,down);
    CHECK(x.b.set.active==1&&x.b.left==x.b.set.paths[0].path_id&&count(&x,FIPS_PATH_OUTPUT_Switched)==1);
    x.b.set.paths[0].state=FIPS_PATH_Live; score(&x.b.set.paths[0],5); score(&x.b.set.paths[1],25);
    step(&x,21,input(FIPS_PATH_INPUT_Poll,0,0)); CHECK(instant_eq(x.b.dwell_at,21));
    score(&x.b.set.paths[0],5); score(&x.b.set.paths[1],25); step(&x,2021,input(FIPS_PATH_INPUT_Poll,0,0)); CHECK(x.b.set.active==0);
    init(&x,true); capable(&x); live_path(&x,2,2); score(&x.b.set.paths[0],5); score(&x.b.set.paths[1],30);
    for(size_t i=0;i<32;++i) x.b.set.paths[0].live.rtts[i]=(fips_path_sample){AT(i),MS(200)};
    step(&x,100,input(FIPS_PATH_INPUT_Poll,0,0)); CHECK(x.b.set.active==0&&!x.b.dwell_at.ops);
    fips_path_input pin=input(FIPS_PATH_INPUT_Pin,2,0); pin.as.has_addr=false; step(&x,101,pin);
    CHECK(x.b.set.active==1&&count(&x,FIPS_PATH_OUTPUT_Switched)==1);
    fips_path_bundle_view v; fips_path_bundle_view_get(&x.b,AT(102),&v); CHECK(v.has_pin&&v.pin_enforced&&v.paths[1].pinned&&v.paths[1].active);
}
static void lifecycle(void)
{
    fixture x; init(&x,true); capable(&x); size_t q=live_path(&x,2,2); x.b.set.paths[0].driver_slot=&x.seq; x.b.set.paths[q].driver_slot=&x.facts;
    fips_transport_addr a=address(1),qa=address(2); uint32_t old=x.b.set.paths[q].path_id;
    for(unsigned i=0;i<19;++i) CHECK(fips_path_bundle_charge(&x.b,AT(i),T(1),&a)==FIPS_PATH_Counted);
    CHECK(fips_path_bundle_charge(&x.b,AT(20),T(1),&a)==FIPS_PATH_Tripped&&instant_eq(fips_path_bundle_next_wake(&x.b),20));
    fips_path_bundle_authentic(&x.b,T(1),&a); CHECK(x.b.set.paths[0].live.fails==0);
    step(&x,21,input(FIPS_PATH_INPUT_Poll,0,0)); CHECK(x.b.set.paths[0].state==FIPS_PATH_Dead&&x.b.set.active==q);
    x.b.set.paths[q].live.fails=UINT32_MAX-1; CHECK(fips_path_bundle_charge(&x.b,AT(22),T(2),&qa)==FIPS_PATH_Counted&&x.b.set.paths[q].live.fails==UINT32_MAX);
    CHECK(fips_path_bundle_charge(&x.b,AT(23),T(2),&qa)==FIPS_PATH_Counted&&x.b.set.paths[q].live.fails==UINT32_MAX);
    fips_path_input pin=input(FIPS_PATH_INPUT_Pin,2,2); pin.as.has_addr=true; step(&x,24,pin);
    CHECK(fips_path_bundle_renew(&x.b,AT(25),&x.env,(fips_path_ref){T(2),qa},&x.out)==FIPS_OK);
    CHECK(x.b.set.len==1&&x.b.set.active==0&&x.b.set.paths[0].driver_slot==&x.facts&&x.b.set.paths[0].live.fails==UINT32_MAX&&x.b.set.paths[0].path_id!=old&&!x.b.capable&&x.b.has_pin);
    CHECK(count(&x,FIPS_PATH_OUTPUT_State)==3&&count(&x,FIPS_PATH_OUTPUT_Released)==1);
    CHECK(fips_path_bundle_close(&x.b,&x.out)==FIPS_OK&&x.out.len==1&&x.actions[0].as.released.keep&&x.actions[0].as.released.driver_slot==&x.facts);
    CHECK(fips_path_bundle_close(&x.b,&x.out)==FIPS_OK&&!x.out.len);
    init(&x,true); fips_path_outputs short_out={x.actions,127,7}; unsigned seq=x.seq; uint32_t id=x.b.original;
    fips_path_input in=input(FIPS_PATH_INPUT_Candidate,2,2);
    CHECK(fips_path_bundle_step(&x.b,AT(0),&in,&x.env,&short_out)==FIPS_ERR_RANGE&&short_out.len==7&&x.seq==seq&&x.b.original==id);
    CHECK(fips_path_bundle_renew(&x.b,AT(0),&x.env,(fips_path_ref){T(2),qa},&short_out)==FIPS_ERR_RANGE&&x.seq==seq);
    CHECK(fips_path_bundle_close(&x.b,&short_out)==FIPS_ERR_RANGE&&x.b.set.len==1);
    fips_path_bundle unchanged=x.b; fips_path_outputs missing={NULL,128,9};
    CHECK(fips_path_bundle_step(&x.b,AT(0),&in,&x.env,&missing)==FIPS_ERR_RANGE&&missing.len==9&&x.seq==seq&&!memcmp(&unchanged,&x.b,sizeof(unchanged)));
    CHECK(fips_path_bundle_renew(&x.b,AT(0),&x.env,(fips_path_ref){T(2),qa},&missing)==FIPS_ERR_RANGE&&x.seq==seq&&!memcmp(&unchanged,&x.b,sizeof(unchanged)));
    CHECK(fips_path_bundle_close(&x.b,&missing)==FIPS_ERR_RANGE&&missing.len==9&&!memcmp(&unchanged,&x.b,sizeof(unchanged)));
    capable(&x); x.facts.stream[2]=true; step(&x,1,in); CHECK(count(&x,FIPS_PATH_OUTPUT_Connect)==1&&x.b.set.paths[1].live.connect==FIPS_PATH_Pending);
    in=input(FIPS_PATH_INPUT_Connected,2,2); in.as.ok=true; step(&x,2,in); CHECK(sent(&x,2).len==1200&&x.b.set.paths[1].live.connect==FIPS_PATH_Up);
    fips_path_probe p=sent(&x,2); answer(&x,12,2,2,p);
    fips_path_bundle_account(&x.b,AT(13),T(2),&qa,100,FIPS_LINK_Heartbeat);
    in=input(FIPS_PATH_INPUT_Present,2,0); in.as.present=false; step(&x,14,in);
    CHECK(x.b.set.len==1&&count(&x,FIPS_PATH_OUTPUT_Released)==1);
    bool keep=false; for(size_t i=0;i<x.out.len;++i) if(x.actions[i].kind==FIPS_PATH_OUTPUT_Released) keep=x.actions[i].as.released.keep; CHECK(keep);
    in.as.present=true; step(&x,15,in); CHECK(x.b.set.len==2&&count(&x,FIPS_PATH_OUTPUT_Connect)==1);
}
void test_path_machine(void) { exchange(); disabled_and_rules(); switching(); lifecycle(); }
