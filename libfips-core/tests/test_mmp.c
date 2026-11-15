#include <fips/proto/mmp/mod.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { ++assertions; if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static unsigned assertions;
static unsigned groups;
#define RUN(f) do { f(); ++groups; puts("PASS " #f); } while (0)
static uint64_t bits(double d) { uint64_t u; memcpy(&u, &d, sizeof(u)); return u; }
static fips_node_addr addr(uint8_t n) { fips_node_addr a; memset(&a, n, sizeof(a)); return a; }
static void rx_init(fips_mmp_receiver_state *s, fips_mmp_owd_sample *storage)
{ CHECK(fips_mmp_receiver_init(s, 32, storage, 32)==FIPS_OK); }
static fips_mmp_receiver_report rr(uint64_t highest, uint64_t packets, uint64_t bytes, uint32_t echo, uint16_t dwell, uint32_t jitter)
{
    return (fips_mmp_receiver_report){.highest_counter=highest, .cumulative_packets_recv=packets,
        .cumulative_bytes_recv=bytes, .timestamp_echo=echo, .dwell_time=dwell, .jitter=jitter};
}
static fips_mmp_rr_log process(fips_mmp_metrics *s, fips_mmp_receiver_report report, uint32_t ts, uint64_t now)
{ fips_mmp_rr_log log; fips_mmp_metrics_process_receiver_report(s, &report, ts, now, &log); return log; }
static void check_same_report(const fips_mmp_receiver_report *a, const fips_mmp_receiver_report *b)
{
    uint8_t x[68], y[68]; fips_writer w;
    fips_writer_init(&w, x, sizeof(x)); CHECK(fips_mmp_receiver_report_encode(a, &w, NULL)==FIPS_OK);
    fips_writer_init(&w, y, sizeof(y)); CHECK(fips_mmp_receiver_report_encode(b, &w, NULL)==FIPS_OK);
    CHECK(!memcmp(x,y,sizeof(x)));
}

/* algorithms.rs: all 15 cases; boundary assertions strengthen the upstream ranges. */
static void algorithms(void)
{
    fips_mmp_jitter_estimator j={0}; fips_mmp_jitter_update(&j,0); CHECK(fips_mmp_jitter_us(&j)==0);
    for (unsigned i=0;i<200;++i) fips_mmp_jitter_update(&j,1000);
    CHECK(fips_mmp_jitter_us(&j)==1000);
    j=(fips_mmp_jitter_estimator){0}; fips_mmp_jitter_update(&j,INT32_MIN); CHECK(fips_mmp_jitter_us(&j)==134217728);
    fips_mmp_srtt_estimator s={0}; fips_mmp_srtt_update(&s,10000);
    CHECK(s.initialized && s.srtt_us==10000 && s.rttvar_us==5000);
    s=(fips_mmp_srtt_estimator){0}; for (unsigned i=0;i<100;++i) fips_mmp_srtt_update(&s,50000);
    CHECK(s.srtt_us==50000);
    fips_mmp_dual_ewma e={0}; CHECK(!e.initialized); fips_mmp_dual_ewma_update(&e,100);
    CHECK(e.initialized && e.short_avg==100 && e.long_avg==100);
    e=(fips_mmp_dual_ewma){0}; fips_mmp_dual_ewma_update(&e,0);
    for (unsigned i=0;i<20;++i) fips_mmp_dual_ewma_update(&e,100);
    CHECK(e.short_avg>e.long_avg);
    fips_mmp_owd_sample storage[32]; fips_mmp_owd_trend_detector o;
    CHECK(fips_mmp_owd_init(&o,32,storage,32)==FIPS_OK);
    fips_mmp_owd_push(&o,0,5000); CHECK(fips_mmp_owd_trend_us_per_sec(&o)==0);
    for (uint32_t i=1;i<20;++i) fips_mmp_owd_push(&o,i,5000);
    CHECK(fips_mmp_owd_trend_us_per_sec(&o)==0);
    fips_mmp_owd_clear(&o);
    for (uint32_t i=0;i<20;++i) fips_mmp_owd_push(&o,i,5000+(int64_t)i*100);
    CHECK(fips_mmp_owd_trend_us_per_sec(&o)==100000);
    CHECK(fips_mmp_compute_etx(1,1)==1);
    CHECK(bits(fips_mmp_compute_etx(.9,.95))==UINT64_C(0x3ff2b6a4ada92b6a));
    CHECK(fips_mmp_compute_etx(0,1)==100 && fips_mmp_compute_etx(1,0)==100);
    CHECK(fips_mmp_quality_index(1,0)==1 && fips_mmp_quality_index(1,100)==2 && fips_mmp_quality_index(2,50)==3);
    CHECK(fips_mmp_quality_index(1,10)<fips_mmp_quality_index(1,20));
    CHECK(fips_mmp_quality_index(1,10)<fips_mmp_quality_index(1.5,10));
    fips_mmp_spin_bit_state init, resp; uint64_t rtt=999;
    fips_mmp_spin_init(&init,true); fips_mmp_spin_init(&resp,false);
    CHECK(!init.current_value && !resp.current_value);
    CHECK(!fips_mmp_spin_rx_observe(&resp,init.current_value,1,0,&rtt));
    CHECK(!fips_mmp_spin_rx_observe(&init,resp.current_value,2,10,&rtt));
    CHECK(init.current_value && rtt==999);
    CHECK(!fips_mmp_spin_rx_observe(&resp,init.current_value,3,10,&rtt)); CHECK(resp.current_value);
    CHECK(fips_mmp_spin_rx_observe(&init,resp.current_value,4,20,&rtt)); CHECK(rtt==10);
    fips_mmp_spin_init(&resp,false); CHECK(!fips_mmp_spin_rx_observe(&resp,true,5,0,&rtt));
    CHECK(!fips_mmp_spin_rx_observe(&resp,false,3,0,&rtt)); CHECK(resp.current_value);
    CHECK(!fips_mmp_spin_rx_observe(&resp,false,5,0,&rtt)); CHECK(resp.current_value);
}

/* state.rs: spelling/serde names become exact C parse/name pairs. */
static void modes_and_sender(void)
{
    CHECK(FIPS_MMP_Full==0 && FIPS_MMP_DEFAULT_LOG_INTERVAL_SECS==30 && FIPS_MMP_DEFAULT_OWD_WINDOW_SIZE==32);
    const char *names[]={"full","lightweight","minimal"};
    for (unsigned i=0;i<3;++i) { fips_mmp_mode mode=FIPS_MMP_Full; CHECK(!strcmp(fips_mmp_mode_name((fips_mmp_mode)i),names[i]));
        CHECK(fips_mmp_mode_parse(names[i],&mode)); CHECK(mode==(fips_mmp_mode)i); }
    fips_mmp_mode mode=FIPS_MMP_Minimal; CHECK(!fips_mmp_mode_parse("Full",&mode)); CHECK(mode==FIPS_MMP_Minimal);
    fips_mmp_sender_state s; fips_mmp_sender_report report;
    fips_mmp_sender_init(&s); CHECK(s.cumulative_packets_sent==0 && s.cumulative_bytes_sent==0);
    CHECK(!fips_mmp_sender_build_report(&s,0,&report)); CHECK(!fips_mmp_sender_should_send_report(&s,0));
    fips_mmp_sender_record_sent(&s,1,100,500); fips_mmp_sender_record_sent(&s,2,200,600);
    CHECK(s.cumulative_packets_sent==2 && s.cumulative_bytes_sent==1100);
    fips_mmp_sender_init(&s);
    fips_mmp_sender_record_sent(&s,10,1000,500); fips_mmp_sender_record_sent(&s,11,1100,600); fips_mmp_sender_record_sent(&s,12,1200,400);
    CHECK(fips_mmp_sender_should_send_report(&s,0)); CHECK(fips_mmp_sender_build_report(&s,0,&report));
    CHECK(report.interval_start_counter==10 && report.interval_end_counter==12 && report.interval_start_timestamp==1000 && report.interval_end_timestamp==1200);
    CHECK(report.interval_bytes_sent==1500 && report.cumulative_packets_sent==3 && report.cumulative_bytes_sent==1500);
    CHECK(!fips_mmp_sender_build_report(&s,0,&report));
    fips_mmp_sender_init(&s); fips_mmp_sender_record_sent(&s,1,100,500); CHECK(fips_mmp_sender_build_report(&s,0,&report));
    fips_mmp_sender_record_sent(&s,2,200,300); CHECK(!fips_mmp_sender_should_send_report(&s,0));
    CHECK(fips_mmp_sender_should_send_report(&s,201)); CHECK(fips_mmp_sender_build_report(&s,201,&report));
    CHECK(report.interval_start_counter==2 && report.interval_bytes_sent==300 && report.cumulative_packets_sent==2 && report.cumulative_bytes_sent==800);
    fips_mmp_sender_init(&s); fips_mmp_sender_update_report_interval_from_srtt(&s,50000); CHECK(s.report_interval_ms==200);
    fips_mmp_sender_update_report_interval_from_srtt(&s,500000); CHECK(s.report_interval_ms==1000);
    fips_mmp_sender_init(&s);
    for (unsigned i=0;i<5;++i) fips_mmp_sender_update_report_interval_from_srtt(&s,500000);
    fips_mmp_sender_update_report_interval_from_srtt(&s,50000); CHECK(s.report_interval_ms==1000);
    fips_mmp_sender_update_report_interval_from_srtt(&s,3000000); CHECK(s.report_interval_ms==5000);
    fips_mmp_sender_update_report_interval_with_bounds(&s,0,500,10000); CHECK(s.report_interval_ms==5000);
    fips_mmp_sender_update_report_interval_with_bounds(&s,INT64_MAX,500,10000); CHECK(s.report_interval_ms==10000);
    fips_mmp_sender_init(&s); CHECK(fips_mmp_sender_send_failure_backoff_multiplier(&s)==1 && s.consecutive_send_failures==0);
    for (unsigned i=1;i<=7;++i) { CHECK(fips_mmp_sender_record_send_failure(&s)==i);
        CHECK(fips_mmp_sender_send_failure_backoff_multiplier(&s)==(double)(1u<<(i<5 ? i : 5))); }
    CHECK(fips_mmp_sender_record_send_success(&s)==7); CHECK(fips_mmp_sender_record_send_success(&s)==0);
    for (unsigned i=0;i<3;++i) fips_mmp_sender_record_send_failure(&s);
    CHECK(fips_mmp_sender_send_failure_backoff_multiplier(&s)==8); CHECK(fips_mmp_sender_record_send_success(&s)==3);
    CHECK(fips_mmp_sender_send_failure_backoff_multiplier(&s)==1);
    fips_mmp_sender_record_sent(&s,1,100,500); CHECK(fips_mmp_sender_build_report(&s,0,&report));
    CHECK(fips_mmp_sender_record_send_failure(&s)==1); fips_mmp_sender_record_sent(&s,2,200,500);
    CHECK(!fips_mmp_sender_should_send_report(&s,201)); CHECK(fips_mmp_sender_should_send_report(&s,401));
    s.interval_bytes_sent=UINT32_MAX-1; fips_mmp_sender_record_sent(&s,3,300,500); CHECK(s.interval_bytes_sent==UINT32_MAX);
}

