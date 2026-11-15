#include "path_test.h"
static void wire(void)
{
    const uint8_t golden[]={0x52,0x44,0x33,0x22,0x11,1,0xd4,0xc3,0xb2,0xa1,1};
    uint8_t buf[1300]; fips_writer w; fips_reader r; fips_proto_error e;
    fips_path_probe p=fips_path_probe_new(0x11223344,0xa1b2c3d4,255,255),d;
    const size_t lens[]={0,11,27,300,1200};
    for(size_t i=0;i<5;++i) {
        p.len=lens[i]; fips_writer_init(&w,buf,sizeof(buf)); CHECK(fips_path_probe_encode(&p,&w,NULL)==FIPS_OK);
        CHECK(w.pos==(lens[i]<27?27:lens[i])); CHECK(!memcmp(buf,golden,sizeof(golden)));
        for(size_t j=11;j<w.pos;++j) CHECK(buf[j]==0);
        fips_reader_init(&r,buf+1,w.pos-1); CHECK(fips_path_probe_decode(&r,&d,NULL)==FIPS_OK);
        CHECK(d.probe_id==p.probe_id&&d.path_id==p.path_id&&d.flags==1&&d.caps==1&&d.len==w.pos);
    }
    for(size_t n=0;n<27;++n) {
        fips_reader_init(&r,buf+1,n); fips_status s=fips_path_probe_decode(&r,&d,&e);
        if(n<10) CHECK(s==FIPS_ERR_RANGE&&r.pos==0&&e.as.message_too_short.expected==10);
        else CHECK(s==FIPS_OK&&fips_path_probe_answerable(&d)==(n>=26));
    }
    buf[5]=0xfd; buf[10]=0xfe; memset(buf+11,0xaa,16);
    fips_reader_init(&r,buf+1,26); CHECK(fips_path_probe_decode(&r,&d,NULL)==FIPS_OK&&d.flags==0xfd&&d.caps==0xfe);
    memset(buf+6,0,4); fips_reader_init(&r,buf+1,26); CHECK(fips_path_probe_decode(&r,&d,&e)==FIPS_ERR_RANGE&&e.variant==FIPS_PROTO_Malformed&&r.pos==0);
    const uint8_t ga[]={0x53,0x44,0x33,0x22,0x11,0,0x88,0x77,0x66,0x55,0xd4,0xc3,0xb2,0xa1,0,4,3,2,1,0,1,0,0,0xef,0xbe,0xad,0xde};
    fips_path_ack a={.probe_id=0x11223344,.path_id=0x55667788,.echo_path_id=0xa1b2c3d4,.rx_time_us=0x01020304,.rx_frames=256,.rx_bytes=0xdeadbeef,.len=1},ad;
    fips_writer_init(&w,buf,sizeof(buf)); CHECK(fips_path_ack_encode(&a,&w,NULL)==FIPS_OK&&w.pos==27&&!memcmp(buf,ga,27));
    for(size_t n=0;n<27;++n) { fips_reader_init(&r,buf+1,n); CHECK((fips_path_ack_decode(&r,&ad,&e)==FIPS_OK)==(n>=26)); }
    a.len=1000; a.flags=0xfe; a.caps=0xfe; fips_writer_init(&w,buf,sizeof(buf)); CHECK(fips_path_ack_encode(&a,&w,NULL)==FIPS_OK);
    CHECK(buf[5]==0&&buf[14]==0); for(size_t i=27;i<w.pos;++i) CHECK(!buf[i]);
    fips_reader_init(&r,buf+1,w.pos-1); CHECK(fips_path_ack_decode(&r,&ad,NULL)==FIPS_OK&&ad.len==1000&&ad.rx_bytes==a.rx_bytes&&ad.echo_path_id==a.echo_path_id);
    CHECK(fips_path_ack_len(1200,500)==464&&fips_path_ack_len(300,1200)==300&&fips_path_ack_len(1300,1236)==1200&&fips_path_ack_len(11,1200)==27&&fips_path_ack_len(1200,20)==27);
    for(unsigned reason=0;reason<256;++reason) {
        uint8_t body[]={1,0,0,0,(uint8_t)reason,255,0xaa}; fips_path_close close;
        fips_reader_init(&r,body,sizeof(body)); CHECK(fips_path_close_decode(&r,&close,NULL)==FIPS_OK&&close.path_id==1&&close.reason==fips_path_close_reason_from_byte((uint8_t)reason));
        fips_writer_init(&w,buf,sizeof(buf)); CHECK(fips_path_close_encode(&close,&w,NULL)==FIPS_OK&&w.pos==7&&buf[6]==0);
    }
    /* Same extension grammar for both codecs: exact errors, truncation and padding. */
    const struct { uint8_t b[12]; size_t len; const char *error; } ext[]={
        {{6,0,1,0,2,0,0xab,0xcd},8,NULL},{{0,0},2,NULL},
        {{4,0,1,0x80,0,0},6,"unknown critical path extension TLV"},
        {{7,0,1,0,2,0,0xab,0xcd},8,"path extension block overruns the message"},
        {{6,0,1,0,3,0,0xab,0xcd},8,"path extension TLV overruns the block"},
        {{3,0,1,0,0},5,"path extension TLV overruns the block"},
        {{0},0,"path extension block overruns the message"},{{0},1,"path extension block overruns the message"}};
    for(unsigned kind=0;kind<2;++kind) for(size_t i=0;i<8;++i) {
        size_t fixed=kind?27:11; memcpy(buf,kind?ga:golden,fixed); buf[5]=2;
        memcpy(buf+fixed,ext[i].b,ext[i].len); fips_reader_init(&r,buf+1,fixed-1+ext[i].len);
        fips_status s=kind?fips_path_ack_decode(&r,&ad,&e):fips_path_probe_decode(&r,&d,&e);
        CHECK(ext[i].error?(s==FIPS_ERR_RANGE&&e.variant==FIPS_PROTO_Malformed&&!strcmp(e.as.malformed,ext[i].error)&&r.pos==0):s==FIPS_OK);
    }
    memset(buf,0xa5,sizeof(buf)); fips_writer_init(&w,buf,26); p.len=27;
    CHECK(fips_path_probe_encode(&p,&w,&e)==FIPS_ERR_RANGE&&w.pos==0); for(size_t i=0;i<sizeof(buf);++i) CHECK(buf[i]==0xa5);
}
static size_t add(fips_path_set *s,unsigned t,unsigned n,fips_entropy e)
{ fips_path_admission a; CHECK(fips_path_set_admit(s,T(t),address(n),FIPS_PATH_Unknown,e,&a,NULL)==FIPS_OK&&!a.known&&!a.has_evicted); return a.index; }
static void set(void)
{
    uint32_t seq=0; fips_entropy e=counter_entropy(&seq); fips_path_set s; fips_path_set_new(&s); fips_path_admission a; fips_path_refused err;
    CHECK(s.active==FIPS_PATH_NONE&&s.len==0); add(&s,1,1,e); CHECK(s.paths[0].path_id==1&&seq==2&&!s.paths[0].acked&&s.paths[0].state==FIPS_PATH_Probing);
    CHECK(fips_path_set_admit(&s,T(1),address(1),FIPS_PATH_Unknown,e,&a,NULL)==FIPS_OK&&a.known&&seq==2);
    add(&s,1,2,e); add(&s,2,1,e); CHECK(s.len==3&&s.paths[0].path_id!=s.paths[1].path_id);
    CHECK(!fips_path_set_learn_peer(&s,0,0)&&fips_path_set_learn_peer(&s,0,77)); CHECK(fips_path_set_find_by_peer(&s,T(1),77)==0&&fips_path_set_find_by_peer(&s,T(2),77)==FIPS_PATH_NONE);
    CHECK(!fips_path_set_activate(&s,0)); fips_path_set_mark_acked(&s,0); CHECK(fips_path_set_activate(&s,0));
    fips_transport_addr addr=address(1); CHECK(fips_path_set_classify(&s,T(1),&addr).kind==FIPS_PATH_Proven);
    addr=address(2); CHECK(fips_path_set_classify(&s,T(1),&addr).kind==FIPS_PATH_Unproven);
    fips_path_set_new(&s); for(unsigned i=0;i<8;++i) add(&s,1+i/4,i,e);
    CHECK(fips_path_set_admit(&s,T(1),address(99),FIPS_PATH_Unknown,e,&a,&err)==FIPS_ERR_RANGE&&err==FIPS_PATH_TransportFull);
    CHECK(fips_path_set_admit(&s,T(3),address(99),FIPS_PATH_Unknown,e,&a,&err)==FIPS_ERR_RANGE&&err==FIPS_PATH_PeerFull);
    fips_path_set_transition(&s,2,FIPS_PATH_Dead,AT(20)); fips_path_set_transition(&s,3,FIPS_PATH_Dead,AT(10));
    s.paths[3].driver_slot=&seq; uint32_t evict=s.paths[3].path_id;
    CHECK(fips_path_set_admit(&s,T(1),address(99),FIPS_PATH_Unknown,e,&a,&err)==FIPS_OK&&a.has_evicted&&a.evicted.path_id==evict&&a.evicted.driver_slot==&seq);
    CHECK(s.len==8&&a.index==7); fips_path_set_mark_acked(&s,2); /* Last acked Dead is protected without Live. */
    CHECK(fips_path_set_admit(&s,T(3),address(100),FIPS_PATH_Unknown,e,&a,&err)==FIPS_ERR_RANGE);
    fips_path_set_transition(&s,0,FIPS_PATH_Live,AT(30)); CHECK(fips_path_set_admit(&s,T(3),address(100),FIPS_PATH_Unknown,e,&a,&err)==FIPS_OK&&a.has_evicted&&a.evicted.acked);
    fips_path_set_new(&s); CHECK(fips_path_set_admit(&s,T(1),address(1),FIPS_PATH_Ble,e,&a,NULL)==FIPS_OK);
    CHECK(fips_path_set_admit(&s,T(1),address(2),FIPS_PATH_Ble,e,&a,&err)==FIPS_ERR_RANGE&&err==FIPS_PATH_TransportFull);
    CHECK(fips_path_set_admit(&s,T(2),address(2),FIPS_PATH_Ble,e,&a,NULL)==FIPS_OK);
    fips_path_set_transition(&s,0,FIPS_PATH_Dead,AT(1)); CHECK(fips_path_set_admit(&s,T(1),address(3),FIPS_PATH_Ble,e,&a,NULL)==FIPS_OK&&a.has_evicted);
    fips_path_set_new(&s); CHECK(fips_path_set_roam(&s,T(1),address(1),e).kind==FIPS_PATH_Bound);
    CHECK(s.active==0&&s.paths[0].acked&&s.paths[0].state==FIPS_PATH_Live);
    s.paths[0].driver_slot=&seq; uint32_t id=s.paths[0].path_id;
    CHECK(fips_path_set_roam(&s,T(1),address(2),e).kind==FIPS_PATH_Roamed&&s.paths[0].path_id==id&&s.paths[0].driver_slot==&seq);
    CHECK(fips_path_set_roam(&s,T(2),address(3),e).kind==FIPS_PATH_Unmoved);
    CHECK(fips_path_set_rebind(&s,T(2),address(3),FIPS_PATH_Unknown,e,&a)==FIPS_PATH_Refused&&s.len==2&&s.active==0);
    CHECK(fips_path_set_roam(&s,T(1),address(4),e).kind==FIPS_PATH_Unmoved);
    fips_path_set_mark_acked(&s,1); CHECK(fips_path_set_rebind(&s,T(2),address(3),FIPS_PATH_Unknown,e,&a)==FIPS_PATH_Switched&&s.active==1);
    fips_peer_path gone; CHECK(fips_path_set_discard(&s,0,&gone)&&gone.driver_slot==&seq&&s.active==0);
    CHECK(fips_path_set_discard(&s,0,&gone)&&s.active==FIPS_PATH_NONE&&!s.len); CHECK(!fips_path_set_discard(&s,0,&gone));
    add(&s,1,1,e); CHECK(fips_path_set_roam(&s,T(1),address(2),e).kind==FIPS_PATH_Standby);
    CHECK(fips_path_set_rebind(&s,T(1),address(2),FIPS_PATH_Unknown,e,&a)==FIPS_PATH_Refused); /* known but not acked */
}
static void params(void)
{
    fips_path_config c=fips_path_config_draft(); fips_path_params p; fips_path_params_error e;
    CHECK(c.enabled&&!c.mix_kinds&&c.switch_margin==1.3&&c.ring==8&&c.budget==3&&c.suspect==3&&c.retire==10&&c.discovery==8&&c.auth_limit==20);
    CHECK(fips_path_params_new(&p,c,&e)==FIPS_OK&&p.window.secs==30); c.min_samples=20;
    CHECK(fips_path_params_new(&p,c,&e)==FIPS_OK&&p.window.secs==40);
    const double margins[]={0.99,0,-1.3,NAN,INFINITY,-INFINITY};
    for(size_t i=0;i<6;++i) { c=fips_path_config_draft(); c.switch_margin=margins[i]; CHECK(fips_path_params_new(&p,c,&e)==FIPS_ERR_RANGE&&e.kind==FIPS_PATH_PARAMS_Margin); }
    c=fips_path_config_draft(); c.ring=9; CHECK(fips_path_params_new(&p,c,&e)==FIPS_ERR_RANGE&&e.kind==FIPS_PATH_PARAMS_Ring);
#define ZERO(member) do { c=fips_path_config_draft(); memset(&c.member,0,sizeof(c.member)); CHECK(fips_path_params_new(&p,c,&e)==FIPS_ERR_RANGE&&e.kind==FIPS_PATH_PARAMS_Zero&&e.field); } while(0)
    ZERO(switch_dwell); ZERO(active_interval); ZERO(standby_interval); ZERO(timeout); ZERO(verdict_cap); ZERO(horizon); ZERO(heartbeat); ZERO(dead_interval); ZERO(grace); ZERO(span); ZERO(cap);
    ZERO(min_samples); ZERO(suspect); ZERO(retire); ZERO(discovery); ZERO(auth_limit); ZERO(ring); ZERO(budget);
#undef ZERO
}
static void candidates(void)
{
    fips_path_set s; fips_path_set_new(&s); fips_path_candidates c; fips_path_candidates_new(&c); fips_transport_addr a=address(1);
    CHECK(fips_path_candidates_report(&c,T(1),a,FIPS_PATH_Config,&s).kind==FIPS_PATH_New);
    CHECK(fips_path_candidates_report(&c,T(1),a,FIPS_PATH_Config,&s).kind==FIPS_PATH_Repeat);
    CHECK(fips_path_candidates_report(&c,T(1),a,FIPS_PATH_Nostr,&s).kind==FIPS_PATH_Again);
    fips_path_candidates_hold(&c,T(1),a,&s); CHECK(c.slots[0].held);
    CHECK(fips_path_candidates_report(&c,T(1),a,FIPS_PATH_Config,&s).kind==FIPS_PATH_Repeat&&c.slots[0].held);
    for(unsigned i=0;i<3;++i) CHECK(fips_path_candidates_report(&c,T(1),a,FIPS_PATH_Request,&s).kind==FIPS_PATH_Again&&!c.slots[0].held);
    fips_path_candidates_mark(&c,0,FIPS_PATH_Dial); CHECK(c.slots[0].refused==FIPS_PATH_Dial);
    fips_path_candidates_absent(&c,T(1)); CHECK(c.slots[0].held&&c.slots[0].refused==FIPS_PATH_RuleNone);
    CHECK(!fips_path_candidates_unlist(&c,T(1),&a,FIPS_PATH_Config)); CHECK(!fips_path_candidates_unlist(&c,T(1),&a,FIPS_PATH_Nostr)); CHECK(!fips_path_candidates_unlist(&c,T(1),&a,FIPS_PATH_Request));
    uint8_t mask=fips_path_candidates_release(&c,T(1)); CHECK(mask==1); fips_path_candidates_prune(&c,mask,&s); CHECK(!c.slots[0].used);
    for(unsigned i=1;i<=8;++i) CHECK(fips_path_candidates_report(&c,T(1),address(i),FIPS_PATH_Config,&s).slot==i-1);
    a=address(6); fips_path_candidates_hold(&c,T(1),a,&s); fips_path_candidates_unlist(&c,T(1),&a,FIPS_PATH_Config);
    fips_path_candidates_hold(&c,T(1),address(7),&s); fips_path_candidates_report(&c,T(1),address(2),FIPS_PATH_Config,&s);
    CHECK(fips_path_candidates_report(&c,T(1),address(9),FIPS_PATH_Config,&s).slot==5);
    CHECK(fips_path_candidates_report(&c,T(1),address(10),FIPS_PATH_Config,&s).slot==0);
    CHECK(fips_path_candidates_report(&c,T(1),address(11),FIPS_PATH_Config,&s).slot==2);
    fips_path_candidates_absent(&c,T(1)); CHECK(fips_path_candidates_report(&c,T(2),address(12),FIPS_PATH_Config,&s).kind==FIPS_PATH_Full);
    fips_path_candidates_forget(&c,T(1)); for(size_t i=0;i<8;++i) CHECK(!c.slots[i].used);
}
static void scope(void)
{
    const char *local[]={"127.0.0.1:2121","127.255.0.9:1","10.1.2.3:2121","172.16.0.1:1","172.31.255.255:1","192.168.1.1:2121","169.254.3.4:2121","[::1]:2121","[fe80::1]:2121","[fe80::1%3]:2121","[febf::1]:2121","[fd00::1]:2121","[fc00::1]:2121","[::ffff:192.168.1.1]:2121","10.0.0.1","fe80::1"};
    const char *internet[]={"8.8.8.8:53","203.0.113.1:2121","100.64.0.1:2121","172.15.255.255:1","172.32.0.1:1","192.169.0.1:1","169.255.0.1:1","[2001:db8::1]:2121","[fec0::1]:2121","[fe00::1]:2121","[::ffff:8.8.8.8]:53","8.8.4.4","example.com:443","abcdefghijklmnop.onion:2121","","not an address","[::1]:99999","127.00.0.1","[::1]","fe80::1%3","[fe80::1%eth0]:1","1:2:3:4:5:6:7:8::"};
    for(size_t i=0;i<sizeof(local)/sizeof(*local);++i) { fips_transport_addr a=text_addr(local[i]); CHECK(fips_path_rules_scope(&a)==FIPS_PATH_Local); }
    for(size_t i=0;i<sizeof(internet)/sizeof(*internet);++i) { fips_transport_addr a=text_addr(internet[i]); CHECK(fips_path_rules_scope(&a)==FIPS_PATH_Internet); }
}
int main(void)
{
    wire(); set(); params(); candidates(); scope(); test_path_live(); test_path_machine(); test_path_net();
    puts("PASS path codec/set/params/candidate/rules/select/live/capability/Bundle cases"); return 0;
}
