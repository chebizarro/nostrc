#include <fips/proto/settle/output.h>
#include "fakes/host.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"gate:%d: %s\n",__LINE__,#x); abort(); } } while (0)
static fips_node_addr peer(unsigned n) { fips_node_addr p={{0}}; p.bytes[0]=(uint8_t)(n>>8); p.bytes[15]=(uint8_t)n; return p; }
static void eph(unsigned n, uint8_t out[32]) { memset(out,0,32); out[0]=(uint8_t)(n>>8); out[31]=(uint8_t)n; }
static fips_settle_liveness unheard(uint64_t at) { return (fips_settle_liveness){.installed=fips_test_instant(at)}; }
static fips_settle_liveness heard(uint64_t at, bool has_last, uint64_t last)
{ return (fips_settle_liveness){.heard=true,.installed=fips_test_instant(at),.has_last=has_last,.last=fips_test_instant(last)}; }
static fips_settle_ask ask(fips_settle_source source, const fips_settle_liveness *held, unsigned hs)
{
    fips_settle_ask a={.source=source,.has_held=held!=NULL,.has_ephemeral=true,.has_epoch=true};
    if (held) a.held=*held;
    eph(hs,a.ephemeral); memset(a.epoch,1,8); return a;
}
static fips_settle_refusal gate(fips_settle_gates *g, fips_settle_ask a, uint64_t now)
{
    fips_settle_params p=fips_settle_params_default(); fips_settle_refusal r;
    CHECK(fips_settle_gate(g,peer(2),&a,fips_test_instant(now),&p,&r)==FIPS_OK); return r;
}
static void promoted(fips_settle_gates *g, unsigned n, uint64_t now)
{
    uint8_t e[32]; eph(n,e); fips_settle_params p=fips_settle_params_default();
    CHECK(fips_settle_gate_promoted(g,peer(2),e,fips_test_instant(now),NULL,&p)==FIPS_OK);
}
static uint64_t ended(fips_settle_gates *g, unsigned n, uint8_t epoch, uint64_t now)
{
    uint8_t e[32], ep[8]; eph(n,e); memset(ep,epoch,8); bool started; fips_duration length;
    fips_settle_params p=fips_settle_params_default();
    CHECK(fips_settle_gate_ended(g,peer(2),ep,e,fips_test_instant(now),NULL,&p,&started,&length)==FIPS_OK);
    return started ? length.secs : 0;
}
static void redialed(fips_settle_gates *g, unsigned n, uint64_t now)
{
    uint8_t e[32]; eph(n,e); fips_settle_params p=fips_settle_params_default();
    CHECK(fips_settle_gate_redialed(g,peer(2),e,fips_test_instant(now),&p)==FIPS_OK);
}
#define TEST(name) static void name(fips_settle_gates *g)
TEST(the_unheard_floor_refuses_until_one_second_and_accepts_at_it)
{
    fips_settle_liveness h=unheard(0); fips_settle_ask a=ask(FIPS_SETTLE_SOURCE_REPLACEMENT,&h,2);
    CHECK(gate(g,a,999)==FIPS_SETTLE_GATE_UNHEARD); CHECK(gate(g,a,1000)==FIPS_SETTLE_GATE_ACCEPT);
}
TEST(the_heard_floor_counts_from_the_last_frame)
{
    fips_settle_liveness h=heard(0,true,5000); fips_settle_ask a=ask(FIPS_SETTLE_SOURCE_RESTART,&h,2);
    CHECK(gate(g,a,19999)==FIPS_SETTLE_GATE_HEARD); CHECK(gate(g,a,20000)==FIPS_SETTLE_GATE_ACCEPT);
}
TEST(the_heard_floor_counts_from_installation_when_no_frame_has_arrived)
{
    fips_settle_liveness h=heard(2000,false,0); fips_settle_ask a=ask(FIPS_SETTLE_SOURCE_REPLACEMENT,&h,2);
    CHECK(gate(g,a,16999)==FIPS_SETTLE_GATE_HEARD); CHECK(gate(g,a,17000)==FIPS_SETTLE_GATE_ACCEPT);
}
TEST(the_dampener_refuses_until_its_ceiling_after_the_last_accepted_restart)
{
    fips_settle_liveness h=heard(0,true,0); CHECK(gate(g,ask(FIPS_SETTLE_SOURCE_RESTART,&h,2),15000)==FIPS_SETTLE_GATE_ACCEPT);
    CHECK(fips_settle_gate_get(g,&(fips_node_addr){{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2}})->stamp.opaque[0]==15000);
    h=unheard(15000); fips_settle_ask a=ask(FIPS_SETTLE_SOURCE_RESTART,&h,3);
    CHECK(gate(g,a,29999)==FIPS_SETTLE_GATE_DAMPENED); CHECK(gate(g,a,30000)==FIPS_SETTLE_GATE_ACCEPT);
}
TEST(an_own_msg2_restart_goes_through_the_gate_and_is_accepted_inside_its_floors)
{
    fips_settle_liveness h=heard(0,true,0); CHECK(gate(g,ask(FIPS_SETTLE_SOURCE_RESTART,&h,2),15000)==FIPS_SETTLE_GATE_ACCEPT);
    h=heard(15000,true,16000); CHECK(gate(g,ask(FIPS_SETTLE_SOURCE_RESTART,&h,3),17000)==FIPS_SETTLE_GATE_HEARD);
    fips_settle_ask a=ask(FIPS_SETTLE_SOURCE_OWN_MSG2,&h,0); a.has_ephemeral=false;
    CHECK(gate(g,a,17000)==FIPS_SETTLE_GATE_ACCEPT);
}
TEST(an_own_msg2_restart_does_not_stamp_the_dampener)
{
    fips_settle_liveness h=heard(0,true,0); fips_settle_ask a=ask(FIPS_SETTLE_SOURCE_OWN_MSG2,&h,0); a.has_ephemeral=false;
    CHECK(gate(g,a,20000)==FIPS_SETTLE_GATE_ACCEPT); a.source=FIPS_SETTLE_SOURCE_REKEY_MSG2;
    CHECK(gate(g,a,20000)==FIPS_SETTLE_GATE_ACCEPT); fips_node_addr p=peer(2); CHECK(!fips_settle_gate_get(g,&p));
}
TEST(the_replay_memory_refuses_an_ephemeral_that_already_promoted_an_unheard_peering)
{
    promoted(g,1,0); fips_settle_liveness h=unheard(0);
    CHECK(gate(g,ask(FIPS_SETTLE_SOURCE_REPLACEMENT,&h,2),1000)==FIPS_SETTLE_GATE_ACCEPT);
    promoted(g,2,1000); h=unheard(1000); fips_settle_ask a=ask(FIPS_SETTLE_SOURCE_REPLACEMENT,&h,1);
    CHECK(gate(g,a,2000)==FIPS_SETTLE_GATE_REPLAYED); CHECK(gate(g,a,15000)==FIPS_SETTLE_GATE_ACCEPT);
}
TEST(the_replay_memory_holds_at_most_eight)
{
    promoted(g,0,0);
    for (unsigned i=1;i<8;++i) { fips_settle_liveness h=unheard((i-1)*1000); CHECK(gate(g,ask(FIPS_SETTLE_SOURCE_REPLACEMENT,&h,i),i*1000)==FIPS_SETTLE_GATE_ACCEPT); promoted(g,i,i*1000); }
    fips_settle_liveness h=unheard(7000); CHECK(gate(g,ask(FIPS_SETTLE_SOURCE_REPLACEMENT,&h,8),8000)==FIPS_SETTLE_GATE_REPLAY_FULL);
}
TEST(a_frame_forgets_the_replay_memory_and_the_silent_sessions)
{
    promoted(g,1,0); for (unsigned i=10;i<13;++i) (void)ended(g,i,1,0);
    CHECK(gate(g,ask(FIPS_SETTLE_SOURCE_NOT_HELD,NULL,20),1000)==FIPS_SETTLE_GATE_SILENT);
    fips_node_addr p=peer(2); fips_settle_gate_heard(g,&p);
    CHECK(gate(g,ask(FIPS_SETTLE_SOURCE_NOT_HELD,NULL,20),1000)==FIPS_SETTLE_GATE_ACCEPT);
    fips_settle_liveness h=unheard(0); CHECK(gate(g,ask(FIPS_SETTLE_SOURCE_REPLACEMENT,&h,1),2000)==FIPS_SETTLE_GATE_ACCEPT);
}
TEST(silent_sessions_start_a_refusal_at_the_third_in_a_row)
{
    CHECK(!ended(g,1,1,0)); CHECK(!ended(g,1,1,0)); CHECK(!ended(g,2,1,0));
    CHECK(gate(g,ask(FIPS_SETTLE_SOURCE_NOT_HELD,NULL,9),0)==FIPS_SETTLE_GATE_ACCEPT);
    CHECK(ended(g,3,1,0)==30); fips_settle_ask a=ask(FIPS_SETTLE_SOURCE_NOT_HELD,NULL,9);
    CHECK(gate(g,a,29999)==FIPS_SETTLE_GATE_SILENT); memset(a.epoch,7,8);
    CHECK(gate(g,a,1000)==FIPS_SETTLE_GATE_ACCEPT); memset(a.epoch,1,8); CHECK(gate(g,a,30000)==FIPS_SETTLE_GATE_ACCEPT);
}
TEST(each_further_silent_session_doubles_the_refusal_up_to_its_cap)
{
    const uint64_t want[]={0,0,30,60,120,240,480,600,600};
    for (unsigned i=1;i<=9;++i) CHECK(ended(g,i,1,i)==want[i-1]);
}
TEST(gate_memory_holds_no_entry_once_an_identitys_state_has_lapsed)
{
    fips_settle_params p=fips_settle_params_default(); fips_settle_refusal r;
    for (unsigned i=0;i<10000;++i) { fips_settle_ask a=ask(FIPS_SETTLE_SOURCE_NOT_HELD,NULL,i); CHECK(fips_settle_gate(g,peer(i),&a,fips_test_instant(i),&p,&r)==FIPS_OK && r==FIPS_SETTLE_GATE_ACCEPT); }
    for (unsigned i=20000;i<20100;++i) {
        uint8_t e[32], ep[8]={1}; eph(i,e); CHECK(fips_settle_gate_promoted(g,peer(i),e,fips_test_instant(20000),NULL,&p)==FIPS_OK);
        for (unsigned j=0;j<3;++j) { eph(i*4+j,e); bool started; fips_duration length; CHECK(fips_settle_gate_ended(g,peer(i),ep,e,fips_test_instant(20000),NULL,&p,&started,&length)==FIPS_OK); }
        fips_settle_liveness h=heard(0,true,0); fips_settle_ask a=ask(FIPS_SETTLE_SOURCE_RESTART,&h,i); memset(a.epoch,2,8);
        CHECK(fips_settle_gate(g,peer(i),&a,fips_test_instant(20000),&p,&r)==FIPS_OK && r==FIPS_SETTLE_GATE_ACCEPT);
    }
    size_t n=0; for (size_t i=0;i<g->capacity;++i) n+=g->slots[i].active; CHECK(n>=100);
    CHECK(gate(g,ask(FIPS_SETTLE_SOURCE_NOT_HELD,NULL,1),3600000)==FIPS_SETTLE_GATE_ACCEPT);
    n=0; for (size_t i=0;i<g->capacity;++i) n+=g->slots[i].active; CHECK(!n);
}
TEST(the_refused_count_goes_with_its_silent_session_record)
{
    fips_node_addr p=peer(2); uint32_t n; CHECK(!fips_settle_gate_count_refused(g,&p,&n));
    for (unsigned i=1;i<=3;++i) (void)ended(g,i,1,0);
    CHECK(fips_settle_gate_count_refused(g,&p,&n) && n==1); CHECK(fips_settle_gate_count_refused(g,&p,&n) && n==2);
    CHECK(ended(g,4,1,1000)); CHECK(fips_settle_gate_take_refused(g,&p)==2);
    CHECK(fips_settle_gate_count_refused(g,&p,&n) && n==1); (void)ended(g,5,2,2000); CHECK(!fips_settle_gate_take_refused(g,&p));
    CHECK(fips_settle_gate_count_refused(g,&p,&n) && n==1);
    fips_settle_params params=fips_settle_params_default(); bool started; fips_duration length;
    CHECK(fips_settle_gate_ended(g,peer(1),NULL,NULL,fips_test_instant(3600000),NULL,&params,&started,&length)==FIPS_OK);
    CHECK(!fips_settle_gate_count_refused(g,&p,&n));
}
TEST(a_redial_is_not_held_by_the_heard_floor)
{
    fips_settle_liveness h=heard(0,true,40000);
    CHECK(gate(g,ask(FIPS_SETTLE_SOURCE_REPLACEMENT,&h,2),40100)==FIPS_SETTLE_GATE_HEARD);
    CHECK(gate(g,ask(FIPS_SETTLE_SOURCE_REDIAL,&h,3),40100)==FIPS_SETTLE_GATE_ACCEPT);
    fips_node_addr p=peer(2); CHECK(!fips_settle_gate_get(g,&p));
}
TEST(the_redial_memory_refuses_a_seen_ephemeral_and_a_full_memory_until_it_ages_out)
{
    fips_settle_liveness h=heard(0,true,0); redialed(g,2,40000);
    CHECK(gate(g,ask(FIPS_SETTLE_SOURCE_REDIAL,&h,2),41000)==FIPS_SETTLE_GATE_REDIALED);
    for (unsigned i=3;i<10;++i) redialed(g,i,41000);
    CHECK(gate(g,ask(FIPS_SETTLE_SOURCE_REDIAL,&h,99),42000)==FIPS_SETTLE_GATE_REDIALED);
    CHECK(gate(g,ask(FIPS_SETTLE_SOURCE_REDIAL,&h,99),56000)==FIPS_SETTLE_GATE_ACCEPT);
}
TEST(the_swap_stamps_the_dampener_only_for_a_heard_peering_and_clears_the_redial_memory)
{
    redialed(g,2,40000); CHECK(fips_settle_gate_swapped(g,peer(2),false,fips_test_instant(40500))==FIPS_OK);
    fips_settle_liveness h=heard(40500,true,40500); CHECK(gate(g,ask(FIPS_SETTLE_SOURCE_REDIAL,&h,2),41000)==FIPS_SETTLE_GATE_ACCEPT);
    CHECK(fips_settle_gate_swapped(g,peer(2),true,fips_test_instant(41000))==FIPS_OK);
    h=heard(41000,true,41000); CHECK(gate(g,ask(FIPS_SETTLE_SOURCE_REDIAL,&h,3),55999)==FIPS_SETTLE_GATE_DAMPENED);
    CHECK(gate(g,ask(FIPS_SETTLE_SOURCE_REDIAL,&h,3),56000)==FIPS_SETTLE_GATE_ACCEPT);
}
TEST(the_redial_memory_outlives_a_frame_and_the_lapse_of_the_rest_of_the_identitys_memory)
{
    redialed(g,2,40000); fips_node_addr p=peer(2); fips_settle_gate_heard(g,&p);
    fips_settle_liveness h=heard(0,true,40500); CHECK(gate(g,ask(FIPS_SETTLE_SOURCE_REDIAL,&h,2),41000)==FIPS_SETTLE_GATE_REDIALED);
}
TEST(the_redial_memory_outlives_the_first_frame_of_a_heard_session_and_a_move)
{
    redialed(g,2,40000); fips_node_addr p=peer(2); fips_settle_gate_framed(g,&p); fips_settle_gate_forget(g,&p);
    fips_settle_liveness h=heard(0,true,40500); CHECK(gate(g,ask(FIPS_SETTLE_SOURCE_REDIAL,&h,2),41000)==FIPS_SETTLE_GATE_REDIALED);
}
static void transactional(fips_settle_gates *g)
{
    g->capacity=1; promoted(g,1,0); fips_settle_memory before=g->slots[0];
    fips_settle_params p=fips_settle_params_default(); fips_settle_liveness h=heard(0,true,0);
    fips_settle_ask a=ask(FIPS_SETTLE_SOURCE_RESTART,&h,1); fips_settle_refusal r=FIPS_SETTLE_GATE_REDIALED;
    CHECK(fips_settle_gate(g,peer(3),&a,fips_test_instant(15000),&p,&r)==FIPS_OK); /* lapsed slot can be reused */
    before=g->slots[0];
    CHECK(fips_settle_gate(g,peer(4),&a,fips_test_instant(15001),&p,&r)==FIPS_ERR_RANGE);
    CHECK(!memcmp(&before,g->slots,sizeof before));
    p.replays=65; CHECK(fips_settle_gate(g,peer(4),&a,fips_test_instant(50000),&p,&r)==FIPS_ERR_RANGE);
    CHECK(!memcmp(&before,g->slots,sizeof before));
    p=fips_settle_params_default(); p.counted=0; bool started=true; fips_duration length={7,8};
    CHECK(fips_settle_gate_ended(g,peer(4),NULL,NULL,fips_test_instant(50000),NULL,&p,&started,&length)==FIPS_ERR_RANGE);
    CHECK(started && length.secs==7 && length.nanos==8 && !memcmp(&before,g->slots,sizeof before));
}
void fips_test_settle_gates(void)
{
    fips_settle_memory *slots=calloc(128,sizeof(*slots)); CHECK(slots); fips_settle_gates g;
#define RUN(name) do { CHECK(fips_settle_gates_init(&g,slots,128)==FIPS_OK); name(&g); } while (0)
    RUN(the_unheard_floor_refuses_until_one_second_and_accepts_at_it);
    RUN(the_heard_floor_counts_from_the_last_frame);
    RUN(the_heard_floor_counts_from_installation_when_no_frame_has_arrived);
    RUN(the_dampener_refuses_until_its_ceiling_after_the_last_accepted_restart);
    RUN(an_own_msg2_restart_goes_through_the_gate_and_is_accepted_inside_its_floors);
    RUN(an_own_msg2_restart_does_not_stamp_the_dampener);
    RUN(the_replay_memory_refuses_an_ephemeral_that_already_promoted_an_unheard_peering);
    RUN(the_replay_memory_holds_at_most_eight);
    RUN(a_frame_forgets_the_replay_memory_and_the_silent_sessions);
    RUN(silent_sessions_start_a_refusal_at_the_third_in_a_row);
    RUN(each_further_silent_session_doubles_the_refusal_up_to_its_cap);
    RUN(gate_memory_holds_no_entry_once_an_identitys_state_has_lapsed);
    RUN(the_refused_count_goes_with_its_silent_session_record);
    RUN(a_redial_is_not_held_by_the_heard_floor);
    RUN(the_redial_memory_refuses_a_seen_ephemeral_and_a_full_memory_until_it_ages_out);
    RUN(the_swap_stamps_the_dampener_only_for_a_heard_peering_and_clears_the_redial_memory);
    RUN(the_redial_memory_outlives_a_frame_and_the_lapse_of_the_rest_of_the_identitys_memory);
    RUN(the_redial_memory_outlives_the_first_frame_of_a_heard_session_and_a_move);
    RUN(transactional);
#undef RUN
    free(slots); puts("settle gates: 18 upstream tests and transactional boundary checks passed");
}