static void receiver_cycles_and_gaps(void)
{
    fips_mmp_owd_sample storage[32]; fips_mmp_receiver_state s; fips_mmp_receiver_report r;
    rx_init(&s,storage); CHECK(s.cumulative_packets_recv==0 && s.cumulative_bytes_recv==0 && s.highest_counter==0);
    CHECK(!fips_mmp_receiver_build_report(&s,0,&r)); CHECK(!fips_mmp_receiver_should_send_report(&s,0));
    fips_mmp_receiver_record_recv(&s,1,100,500,false,0); CHECK(fips_mmp_receiver_should_send_report(&s,0));
    fips_mmp_receiver_record_recv(&s,2,200,600,false,100);
    CHECK(s.cumulative_packets_recv==2 && s.cumulative_bytes_recv==1100 && s.highest_counter==2);
    CHECK(fips_mmp_receiver_build_report(&s,150,&r));
    CHECK(r.highest_counter==2 && r.cumulative_packets_recv==2 && r.cumulative_bytes_recv==1100 && r.timestamp_echo==200 && r.dwell_time==50);
    CHECK(r.interval_packets_recv==2 && r.interval_bytes_recv==1100);
    CHECK(!fips_mmp_receiver_build_report(&s,150,&r));
    fips_mmp_receiver_record_recv(&s,3,300,300,false,200); CHECK(!fips_mmp_receiver_should_send_report(&s,200));
    CHECK(fips_mmp_receiver_should_send_report(&s,351)); CHECK(fips_mmp_receiver_build_report(&s,351,&r));
    CHECK(r.interval_packets_recv==1 && r.interval_bytes_recv==300 && r.cumulative_packets_recv==3);
    rx_init(&s,storage); fips_mmp_receiver_record_recv(&s,5,500,100,false,0); fips_mmp_receiver_record_recv(&s,3,300,100,false,10);
    CHECK(fips_mmp_receiver_build_report(&s,20,&r)); CHECK(r.cumulative_reorder_count==1 && s.highest_counter==5);
    rx_init(&s,storage); for (unsigned i=1;i<=3;++i) fips_mmp_receiver_record_recv(&s,i,i*100,100,i!=2,0);
    CHECK(s.ecn_ce_count==2); CHECK(fips_mmp_receiver_build_report(&s,0,&r));
    CHECK(r.burst_loss_count==0 && r.max_burst_loss==0 && r.mean_burst_loss==0);
    const uint64_t counters[]={1,4,5,8,9};
    for (unsigned bursts=1;bursts<=2;++bursts) {
        rx_init(&s,storage); for (unsigned i=0;i<2*bursts+1;++i) fips_mmp_receiver_record_recv(&s,counters[i],0,100,false,0);
        CHECK(fips_mmp_receiver_build_report(&s,0,&r)); CHECK(r.burst_loss_count==bursts && r.max_burst_loss==2 && r.mean_burst_loss==512);
    }
    rx_init(&s,storage); fips_mmp_receiver_record_recv(&s,1,100,500,false,0);
    CHECK(fips_mmp_receiver_build_report(&s,65536,&r)); CHECK(r.timestamp_echo==0 && r.dwell_time==UINT16_MAX && r.cumulative_packets_recv==1);
    rx_init(&s,storage); fips_mmp_receiver_update_report_interval_from_srtt(&s,50000); CHECK(s.report_interval_ms==200);
    fips_mmp_receiver_update_report_interval_from_srtt(&s,500000); CHECK(s.report_interval_ms==500);
    rx_init(&s,storage); for (unsigned i=0;i<5;++i) fips_mmp_receiver_update_report_interval_from_srtt(&s,500000);
    fips_mmp_receiver_update_report_interval_from_srtt(&s,50000); CHECK(s.report_interval_ms==1000);
    fips_mmp_receiver_update_report_interval_from_srtt(&s,3000000); CHECK(s.report_interval_ms==3000);
    fips_mmp_receiver_update_report_interval_with_bounds(&s,-1,500,10000); CHECK(s.report_interval_ms==3000);
    rx_init(&s,storage); fips_mmp_receiver_record_recv(&s,UINT64_MAX,0,100,false,0); fips_mmp_receiver_record_recv(&s,UINT64_MAX,0,100,false,0);
    CHECK(fips_mmp_receiver_build_report(&s,0,&r)); CHECK(r.burst_loss_count==0 && r.max_burst_loss==0);
    rx_init(&s,storage); fips_mmp_receiver_record_recv(&s,1,0,100,false,0); fips_mmp_receiver_record_recv(&s,UINT64_MAX,0,100,false,0);
    CHECK(fips_mmp_receiver_build_report(&s,0,&r)); CHECK(r.burst_loss_count>=1);
    fips_mmp_receiver_record_recv(&s,UINT64_MAX,0,100,false,0); CHECK(fips_mmp_receiver_build_report(&s,0,&r)); CHECK(r.burst_loss_count==0);
}

/* state.rs sampled/unsampled streams and OWD/reorder November fixes. */
static int32_t trend_after(fips_mmp_receiver_state *s, uint64_t frames, uint64_t t0, uint64_t spacing, uint32_t ts0, uint32_t send)
{
    uint64_t now=t0;
    for (uint64_t i=0;i<frames;++i) { now=t0+i*spacing; fips_mmp_receiver_record_recv(s,i+1,ts0+send*(uint32_t)i,100,false,now); }
    fips_mmp_receiver_report r; CHECK(fips_mmp_receiver_build_report(s,now,&r)); return r.owd_trend;
}
static void timing_owd_reorder(void)
{
    fips_mmp_owd_sample storage[32], other[32]; fips_mmp_receiver_state s, alone; fips_mmp_receiver_report a,b;
    rx_init(&s,storage); rx_init(&alone,other);
    const struct { uint64_t counter; uint32_t ts; uint64_t at; bool sampled; } mixed[]={
        {4,1000,1020,true},{1,700,1100,false},{5,1100,1125,true},{2,800,1205,false},
        {6,1200,1218,true},{7,1300,1330,true},{3,950,1352,false}};
    for (size_t i=0;i<7;++i) {
        fips_mmp_receiver_record_frame(&s,mixed[i].counter,mixed[i].ts,100,false,mixed[i].at,mixed[i].sampled);
        if (mixed[i].sampled) fips_mmp_receiver_record_recv(&alone,mixed[i].counter,mixed[i].ts,100,false,mixed[i].at);
    }
    CHECK(fips_mmp_receiver_build_report(&s,1400,&a)); CHECK(fips_mmp_receiver_build_report(&alone,1400,&b));
    CHECK(a.jitter!=0 && a.jitter==b.jitter && a.timestamp_echo==b.timestamp_echo && a.dwell_time==b.dwell_time && a.owd_trend==b.owd_trend);
    CHECK(s.last_recv_ms==alone.last_recv_ms);
    rx_init(&s,storage); const uint64_t at[]={10000,10120,10210,10330,10400};
    for (uint32_t i=0;i<5;++i) fips_mmp_receiver_record_frame(&s,i+1,1000+100*i,100,false,at[i],true);
    CHECK(fips_mmp_receiver_build_report(&s,10450,&a));
    CHECK(a.jitter==4626 && s.last_recv_ms==10400 && a.timestamp_echo==1400 && a.dwell_time==50 && a.owd_trend==16361);
    rx_init(&s,storage); rx_init(&alone,other);
    const uint64_t counters[]={1,2,5,3,6,9,10,7};
    for (unsigned i=0;i<8;++i) {
        bool ce=i==1 || i==4 || i==6;
        fips_mmp_receiver_record_frame(&s,counters[i],(uint32_t)counters[i]*10,300+100*i,ce,1000+10*i,true);
        fips_mmp_receiver_record_frame(&alone,counters[i],(uint32_t)counters[i]*10,300+100*i,ce,1000+10*i,i%2==0);
    }
    CHECK(fips_mmp_receiver_build_report(&s,2000,&a)); CHECK(fips_mmp_receiver_build_report(&alone,2000,&b));
    CHECK(a.cumulative_packets_recv==8 && a.cumulative_bytes_recv==5200 && a.interval_packets_recv==8 && a.interval_bytes_recv==5200);
    CHECK(a.highest_counter==10 && a.cumulative_reorder_count==2 && a.burst_loss_count==2 && a.max_burst_loss==2 && a.mean_burst_loss==512 && a.ecn_ce_count==3);
    a.timestamp_echo=b.timestamp_echo; a.dwell_time=b.dwell_time; a.jitter=b.jitter; a.owd_trend=b.owd_trend; check_same_report(&a,&b);
    const uint64_t spacings[]={10,100,1000,3000};
    for (unsigned i=0;i<4;++i) { rx_init(&s,storage); CHECK(trend_after(&s,40,50000,spacings[i],7,(uint32_t)spacings[i])==0); }
    rx_init(&s,storage); CHECK(trend_after(&s,40,50000,100,7,99)==10000);
    rx_init(&s,storage); CHECK(trend_after(&s,40,50000,100,7,100)==0);
    fips_mmp_receiver_reset_for_rekey(&s,60000); fips_mmp_receiver_record_recv(&s,900,5000000,100,false,61000);
    CHECK(trend_after(&s,20,76000,100,1000,100)==0);
    rx_init(&s,storage); CHECK(trend_after(&s,10,1000,100,7,100)==0);
    CHECK(trend_after(&s,10,1000+(UINT64_C(1)<<31),100,7+(UINT32_C(1)<<31),100)==0);
    fips_mmp_owd_trend_detector d; fips_mmp_owd_sample huge[FIPS_MMP_MAX_OWD_WINDOW_SIZE];
    CHECK(fips_mmp_owd_init(&d,SIZE_MAX,huge,FIPS_MMP_MAX_OWD_WINDOW_SIZE)==FIPS_OK);
    CHECK(d.capacity==4096); fips_mmp_owd_push(&d,0,0); fips_mmp_owd_push(&d,1,1000); CHECK(fips_mmp_owd_trend_us_per_sec(&d)==1000000);
    CHECK(fips_mmp_owd_init(&d,0,storage,32)==FIPS_OK); CHECK(d.capacity==2);
    fips_mmp_owd_push(&d,0,0); fips_mmp_owd_push(&d,1,1000); fips_mmp_owd_push(&d,2,0); CHECK(fips_mmp_owd_trend_us_per_sec(&d)==-1000000);
    fips_mmp_owd_clear(&d); fips_mmp_owd_push(&d,1,INT64_MAX); fips_mmp_owd_push(&d,1,INT64_MIN); CHECK(fips_mmp_owd_trend_us_per_sec(&d)==0);
    fips_mmp_owd_clear(&d); fips_mmp_owd_push(&d,0,INT64_MIN); fips_mmp_owd_push(&d,1,INT64_MAX); CHECK(fips_mmp_owd_trend_us_per_sec(&d)==INT32_MAX);
    fips_mmp_owd_clear(&d); fips_mmp_owd_push(&d,0,INT64_MAX); fips_mmp_owd_push(&d,1,INT64_MIN); CHECK(fips_mmp_owd_trend_us_per_sec(&d)==INT32_MIN);
    fips_mmp_metrics m; fips_mmp_metrics_init(&m); rx_init(&s,storage);
    for (uint32_t c=1;c<=5;++c) fips_mmp_receiver_record_recv(&s,c,c*10,100,false,1000+c*10);
    s.cumulative_reorder_count=(uint64_t)UINT32_MAX-1;
    CHECK(fips_mmp_receiver_build_report(&s,2000,&a)); CHECK(process(&m,a,100000,2000).kind!=FIPS_MMP_RrStale);
    for (uint32_t c=6;c<=10;++c) fips_mmp_receiver_record_recv(&s,c,c*10,100,false,1000+c*10);
    s.cumulative_reorder_count=(uint64_t)UINT32_MAX+5;
    CHECK(fips_mmp_receiver_build_report(&s,3000,&b)); CHECK(b.cumulative_reorder_count==UINT32_MAX);
    CHECK(b.cumulative_packets_recv>a.cumulative_packets_recv); CHECK(process(&m,b,100000,3000).kind!=FIPS_MMP_RrStale);
    /* Grace suppresses jitter, including an old-session timestamp straggler. */
    rx_init(&s,storage); fips_mmp_receiver_record_recv(&s,1,1000,100,false,0); fips_mmp_receiver_record_recv(&s,2,2000,100,false,1000);
    CHECK(fips_mmp_jitter_us(&s.jitter)==0); fips_mmp_receiver_reset_for_rekey(&s,2000);
    fips_mmp_receiver_record_recv(&s,0,120000,100,false,3000); fips_mmp_receiver_record_recv(&s,1,100,100,false,4000);
    CHECK(fips_mmp_jitter_us(&s.jitter)==0);
    fips_mmp_receiver_record_recv(&s,2,200,100,false,18000); fips_mmp_receiver_record_recv(&s,3,300,100,false,18100);
    CHECK(fips_mmp_jitter_us(&s.jitter)<1000000);
}

