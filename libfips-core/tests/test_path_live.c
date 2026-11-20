#include "path_test.h"
static void ring_and_window(void)
{
    fips_path_liveness l; uint32_t seq=100,id; fips_entropy e=counter_entropy(&seq);
    fips_path_liveness_new(&l);
    for(unsigned i=0;i<8;++i) CHECK(fips_path_liveness_send(&l,AT(i),AT(i+400),27,e,&id)&&id==100+i);
    CHECK(fips_path_liveness_outstanding(&l)==8&&!fips_path_liveness_send(&l,AT(99),AT(499),27,e,&id));
    CHECK(fips_path_liveness_answer(&l,100).kind==FIPS_PATH_InTime);
    CHECK(fips_path_liveness_send(&l,AT(100),AT(500),27,e,&id)&&id==108&&seq==101);
    CHECK(fips_path_liveness_answer(&l,102).kind==FIPS_PATH_InTime&&fips_path_liveness_answer(&l,102).kind==FIPS_PATH_AnswerNone);
    CHECK(l.ring[0].probe_id==101&&l.ring[1].probe_id==103&&l.ring[6].probe_id==108&&!l.ring[7].at.ops);
    fips_path_liveness_new(&l); fips_duration d;
    const unsigned rtts[]={50,80,30,70};
    for(unsigned i=0;i<4;++i) fips_path_liveness_sample(&l,AT(i*1000),MS(rtts[i]),MS(30000),MS(60000));
    CHECK(fips_path_liveness_min_rtt(&l,&d)&&duration_eq(d,MS(30))&&duration_eq(l.rtt,MS(70))&&fips_path_liveness_samples(&l)==4);
    fips_path_liveness_sample(&l,AT(33000),MS(90),MS(30000),MS(60000));
    CHECK(fips_path_liveness_samples(&l)==2&&fips_path_liveness_min_rtt(&l,&d)&&duration_eq(d,MS(70)));
    for(unsigned i=0;i<40;++i) fips_path_liveness_sample(&l,AT(34000+i),MS(200+i),MS(30000),MS(60000));
    CHECK(fips_path_liveness_samples(&l)==32&&fips_path_liveness_min_rtt(&l,&d)&&duration_eq(d,MS(208)));
    fips_path_liveness_new(&l); fips_path_liveness_sample(&l,AT(0),MS(10),MS(30000),MS(60000));
    for(unsigned t=200;t<=180000;t+=200) {
        fips_path_liveness_sample(&l,AT(t),MS(t==100000?20:500),MS(30000),MS(60000));
        if(t==60000) { CHECK(fips_path_liveness_min_rtt(&l,&d)&&duration_eq(d,MS(500))); CHECK(fips_path_liveness_base_rtt(&l,&d)&&duration_eq(d,MS(10))); }
        if(t==70000||t==180000) CHECK(fips_path_liveness_base_rtt(&l,&d)&&duration_eq(d,MS(500)));
        if(t==160000) CHECK(fips_path_liveness_base_rtt(&l,&d)&&duration_eq(d,MS(20)));
    }
    fips_path_liveness_new(&l); CHECK(l.etx==1); l.etx=4; fips_path_liveness_fold(&l,true);
    CHECK(fabs(l.etx-1.0/(.25+.75/32))<1e-9);
    for(unsigned i=0;i<200;++i) { double before=l.etx; fips_path_liveness_fold(&l,true); CHECK(l.etx<before||l.etx==1); } CHECK(l.etx<1.01);
    fips_path_liveness_heard(&l,100,AT(5)); CHECK(l.frames==1&&l.bytes==100&&instant_eq(l.heard_at,5));
    l.frames=UINT32_MAX; l.bytes=UINT32_MAX-10; fips_path_liveness_heard(&l,20,AT(9)); CHECK(l.frames==0&&l.bytes==9&&instant_eq(l.heard_at,9));
}
static void stop_and_answers(void)
{
    fips_path_liveness l; fips_path_liveness_new(&l); uint32_t seq=10,id,second; fips_entropy e=counter_entropy(&seq); bool counted;
    fips_path_liveness_halt(&l,AT(0),false); fips_path_liveness_halt(&l,AT(500),false); CHECK(instant_eq(l.stop.free,1000));
    const unsigned gaps[]={1000,2000,4000,8000,16000,30000,30000}; uint64_t time=1000;
    for(size_t i=0;i<7;++i) { fips_path_liveness_spend(&l,AT(time)); time+=gaps[i]; CHECK(instant_eq(l.stop.free,time)); }
    fips_path_liveness_new(&l); CHECK(fips_path_liveness_send(&l,AT(5000),AT(5400),27,e,&id));
    l.live_at=AT(5600); l.carried=true; fips_path_liveness_halt(&l,AT(14000),false);
    CHECK(instant_eq(l.stop.until,35000)); time=15000;
    for(unsigned i=0;i<26;++i) {
        CHECK(instant_eq(l.stop.free,time)); fips_path_liveness_spend(&l,AT(time));
        time+=i<=20?1000:i==21?2000:i==22?4000:i==23?8000:i==24?16000:30000;
    }
    fips_path_liveness_resume(&l); CHECK(!l.quiet.ops&&!l.stop.free.ops);
    CHECK(fips_path_liveness_send(&l,AT(100000),AT(100400),27,e,&id)); fips_path_liveness_halt(&l,AT(101000),false); CHECK(!l.stop.until.ops);
    fips_path_liveness_new(&l); fips_path_liveness_halt(&l,AT(10000),true);
    for(unsigned i=0;i<29;++i) fips_path_liveness_spend(&l,AT(11000+i*1000));
    CHECK(instant_eq(l.stop.free,40000));
    fips_path_liveness_spend(&l,AT(40000)); CHECK(instant_eq(l.stop.free,41000)&&duration_eq(l.stop.gap,MS(2000)));
    for(unsigned mode=0;mode<3;++mode) {
        fips_path_liveness_new(&l); fips_path_liveness_halt(&l,AT(0),false); fips_path_liveness_spend(&l,AT(10000));
        l.stop.owed=mode!=2; fips_path_liveness_rebase(&l,AT(10900),mode!=1);
        CHECK(instant_eq(l.stop.free,mode==0?11900:11000));
        if(mode==0) { fips_path_liveness_spend(&l,AT(11900)); CHECK(instant_eq(l.stop.free,13900)); }
    }
    fips_path_liveness_new(&l); CHECK(fips_path_liveness_send(&l,AT(0),AT(400),27,e,&id));
    CHECK(fips_path_liveness_send(&l,AT(1),AT(401),27,e,&second)); CHECK(instant_eq(l.quiet,0));
    CHECK(!fips_path_liveness_expire(&l,AT(400),true,MS(3000),&counted));
    CHECK(fips_path_liveness_expire(&l,AT(400),false,MS(3000),&counted)&&counted);
    CHECK(fips_path_liveness_answer(&l,id).kind==FIPS_PATH_Late); CHECK(fips_path_liveness_answer(&l,second).kind==FIPS_PATH_InTime&&l.misses==0);
    /* Old probes after leaving Dead are not misses, but can still match. */
    fips_path_liveness_new(&l); CHECK(fips_path_liveness_send(&l,AT(0),AT(400),27,e,&id)); CHECK(fips_path_liveness_send(&l,AT(1),AT(401),27,e,&second));
    fips_path_liveness_fresh(&l); fips_path_liveness_thaw(&l); CHECK(l.has_since);
    CHECK(fips_path_liveness_expire(&l,AT(400),false,MS(3000),&counted)&&!counted&&l.misses==0);
    CHECK(fips_path_liveness_answer(&l,second).kind==FIPS_PATH_InTime); CHECK(fips_path_liveness_answer(&l,id).kind==FIPS_PATH_Late);
    CHECK(fips_path_liveness_send(&l,AT(500),AT(900),27,e,&id)&&!l.has_since);
    CHECK(fips_path_liveness_expire(&l,AT(900),false,MS(3000),&counted)&&counted&&l.misses==1);
    /* Out-of-order timely answers prevent older misses, including wrap. */
    fips_path_liveness_new(&l); l.has_next_id=true; l.next_id=UINT32_MAX;
    CHECK(fips_path_liveness_send(&l,AT(0),AT(400),27,e,&id)&&id==UINT32_MAX);
    CHECK(fips_path_liveness_send(&l,AT(1),AT(401),27,e,&second)&&second==0);
    CHECK(fips_path_liveness_answer(&l,second).kind==FIPS_PATH_InTime);
    CHECK(fips_path_liveness_expire(&l,AT(400),false,MS(3000),&counted)&&!counted);
    /* Only data after sending defers; boundary is the original send + cap. */
    fips_path_liveness_new(&l); CHECK(fips_path_liveness_send(&l,AT(0),AT(400),27,e,&id)); l.data_at=AT(1);
    CHECK(!fips_path_liveness_expire(&l,AT(400),false,MS(3000),&counted)&&instant_eq(l.ring[0].due,3000));
    l.data_at=AT(2999); CHECK(fips_path_liveness_expire(&l,AT(3000),false,MS(3000),&counted)&&counted);
}
static void mtu(void)
{
    fips_path_liveness l; fips_path_liveness_new(&l); uint32_t seq=1,id; fips_entropy e=counter_entropy(&seq); bool counted;
    const size_t lengths[]={1200,900,600,27,1200};
    for(unsigned i=0;i<5;++i) {
        CHECK(fips_path_liveness_size(&l,1200)==lengths[i]);
        CHECK(fips_path_liveness_send(&l,AT(i*1000),AT(i*1000+400),lengths[i],e,&id));
        CHECK(fips_path_liveness_expire(&l,AT(i*1000+400),false,MS(3000),&counted));
    }
    fips_path_liveness_fresh(&l); CHECK(fips_path_liveness_size(&l,1200)==1200);
    CHECK(fips_path_liveness_prove(&l,600)); size_t n; CHECK(fips_path_liveness_seek_size(&l,1200,AT(0),&n)&&n==1200);
    id=fips_path_liveness_seek_send(&l,AT(0),n,e); CHECK(!fips_path_liveness_seek_size(&l,1200,AT(1000),&n));
    CHECK(!fips_path_liveness_seek_answer(&l,id+1,MS(1000),&n));
    CHECK(fips_path_liveness_seek_answer(&l,id,MS(1000),&n)&&n==1200); CHECK(fips_path_liveness_seek_proved(&l,1200,800,AT(100))&&l.mtu==800);
    CHECK(!fips_path_liveness_seek_size(&l,1200,AT(999),&n)); CHECK(fips_path_liveness_seek_size(&l,1200,AT(1000),&n)&&n==1000);
    id=fips_path_liveness_seek_send(&l,AT(1000),n,e);
    fips_path_liveness_seek_judge(&l,AT(1400),MS(400),MS(1000)); CHECK(l.seek.len==1000); /* no later answer */
    uint32_t later; CHECK(fips_path_liveness_send(&l,AT(1001),AT(1401),27,e,&later)); CHECK(fips_path_liveness_answer(&l,later).kind==FIPS_PATH_InTime);
    fips_path_liveness_seek_judge(&l,AT(1400),MS(400),MS(1000)); CHECK(l.seek.len==0&&l.seek.lost==1000&&l.misses==0);
    CHECK(!fips_path_liveness_seek_answer(&l,id,MS(1000),&n));
    CHECK(fips_path_liveness_seek_size(&l,1200,AT(2000),&n)&&n==900);
    CHECK(fips_path_liveness_seek_proved(&l,832,820,AT(2000))&&l.seek.lost==0&&instant_eq(l.seek.at,602000));
    CHECK(!fips_path_liveness_seek_size(&l,1200,AT(601999),&n)); CHECK(fips_path_liveness_seek_size(&l,1200,AT(602000),&n)&&n==1200);
    CHECK(fips_path_carried(1280,true,600,false)==636&&fips_path_carried(1280,true,600,true)==1280&&fips_path_carried(1280,true,SIZE_MAX,false)==1280);
}
static fips_path_rival rival(size_t i,double score,unsigned rtt)
{
    return (fips_path_rival){.used=true,.index=i,.has_score=score>=0,.score=score,.has_rtt=rtt!=UINT32_MAX,.rtt=MS(rtt),
        .has_steady=score>=0,.steady=score,.has_base=rtt!=UINT32_MAX,.base=MS(rtt),.tier=2,.fits=true,.medium=FIPS_PATH_Unknown};
}
static void selection(void)
{
    fips_path_rival f[8]={0}; double d;
    CHECK(fips_path_select_choose(f,1.3)==FIPS_PATH_NONE&&!fips_path_select_best(f,&d));
    f[0]=rival(0,-1,1); f[1]=rival(1,2,50); CHECK(fips_path_select_choose(f,1.3)==1);
    f[0]=rival(0,1.5,1); f[1]=rival(1,1.2,90); f[2]=rival(2,1.4,2); CHECK(fips_path_select_choose(f,1.3)==1);
    f[2].score=1.2; CHECK(fips_path_select_choose(f,1.3)==1);
    memset(f,0,sizeof(f)); f[0]=rival(0,-1,UINT32_MAX); f[1]=rival(1,-1,9); f[2]=rival(2,-1,3); f[3]=rival(3,-1,3); CHECK(fips_path_select_choose(f,1.3)==2);
    memset(f,0,sizeof(f)); f[0]=rival(0,1,1); f[0].tier=1; f[1]=rival(1,-1,50); CHECK(fips_path_select_choose(f,1.3)==1);
    f[1]=rival(1,2,50); CHECK(fips_path_select_choose(f,1.3)==1); f[0].tier=0; f[1].tier=1; CHECK(fips_path_select_choose(f,1.3)==1);
    f[0]=rival(0,1,1); f[1]=rival(1,1.25,9); f[1].preferred=true;
    CHECK(fips_path_select_choose(f,1.3)==1); f[1].score=1.3; CHECK(fips_path_select_choose(f,1.3)==1);
    f[1].score=1.35; CHECK(fips_path_select_choose(f,1.3)==0); f[1].has_score=false; CHECK(fips_path_select_choose(f,1.3)==0);
    f[1]=rival(1,1.2,9); f[1].preferred=true; f[2]=rival(2,1.1,9); f[2].preferred=true; f[3]=rival(3,1.1,9); f[3].preferred=true;
    CHECK(fips_path_select_choose(f,1.3)==2&&fips_path_select_best(f,&d)&&d==1);
    memset(f,0,sizeof(f)); f[0]=rival(0,1,1); f[0].fits=false; f[1]=rival(1,3,200); CHECK(fips_path_select_choose(f,1.3)==1);
    f[1].tier=1; CHECK(fips_path_select_choose(f,1.3)==0); f[1].pinned=true; CHECK(fips_path_select_choose(f,1.3)==1);
    memset(f,0,sizeof(f)); f[0]=rival(0,1,1); f[0].medium=FIPS_PATH_Wireless; f[1]=rival(1,1.25,9); f[1].medium=FIPS_PATH_Wired;
    CHECK(fips_path_select_choose(f,1.3)==1); f[1].score=1.35; CHECK(fips_path_select_choose(f,1.3)==0);
    f[1].score=1.25; f[0].medium=FIPS_PATH_Unknown; CHECK(fips_path_select_choose(f,1.3)==0);
    f[0].medium=FIPS_PATH_Wireless; f[0].preferred=true; CHECK(fips_path_select_choose(f,1.3)==0); f[1].preferred=true; CHECK(fips_path_select_choose(f,1.3)==1);
    fips_path_rival cable=rival(0,1.06,6),wifi=rival(1,1.05,5); cable.medium=FIPS_PATH_Wired; wifi.medium=FIPS_PATH_Wireless; cable.score=3;
    CHECK(fips_path_select_held(&wifi,&cable,1.3)); cable.steady=1.5; cable.base=MS(50); CHECK(!fips_path_select_held(&wifi,&cable,1.3));
    cable=rival(0,1.06,6); cable.score=3; CHECK(!fips_path_select_held(&wifi,&cable,1.3));
    wifi.score=3; cable=rival(0,1.5,50); CHECK(fips_path_select_waits(&cable,&wifi,1.3)); cable.score=1.06; CHECK(!fips_path_select_waits(&cable,&wifi,1.3));
    for(unsigned n=0;n<4;++n) {
        fips_path_hold h=fips_path_hold_new(1.5,AT(1000)); for(unsigned i=0;i<n;++i) fips_path_hold_report(&h);
        CHECK(fips_path_hold_held(&h,AT(2999),MS(2000),&d)&&d==1.5); CHECK(fips_path_hold_held(&h,AT(3000),MS(2000),&d)==(n<2));
    }
}
static void capability(void)
{
    fips_path_capability c; fips_path_capability_new(&c); fips_path_config cfg=fips_path_config_draft();
    const uint64_t schedule[]={0,10000,20000,50000,110000,230000,470000,950000,1550000,2150000,2750000,3350000,3950000};
    for(unsigned i=0;i<13;++i) {
        CHECK(fips_path_capability_due(&c,AT(schedule[i]))); fips_path_capability_send(&c,AT(schedule[i]),7,i,MS(400),&cfg);
        CHECK(!fips_path_capability_due(&c,AT(schedule[i]+1000))); fips_path_sent sent; bool round;
        CHECK(!fips_path_capability_answer(&c,8,i,&sent)&&!fips_path_capability_answer(&c,7,i+1,&sent));
        CHECK(!fips_path_capability_expire(&c,AT(schedule[i]+400),true,&cfg,&sent,&round));
        CHECK(fips_path_capability_expire(&c,AT(schedule[i]+400),false,&cfg,&sent,&round)&&round==(i==2));
        CHECK(!fips_path_capability_answer(&c,7,i,&sent)); if(i<12) CHECK(instant_eq(c.next_at,schedule[i+1]));
    }
    CHECK(c.told&&c.sent==13); fips_path_capability_new(&c); cfg.span=MS(900);
    fips_path_capability_send(&c,AT(0),7,41,MS(400),&cfg); CHECK(instant_eq(c.next_at,300));
    CHECK(!fips_path_capability_due(&c,AT(300))&&instant_eq(fips_path_capability_wake(&c,true),400));
    fips_path_sent sent; CHECK(fips_path_capability_answer(&c,7,41,&sent)&&sent.len==27&&instant_eq(sent.at,0));
    CHECK(!fips_path_capability_answer(&c,7,41,&sent)&&!fips_path_capability_wake(&c,false).ops);
}
void test_path_live(void) { ring_and_window(); stop_and_answers(); mtu(); selection(); capability(); }
