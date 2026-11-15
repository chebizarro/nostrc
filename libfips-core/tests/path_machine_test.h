#ifndef FIPS_PATH_MACHINE_TEST_H
#define FIPS_PATH_MACHINE_TEST_H
#include "path_test.h"
typedef struct facts { bool stream[16],accept[16],dial[16]; size_t mtu[16]; fips_path_role role[16]; fips_path_medium medium[16]; fips_path_kind kind[16]; fips_path_class class_[16]; } facts;
static inline size_t mtu(void *v,fips_transport_id t,const fips_transport_addr *a) { (void)a; return ((facts *)v)->mtu[t.value%16]; }
static inline fips_path_medium medium(void *v,fips_transport_id t,const fips_transport_addr *a) { (void)a; return ((facts *)v)->medium[t.value%16]; }
static inline bool oriented(void *v,fips_transport_id t) { return ((facts *)v)->stream[t.value%16]; }
static inline fips_path_class class_(void *v,fips_transport_id t) { return ((facts *)v)->class_[t.value%16]; }
static inline fips_path_kind kind(void *v,fips_transport_id t) { return ((facts *)v)->kind[t.value%16]; }
static inline fips_path_role role(void *v,fips_transport_id t) { return ((facts *)v)->role[t.value%16]; }
static inline bool accepts(void *v,fips_transport_id t) { return ((facts *)v)->accept[t.value%16]; }
static inline bool dials(void *v,fips_transport_id t) { return ((facts *)v)->dial[t.value%16]; }
typedef struct fixture { fips_path_bundle b; facts facts; fips_path_facts f; fips_path_env env; uint32_t seq; fips_path_output actions[128]; fips_path_outputs out; } fixture;
static inline void init(fixture *x,bool enabled)
{
    memset(x,0,sizeof(*x)); x->seq=1;
    for(size_t i=0;i<16;++i) { x->facts.mtu[i]=1236; x->facts.medium[i]=FIPS_PATH_Unknown; x->facts.kind[i]=FIPS_PATH_Unfixed; x->facts.role[i]=FIPS_PATH_Normal; x->facts.accept[i]=x->facts.dial[i]=true; }
    x->f=(fips_path_facts){&x->facts,mtu,medium,oriented,class_,kind,role,accepts,dials}; x->env=(fips_path_env){&x->f,1.5};
    x->out=(fips_path_outputs){x->actions,128,0}; fips_path_config cfg=fips_path_config_draft(); cfg.enabled=enabled;
    fips_path_params p; CHECK(fips_path_params_new(&p,cfg,NULL)==FIPS_OK);
    fips_path_bundle_new(&x->b,AT(0),T(1),address(1),p,counter_entropy(&x->seq),&x->env);
}
static inline fips_path_input input(fips_path_input_kind k,unsigned t,unsigned n)
{ return (fips_path_input){.kind=k,.to={T(t),address(n)}}; }
static inline void step(fixture *x,uint64_t ms,fips_path_input in)
{ CHECK(fips_path_bundle_step(&x->b,AT(ms),&in,&x->env,&x->out)==FIPS_OK); }
static inline size_t count(const fixture *x,fips_path_output_kind k)
{ size_t n=0; for(size_t i=0;i<x->out.len;++i) n+=x->out.items[i].kind==k; return n; }
static inline fips_path_probe sent(const fixture *x,unsigned t)
{
    for(size_t i=0;i<x->out.len;++i) { const fips_path_output *o=&x->out.items[i];
        if(o->kind==FIPS_PATH_OUTPUT_Send&&o->as.send.type==FIPS_LINK_PathProbe&&o->to.transport.value==t) return o->as.send.as.probe; }
    CHECK(false); return (fips_path_probe){0};
}
static inline void answer(fixture *x,uint64_t ms,unsigned t,unsigned n,fips_path_probe p)
{
    fips_path_input in=input(FIPS_PATH_INPUT_Ack,t,n);
    in.as.ack=(fips_path_ack){.probe_id=p.probe_id,.path_id=100+t,.echo_path_id=p.path_id,.len=p.len};
    fips_path_bundle_account(&x->b,AT(ms),in.to.transport,&in.to.addr,p.len+36,FIPS_LINK_PathAck); step(x,ms,in);
}
static inline void capable(fixture *x)
{
    fips_path_input in=input(FIPS_PATH_INPUT_Probe,1,1); in.as.probe=fips_path_probe_new(99,101,1,0);
    fips_path_bundle_account(&x->b,AT(0),T(1),&in.to.addr,63,FIPS_LINK_PathProbe); step(x,0,in); CHECK(x->b.capable);
}
static inline size_t live_path(fixture *x,unsigned t,unsigned n)
{
    fips_path_input in=input(FIPS_PATH_INPUT_Candidate,t,n); in.as.source=FIPS_PATH_Config; step(x,1,in);
    fips_path_probe p=sent(x,t); answer(x,11,t,n,p);
    fips_transport_addr a=address(n); size_t i=fips_path_set_find(&x->b.set,T(t),&a);
    CHECK(i!=FIPS_PATH_NONE&&x->b.set.paths[i].state==FIPS_PATH_Live&&x->b.set.paths[i].acked); return i;
}
static inline void score(fips_peer_path *p,unsigned ms)
{
    fips_path_liveness_new(&p->live);
    fips_path_liveness_sample(&p->live,AT(10),MS(ms),MS(30000),MS(60000));
    fips_path_liveness_sample(&p->live,AT(11),MS(ms),MS(30000),MS(60000));
}
#endif