static void metrics_cycles(void)
{
    fips_mmp_metrics m; fips_mmp_metrics_init(&m); fips_mmp_rr_log log;
    double value=999; CHECK(!fips_mmp_metrics_srtt_ms(&m,&value)); CHECK(value==999);
    CHECK(!fips_mmp_metrics_smoothed_etx(&m,&value)); CHECK(!fips_mmp_metrics_smoothed_loss(&m,&value)); CHECK(!fips_mmp_metrics_quality_index(&m,&value));
    fips_mmp_receiver_report a=rr(10,10,5000,1000,5,0), b=rr(20,18,14000,1100,5,0);
    CHECK(fips_mmp_metrics_process_receiver_report(&m,&a,1050,0,&log)); CHECK(log.kind==FIPS_MMP_RrRttSample && log.rtt_ms==45 && log.srtt_ms==0);
    CHECK(fips_mmp_metrics_srtt_ms(&m,&value) && value==45);
    CHECK(m.reports_seen==1 && m.rtt_samples==1 && m.has_last_rtt && m.last_rtt_ms==45 && m.rtt_zero==0 && m.rtt_arith_fail==0);
    CHECK(!fips_mmp_metrics_process_receiver_report(&m,&b,1150,1000,&log));
    CHECK(fips_mmp_metrics_loss_rate(&m)>0 && m.goodput_bps==9000);
    fips_mmp_metrics saved=m;
    CHECK(process(&m,b,6000,5000).kind==FIPS_MMP_RrStale); CHECK(m.srtt.srtt_us==saved.srtt.srtt_us && m.goodput_bps==saved.goodput_bps);
    CHECK(m.delivery_ratio_forward==saved.delivery_ratio_forward && m.rtt_samples==saved.rtt_samples && m.reports_seen==saved.reports_seen+1);
    CHECK(process(&m,a,6000,6000).kind==FIPS_MMP_RrStale);
    /* Every cumulative field participates in the stale and duplicate guards. */
    for (unsigned i=0;i<5;++i) {
        fips_mmp_receiver_report current=rr(100,100,10000,1000,0,1); current.ecn_ce_count=5; current.cumulative_reorder_count=5;
        fips_mmp_metrics_init(&m); process(&m,current,1050,0);
        switch (i) { case 0: --current.highest_counter; break; case 1: --current.cumulative_packets_recv; break;
            case 2: --current.cumulative_bytes_recv; break; case 3: --current.ecn_ce_count; break; default: --current.cumulative_reorder_count; break; }
        CHECK(process(&m,current,6000,1000).kind==FIPS_MMP_RrStale);
    }
    fips_mmp_metrics_init(&m); process(&m,rr(20,20,10000,1000,5,0),1050,0);
    CHECK(process(&m,rr(10,10,5000,1000,0,0),6000,5000).kind==FIPS_MMP_RrStale); CHECK(m.srtt.srtt_us==45000);
    fips_mmp_metrics_init(&m); CHECK(process(&m,rr(10,10,5000,UINT32_MAX-10,20,0),15,0).kind==FIPS_MMP_RrInvalidRtt);
    CHECK(!m.srtt.initialized && m.reports_seen==1 && m.rtt_arith_fail==1 && m.rtt_zero==0 && m.rtt_samples==0);
    fips_mmp_metrics_init(&m); process(&m,rr(10,10,5000,2000,5,0),1000,0); CHECK(!m.srtt.initialized && m.rtt_arith_fail==1);
    fips_mmp_metrics_init(&m); process(&m,a,1005,0); CHECK(m.rtt_zero==1 && m.rtt_samples==0 && m.rtt_arith_fail==0 && !m.has_last_rtt);
    fips_mmp_metrics_init(&m); process(&m,rr(10,10,5000,0,0,0),1000,0); CHECK(!m.srtt.initialized);
    fips_mmp_metrics_init(&m); process(&m,rr(100,100,50000,0,0,0),0,0);
    CHECK(m.goodput_bps==0); process(&m,rr(300,290,150000,0,0,0),0,1000);
    CHECK(fabs(fips_mmp_metrics_loss_rate(&m)-.05)<.01 && m.goodput_bps==100000);
    fips_mmp_metrics_init(&m); process(&m,rr(10,10,5000,0,0,100),0,0); process(&m,rr(20,20,10000,0,0,500),0,1000);
    CHECK(m.jitter_trend.initialized && m.jitter_trend.short_avg==200 && m.jitter_trend.long_avg==112.5);
    fips_mmp_metrics_init(&m); CHECK(m.etx==1); m.delivery_ratio_forward=.9; fips_mmp_metrics_update_reverse_delivery(&m,100,100); CHECK(m.etx==1);
    fips_mmp_metrics_update_reverse_delivery(&m,290,300); CHECK(m.etx>1 && m.etx<2);
    fips_mmp_metrics_init(&m); fips_mmp_metrics_update_reverse_delivery(&m,100,100); CHECK(m.delivery_ratio_reverse==1);
    fips_mmp_metrics_update_reverse_delivery(&m,300,300); CHECK(m.delivery_ratio_reverse==1);
    fips_mmp_metrics_update_reverse_delivery(&m,350,400); CHECK(m.delivery_ratio_reverse==.5);
    fips_mmp_metrics_reset_for_rekey(&m); fips_mmp_metrics_update_reverse_delivery(&m,50,50); CHECK(m.delivery_ratio_reverse==.5);
    fips_mmp_metrics_update_reverse_delivery(&m,90,100); CHECK(m.delivery_ratio_reverse==.8);
    /* Old-session report poisoning is intentionally caller-filtered upstream. */
    for (unsigned old=0;old<2;++old) {
        fips_mmp_metrics_init(&m); process(&m,rr(1000,1000,100000,1000,0,0),1050,0); fips_mmp_metrics_reset_for_rekey(&m);
        CHECK(!m.has_prev_rr); if (old) process(&m,rr(1200,1200,120000,1100,0,0),1150,1000);
        process(&m,rr(10,10,1000,1200,0,0),1250,2000); CHECK(m.prev_rr_highest_counter==(old ? 1200u : 10u));
    }
}

/* etx_trend.rs: reverse refresh never steps the trend; stale RR never steps it.
 * The independent python reference also pins a multi-report smoothing table. */
static void etx_once_per_report(void)
{
    fips_mmp_metrics m; fips_mmp_metrics_init(&m);
    fips_mmp_metrics_update_reverse_delivery(&m,100,100);
    fips_mmp_metrics_update_reverse_delivery(&m,150,200); CHECK(m.delivery_ratio_reverse==.5);
    fips_mmp_metrics_update_reverse_delivery(&m,240,300); CHECK(m.delivery_ratio_reverse==.9); CHECK(!m.etx_trend.initialized);
    fips_mmp_metrics_init(&m); fips_mmp_metrics_update_reverse_delivery(&m,100,100);
    process(&m,rr(100,100,10000,0,0,0),0,1000); process(&m,rr(200,180,18000,0,0,0),0,2000);
    CHECK(m.etx_trend.initialized); fips_mmp_dual_ewma saved=m.etx_trend;
    CHECK(process(&m,rr(200,180,18000,0,0,0),0,3000).kind==FIPS_MMP_RrStale);
    fips_mmp_metrics_update_reverse_delivery(&m,150,200); CHECK(m.delivery_ratio_reverse<1);
    CHECK(bits(m.etx_trend.short_avg)==bits(saved.short_avg) && bits(m.etx_trend.long_avg)==bits(saved.long_avg));
    fips_mmp_metrics_init(&m); fips_mmp_metrics_update_reverse_delivery(&m,100,100);
    process(&m,rr(100,100,10000,0,0,0),0,1000); fips_mmp_metrics_update_reverse_delivery(&m,190,200);
    double reverse=m.delivery_ratio_reverse;
    process(&m,rr(200,180,18000,0,0,0),0,2000); fips_mmp_metrics_update_reverse_delivery(&m,290,300);
    double expected=fips_mmp_compute_etx(.8,reverse);
    CHECK(m.etx_trend.initialized && bits(m.etx_trend.short_avg)==bits(expected) && bits(m.etx_trend.long_avg)==bits(expected));
}

/* core.rs: exhaustive predicates cover its 29 named decision tests, plus
 * explicit multi-peer ordering and C append/capacity contracts. */
static void decisions(void)
{
    fips_mmp_action items[16]; fips_mmp_action_list out={items,0,16};
    CHECK(fips_mmp_plan_heartbeats(NULL,0,&out)==FIPS_OK && out.len==0);
    CHECK(fips_mmp_plan_link_reports(NULL,0,&out)==FIPS_OK && out.len==0);
    CHECK(fips_mmp_plan_session_reports(NULL,0,&out)==FIPS_OK && out.len==0);
    for (unsigned flags=0;flags<8;++flags) {
        fips_mmp_peer_liveness_snapshot p={addr(17),(flags&1)!=0,(flags&2)!=0,(flags&4)!=0};
        out.len=0; CHECK(fips_mmp_plan_heartbeats(&p,1,&out)==FIPS_OK);
        bool reap=p.time_dead && !p.rekey_active;
        CHECK(out.len==(size_t)(reap || p.heartbeat_due));
        if (out.len) CHECK(items[0].kind==(reap ? FIPS_MMP_ReapPeer : FIPS_MMP_Heartbeat));
    }
    fips_mmp_peer_liveness_snapshot peers[]={{addr(1),false,false,true},{addr(2),true,false,false},{addr(3),false,false,true},{addr(4),true,false,false}};
    out.len=0; CHECK(fips_mmp_plan_heartbeats(peers,4,&out)==FIPS_OK && out.len==4);
    CHECK(items[0].kind==FIPS_MMP_ReapPeer && !memcmp(&items[0].peer,&peers[1].peer,sizeof(fips_node_addr)));
    CHECK(items[1].kind==FIPS_MMP_ReapPeer && !memcmp(&items[1].peer,&peers[3].peer,sizeof(fips_node_addr)));
    CHECK(items[2].kind==FIPS_MMP_Heartbeat && items[3].kind==FIPS_MMP_Heartbeat);
    for (unsigned mode=0;mode<3;++mode) for (unsigned flags=0;flags<32;++flags) {
        fips_mmp_link_report_snapshot p={addr(1),(fips_mmp_mode)mode,(flags&1)!=0,(flags&2)!=0,(flags&4)!=0,(flags&8)!=0,(flags&16)!=0};
        out.len=0; CHECK(fips_mmp_plan_link_reports(&p,1,&out)==FIPS_OK);
        size_t at=0;
        if (p.log_due) { CHECK(items[at].kind==FIPS_MMP_LogLink); ++at; }
        if (mode==FIPS_MMP_Full && p.send_sr && p.sr_due) { CHECK(items[at].kind==FIPS_MMP_SendLinkReport && items[at].report.link==FIPS_MMP_LinkSender); ++at; }
        if (mode!=FIPS_MMP_Minimal && p.send_rr && p.rr_due) { CHECK(items[at].kind==FIPS_MMP_SendLinkReport && items[at].report.link==FIPS_MMP_LinkReceiver); ++at; }
        CHECK(out.len==at);
    }
    fips_mmp_link_report_snapshot links[]={{addr(1),FIPS_MMP_Full,true,true,true,true,true},{addr(2),FIPS_MMP_Full,true,true,true,true,true}};
    out.len=0; CHECK(fips_mmp_plan_link_reports(links,2,&out)==FIPS_OK && out.len==6);
    for (size_t i=0;i<6;++i) { CHECK(!memcmp(&items[i].peer,&links[i%2].peer,sizeof(fips_node_addr)));
        CHECK(items[i].kind==(i<2 ? FIPS_MMP_LogLink : FIPS_MMP_SendLinkReport));
        if (i>=2) CHECK(items[i].report.link==(i<4 ? FIPS_MMP_LinkSender : FIPS_MMP_LinkReceiver)); }
    for (unsigned mode=0;mode<3;++mode) for (unsigned flags=0;flags<16;++flags) {
        fips_mmp_session_report_snapshot s={addr(1),(fips_mmp_mode)mode,(flags&1)!=0,(flags&2)!=0,(flags&4)!=0,(flags&8)!=0};
        out.len=0; CHECK(fips_mmp_plan_session_reports(&s,1,&out)==FIPS_OK);
        size_t at=0;
        if (s.log_due) { CHECK(items[at].kind==FIPS_MMP_LogSession); ++at; }
        if (mode==FIPS_MMP_Full && s.sr_due) { CHECK(items[at].kind==FIPS_MMP_SendSessionReport && items[at].report.session==FIPS_MMP_SessionSender); ++at; }
        if (mode!=FIPS_MMP_Minimal && s.rr_due) { CHECK(items[at].kind==FIPS_MMP_SendSessionReport && items[at].report.session==FIPS_MMP_SessionReceiver); ++at; }
        if (s.mtu_due) { CHECK(items[at].kind==FIPS_MMP_SendSessionReport && items[at].report.session==FIPS_MMP_SessionPathMtu); ++at; }
        CHECK(out.len==at);
    }
    fips_mmp_session_report_snapshot sessions[]={{addr(1),FIPS_MMP_Full,true,true,true,true},{addr(2),FIPS_MMP_Full,true,true,true,true}};
    out.len=0; CHECK(fips_mmp_plan_session_reports(sessions,2,&out)==FIPS_OK && out.len==8);
    CHECK(items[0].kind==FIPS_MMP_LogSession && items[1].kind==FIPS_MMP_LogSession);
    for (size_t i=2;i<8;++i) { CHECK(items[i].kind==FIPS_MMP_SendSessionReport && items[i].report.session==(fips_mmp_session_report_kind)((i-2)%3));
        CHECK(!memcmp(&items[i].peer,&sessions[(i-2)/3].dest,sizeof(fips_node_addr))); }
    /* Refusals leave all storage (not just len) untouched. */
    memset(items,0xa5,sizeof(items)); fips_mmp_action copy[16]; memcpy(copy,items,sizeof(items));
    out.len=1; out.capacity=1;
    CHECK(fips_mmp_plan_heartbeats(peers,4,&out)==FIPS_ERR_RANGE && out.len==1 && !memcmp(items,copy,sizeof(items)));
    CHECK(fips_mmp_plan_link_reports(links,2,&out)==FIPS_ERR_RANGE && out.len==1 && !memcmp(items,copy,sizeof(items)));
    CHECK(fips_mmp_plan_session_reports(sessions,2,&out)==FIPS_ERR_RANGE && out.len==1 && !memcmp(items,copy,sizeof(items)));
    out.capacity=16; CHECK(fips_mmp_plan_session_reports(sessions,2,&out)==FIPS_OK && out.len==9); CHECK(!memcmp(items,copy,sizeof(*items)));
    fips_mmp_backoff_update updates[4]; fips_mmp_backoff_list backoff={updates,0,4};
    CHECK(fips_mmp_plan_backoff(NULL,0,&backoff)==FIPS_OK && backoff.len==0);
    for (unsigned flags=0;flags<4;++flags) {
        fips_mmp_send_result results[]={{addr(1),(flags&1)!=0},{addr(1),(flags&2)!=0}};
        backoff.len=0; CHECK(fips_mmp_plan_backoff(results,2,&backoff)==FIPS_OK && backoff.len==1);
        CHECK(updates[0].kind==(flags ? FIPS_MMP_Success : FIPS_MMP_Failure));
    }
    fips_mmp_send_result results[]={{addr(2),false},{addr(1),true},{addr(3),true},{addr(1),false},{addr(2),false}};
    backoff.len=0; CHECK(fips_mmp_plan_backoff(results,5,&backoff)==FIPS_OK && backoff.len==3);
    for (unsigned i=0;i<3;++i) { fips_node_addr a=addr((uint8_t)(i+1)); CHECK(!memcmp(&a,&updates[i].dest,sizeof(a)));
        CHECK(updates[i].kind==(i==1 ? FIPS_MMP_Failure : FIPS_MMP_Success)); }
    fips_mmp_backoff_update saved[4]; memcpy(saved,updates,sizeof(saved)); backoff.len=1; backoff.capacity=2;
    CHECK(fips_mmp_plan_backoff(results,5,&backoff)==FIPS_ERR_RANGE && backoff.len==1 && !memcmp(saved,updates,sizeof(saved)));
}

static void path_mtu_flow(void)
{
    fips_mmp_path_mtu_state s;
    const uint16_t below[]={0,1,137,138,200,255};
    for (unsigned i=0;i<6;++i) { fips_mmp_path_mtu_init(&s); fips_mmp_path_mtu_seed_source_mtu(&s,1400);
        CHECK(!fips_mmp_path_mtu_apply_notification(&s,below[i],1000)); CHECK(s.current_mtu==1400); }
    const uint16_t accepted[]={256,576,800};
    for (unsigned i=0;i<3;++i) { fips_mmp_path_mtu_init(&s); fips_mmp_path_mtu_seed_source_mtu(&s,1400);
        CHECK(fips_mmp_path_mtu_apply_notification(&s,accepted[i],1000)); CHECK(s.current_mtu==accepted[i]); }
    fips_mmp_path_mtu_init(&s); fips_mmp_path_mtu_seed_source_mtu(&s,1000);
    for (unsigned i=0;i<600;++i) CHECK(!fips_mmp_path_mtu_apply_notification(&s,1200,1000));
    CHECK(s.current_mtu==1000 && s.consecutive_increase_count==255);
    fips_mmp_path_mtu_init(&s); CHECK(fips_mmp_path_mtu_apply_notification(&s,800,100000));
    CHECK(!fips_mmp_path_mtu_apply_notification(&s,1400,100000)); CHECK(!fips_mmp_path_mtu_apply_notification(&s,1400,111000));
    CHECK(s.current_mtu==800); fips_mmp_path_mtu_reset_source_mtu(&s); CHECK(s.current_mtu==UINT16_MAX && s.pending_increase_mtu==0);
    CHECK(fips_mmp_path_mtu_apply_notification(&s,1000,120000));
    CHECK(!fips_mmp_path_mtu_apply_notification(&s,1400,130000)); CHECK(!fips_mmp_path_mtu_apply_notification(&s,1400,141000));
    CHECK(fips_mmp_path_mtu_apply_notification(&s,1400,151000)); CHECK(s.current_mtu==1400);
    fips_mmp_path_mtu_init(&s); fips_mmp_path_mtu_seed_source_mtu(&s,1200);
    CHECK(!fips_mmp_path_mtu_apply_notification(&s,1400,100000)); CHECK(!fips_mmp_path_mtu_apply_notification(&s,1400,110000));
    CHECK(!fips_mmp_path_mtu_apply_reactive_decrease(&s,1200)); CHECK(!fips_mmp_path_mtu_apply_reactive_decrease(&s,1300)); CHECK(!fips_mmp_path_mtu_apply_reactive_decrease(&s,1400));
    CHECK(s.current_mtu==1200); CHECK(fips_mmp_path_mtu_apply_notification(&s,1400,125000)); CHECK(s.current_mtu==1400);
    fips_mmp_path_mtu_init(&s); fips_mmp_path_mtu_seed_source_mtu(&s,1200);
    fips_mmp_path_mtu_apply_notification(&s,1400,100000); fips_mmp_path_mtu_apply_notification(&s,1400,110000);
    CHECK(!fips_mmp_path_mtu_apply_reactive_decrease(&s,255)); CHECK(s.current_mtu==1200);
    CHECK(fips_mmp_path_mtu_apply_reactive_decrease(&s,1000)); CHECK(s.current_mtu==1000);
    CHECK(!fips_mmp_path_mtu_apply_notification(&s,1400,125000));
    fips_mmp_path_mtu_apply_notification(&s,1300,130000); fips_mmp_path_mtu_apply_notification(&s,1300,141000);
    CHECK(fips_mmp_path_mtu_apply_notification(&s,1300,151000) && s.current_mtu==1300);
    for (unsigned reactive=0;reactive<2;++reactive) {
        fips_mmp_path_mtu_init(&s); fips_mmp_path_mtu_seed_source_mtu(&s,1200); CHECK(!fips_mmp_path_mtu_apply_notification(&s,1400,100000));
        CHECK(reactive ? fips_mmp_path_mtu_apply_reactive_decrease(&s,1000) : fips_mmp_path_mtu_apply_notification(&s,1000,105000));
        CHECK(s.current_mtu==1000); fips_mmp_path_mtu_apply_notification(&s,1400,110000); fips_mmp_path_mtu_apply_notification(&s,1400,120000);
        CHECK(fips_mmp_path_mtu_apply_notification(&s,1400,131000) && s.current_mtu==1400);
    }
    fips_mmp_path_mtu_init(&s); fips_mmp_path_mtu_seed_source_mtu(&s,1200);
    fips_mmp_path_mtu_apply_notification(&s,1400,100000); fips_mmp_path_mtu_apply_notification(&s,1400,110000);
    CHECK(fips_mmp_path_mtu_apply_notification(&s,1400,121000)); fips_mmp_path_mtu_seed_source_mtu(&s,1000); CHECK(s.current_mtu==1000);
    for (unsigned i=0;i<4;++i) fips_mmp_path_mtu_apply_notification(&s,1400,130000+i*11000);
    CHECK(s.current_mtu==1400);
    fips_mmp_path_mtu_init(&s); fips_mmp_path_mtu_seed_source_mtu(&s,1200); CHECK(!fips_mmp_path_mtu_apply_notification(&s,1400,100000));
    fips_mmp_path_mtu_seed_source_mtu(&s,1000); fips_mmp_path_mtu_apply_notification(&s,1400,110000);
    CHECK(fips_mmp_path_mtu_apply_notification(&s,1400,121000) && s.current_mtu==1400);
    /* Destination notifications compare only to last notified, not reverse. */
    uint16_t mtu=123;
    fips_mmp_path_mtu_init(&s); CHECK(!fips_mmp_path_mtu_should_send_notification(&s,0)); CHECK(!fips_mmp_path_mtu_build_notification(&s,0,&mtu) && mtu==123);
    fips_mmp_path_mtu_seed_source_mtu(&s,1200); fips_mmp_path_mtu_observe_incoming_mtu(&s,1500);
    CHECK(fips_mmp_path_mtu_should_send_notification(&s,0)); CHECK(fips_mmp_path_mtu_build_notification(&s,0,&mtu) && mtu==1500);
    fips_mmp_path_mtu_observe_incoming_mtu(&s,1300); CHECK(fips_mmp_path_mtu_should_send_notification(&s,1));
    fips_mmp_path_mtu_init(&s); fips_mmp_path_mtu_seed_source_mtu(&s,1400); fips_mmp_path_mtu_observe_incoming_mtu(&s,1000);
    CHECK(fips_mmp_path_mtu_build_notification(&s,0,&mtu) && mtu==1000); fips_mmp_path_mtu_observe_incoming_mtu(&s,1100);
    CHECK(!fips_mmp_path_mtu_should_send_notification(&s,1)); CHECK(fips_mmp_path_mtu_should_send_notification(&s,10000));
    fips_mmp_path_mtu_update_interval_from_srtt(&s,3000); CHECK(s.notification_interval_ms==15000);
    CHECK(!fips_mmp_path_mtu_should_send_notification(&s,14999)); CHECK(fips_mmp_path_mtu_should_send_notification(&s,15000));
    /* Source reset preserves reverse observations and their schedule. */
    fips_mmp_path_mtu_reset_source_mtu(&s); CHECK(s.last_observed_mtu==1100 && s.last_notified_mtu==1000 && s.has_last_notification);
}

/* wire.rs: all 20 tests, all truncation prefixes and undersized writers. */
static void wire_codecs(void)
{
    uint8_t data[80], again[80], snapshot[80]; fips_writer w; fips_reader r; fips_proto_error error;
    {
        fips_mmp_sender_report input={100,200,5000,6000,50000,10000,5000000}, decoded;
        for (unsigned round=0;round<4;++round) {
            if (round==1) input=(fips_mmp_sender_report){0};
            if (round==2) {
                input.interval_start_counter=UINT64_MAX;
                input.interval_end_counter=UINT64_MAX;
                input.interval_start_timestamp=UINT32_MAX;
                input.interval_end_timestamp=UINT32_MAX;
                input.interval_bytes_sent=UINT32_MAX;
                input.cumulative_packets_sent=UINT64_MAX;
                input.cumulative_bytes_sent=UINT64_MAX;
            }
            fips_writer_init(&w,data,sizeof(data)); CHECK(fips_mmp_sender_report_encode(&input,&w,&error)==FIPS_OK); CHECK(w.pos==48);
            fips_reader_init(&r,data+1,47); CHECK(fips_mmp_sender_report_decode(&r,&decoded,&error)==FIPS_OK); CHECK(r.pos==47);
            CHECK(input.interval_start_counter==decoded.interval_start_counter);
            CHECK(input.interval_end_counter==decoded.interval_end_counter);
            CHECK(input.interval_start_timestamp==decoded.interval_start_timestamp);
            CHECK(input.interval_end_timestamp==decoded.interval_end_timestamp);
            CHECK(input.interval_bytes_sent==decoded.interval_bytes_sent);
            CHECK(input.cumulative_packets_sent==decoded.cumulative_packets_sent);
            CHECK(input.cumulative_bytes_sent==decoded.cumulative_bytes_sent);
            CHECK(data[0]==1 && data[1]==0 && data[2]==0 && data[3]==0);
            fips_writer_init(&w,again,sizeof(again)); CHECK(fips_mmp_sender_report_encode(&decoded,&w,NULL)==FIPS_OK); CHECK(!memcmp(data,again,48));
            for (size_t len=0;len<47;++len) {
                fips_mmp_sender_report sentinel=decoded;
                fips_reader_init(&r,data+1,len); CHECK(fips_mmp_sender_report_decode(&r,&decoded,&error)==FIPS_ERR_RANGE);
                CHECK(r.pos==0 && !memcmp(&sentinel,&decoded,sizeof(decoded)));
                CHECK(error.variant==FIPS_PROTO_MessageTooShort && error.as.message_too_short.expected==47 && error.as.message_too_short.got==len);
            }
            for (size_t capacity=0;capacity<48;++capacity) {
                memset(again,0xa5,sizeof(again)); memcpy(snapshot,again,sizeof(snapshot));
                fips_writer_init(&w,again,capacity); CHECK(fips_mmp_sender_report_encode(&input,&w,&error)==FIPS_ERR_RANGE);
                CHECK(w.pos==0 && !memcmp(snapshot,again,sizeof(again)) && error.variant==FIPS_PROTO_MessageTooLong);
            }
        }
    }
    {
        fips_mmp_session_sender_report input={100,200,5000,6000,50000,10000,5000000}, decoded;
        for (unsigned round=0;round<4;++round) {
            if (round==1) input=(fips_mmp_session_sender_report){0};
            if (round==2) {
                input.interval_start_counter=UINT64_MAX;
                input.interval_end_counter=UINT64_MAX;
                input.interval_start_timestamp=UINT32_MAX;
                input.interval_end_timestamp=UINT32_MAX;
                input.interval_bytes_sent=UINT32_MAX;
                input.cumulative_packets_sent=UINT64_MAX;
                input.cumulative_bytes_sent=UINT64_MAX;
            }
            fips_writer_init(&w,data,sizeof(data)); CHECK(fips_mmp_session_sender_report_encode(&input,&w,&error)==FIPS_OK); CHECK(w.pos==46);
            fips_reader_init(&r,data+0,46); CHECK(fips_mmp_session_sender_report_decode(&r,&decoded,&error)==FIPS_OK); CHECK(r.pos==46);
            CHECK(input.interval_start_counter==decoded.interval_start_counter);
            CHECK(input.interval_end_counter==decoded.interval_end_counter);
            CHECK(input.interval_start_timestamp==decoded.interval_start_timestamp);
            CHECK(input.interval_end_timestamp==decoded.interval_end_timestamp);
            CHECK(input.interval_bytes_sent==decoded.interval_bytes_sent);
            CHECK(input.cumulative_packets_sent==decoded.cumulative_packets_sent);
            CHECK(input.cumulative_bytes_sent==decoded.cumulative_bytes_sent);
            CHECK(data[0]==0 && data[1]==0);
            fips_writer_init(&w,again,sizeof(again)); CHECK(fips_mmp_session_sender_report_encode(&decoded,&w,NULL)==FIPS_OK); CHECK(!memcmp(data,again,46));
            for (size_t len=0;len<46;++len) {
                fips_mmp_session_sender_report sentinel=decoded;
                fips_reader_init(&r,data+0,len); CHECK(fips_mmp_session_sender_report_decode(&r,&decoded,&error)==FIPS_ERR_RANGE);
                CHECK(r.pos==0 && !memcmp(&sentinel,&decoded,sizeof(decoded)));
                CHECK(error.variant==FIPS_PROTO_MessageTooShort && error.as.message_too_short.expected==46 && error.as.message_too_short.got==len);
            }
            for (size_t capacity=0;capacity<46;++capacity) {
                memset(again,0xa5,sizeof(again)); memcpy(snapshot,again,sizeof(snapshot));
                fips_writer_init(&w,again,capacity); CHECK(fips_mmp_session_sender_report_encode(&input,&w,&error)==FIPS_ERR_RANGE);
                CHECK(w.pos==0 && !memcmp(snapshot,again,sizeof(again)) && error.variant==FIPS_PROTO_MessageTooLong);
            }
        }
    }
    {
        fips_mmp_receiver_report input={195,9500,4750000,5900,5,3,384,1200,0,-50,2,10,95,47500}, decoded;
        for (unsigned round=0;round<4;++round) {
            if (round==1) input=(fips_mmp_receiver_report){0};
            if (round==2) {
                input.highest_counter=UINT64_MAX;
                input.cumulative_packets_recv=UINT64_MAX;
                input.cumulative_bytes_recv=UINT64_MAX;
                input.timestamp_echo=UINT32_MAX;
                input.dwell_time=UINT16_MAX;
                input.max_burst_loss=UINT16_MAX;
                input.mean_burst_loss=UINT16_MAX;
                input.jitter=UINT32_MAX;
                input.ecn_ce_count=UINT32_MAX;
                input.owd_trend=INT32_MAX;
                input.burst_loss_count=UINT32_MAX;
                input.cumulative_reorder_count=UINT32_MAX;
                input.interval_packets_recv=UINT32_MAX;
                input.interval_bytes_recv=UINT32_MAX;
            }
            if (round==3) input.owd_trend=-12345;
            fips_writer_init(&w,data,sizeof(data)); CHECK(fips_mmp_receiver_report_encode(&input,&w,&error)==FIPS_OK); CHECK(w.pos==68);
            fips_reader_init(&r,data+1,67); CHECK(fips_mmp_receiver_report_decode(&r,&decoded,&error)==FIPS_OK); CHECK(r.pos==67);
            CHECK(input.highest_counter==decoded.highest_counter);
            CHECK(input.cumulative_packets_recv==decoded.cumulative_packets_recv);
            CHECK(input.cumulative_bytes_recv==decoded.cumulative_bytes_recv);
            CHECK(input.timestamp_echo==decoded.timestamp_echo);
            CHECK(input.dwell_time==decoded.dwell_time);
            CHECK(input.max_burst_loss==decoded.max_burst_loss);
            CHECK(input.mean_burst_loss==decoded.mean_burst_loss);
            CHECK(input.jitter==decoded.jitter);
            CHECK(input.ecn_ce_count==decoded.ecn_ce_count);
            CHECK(input.owd_trend==decoded.owd_trend);
            CHECK(input.burst_loss_count==decoded.burst_loss_count);
            CHECK(input.cumulative_reorder_count==decoded.cumulative_reorder_count);
            CHECK(input.interval_packets_recv==decoded.interval_packets_recv);
            CHECK(input.interval_bytes_recv==decoded.interval_bytes_recv);
            CHECK(data[0]==2 && data[1]==0 && data[2]==0 && data[3]==0);
            fips_writer_init(&w,again,sizeof(again)); CHECK(fips_mmp_receiver_report_encode(&decoded,&w,NULL)==FIPS_OK); CHECK(!memcmp(data,again,68));
            for (size_t len=0;len<67;++len) {
                fips_mmp_receiver_report sentinel=decoded;
                fips_reader_init(&r,data+1,len); CHECK(fips_mmp_receiver_report_decode(&r,&decoded,&error)==FIPS_ERR_RANGE);
                CHECK(r.pos==0 && !memcmp(&sentinel,&decoded,sizeof(decoded)));
                CHECK(error.variant==FIPS_PROTO_MessageTooShort && error.as.message_too_short.expected==67 && error.as.message_too_short.got==len);
            }
            for (size_t capacity=0;capacity<68;++capacity) {
                memset(again,0xa5,sizeof(again)); memcpy(snapshot,again,sizeof(snapshot));
                fips_writer_init(&w,again,capacity); CHECK(fips_mmp_receiver_report_encode(&input,&w,&error)==FIPS_ERR_RANGE);
                CHECK(w.pos==0 && !memcmp(snapshot,again,sizeof(again)) && error.variant==FIPS_PROTO_MessageTooLong);
            }
        }
    }
    {
        fips_mmp_session_receiver_report input={195,9500,4750000,5900,5,3,384,1200,0,-50,2,10,95,47500}, decoded;
        for (unsigned round=0;round<4;++round) {
            if (round==1) input=(fips_mmp_session_receiver_report){0};
            if (round==2) {
                input.highest_counter=UINT64_MAX;
                input.cumulative_packets_recv=UINT64_MAX;
                input.cumulative_bytes_recv=UINT64_MAX;
                input.timestamp_echo=UINT32_MAX;
                input.dwell_time=UINT16_MAX;
                input.max_burst_loss=UINT16_MAX;
                input.mean_burst_loss=UINT16_MAX;
                input.jitter=UINT32_MAX;
                input.ecn_ce_count=UINT32_MAX;
                input.owd_trend=INT32_MAX;
                input.burst_loss_count=UINT32_MAX;
                input.cumulative_reorder_count=UINT32_MAX;
                input.interval_packets_recv=UINT32_MAX;
                input.interval_bytes_recv=UINT32_MAX;
            }
            if (round==3) input.owd_trend=-12345;
            fips_writer_init(&w,data,sizeof(data)); CHECK(fips_mmp_session_receiver_report_encode(&input,&w,&error)==FIPS_OK); CHECK(w.pos==66);
            fips_reader_init(&r,data+0,66); CHECK(fips_mmp_session_receiver_report_decode(&r,&decoded,&error)==FIPS_OK); CHECK(r.pos==66);
            CHECK(input.highest_counter==decoded.highest_counter);
            CHECK(input.cumulative_packets_recv==decoded.cumulative_packets_recv);
            CHECK(input.cumulative_bytes_recv==decoded.cumulative_bytes_recv);
            CHECK(input.timestamp_echo==decoded.timestamp_echo);
            CHECK(input.dwell_time==decoded.dwell_time);
            CHECK(input.max_burst_loss==decoded.max_burst_loss);
            CHECK(input.mean_burst_loss==decoded.mean_burst_loss);
            CHECK(input.jitter==decoded.jitter);
            CHECK(input.ecn_ce_count==decoded.ecn_ce_count);
            CHECK(input.owd_trend==decoded.owd_trend);
            CHECK(input.burst_loss_count==decoded.burst_loss_count);
            CHECK(input.cumulative_reorder_count==decoded.cumulative_reorder_count);
            CHECK(input.interval_packets_recv==decoded.interval_packets_recv);
            CHECK(input.interval_bytes_recv==decoded.interval_bytes_recv);
            CHECK(data[0]==0 && data[1]==0);
            fips_writer_init(&w,again,sizeof(again)); CHECK(fips_mmp_session_receiver_report_encode(&decoded,&w,NULL)==FIPS_OK); CHECK(!memcmp(data,again,66));
            for (size_t len=0;len<66;++len) {
                fips_mmp_session_receiver_report sentinel=decoded;
                fips_reader_init(&r,data+0,len); CHECK(fips_mmp_session_receiver_report_decode(&r,&decoded,&error)==FIPS_ERR_RANGE);
                CHECK(r.pos==0 && !memcmp(&sentinel,&decoded,sizeof(decoded)));
                CHECK(error.variant==FIPS_PROTO_MessageTooShort && error.as.message_too_short.expected==66 && error.as.message_too_short.got==len);
            }
            for (size_t capacity=0;capacity<66;++capacity) {
                memset(again,0xa5,sizeof(again)); memcpy(snapshot,again,sizeof(snapshot));
                fips_writer_init(&w,again,capacity); CHECK(fips_mmp_session_receiver_report_encode(&input,&w,&error)==FIPS_ERR_RANGE);
                CHECK(w.pos==0 && !memcmp(snapshot,again,sizeof(again)) && error.variant==FIPS_PROTO_MessageTooLong);
            }
        }
    }
    {
        fips_mmp_path_mtu_notification input={1400}, decoded;
        for (unsigned round=0;round<4;++round) {
            if (round==1) input=(fips_mmp_path_mtu_notification){0};
            if (round==2) {
                input.path_mtu=UINT16_MAX;
            }
            if (round==3) input.path_mtu=1280;
            fips_writer_init(&w,data,sizeof(data)); CHECK(fips_mmp_path_mtu_notification_encode(&input,&w,&error)==FIPS_OK); CHECK(w.pos==2);
            fips_reader_init(&r,data+0,2); CHECK(fips_mmp_path_mtu_notification_decode(&r,&decoded,&error)==FIPS_OK); CHECK(r.pos==2);
            CHECK(input.path_mtu==decoded.path_mtu);
            fips_writer_init(&w,again,sizeof(again)); CHECK(fips_mmp_path_mtu_notification_encode(&decoded,&w,NULL)==FIPS_OK); CHECK(!memcmp(data,again,2));
            for (size_t len=0;len<2;++len) {
                fips_mmp_path_mtu_notification sentinel=decoded;
                fips_reader_init(&r,data+0,len); CHECK(fips_mmp_path_mtu_notification_decode(&r,&decoded,&error)==FIPS_ERR_RANGE);
                CHECK(r.pos==0 && !memcmp(&sentinel,&decoded,sizeof(decoded)));
                CHECK(error.variant==FIPS_PROTO_MessageTooShort && error.as.message_too_short.expected==2 && error.as.message_too_short.got==len);
            }
            for (size_t capacity=0;capacity<2;++capacity) {
                memset(again,0xa5,sizeof(again)); memcpy(snapshot,again,sizeof(snapshot));
                fips_writer_init(&w,again,capacity); CHECK(fips_mmp_path_mtu_notification_encode(&input,&w,&error)==FIPS_ERR_RANGE);
                CHECK(w.pos==0 && !memcmp(snapshot,again,sizeof(again)) && error.variant==FIPS_PROTO_MessageTooLong);
            }
        }
        const uint16_t mtus[]={0,1280,1500,UINT16_MAX};
        for (unsigned i=0;i<4;++i) { input.path_mtu=mtus[i]; fips_writer_init(&w,data,sizeof(data)); CHECK(fips_mmp_path_mtu_notification_encode(&input,&w,NULL)==FIPS_OK); fips_reader_init(&r,data,2); CHECK(fips_mmp_path_mtu_notification_decode(&r,&decoded,NULL)==FIPS_OK && decoded.path_mtu==mtus[i]); }
    }
    fips_mmp_sender_report sr={100,200,5000,6000,50000,10000,5000000};
    fips_mmp_session_sender_report ss=fips_mmp_session_sender_report_from_sender_report(&sr);
    fips_mmp_sender_report sr2=fips_mmp_sender_report_from_session_sender_report(&ss);
    fips_writer_init(&w,data,sizeof(data)); CHECK(fips_mmp_sender_report_encode(&sr,&w,NULL)==FIPS_OK);
    fips_writer_init(&w,again,sizeof(again)); CHECK(fips_mmp_sender_report_encode(&sr2,&w,NULL)==FIPS_OK); CHECK(!memcmp(data,again,48));
    fips_mmp_receiver_report rr1=rr(100,90,9000,1000,5,1200);
    fips_mmp_session_receiver_report rs=fips_mmp_session_receiver_report_from_receiver_report(&rr1);
    fips_mmp_receiver_report rr2=fips_mmp_receiver_report_from_session_receiver_report(&rs); check_same_report(&rr1,&rr2);
    /* Reader positions are cumulative; reserved/trailing bytes are accepted. */
    memset(data,0xff,sizeof(data)); fips_reader_init(&r,data,sizeof(data)); r.pos=3;
    CHECK(fips_mmp_receiver_report_decode(&r,&rr2,NULL)==FIPS_OK && r.pos==70 && rr2.owd_trend==-1);
}

static uint64_t frames(fips_mmp_receiver_state *s, uint64_t first, uint64_t last, const uint64_t *lost, size_t lost_len, uint64_t late, uint64_t start)
{
    uint64_t t=start;
    for (uint64_t c=first;c<=last;++c) {
        bool skip=false; for (size_t i=0;i<lost_len;++i) if (lost[i]==c) skip=true;
        if (skip) continue;
        t+=10+(c%7)*3; fips_mmp_receiver_record_recv(s,c,(uint32_t)c*10,100,c%5==0,t);
    }
    if (late) { t+=5; fips_mmp_receiver_record_recv(s,late,(uint32_t)late*10,100,false,t); }
    return t;
}
static void old_session(fips_mmp_receiver_state *s)
{
    const uint64_t lost1[]={30,31,32}, lost2[]={52};
    uint64_t t=frames(s,1,40,lost1,3,31,1000); fips_mmp_receiver_report report; CHECK(fips_mmp_receiver_build_report(s,t+20,&report));
    frames(s,41,60,lost2,1,0,t+40);
}
static void judged_same(const fips_mmp_metrics *a, const fips_mmp_metrics *b)
{
    CHECK(fips_mmp_metrics_loss_rate(a)==fips_mmp_metrics_loss_rate(b)); CHECK(a->has_prev_rr==b->has_prev_rr);
    CHECK(a->prev_rr_highest_counter==b->prev_rr_highest_counter && a->prev_rr_cum_packets==b->prev_rr_cum_packets && a->prev_rr_reorder==b->prev_rr_reorder);
    CHECK(a->prev_rr_ecn_ce==b->prev_rr_ecn_ce);
}
static void kept_metrics(const fips_mmp_metrics *a, const fips_mmp_metrics *b)
{
    CHECK(a->srtt.srtt_us==b->srtt.srtt_us && a->srtt.rttvar_us==b->srtt.rttvar_us && a->srtt.initialized==b->srtt.initialized);
    CHECK(a->reports_seen==b->reports_seen && a->rtt_samples==b->rtt_samples && a->rtt_zero==b->rtt_zero && a->rtt_arith_fail==b->rtt_arith_fail);
    CHECK(a->last_rtt_ms==b->last_rtt_ms && a->has_last_rtt==b->has_last_rtt);
    CHECK(a->delivery_ratio_reverse==b->delivery_ratio_reverse && a->etx==b->etx && a->goodput_bps==b->goodput_bps);
    const fips_mmp_dual_ewma *x[]={&a->rtt_trend,&a->loss_trend,&a->goodput_trend,&a->jitter_trend,&a->etx_trend};
    const fips_mmp_dual_ewma *y[]={&b->rtt_trend,&b->loss_trend,&b->goodput_trend,&b->jitter_trend,&b->etx_trend};
    for (unsigned i=0;i<5;++i) CHECK(x[i]->initialized==y[i]->initialized && bits(x[i]->short_avg)==bits(y[i]->short_avg) && bits(x[i]->long_avg)==bits(y[i]->long_avg));
}

/* rekey_reset.rs's 8 cases run for both aggregate forms and both spin roles.
 * state.rs's two private fresh_diff/dirtied tests are replaced by explicit
 * complete field-carry assertions below (no test-only library API). */
static void reset_and_spin(void)
{
    for (unsigned session=0;session<2;++session) for (unsigned initiator=0;initiator<2;++initiator) {
        fips_mmp_session_state s,f; fips_mmp_peer_state *a=&s.roles, *b=&f.roles;
        fips_mmp_owd_sample x[32], y[32];
        if (session) { CHECK(fips_mmp_session_init(&s,FIPS_MMP_Full,30,32,initiator!=0,x,32)==FIPS_OK);
            CHECK(fips_mmp_session_init(&f,FIPS_MMP_Full,30,32,initiator!=0,y,32)==FIPS_OK); }
        else { CHECK(fips_mmp_peer_init(a,FIPS_MMP_Full,30,32,initiator!=0,x,32)==FIPS_OK);
            CHECK(fips_mmp_peer_init(b,FIPS_MMP_Full,30,32,initiator!=0,y,32)==FIPS_OK); }
        CHECK(a->sender.report_interval_ms==(session ? 1000u : 200u)); CHECK(a->receiver.report_interval_ms==a->sender.report_interval_ms);
        CHECK(fips_mmp_peer_should_log(a,0)); fips_mmp_peer_mark_logged(a,0); CHECK(!fips_mmp_peer_should_log(a,29999) && fips_mmp_peer_should_log(a,30000));
        old_session(&a->receiver); fips_mmp_receiver_update_report_interval_from_srtt(&a->receiver,300000);
        for (uint32_t c=1;c<=60;++c) fips_mmp_sender_record_sent(&a->sender,c,c*10,100);
        uint64_t packets=a->receiver.cumulative_packets_recv, bytes=a->receiver.cumulative_bytes_recv, interval=a->receiver.report_interval_ms;
        CHECK(packets==57 && a->receiver.highest_counter==60 && a->receiver.ecn_ce_count>0 && fips_mmp_jitter_us(&a->receiver.jitter)>0);
        process(&a->metrics,rr(20,20,2000,400,0,0),500,1000); fips_mmp_metrics_update_reverse_delivery(&a->metrics,20,20);
        fips_mmp_receiver_report old=rr(50,40,4000,1400,0,0); old.ecn_ce_count=3; old.cumulative_reorder_count=1;
        process(&a->metrics,old,1500,2000); fips_mmp_metrics_update_reverse_delivery(&a->metrics,35,50);
        fips_mmp_metrics kept=a->metrics;
        CHECK(fips_mmp_metrics_loss_rate(&kept)>0 && kept.delivery_ratio_reverse<1);
        uint64_t dummy=0;
        for (uint64_t c=1;c<=61;++c) { bool bit=a->spin_bit.current_value ^ !a->spin_bit.is_initiator;
            fips_mmp_spin_rx_observe(&a->spin_bit,bit,c,c*10,&dummy); }
        CHECK(a->spin_bit.current_value);
        if (session) { fips_mmp_path_mtu_seed_source_mtu(&s.path_mtu,1200); fips_mmp_session_mark_logged(&s,30000); }
        fips_mmp_sender_state sender=a->sender;
        if (session) fips_mmp_session_reset_for_rekey(&s,100000); else fips_mmp_peer_reset_for_rekey(a,100000);
        /* Whole sender is carried, including in-flight interval, unlike RX. */
        CHECK(!memcmp(&sender,&a->sender,sizeof(sender)));
        CHECK(a->receiver.cumulative_packets_recv==packets && a->receiver.cumulative_bytes_recv==bytes && a->receiver.report_interval_ms==interval && a->receiver.srtt_sample_count==1);
        CHECK(!a->receiver.has_last_recv && !a->receiver.has_owd_ref && a->receiver.owd_trend.len==0 && !a->receiver.interval_has_data);
        CHECK(a->receiver.highest_counter==0 && a->receiver.cumulative_reorder_count==0 && a->receiver.ecn_ce_count==0);
        CHECK(a->receiver.has_last_report && a->receiver.has_rekey_grace && a->receiver.rekey_jitter_grace_until_ms==115000);
        CHECK(!a->receiver.gap_tracker.has_expected && !a->receiver.gap_tracker.in_burst && a->receiver.gap_tracker.current_burst_len==0 &&
            a->receiver.gap_tracker.burst_count==0 && a->receiver.gap_tracker.max_burst_len==0 && a->receiver.gap_tracker.total_burst_len==0);
        CHECK(a->receiver.interval_bytes_recv==0 && a->receiver.interval_packets_recv==0 && a->receiver.last_sender_timestamp==0 && a->receiver.jitter.jitter_q4==0);
        CHECK(a->receiver.owd_trend.capacity==32 && a->receiver.owd_trend.samples==x);
        judged_same(&a->metrics,&b->metrics); kept_metrics(&a->metrics,&kept);
        CHECK(a->metrics.prev_rr_cum_bytes==0 && a->metrics.prev_rr_ms==0 && a->metrics.prev_reverse_packets==0 && a->metrics.prev_reverse_highest==0 && !a->metrics.has_prev_reverse);
        CHECK(!a->spin_bit.current_value && a->spin_bit.is_initiator==(initiator!=0) && !a->spin_bit.has_last_edge && a->spin_bit.highest_counter_for_spin==0);
        if (initiator) CHECK(!fips_mmp_spin_rx_observe(&a->spin_bit,false,1,100010,&dummy));
        else { CHECK(!fips_mmp_spin_rx_observe(&a->spin_bit,true,1,100010,&dummy)); CHECK(a->spin_bit.current_value); }
        if (session) CHECK(s.path_mtu.current_mtu==1200 && !fips_mmp_session_should_log(&s,30001));
        else CHECK(a->has_last_log && a->last_log_ms==0);
        fips_mmp_receiver_report report_a, report_b;
        CHECK(!fips_mmp_receiver_build_report(&a->receiver,100000,&report_a)); CHECK(!fips_mmp_receiver_build_report(&b->receiver,100000,&report_b));
        uint64_t start=160000;
        for (unsigned round=0;round<2;++round) {
            const uint64_t lost1[]={8,9},lost2[]={33};
            uint64_t t=frames(&a->receiver,round ? 21 : 1,round ? 40 : 20,round ? lost2 : lost1,round ? 1 : 2,round ? 0 : 9,start);
            frames(&b->receiver,round ? 21 : 1,round ? 40 : 20,round ? lost2 : lost1,round ? 1 : 2,round ? 0 : 9,start);
            CHECK(fips_mmp_receiver_build_report(&a->receiver,t+50,&report_a)); CHECK(fips_mmp_receiver_build_report(&b->receiver,t+50,&report_b));
            report_a.cumulative_packets_recv-=packets; report_a.cumulative_bytes_recv-=bytes; check_same_report(&report_a,&report_b); start=t+100;
        }
        fips_mmp_receiver_report fresh_rr=rr(10,8,800,0,0,0); fresh_rr.ecn_ce_count=1;
        process(&a->metrics,fresh_rr,0,160000); process(&b->metrics,fresh_rr,0,160000);
        fips_mmp_metrics_update_reverse_delivery(&a->metrics,43,10); fips_mmp_metrics_update_reverse_delivery(&b->metrics,8,10);
        judged_same(&a->metrics,&b->metrics); CHECK(a->metrics.delivery_ratio_reverse==kept.delivery_ratio_reverse);
        fresh_rr=rr(30,25,2500,0,0,0); fresh_rr.ecn_ce_count=2; fresh_rr.cumulative_reorder_count=1;
        process(&a->metrics,fresh_rr,0,161000); process(&b->metrics,fresh_rr,0,161000);
        fips_mmp_metrics_update_reverse_delivery(&a->metrics,55,30); fips_mmp_metrics_update_reverse_delivery(&b->metrics,20,30);
        judged_same(&a->metrics,&b->metrics); CHECK(a->metrics.delivery_ratio_reverse==b->metrics.delivery_ratio_reverse && a->metrics.etx==b->metrics.etx);
    }
    /* spin_rekey.rs: old counters exceed every new counter, both aggregates. */
    for (unsigned session=0;session<2;++session) {
        fips_mmp_session_state a,b; fips_mmp_owd_sample x[32],y[32];
        CHECK(fips_mmp_session_init(&a,FIPS_MMP_Full,30,32,true,x,32)==FIPS_OK);
        CHECK(fips_mmp_session_init(&b,FIPS_MMP_Full,30,32,false,y,32)==FIPS_OK);
        for (unsigned phase=0;phase<2;++phase) {
            unsigned flips=0, trips=phase ? 20 : 200; uint64_t dummy=0;
            for (unsigned i=0;i<trips;++i) {
                bool sent=a.roles.spin_bit.current_value;
                fips_mmp_spin_rx_observe(&b.roles.spin_bit,sent,i+1,1000+i*10,&dummy); CHECK(b.roles.spin_bit.current_value==sent);
                fips_mmp_spin_rx_observe(&a.roles.spin_bit,b.roles.spin_bit.current_value,i+1,1005+i*10,&dummy);
                if (a.roles.spin_bit.current_value!=sent) ++flips;
            }
            CHECK(flips==trips);
            if (session) { fips_mmp_session_reset_for_rekey(&a,10000); fips_mmp_session_reset_for_rekey(&b,10000); }
            else { fips_mmp_peer_reset_for_rekey(&a.roles,10000); fips_mmp_peer_reset_for_rekey(&b.roles,10000); }
        }
    }
}

#include "mmp_pins.h"
static void arithmetic_pins(void)
{
    const double samples[]={0,100,50,1,1e20,1e-10,-3.5,0};
    fips_mmp_dual_ewma e={0};
    for (unsigned i=0;i<8;++i) { fips_mmp_dual_ewma_update(&e,samples[i]);
        CHECK(bits(e.short_avg)==mmp_ewma_pins[i][0] && bits(e.long_avg)==mmp_ewma_pins[i][1]); }
    const int64_t rtts[]={10000,45001,0,500000,999,INT64_C(4294967295000)};
    fips_mmp_srtt_estimator srtt={0};
    for (unsigned i=0;i<6;++i) { fips_mmp_srtt_update(&srtt,rtts[i]); CHECK(srtt.srtt_us==mmp_srtt_pins[i][0] && srtt.rttvar_us==mmp_srtt_pins[i][1]); }
    CHECK(bits(fips_mmp_compute_etx(.9,.95))==MMP_LOSSY_ETX_BITS);
    const uint64_t received[]={80,50,100,0,99,80}; const double reverse[]={.9,1.0,.5,.75,.99,.9};
    fips_mmp_metrics m; fips_mmp_metrics_init(&m); process(&m,rr(100,100,10000,0,0,0),0,1000);
    uint64_t packets=100;
    for (unsigned i=0;i<6;++i) {
        packets+=received[i]; m.delivery_ratio_reverse=reverse[i];
        process(&m,rr(200+100*i,packets,packets*100,0,0,0),0,2000+1000*i);
        CHECK(bits(m.etx)==mmp_etx_pins[i][0]); CHECK(bits(m.etx_trend.short_avg)==mmp_etx_pins[i][1]); CHECK(bits(m.etx_trend.long_avg)==mmp_etx_pins[i][2]);
    }
    CHECK(MMP_OWD_STREAM_TREND==16361);
}

static void delivery(void)
{
    fips_mmp_ack_state storage[2]; fips_mmp_acks a; fips_mmp_resend_reason reason=FIPS_MMP_Loss; fips_node_addr peer=addr(1), peer2=addr(2), peer3=addr(3);
    fips_mmp_link_evidence link={.epoch=1,.next_counter=10}; uint64_t counter=999;
    fips_mmp_acks_init(&a,storage,2); CHECK(!fips_mmp_acks_outstanding(&a,&peer,&counter) && counter==999);
    CHECK(!fips_mmp_acks_check(&a,&peer,&link,0,&reason));
    CHECK(fips_mmp_acks_record(&a,peer,true,5,&link,0)==FIPS_OK); CHECK(fips_mmp_acks_outstanding(&a,&peer,&counter) && counter==5);
    link.has_rr=true; link.rr=(fips_mmp_rr_counters){5,6,0};
    CHECK(!fips_mmp_acks_check(&a,&peer,&link,0,&reason)); CHECK(!fips_mmp_acks_outstanding(&a,&peer,&counter));
    /* Zero base loss; bounded lineage losses, with backoff independent of fresh. */
    link.has_rr=false; CHECK(fips_mmp_acks_record(&a,peer,true,5,&link,0)==FIPS_OK);
    link.has_rr=true; link.rr=(fips_mmp_rr_counters){5,5,0};
    CHECK(fips_mmp_acks_check(&a,&peer,&link,0,&reason) && reason==FIPS_MMP_Loss);
    CHECK(!fips_mmp_acks_check(&a,&peer,&link,999,&reason));
    CHECK(fips_mmp_acks_check(&a,&peer,&link,1000,&reason) && reason==FIPS_MMP_Loss);
    CHECK(!fips_mmp_acks_check(&a,&peer,&link,2999,&reason));
    CHECK(fips_mmp_acks_check(&a,&peer,&link,3000,&reason) && reason==FIPS_MMP_Loss);
    CHECK(!fips_mmp_acks_check(&a,&peer,&link,7000,&reason));
    CHECK(fips_mmp_acks_check(&a,&peer,&link,30000,&reason) && reason==FIPS_MMP_Timeout);
    CHECK(!fips_mmp_acks_check(&a,&peer,&link,300000,&reason));
    /* First session whole counted base: reorder receipts may still prove delivery. */
    fips_mmp_acks_init(&a,storage,2); link.rr=(fips_mmp_rr_counters){2,3,0};
    CHECK(fips_mmp_acks_record(&a,peer,true,5,&link,0)==FIPS_OK); link.rr=(fips_mmp_rr_counters){5,6,1};
    CHECK(!fips_mmp_acks_check(&a,&peer,&link,0,&reason)); CHECK(!fips_mmp_acks_outstanding(&a,&peer,&counter));
    /* Holed base + old reorders is ambiguous, not an ACK. */
    link.rr=(fips_mmp_rr_counters){2,2,0}; CHECK(fips_mmp_acks_record(&a,peer,true,5,&link,0)==FIPS_OK);
    link.rr=(fips_mmp_rr_counters){5,5,1}; CHECK(!fips_mmp_acks_check(&a,&peer,&link,0,&reason)); CHECK(fips_mmp_acks_outstanding(&a,&peer,&counter));
    link.rr=(fips_mmp_rr_counters){6,7,1}; CHECK(!fips_mmp_acks_check(&a,&peer,&link,0,&reason)); CHECK(!fips_mmp_acks_outstanding(&a,&peer,&counter));
    /* New epoch or impossible RR: no usable evidence. Unknown can adopt a
     * noncovering report, but an orphan can never be checked in the new epoch. */
    link.has_rr=false; CHECK(fips_mmp_acks_record(&a,peer,true,5,&link,0)==FIPS_OK); link.epoch=2;
    CHECK(!fips_mmp_acks_check(&a,&peer,&link,100,&reason)); link.has_rr=true; link.rr=(fips_mmp_rr_counters){2,100,0};
    CHECK(fips_mmp_acks_check(&a,&peer,&link,101,&reason) && reason==FIPS_MMP_SessionChanged);
    CHECK(!fips_mmp_acks_check(&a,&peer,&link,200000,&reason));
    CHECK(fips_mmp_acks_record(&a,peer,true,5,&link,200000)==FIPS_OK); /* noncovering base */
    link.rr=(fips_mmp_rr_counters){5,103,0}; CHECK(!fips_mmp_acks_check(&a,&peer,&link,200000,&reason)); CHECK(!fips_mmp_acks_outstanding(&a,&peer,&counter));
    /* No RR on later epoch yields unknown; later noncovering RR becomes base. */
    link.has_rr=false; CHECK(fips_mmp_acks_record(&a,peer,true,8,&link,200000)==FIPS_OK);
    link.has_rr=true; link.rr=(fips_mmp_rr_counters){5,103,0}; CHECK(!fips_mmp_acks_check(&a,&peer,&link,200001,&reason));
    link.rr=(fips_mmp_rr_counters){8,106,0}; CHECK(!fips_mmp_acks_check(&a,&peer,&link,200002,&reason)); CHECK(!fips_mmp_acks_outstanding(&a,&peer,&counter));
    /* Unknown covering report gets one Unverified, not a forged ACK. */
    CHECK(fips_mmp_acks_record(&a,peer,true,8,&link,300000)==FIPS_OK);
    CHECK(fips_mmp_acks_check(&a,&peer,&link,320000,&reason) && reason==FIPS_MMP_Unverified);
    CHECK(fips_mmp_acks_record(&a,peer,false,8,&link,320000)==FIPS_OK);
    CHECK(!fips_mmp_acks_check(&a,&peer,&link,500000,&reason));
    /* RR highest >= next_counter is from a different session; fallback only. */
    fips_mmp_acks_init(&a,storage,2); link=(fips_mmp_link_evidence){.epoch=1,.next_counter=10,.has_rr=true,.rr={10,11,0}};
    fips_mmp_rr_counters sentinel={42,42,42}; CHECK(!fips_mmp_link_evidence_usable_rr(&link,&sentinel) && sentinel.highest==42);
    CHECK(fips_mmp_acks_record(&a,peer,true,5,&link,0)==FIPS_OK); CHECK(!fips_mmp_acks_check(&a,&peer,&link,29999,&reason));
    CHECK(fips_mmp_acks_check(&a,&peer,&link,30000,&reason) && reason==FIPS_MMP_Timeout);
    CHECK(fips_mmp_acks_record(&a,peer2,true,5,&link,0)==FIPS_OK);
    fips_mmp_ack_state snapshot[2]; memcpy(snapshot,storage,sizeof(storage));
    CHECK(fips_mmp_acks_record(&a,peer3,true,5,&link,0)==FIPS_ERR_RANGE && a.len==2 && !memcmp(snapshot,storage,sizeof(storage)));
    fips_mmp_acks_remove(&a,&peer); CHECK(a.len==1 && !fips_mmp_acks_outstanding(&a,&peer,&counter));
    CHECK(fips_mmp_acks_outstanding(&a,&peer2,&counter)); fips_mmp_acks_remove(&a,&peer); CHECK(a.len==1);
    CHECK(fips_mmp_acks_record(&a,peer3,true,5,&link,0)==FIPS_OK);
    fips_mmp_acks_set_fallback(&a,7); CHECK(fips_mmp_acks_check(&a,&peer3,&link,7,&reason) && reason==FIPS_MMP_Timeout);
    /* Saturated fallback deadline, not wrapped. */
    fips_mmp_acks_init(&a,storage,2); link.has_rr=false;
    CHECK(fips_mmp_acks_record(&a,peer,true,5,&link,UINT64_MAX-10)==FIPS_OK);
    CHECK(!fips_mmp_acks_check(&a,&peer,&link,UINT64_MAX-1,&reason)); CHECK(fips_mmp_acks_check(&a,&peer,&link,UINT64_MAX,&reason));
}
static void bounded_initialization(void)
{
    fips_mmp_owd_sample storage[32]; fips_mmp_receiver_state s; memset(&s,0xa5,sizeof(s)); fips_mmp_receiver_state copy=s;
    CHECK(fips_mmp_receiver_init(&s,32,storage,31)==FIPS_ERR_RANGE && !memcmp(&s,&copy,sizeof(s)));
    CHECK(fips_mmp_receiver_init(&s,0,NULL,0)==FIPS_ERR_RANGE && !memcmp(&s,&copy,sizeof(s)));
    fips_mmp_session_state session; memset(&session,0xa5,sizeof(session)); fips_mmp_session_state saved=session;
    CHECK(fips_mmp_session_init(&session,FIPS_MMP_Full,30,32,false,storage,31)==FIPS_ERR_RANGE && !memcmp(&session,&saved,sizeof(session)));
}
/* C equivalent of fresh_diff/dirtied: every runtime field is deliberately
 * away from fresh, then every field is compared after reset to its stated
 * carry-or-clear value. No padding comparison and no private test hooks. */
static void reset_field_inventory(void)
{
    for (unsigned initiator=0;initiator<2;++initiator) {
        fips_mmp_peer_state a; fips_mmp_owd_sample storage[32];
        CHECK(fips_mmp_peer_init(&a,FIPS_MMP_Full,30,32,initiator!=0,storage,32)==FIPS_OK);
        a.sender=(fips_mmp_sender_state){.cumulative_packets_sent=120,.cumulative_bytes_sent=96000,
            .interval_start_counter=100,.interval_start_timestamp=5000,.interval_bytes_sent=16000,.last_counter=119,
            .last_timestamp=5900,.interval_has_data=true,.has_last_report=true,.last_report_ms=6000,
            .report_interval_ms=1500,.consecutive_send_failures=2,.srtt_sample_count=3};
        a.receiver.cumulative_packets_recv=100; a.receiver.cumulative_bytes_recv=80000;
        a.receiver.cumulative_reorder_count=2; a.receiver.highest_counter=110;
        a.receiver.interval_packets_recv=7; a.receiver.interval_bytes_recv=5600; a.receiver.jitter.jitter_q4=1600;
        fips_mmp_owd_push(&a.receiver.owd_trend,7,1200); fips_mmp_owd_push(&a.receiver.owd_trend,8,1350);
        a.receiver.has_owd_ref=true; a.receiver.owd_ref_recv_ms=5000; a.receiver.owd_ref_timestamp=400;
        a.receiver.gap_tracker=(fips_mmp_gap_tracker){.expected_next=111,.has_expected=true,.in_burst=true,.current_burst_len=2,.burst_count=3,.max_burst_len=4,.total_burst_len=7};
        a.receiver.ecn_ce_count=1; a.receiver.last_sender_timestamp=5900; a.receiver.last_recv_ms=6000;
        a.receiver.rekey_jitter_grace_until_ms=7000; a.receiver.last_report_ms=5500; a.receiver.report_interval_ms=800;
        a.receiver.has_last_recv=true; a.receiver.has_rekey_grace=true; a.receiver.has_last_report=true;
        a.receiver.interval_has_data=true; a.receiver.srtt_sample_count=4;
        a.metrics.srtt=(fips_mmp_srtt_estimator){40000,5000,true};
        a.metrics.rtt_trend=a.metrics.loss_trend=a.metrics.goodput_trend=a.metrics.jitter_trend=a.metrics.etx_trend=(fips_mmp_dual_ewma){3.5,2.5,true};
        a.metrics.delivery_ratio_forward=.9; a.metrics.delivery_ratio_reverse=.8; a.metrics.etx=1.4; a.metrics.goodput_bps=12000;
        a.metrics.prev_rr_cum_packets=100; a.metrics.prev_rr_cum_bytes=80000; a.metrics.prev_rr_highest_counter=110;
        a.metrics.prev_rr_ecn_ce=1; a.metrics.prev_rr_reorder=2; a.metrics.prev_rr_ms=5000; a.metrics.has_prev_rr=true;
        a.metrics.prev_reverse_packets=90; a.metrics.prev_reverse_highest=95; a.metrics.has_prev_reverse=true;
        a.metrics.reports_seen=4; a.metrics.rtt_samples=3; a.metrics.rtt_zero=1; a.metrics.rtt_arith_fail=1;
        a.metrics.last_rtt_ms=40; a.metrics.has_last_rtt=true;
        a.spin_bit=(fips_mmp_spin_bit_state){initiator!=0,true,4000,9000,true}; a.has_last_log=true; a.last_log_ms=6200;
        fips_mmp_peer_state before=a;
        fips_mmp_peer_reset_for_rekey(&a,100000);
        CHECK(a.sender.cumulative_packets_sent==before.sender.cumulative_packets_sent);
        CHECK(a.sender.cumulative_bytes_sent==before.sender.cumulative_bytes_sent);
        CHECK(a.sender.interval_start_counter==before.sender.interval_start_counter);
        CHECK(a.sender.interval_start_timestamp==before.sender.interval_start_timestamp);
        CHECK(a.sender.interval_bytes_sent==before.sender.interval_bytes_sent);
        CHECK(a.sender.last_counter==before.sender.last_counter);
        CHECK(a.sender.last_timestamp==before.sender.last_timestamp);
        CHECK(a.sender.interval_has_data==before.sender.interval_has_data);
        CHECK(a.sender.has_last_report==before.sender.has_last_report);
        CHECK(a.sender.last_report_ms==before.sender.last_report_ms);
        CHECK(a.sender.report_interval_ms==before.sender.report_interval_ms);
        CHECK(a.sender.consecutive_send_failures==before.sender.consecutive_send_failures);
        CHECK(a.sender.srtt_sample_count==before.sender.srtt_sample_count);
        CHECK(a.receiver.cumulative_packets_recv==before.receiver.cumulative_packets_recv);
        CHECK(a.receiver.cumulative_bytes_recv==before.receiver.cumulative_bytes_recv);
        CHECK(a.receiver.has_last_report==before.receiver.has_last_report);
        CHECK(a.receiver.last_report_ms==before.receiver.last_report_ms);
        CHECK(a.receiver.report_interval_ms==before.receiver.report_interval_ms);
        CHECK(a.receiver.srtt_sample_count==before.receiver.srtt_sample_count);
        CHECK(a.receiver.cumulative_reorder_count==0);
        CHECK(a.receiver.highest_counter==0);
        CHECK(a.receiver.interval_packets_recv==0);
        CHECK(a.receiver.interval_bytes_recv==0);
        CHECK(a.receiver.has_owd_ref==0);
        CHECK(a.receiver.owd_ref_recv_ms==0);
        CHECK(a.receiver.owd_ref_timestamp==0);
        CHECK(a.receiver.ecn_ce_count==0);
        CHECK(a.receiver.last_sender_timestamp==0);
        CHECK(a.receiver.last_recv_ms==0);
        CHECK(a.receiver.has_last_recv==0);
        CHECK(a.receiver.interval_has_data==0);
        CHECK(a.receiver.has_rekey_grace && a.receiver.rekey_jitter_grace_until_ms==115000);
        CHECK(a.receiver.jitter.jitter_q4==0 && a.receiver.owd_trend.len==0 && a.receiver.owd_trend.head==0 && a.receiver.owd_trend.capacity==32 && a.receiver.owd_trend.samples==storage);
        CHECK(a.receiver.gap_tracker.expected_next==0);
        CHECK(a.receiver.gap_tracker.has_expected==0);
        CHECK(a.receiver.gap_tracker.in_burst==0);
        CHECK(a.receiver.gap_tracker.current_burst_len==0);
        CHECK(a.receiver.gap_tracker.burst_count==0);
        CHECK(a.receiver.gap_tracker.max_burst_len==0);
        CHECK(a.receiver.gap_tracker.total_burst_len==0);
        kept_metrics(&a.metrics,&before.metrics);
        CHECK(a.metrics.prev_rr_cum_packets==0);
        CHECK(a.metrics.prev_rr_cum_bytes==0);
        CHECK(a.metrics.prev_rr_highest_counter==0);
        CHECK(a.metrics.prev_rr_ecn_ce==0);
        CHECK(a.metrics.prev_rr_reorder==0);
        CHECK(a.metrics.prev_rr_ms==0);
        CHECK(a.metrics.has_prev_rr==0);
        CHECK(a.metrics.prev_reverse_packets==0);
        CHECK(a.metrics.prev_reverse_highest==0);
        CHECK(a.metrics.has_prev_reverse==0);
        CHECK(a.metrics.delivery_ratio_forward==1.0);
        CHECK(a.spin_bit.is_initiator==(initiator!=0) && !a.spin_bit.current_value && !a.spin_bit.has_last_edge && a.spin_bit.last_edge_ms==0 && a.spin_bit.highest_counter_for_spin==0);
        CHECK(a.mode==before.mode && a.log_interval_ms==before.log_interval_ms && a.has_last_log && a.last_log_ms==before.last_log_ms);
    }
}
int main(void)
{
    RUN(algorithms); RUN(modes_and_sender); RUN(receiver_cycles_and_gaps); RUN(timing_owd_reorder);
    RUN(metrics_cycles); RUN(etx_once_per_report); RUN(decisions); RUN(path_mtu_flow); RUN(wire_codecs);
    RUN(reset_and_spin); RUN(arithmetic_pins); RUN(delivery); RUN(bounded_initialization); RUN(reset_field_inventory);
    printf("MMP: %u behavioral groups, %u assertions\n",groups,assertions); return 0;
}
