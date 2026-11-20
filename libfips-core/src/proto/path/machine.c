#include <fips/proto/path/machine.h>
#include <fips/proto/framing.h>
#include <fips/proto/bloom/core.h>
#include "internal.h"
#define OVERHEAD FIPS_FRAMING_FMP_FRAME_OVERHEAD
#define FILTER_FRAME (OVERHEAD+11+(512u<<FIPS_BLOOM_V1_SIZE_CLASS))
static fips_path_ref pair(const fips_peer_path *p) { return (fips_path_ref){p->transport_id,p->addr}; }
static void push(fips_path_outputs *o,fips_path_output v) { assert(o->len<o->capacity); o->items[o->len++]=v; }
static bool ready(const fips_path_outputs *o) { return o->items&&o->capacity>=FIPS_PATH_OUTPUTS_MAX; }
static size_t ours(const fips_path_bundle *b,uint32_t id)
{ for(size_t i=0;i<b->set.len;++i) if(id&&b->set.paths[i].path_id==id) return i; return FIPS_PATH_NONE; }
static fips_path_state state(const fips_path_bundle *b,size_t i) { return i<b->set.len?b->set.paths[i].state:FIPS_PATH_NoState; }
static bool any_live(const fips_path_bundle *b)
{ for(size_t i=0;i<b->set.len;++i) if(state(b,i)==FIPS_PATH_Live) return true; return false; }
static bool first(const fips_path_bundle *b,size_t i) { return b->original&&i<b->set.len&&b->set.paths[i].path_id==b->original; }
static bool single(const fips_path_bundle *b) { return b->set.len==1&&first(b,0); }
static bool speaks(const fips_path_bundle *b) { return b->params.cfg.enabled&&b->capable; }
static bool contends(const fips_peer_path *p) { return p->acked&&p->state==FIPS_PATH_Live; }
static bool signal(uint8_t type) { return type>=FIPS_LINK_PathProbe&&type<=FIPS_LINK_PathClose; }
static bool multipath(fips_path_input_kind k)
{ return k==FIPS_PATH_INPUT_Candidate||k==FIPS_PATH_INPUT_Unlisted||k==FIPS_PATH_INPUT_Probe||k==FIPS_PATH_INPUT_Ack||k==FIPS_PATH_INPUT_Close||k==FIPS_PATH_INPUT_Connected; }
static bool heard(fips_path_input_kind k) { return k==FIPS_PATH_INPUT_Probe||k==FIPS_PATH_INPUT_Ack||k==FIPS_PATH_INPUT_Close; }
static fips_path_rule probes(const fips_path_bundle *b,const fips_path_ref *to,const fips_path_env *env)
{ return fips_path_rules_probe(env->facts,b->scope,b->params.cfg.mix_kinds,to->transport,&to->addr); }
static fips_path_rule creates(const fips_path_bundle *b,const fips_path_ref *to,const fips_path_env *env)
{ return fips_path_rules_admit(env->facts,b->scope,b->params.cfg.mix_kinds,to->transport,&to->addr); }
static fips_path_rule again(const fips_path_bundle *b,size_t i,const fips_path_env *env)
{ if(i>=b->set.len||first(b,i)) return FIPS_PATH_RuleNone; fips_path_ref to=pair(&b->set.paths[i]); return probes(b,&to,env); }
static fips_path_rule revives(const fips_path_bundle *b,size_t i,const fips_path_env *env)
{ if(i>=b->set.len||first(b,i)) return FIPS_PATH_RuleNone; fips_path_ref to=pair(&b->set.paths[i]); return creates(b,&to,env); }
static fips_duration interval(const fips_path_bundle *b,size_t i)
{
    const fips_path_config *cfg=&b->params.cfg;
    if(i>=b->set.len) return cfg->standby_interval;
    const fips_peer_path *p=&b->set.paths[i]; const fips_path_liveness *l=&p->live;
    switch(p->state) {
    case FIPS_PATH_Probing: return fips_backoff(l->misses,cfg->active_interval,cfg->heartbeat);
    case FIPS_PATH_Dead:
        return pt_has(l->stop.free)&&pt_has(l->stop.until)?pd_max(pd_min(l->stop.gap,cfg->dead_interval),cfg->standby_interval):fips_backoff(l->misses,cfg->standby_interval,cfg->dead_interval);
    default: return b->set.active==i||l->peer?cfg->active_interval:cfg->standby_interval;
    }
}
static fips_instant stopped(fips_path_bundle *b,size_t i,fips_instant now)
{
    fips_peer_path *p=&b->set.paths[i]; fips_path_liveness *l=&p->live;
    if(p->state!=FIPS_PATH_Probing&&!pt_has(l->stop.free)&&pt_has(l->quiet)) {
        fips_instant at=fips_deadline(l->quiet,FIPS_PATH_SILENCE_MAX);
        if(pt_due(at,now)) fips_path_liveness_halt(l,at,b->set.active==i);
    }
    return l->stop.free;
}
static void note(const fips_path_bundle *b,size_t i,fips_path_state from,fips_path_why why,fips_path_outputs *o)
{
    const fips_peer_path *p=&b->set.paths[i];
    fips_path_output v={.kind=FIPS_PATH_OUTPUT_State,.to=pair(p)};
    v.as.state.path_id=p->path_id; v.as.state.from=from; v.as.state.next=p->state; v.as.state.why=why; push(o,v);
}
static void shift(fips_path_bundle *b,size_t i,fips_path_state next,fips_instant now,fips_path_why why,fips_path_outputs *o)
{
    fips_path_state from=state(b,i); bool active=b->set.active==i;
    if(next==FIPS_PATH_Dead) stopped(b,i,now);
    fips_path_set_transition(&b->set,i,next,now);
    fips_path_liveness *l=&b->set.paths[i].live;
    fips_path_liveness_fresh(l);
    if(from==FIPS_PATH_Dead) fips_path_liveness_thaw(l);
    if(from==FIPS_PATH_Live&&next!=FIPS_PATH_Live) { l->live_at=now; l->carried=active; }
    if(next==FIPS_PATH_Dead) {
        fips_path_liveness_clear(l); l->next_at=fips_deadline(now,b->params.cfg.standby_interval);
        fips_path_liveness_halt(l,now,active);
    }
    note(b,i,from,why,o);
}
static void revive(fips_path_bundle *b,size_t i,fips_instant now,fips_path_why why,fips_path_outputs *o)
{
    shift(b,i,FIPS_PATH_Probing,now,why,o); fips_path_liveness *l=&b->set.paths[i].live;
    if(why==FIPS_PATH_WHY_Probed) { fips_path_liveness_resume(l); l->urgent=true; }
    else l->next_at=(fips_instant){0};
}
static void flag(fips_path_bundle *b,size_t i,bool active)
{ fips_path_liveness *l=&b->set.paths[i].live; if(l->peer&&!active) l->urgent=true; l->peer=active; }
static void refuse(fips_path_outputs *o,fips_path_ref to,fips_path_refusal why,fips_path_rule rule)
{ fips_path_output v={.kind=FIPS_PATH_OUTPUT_Refused,.to=to}; v.as.refused.why=why; v.as.refused.rule=rule; push(o,v); }
static void released(const fips_path_bundle *b,const fips_peer_path *p,fips_path_outputs *o)
{
    fips_path_output v={.kind=FIPS_PATH_OUTPUT_Released,.to=pair(p)};
    v.as.released.driver_slot=p->driver_slot; v.as.released.keep=!b->capable||b->peer_active==p->path_id; push(o,v);
}
static void release(fips_path_bundle *b,const fips_peer_path *p,fips_path_why why,fips_path_outputs *o)
{
    if(b->original==p->path_id) b->original=0;
    fips_path_output v={.kind=FIPS_PATH_OUTPUT_State,.to=pair(p)};
    v.as.state.path_id=p->path_id; v.as.state.from=p->state; v.as.state.next=FIPS_PATH_NoState; v.as.state.why=why;
    push(o,v); released(b,p,o);
}
static void shelve(fips_path_bundle *b,fips_path_ref to)
{ if(b->params.cfg.enabled) fips_path_candidates_hold(&b->table,to.transport,to.addr,&b->set); }
static size_t admit(fips_path_bundle *b,fips_path_ref to,const fips_path_env *env,fips_path_outputs *o)
{
    fips_path_medium m=env->facts->medium(env->facts->ctx,to.transport,&to.addr); fips_path_admission a;
    if(fips_path_set_admit(&b->set,to.transport,to.addr,m,b->entropy,&a,NULL)!=FIPS_OK) {
        refuse(o,to,FIPS_PATH_REFUSAL_Full,FIPS_PATH_RuleNone); return FIPS_PATH_NONE;
    }
    if(a.has_evicted) { fips_path_ref gone=pair(&a.evicted); release(b,&a.evicted,FIPS_PATH_WHY_Evicted,o); shelve(b,gone); }
    return a.index;
}
static void enlist(fips_path_bundle *b,size_t slot,bool retry,const fips_path_env *env,fips_path_outputs *o)
{
    fips_path_entry *e=&b->table.slots[slot];
    if(!b->capable||!e->used||e->held||fips_path_set_find(&b->set,e->transport,&e->addr)!=FIPS_PATH_NONE) return;
    fips_path_rule last=e->refused; fips_path_ref to={e->transport,e->addr};
    fips_path_rule rule=probes(b,&to,env);
    if(rule!=FIPS_PATH_RuleNone) {
        fips_path_candidates_mark(&b->table,slot,rule);
        if(!retry||last!=rule) refuse(o,to,FIPS_PATH_REFUSAL_Rule,rule);
        return;
    }
    size_t i=admit(b,to,env,o); fips_path_candidates_mark(&b->table,slot,FIPS_PATH_RuleNone);
    if(i==FIPS_PATH_NONE) return;
    if(env->facts->oriented(env->facts->ctx,to.transport)) b->set.paths[i].live.connect=FIPS_PATH_Down;
    note(b,i,FIPS_PATH_NoState,FIPS_PATH_WHY_Candidate,o);
}
static void enlists(fips_path_bundle *b,uint8_t mask,const fips_path_env *env,fips_path_outputs *o)
{
    for(size_t i=0;i<8;++i) if(mask&(1u<<i)) enlist(b,i,false,env,o);
    fips_path_candidates_prune(&b->table,mask,&b->set);
}
static void turn(fips_path_bundle *b,const fips_path_env *env,fips_path_outputs *o)
{ b->capable=true; push(o,(fips_path_output){.kind=FIPS_PATH_OUTPUT_Capable}); for(size_t i=0;i<8;++i) enlist(b,i,false,env,o); }
static void candidate(fips_path_bundle *b,fips_instant now,fips_path_ref to,fips_path_source source,const fips_path_env *env,fips_path_outputs *o)
{
    fips_path_report r=fips_path_candidates_report(&b->table,to.transport,to.addr,source,&b->set);
    if(r.kind==FIPS_PATH_Repeat) return;
    if(r.kind==FIPS_PATH_Full) { refuse(o,to,FIPS_PATH_REFUSAL_Table,FIPS_PATH_RuleNone); return; }
    bool found=source==FIPS_PATH_Discovery;
    uint8_t mask=found?fips_path_candidates_release(&b->table,to.transport):0;
    if(b->capable) for(size_t i=0;i<b->set.len;++i) {
        const fips_peer_path *p=&b->set.paths[i];
        bool on=p->transport_id.value==to.transport.value,here=on&&fips_transport_addr_equal(&p->addr,&to.addr);
        if(p->state!=FIPS_PATH_Dead||!(here||(found&&on))) continue;
        fips_path_rule rule=again(b,i,env);
        if(rule==FIPS_PATH_RuleNone) revive(b,i,now,FIPS_PATH_WHY_Found,o);
        else if(here) refuse(o,to,FIPS_PATH_REFUSAL_Rule,rule);
    }
    enlist(b,r.slot,false,env,o); enlists(b,(uint8_t)(mask&~(1u<<r.slot)),env,o);
}
static fips_path_rule follow(fips_path_bundle *b,size_t i,fips_path_ref to,const fips_path_env *env,fips_path_outputs *o)
{
    if(b->set.paths[i].transport_id.value!=to.transport.value||fips_path_set_find(&b->set,to.transport,&to.addr)!=FIPS_PATH_NONE||single(b)) return FIPS_PATH_RuleNone;
    if(!first(b,i)) {
        fips_path_rule r=fips_path_rules_moves(env->facts,b->scope,b->params.cfg.mix_kinds,to.transport,&to.addr);
        if(r!=FIPS_PATH_RuleNone) return r;
    }
    fips_transport_addr old;
    if(fips_path_set_readdress(&b->set,i,to.addr,&old)) {
        fips_path_output v={.kind=FIPS_PATH_OUTPUT_Moved,.to=to};
        v.as.moved.from=(fips_path_ref){to.transport,old}; v.as.moved.driver_slot=b->set.paths[i].driver_slot;
        b->set.paths[i].driver_slot=NULL; push(o,v);
    }
    return FIPS_PATH_RuleNone;
}
static void attribute(fips_path_bundle *b,size_t i,bool frame,uint32_t from,size_t len,fips_instant now)
{
    if(!frame||b->set.paths[i].path_id==from) return;
    size_t was=ours(b,from);
    if(was!=FIPS_PATH_NONE) { --b->set.paths[was].live.frames; b->set.paths[was].live.bytes-=(uint32_t)len; }
    fips_path_liveness_heard(&b->set.paths[i].live,len,now);
}
static uint32_t clock_us(const fips_path_bundle *b,fips_instant now)
{ fips_duration d=pt_since(now,b->origin); return (uint32_t)d.secs*UINT32_C(1000000)+d.nanos/1000; }
static void probe(fips_path_bundle *b,fips_instant now,fips_path_ref from,fips_path_probe msg,bool frame,uint32_t fp,size_t fl,const fips_path_env *env,fips_path_outputs *o)
{
    if(!fips_path_probe_answerable(&msg)) { refuse(o,from,FIPS_PATH_REFUSAL_Short,FIPS_PATH_RuleNone); return; }
    size_t i=fips_path_set_find_by_peer(&b->set,from.transport,msg.path_id); bool added=false;
    if(i!=FIPS_PATH_NONE) {
        fips_path_rule r=follow(b,i,from,env,o);
        if(r!=FIPS_PATH_RuleNone) { refuse(o,from,FIPS_PATH_REFUSAL_Rule,r); return; }
    } else {
        i=fips_path_set_find(&b->set,from.transport,&from.addr);
        if(i==FIPS_PATH_NONE) {
            fips_path_rule r=creates(b,&from,env);
            if(r!=FIPS_PATH_RuleNone) { refuse(o,from,FIPS_PATH_REFUSAL_Rule,r); return; }
            i=admit(b,from,env,o); if(i==FIPS_PATH_NONE) return; added=true;
        }
        fips_path_set_learn_peer(&b->set,i,msg.path_id);
        if(added) note(b,i,FIPS_PATH_NoState,FIPS_PATH_WHY_Probed,o);
    }
    if(!added&&state(b,i)==FIPS_PATH_Dead&&revives(b,i,env)==FIPS_PATH_RuleNone) revive(b,i,now,FIPS_PATH_WHY_Probed,o);
    attribute(b,i,frame,fp,fl,now); flag(b,i,(msg.flags&FIPS_PATH_ACTIVE)!=0);
    const fips_peer_path *p=&b->set.paths[i];
    fips_path_output v={.kind=FIPS_PATH_OUTPUT_Send,.to=from}; v.as.send.type=FIPS_LINK_PathAck;
    v.as.send.as.ack=(fips_path_ack){.probe_id=msg.probe_id,.flags=b->set.active==i?FIPS_PATH_ACTIVE:0,
        .path_id=p->path_id,.echo_path_id=msg.path_id,.caps=0,.rx_time_us=clock_us(b,now),.rx_frames=p->live.frames,
        .rx_bytes=p->live.bytes,.len=fips_path_ack_len(msg.len,env->facts->mtu(env->facts->ctx,from.transport,&from.addr))};
    push(o,v);
}
static void proven(const fips_path_bundle *b,size_t i,fips_path_outputs *o)
{
    const fips_peer_path *p=&b->set.paths[i];
    if(p->live.has_mtu) { fips_path_output v={.kind=FIPS_PATH_OUTPUT_Proven,.to=pair(p)}; v.as.mtu=p_add(p->live.mtu,OVERHEAD); push(o,v); }
}
static void ack(fips_path_bundle *b,fips_instant now,fips_path_ref from,fips_path_ack msg,bool frame,uint32_t fp,size_t fl,const fips_path_env *env,fips_path_outputs *o)
{
    size_t i=ours(b,msg.echo_path_id);
    if(i==FIPS_PATH_NONE) { refuse(o,from,FIPS_PATH_REFUSAL_Unmatched,FIPS_PATH_RuleNone); return; }
    fips_path_liveness *l=&b->set.paths[i].live; size_t len;
    if(fips_path_liveness_seek_answer(l,msg.probe_id,b->params.cfg.standby_interval,&len)) {
        attribute(b,i,frame,fp,fl,now);
        if(fips_path_liveness_seek_proved(l,len,msg.len,now)) proven(b,i,o);
        return;
    }
    fips_path_answer ring=fips_path_liveness_answer(l,msg.probe_id); fips_path_sent sent=ring.sent;
    bool held=ring.kind==FIPS_PATH_AnswerNone&&fips_path_capability_answer(&b->record,msg.echo_path_id,msg.probe_id,&sent);
    if(ring.kind!=FIPS_PATH_AnswerNone||held) attribute(b,i,frame,fp,fl,now);
    if(held||ring.kind==FIPS_PATH_InTime) fips_path_liveness_resume(l);
    if(ring.kind==FIPS_PATH_Late) {
        fips_path_liveness_sample(l,now,pt_since(now,sent.at),b->params.window,b->params.cfg.horizon); return;
    }
    if(ring.kind==FIPS_PATH_AnswerNone&&!held) { refuse(o,from,FIPS_PATH_REFUSAL_Unmatched,FIPS_PATH_RuleNone); return; }
    fips_path_set_learn_peer(&b->set,i,msg.path_id); fips_path_state from_state=state(b,i);
    fips_path_liveness_sample(l,now,pt_since(now,sent.at),b->params.window,b->params.cfg.horizon);
    bool changed=false;
    if(ring.kind!=FIPS_PATH_AnswerNone) {
        fips_path_liveness_fold(l,true);
        if(sent.len>27) changed=fips_path_liveness_prove(l,p_min(sent.len,msg.len));
    }
    if(changed) proven(b,i,o);
    fips_path_set_mark_acked(&b->set,i); flag(b,i,(msg.flags&FIPS_PATH_ACTIVE)!=0); follow(b,i,from,env,o);
    if(from_state!=FIPS_PATH_Live) shift(b,i,FIPS_PATH_Live,now,FIPS_PATH_WHY_Answered,o);
}
static void closed(fips_path_bundle *b,fips_instant now,fips_path_ref from,fips_path_close msg,fips_path_outputs *o)
{
    size_t i=ours(b,msg.path_id);
    if(i==FIPS_PATH_NONE) { refuse(o,from,FIPS_PATH_REFUSAL_Unknown,FIPS_PATH_RuleNone); return; }
    if(state(b,i)==FIPS_PATH_Live||state(b,i)==FIPS_PATH_Suspect) shift(b,i,FIPS_PATH_Dead,now,FIPS_PATH_WHY_Closed,o);
}
static void farewell(const fips_path_bundle *b,fips_transport_id t,fips_path_close_reason reason,fips_path_outputs *o)
{
    if(!speaks(b)) return;
    size_t via=FIPS_PATH_NONE;
    for(size_t i=0;i<b->set.len;++i) if(state(b,i)==FIPS_PATH_Live&&b->set.paths[i].transport_id.value!=t.value) {
        if(via==FIPS_PATH_NONE||i==b->set.active) via=i;
    }
    if(via==FIPS_PATH_NONE) return;
    for(size_t i=0;i<b->set.len;++i) {
        const fips_peer_path *p=&b->set.paths[i];
        if(p->transport_id.value==t.value&&p->peer_id) {
            fips_path_output v={.kind=FIPS_PATH_OUTPUT_Send,.to=pair(&b->set.paths[via])};
            v.as.send.type=FIPS_LINK_PathClose; v.as.send.as.close=(fips_path_close){p->peer_id,reason}; push(o,v);
        }
    }
}
static void carrier(fips_path_bundle *b,fips_instant now,fips_transport_id t,bool up,const fips_path_env *env,fips_path_outputs *o)
{
    if(!up) farewell(b,t,FIPS_PATH_CarrierLost,o);
    for(size_t i=0;i<b->set.len;++i) if(b->set.paths[i].transport_id.value==t.value) {
        if(state(b,i)==FIPS_PATH_Live&&!up) shift(b,i,FIPS_PATH_Suspect,now,FIPS_PATH_WHY_Carrier,o);
        else if(state(b,i)==FIPS_PATH_Dead&&up&&again(b,i,env)==FIPS_PATH_RuleNone) revive(b,i,now,FIPS_PATH_WHY_Returned,o);
    }
    if(up) { uint8_t mask=fips_path_candidates_release(&b->table,t); enlists(b,mask,env,o); }
}
static void removed(fips_path_bundle *b,fips_transport_id t,bool absent,fips_path_outputs *o)
{
    if(b->has_pin&&b->pin.transport.value==t.value) b->has_pin=false;
    if(absent&&b->params.cfg.enabled) fips_path_candidates_absent(&b->table,t);
    farewell(b,t,FIPS_PATH_InterfaceGone,o);
    size_t i=0;
    while(i<b->set.len) {
        if(b->set.paths[i].transport_id.value!=t.value) { ++i; continue; }
        fips_peer_path p; fips_path_set_discard(&b->set,i,&p); fips_path_ref to=pair(&p);
        release(b,&p,FIPS_PATH_WHY_Removed,o); if(absent) shelve(b,to);
    }
    if(!absent) fips_path_candidates_forget(&b->table,t);
}
static void connected(fips_path_bundle *b,fips_instant now,fips_path_ref to,bool ok,fips_path_outputs *o)
{
    size_t i=fips_path_set_find(&b->set,to.transport,&to.addr); if(i==FIPS_PATH_NONE) return;
    fips_path_liveness *l=&b->set.paths[i].live;
    if(!fips_path_liveness_settle(l,ok)) return;
    fips_path_liveness_rebase(l,now,ok);
    if(ok) { l->urgent=true; return; }
    if(state(b,i)==FIPS_PATH_Probing&&l->misses>=b->params.cfg.discovery) { shift(b,i,FIPS_PATH_Dead,now,FIPS_PATH_WHY_Connect,o); return; }
    l->next_at=fips_deadline(now,interval(b,i));
}
static void rivals(const fips_path_bundle *b,const fips_path_env *env,fips_path_rival field[8])
{
    memset(field,0,8*sizeof(*field)); const fips_path_facts *f=env->facts;
    for(size_t i=0;i<b->set.len;++i) {
        const fips_peer_path *p=&b->set.paths[i]; if(!contends(p)) continue;
        const fips_path_liveness *l=&p->live; fips_path_role role=f->role(f->ctx,p->transport_id);
        size_t mtu=f->mtu(f->ctx,p->transport_id,&p->addr);
        bool oriented=f->oriented(f->ctx,p->transport_id); fips_path_ref to=pair(p);
        fips_path_rival *r=&field[i]; r->used=true; r->index=i;
        r->has_score=fips_path_select_score(l,&r->score)&&fips_path_liveness_samples(l)>=b->params.cfg.min_samples;
        r->has_steady=fips_path_select_steady(l,&r->steady)&&fips_path_liveness_samples(l)>=b->params.cfg.min_samples;
        r->has_rtt=l->has_rtt; r->rtt=l->rtt; r->has_base=fips_path_liveness_base_rtt(l,&r->base);
        r->tier=fips_path_select_tier(role); r->preferred=role==FIPS_PATH_Preferred;
        r->pinned=b->has_pin&&fips_path_pin_names(&b->pin,&to);
        r->fits=fips_path_carried(mtu,l->has_mtu,l->mtu,oriented)>=FILTER_FRAME;
        r->medium=f->medium(f->ctx,p->transport_id,&p->addr);
    }
}
static size_t rank(const fips_path_bundle *b,size_t active,const fips_path_env *env)
{ fips_path_rival f[8]; rivals(b,env,f); if(active<8) f[active].used=false; return fips_path_select_choose(f,b->params.cfg.switch_margin); }
static size_t best(const fips_path_bundle *b,fips_path_state st)
{
    size_t pick=FIPS_PATH_NONE;
    for(size_t i=0;i<b->set.len;++i) {
        const fips_peer_path *p=&b->set.paths[i]; if(p->state!=st||!p->acked) continue;
        if(pick==FIPS_PATH_NONE) { pick=i; continue; }
        const fips_path_liveness *l=&p->live,*r=&b->set.paths[pick].live;
        if(l->has_rtt>r->has_rtt||(l->has_rtt&&r->has_rtt&&pd_cmp(l->rtt,r->rtt)<0)) pick=i;
    }
    return pick;
}
static size_t recent(const fips_path_bundle *b)
{
    size_t pick=FIPS_PATH_NONE;
    for(size_t i=0;i<b->set.len;++i) {
        const fips_peer_path *p=&b->set.paths[i];
        if(!p->acked||(p->state!=FIPS_PATH_Dead&&p->state!=FIPS_PATH_Probing)) continue;
        if(pick==FIPS_PATH_NONE||(pt_has(p->live.live_at)&&(!pt_has(b->set.paths[pick].live.live_at)||pt_cmp(p->live.live_at,b->set.paths[pick].live.live_at)>0))) pick=i;
    }
    return pick;
}
static size_t better(fips_path_bundle *b,fips_instant now,size_t active,const fips_path_env *env,fips_path_switch *why)
{
    fips_path_rival f[8]; rivals(b,env,f); double margin=b->params.cfg.switch_margin;
    size_t t=fips_path_select_choose(f,margin);
    if(t==FIPS_PATH_NONE||t==active||!f[active].used) { b->dwell_at=(fips_instant){0}; return FIPS_PATH_NONE; }
    const fips_path_rival *to=&f[t],*from=&f[active];
    if(to->pinned&&!from->pinned) { *why=FIPS_PATH_Pinned; return t; }
    if(to->tier>from->tier) { *why=FIPS_PATH_Discretionary; return t; }
    double least; bool has_best=fips_path_select_best(f,&least);
    bool back=b->left&&b->set.paths[t].path_id==b->left;
    bool scored=from->has_score&&(from->score>to->score*margin||(has_best&&from->score>least*margin)||(back&&from->score>to->score));
    bool target=to->has_score&&((to->fits&&!from->fits)||(to->preferred&&!from->preferred)||
        (fips_path_select_quicker(to->medium,from->medium)&&!fips_path_select_waits(to,from,margin))||
        (scored&&!fips_path_select_held(to,from,margin)));
    if(!target) { b->dwell_at=(fips_instant){0}; return FIPS_PATH_NONE; }
    uint32_t id=b->set.paths[t].path_id;
    if(!pt_has(b->dwell_at)||b->dwell_path!=id) { b->dwell_path=id; b->dwell_at=now; }
    if(!pt_due(fips_deadline(b->dwell_at,b->params.cfg.switch_dwell),now)) return FIPS_PATH_NONE;
    *why=FIPS_PATH_Discretionary; return t;
}
static bool select_path(fips_path_bundle *b,fips_instant now,double held,uint32_t before,fips_path_state was,const fips_path_env *env,fips_path_outputs *o)
{
    size_t active=b->set.active,next=FIPS_PATH_NONE; fips_path_switch why=FIPS_PATH_Mandatory;
    if(state(b,active)!=FIPS_PATH_Live) b->dwell_at=(fips_instant){0};
    if(active==FIPS_PATH_NONE) {
        next=rank(b,FIPS_PATH_NONE,env);
        if(next==FIPS_PATH_NONE) next=best(b,FIPS_PATH_Suspect);
        if(next==FIPS_PATH_NONE) next=recent(b);
        if(next==FIPS_PATH_NONE) { b->orphaned=true; push(o,(fips_path_output){.kind=FIPS_PATH_OUTPUT_Orphaned}); return false; }
        why=FIPS_PATH_Handover;
    } else switch(state(b,active)) {
    case FIPS_PATH_Suspect: next=rank(b,active,env); break;
    case FIPS_PATH_Dead: case FIPS_PATH_Probing:
        next=rank(b,active,env);
        if(next==FIPS_PATH_NONE) next=best(b,FIPS_PATH_Suspect);
        break;
    case FIPS_PATH_Live: next=better(b,now,active,env,&why); break;
    default: break;
    }
    if(next!=FIPS_PATH_NONE) {
        b->dwell_at=(fips_instant){0};
        if(active==FIPS_PATH_NONE&&!any_live(b)&&(state(b,next)==FIPS_PATH_Suspect||state(b,next)==FIPS_PATH_Probing)) b->set.paths[next].live.urgent=true;
        fips_path_output v={.kind=FIPS_PATH_OUTPUT_Switched,.to=pair(&b->set.paths[next])};
        v.as.switched.has_from=active!=FIPS_PATH_NONE; v.as.switched.why=why;
        uint32_t left=0;
        if(active!=FIPS_PATH_NONE) { v.as.switched.from=pair(&b->set.paths[active]); if(why==FIPS_PATH_Mandatory) left=b->set.paths[active].path_id; }
        if(fips_path_set_activate(&b->set,next)) { b->left=left; push(o,v); b->hold=fips_path_hold_new(held,now); }
    }
    size_t prev=ours(b,before); fips_path_state st=state(b,prev);
    bool failed=before&&(prev==FIPS_PATH_NONE||(st!=was&&(st==FIPS_PATH_Suspect||st==FIPS_PATH_Dead)));
    if(failed&&!any_live(b)) for(size_t i=0;i<b->set.len;++i) {
        fips_peer_path *p=&b->set.paths[i];
        if(p->acked&&(p->state==FIPS_PATH_Probing||(p->state==FIPS_PATH_Dead&&again(b,i,env)==FIPS_PATH_RuleNone))) p->live.urgent=true;
    }
    return true;
}
static bool trippable(const fips_path_bundle *b,size_t i)
{
    if(state(b,i)!=FIPS_PATH_Live&&state(b,i)!=FIPS_PATH_Suspect) return false;
    for(size_t j=0;j<b->set.len;++j) if(j!=i&&(state(b,j)==FIPS_PATH_Live||(state(b,j)==FIPS_PATH_Suspect&&b->set.paths[j].acked))) return true;
    return false;
}
static void trips(fips_path_bundle *b,fips_path_outputs *o)
{
    for(size_t i=0;i<b->set.len;++i) {
        fips_instant at=b->set.paths[i].live.trip; b->set.paths[i].live.trip=(fips_instant){0};
        if(pt_has(at)&&trippable(b,i)) shift(b,i,FIPS_PATH_Dead,at,FIPS_PATH_WHY_AuthLimit,o);
    }
}
static void lapse(fips_path_bundle *b,fips_path_sent sent)
{ size_t i=ours(b,b->original); if(i!=FIPS_PATH_NONE) fips_path_liveness_lapse(&b->set.paths[i].live,sent); }
static void expire(fips_path_bundle *b,fips_instant now,bool overdue,fips_path_outputs *o)
{
    const fips_path_config *cfg=&b->params.cfg;
    for(size_t i=0;i<b->set.len;++i) {
        bool counted;
        while(fips_path_liveness_expire(&b->set.paths[i].live,now,overdue,cfg->verdict_cap,&counted)) {
            if(!counted) continue;
            fips_path_state st=state(b,i); fips_path_liveness *l=&b->set.paths[i].live;
            if(st==FIPS_PATH_Live) fips_path_liveness_fold(l,false);
            if(st==FIPS_PATH_Live&&l->misses>=cfg->suspect) shift(b,i,FIPS_PATH_Suspect,now,FIPS_PATH_WHY_Timeout,o);
            else if(st==FIPS_PATH_Suspect&&l->misses>=cfg->retire) shift(b,i,FIPS_PATH_Dead,now,FIPS_PATH_WHY_Retired,o);
            else if(st==FIPS_PATH_Probing&&l->misses>=cfg->discovery) shift(b,i,FIPS_PATH_Dead,now,FIPS_PATH_WHY_Exhausted,o);
        }
    }
    fips_path_sent sent; bool round;
    if(fips_path_capability_expire(&b->record,now,overdue,cfg,&sent,&round)) {
        lapse(b,sent); if(round) push(o,(fips_path_output){.kind=FIPS_PATH_OUTPUT_NotCapable});
    }
}
static fips_instant ends(const fips_path_bundle *b,size_t i,bool live)
{
    const fips_peer_path *p=&b->set.paths[i];
    return p->state==FIPS_PATH_Dead&&b->set.active!=i&&(!p->acked||live)&&pt_has(p->dead_since)?fips_deadline(p->dead_since,b->params.cfg.grace):(fips_instant){0};
}
static void grace(fips_path_bundle *b,fips_instant now,fips_path_outputs *o)
{
    bool live=any_live(b); size_t i=0;
    while(i<b->set.len) {
        fips_instant end=ends(b,i,live);
        if(pt_has(end)&&pt_due(end,now)) {
            fips_peer_path p; fips_path_set_discard(&b->set,i,&p); fips_path_ref to=pair(&p);
            release(b,&p,FIPS_PATH_WHY_Grace,o); shelve(b,to);
        } else ++i;
    }
}
static bool probed(const fips_path_bundle *b,size_t i)
{
    const fips_peer_path *p=&b->set.paths[i];
    return speaks(b)&&!(b->set.len==1&&p->state==FIPS_PATH_Live)&&!(p->state==FIPS_PATH_Dead&&!p->acked)&&p->live.connect!=FIPS_PATH_Pending;
}
static size_t cap(const fips_path_bundle *b,size_t i,bool urgent)
{ return state(b,i)==FIPS_PATH_Live||state(b,i)==FIPS_PATH_Suspect||urgent?b->params.cfg.ring:1; }
static bool gate(const fips_path_bundle *b,const fips_path_env *env)
{
    for(size_t i=0;i<8;++i) {
        const fips_path_entry *e=&b->table.slots[i]; fips_path_ref to={e->transport,e->addr};
        if(e->used&&!e->held&&fips_path_set_find(&b->set,e->transport,&e->addr)==FIPS_PATH_NONE&&probes(b,&to,env)==FIPS_PATH_RuleNone) return true;
    }
    return false;
}
static void send_probe(fips_path_outputs *o,fips_path_ref to,uint32_t id,uint32_t path,bool active,size_t len)
{
    fips_path_output v={.kind=FIPS_PATH_OUTPUT_Send,.to=to}; v.as.send.type=FIPS_LINK_PathProbe;
    v.as.send.as.probe=fips_path_probe_new(id,path,active?FIPS_PATH_ACTIVE:0,0); v.as.send.as.probe.len=len; push(o,v);
}
static void ask(fips_path_bundle *b,fips_instant now,const fips_path_env *env,fips_path_outputs *o)
{
    if(!b->params.cfg.enabled||b->capable) return;
    b->eligible=gate(b,env); if(!b->eligible||!fips_path_capability_due(&b->record,now)) return;
    size_t i=ours(b,b->original); if(i==FIPS_PATH_NONE) return;
    fips_peer_path *p=&b->set.paths[i];
    fips_duration timeout=fips_path_liveness_timeout(&p->live,b->params.cfg.timeout,b->params.cfg.verdict_cap);
    uint32_t id=fips_path_liveness_take_id(&p->live,b->entropy);
    fips_path_capability_send(&b->record,now,p->path_id,id,timeout,&b->params.cfg);
    send_probe(o,pair(p),id,p->path_id,b->set.active==i,27);
}
static size_t full_size(const fips_path_env *env,const fips_peer_path *p)
{ size_t mtu=env->facts->mtu(env->facts->ctx,p->transport_id,&p->addr); return p_max(mtu>OVERHEAD?mtu-OVERHEAD:0,27); }
static void send(fips_path_bundle *b,size_t i,fips_instant now,const fips_path_env *env,fips_path_outputs *o)
{
    fips_duration gap=interval(b,i); fips_peer_path *p=&b->set.paths[i]; fips_path_liveness *l=&p->live;
    size_t full=full_size(env,p),len=27;
    bool oriented=env->facts->oriented(env->facts->ctx,p->transport_id);
    if(p->state==FIPS_PATH_Dead&&l->has_mtu&&pt_has(l->stop.until)&&oriented) len=p_max(27,p_min(l->mtu,full));
    else if(p->state==FIPS_PATH_Probing||p->state==FIPS_PATH_Dead) len=fips_path_liveness_size(l,full);
    fips_instant due=fips_deadline(now,fips_path_liveness_timeout(l,b->params.cfg.timeout,b->params.cfg.verdict_cap)); uint32_t id;
    if(!fips_path_liveness_send(l,now,due,len,b->entropy,&id)) return;
    l->next_at=fips_deadline(now,gap); send_probe(o,pair(p),id,p->path_id,b->set.active==i,len);
}
static void seek(fips_path_bundle *b,size_t i,fips_instant now,const fips_path_env *env,fips_path_outputs *o)
{
    fips_peer_path *p=&b->set.paths[i];
    if(p->state!=FIPS_PATH_Live||env->facts->oriented(env->facts->ctx,p->transport_id)) return;
    fips_path_liveness *l=&p->live;
    fips_duration timeout=fips_path_liveness_timeout(l,b->params.cfg.timeout,b->params.cfg.verdict_cap);
    fips_path_liveness_seek_judge(l,now,timeout,b->params.cfg.standby_interval);
    size_t len;
    if(l->misses||!fips_path_liveness_seek_size(l,full_size(env,p),now,&len)) return;
    uint32_t id=fips_path_liveness_seek_send(l,now,len,b->entropy); send_probe(o,pair(p),id,p->path_id,b->set.active==i,len);
}
static void sends(fips_path_bundle *b,fips_instant now,const fips_path_env *env,fips_path_outputs *o)
{
    ask(b,now,env,o);
    for(size_t i=0;i<b->set.len;++i) {
        fips_peer_path *p=&b->set.paths[i]; fips_path_liveness *l=&p->live; bool urgent=l->urgent; l->urgent=false;
        if(!probed(b,i)) continue;
        fips_instant by=fips_deadline(now,interval(b,i)); by=pt_max(by,stopped(b,i,now));
        if(pt_has(l->next_at)&&pt_cmp(l->next_at,by)>0) l->next_at=by;
        size_t outstanding=fips_path_liveness_outstanding(l);
        if(outstanding>=p_min(cap(b,i,urgent),8)||(!urgent&&!pt_due(l->next_at,now))) continue;
        if(p->state==FIPS_PATH_Probing&&((uint64_t)l->misses+outstanding)>=b->params.cfg.discovery) continue;
        fips_instant held=urgent?(fips_instant){0}:stopped(b,i,now);
        if(pt_has(held)&&pt_cmp(now,held)<0) { l->next_at=held; continue; }
        if(p->state==FIPS_PATH_Dead&&again(b,i,env)!=FIPS_PATH_RuleNone) {
            if(pt_due(l->next_at,now)) l->next_at=fips_deadline(now,interval(b,i));
            continue;
        }
        if(l->connect==FIPS_PATH_Down) {
            l->connect=FIPS_PATH_Pending; push(o,(fips_path_output){.kind=FIPS_PATH_OUTPUT_Connect,.to=pair(p)});
            if(pt_has(held)) { fips_path_liveness_spend(l,now); l->stop.owed=true; }
        } else {
            if(!urgent&&!pt_has(held)) seek(b,i,now,env,o);
            send(b,i,now,env,o);
            if(pt_has(held)) { fips_path_liveness_spend(l,now); l->next_at=pt_max(l->next_at,l->stop.free); }
        }
    }
}
void fips_path_bundle_new(fips_path_bundle *b,fips_instant now,fips_transport_id t,fips_transport_addr a,fips_path_params params,fips_entropy entropy,const fips_path_env *env)
{
    memset(b,0,sizeof(*b)); b->params=params; b->entropy=entropy; b->origin=now;
    b->scope=fips_path_rules_link(env->facts,t,&a); fips_path_set_new(&b->set);
    fips_path_admission v; fips_path_set_bind_original(&b->set,t,a,entropy,&v,NULL); b->original=b->set.paths[0].path_id;
}
fips_status fips_path_bundle_step(fips_path_bundle *b,fips_instant now,const fips_path_input *input,const fips_path_env *env,fips_path_outputs *o)
{
    if(!ready(o)) return FIPS_ERR_RANGE;
    o->len=0; bool frame=b->has_frame; uint32_t fp=b->frame_path; size_t fl=b->frame_len; b->has_frame=false;
    if(b->orphaned) return FIPS_OK;
    double ignored;
    fips_instant wake=fips_path_bundle_next_wake(b);
    if(pt_has(b->hold.at)&&!fips_path_hold_held(&b->hold,now,b->params.cfg.switch_dwell,&ignored)&&
        !(input->kind==FIPS_PATH_INPUT_Poll&&(!pt_has(wake)||pt_cmp(now,wake)<0))) b->hold.at=(fips_instant){0};
    fips_path_input_kind k=!b->params.cfg.enabled&&multipath(input->kind)?FIPS_PATH_INPUT_Poll:input->kind;
    double held=fips_path_bundle_cost(b,now,env->cost);
    const fips_peer_path *active=fips_path_set_active(&b->set);
    uint32_t before=active?active->path_id:0; fips_path_state was=active?active->state:FIPS_PATH_NoState;
    trips(b,o); expire(b,now,true,o);
    if(heard(k)&&!b->capable) turn(b,env,o);
    fips_path_ref to=input->to;
    switch(k) {
    case FIPS_PATH_INPUT_Poll:
        if(b->capable) for(size_t i=0;i<8;++i) if(b->table.slots[i].used&&b->table.slots[i].refused!=FIPS_PATH_RuleNone) enlist(b,i,true,env,o);
        break;
    case FIPS_PATH_INPUT_Candidate: candidate(b,now,to,input->as.source,env,o); break;
    case FIPS_PATH_INPUT_Unlisted: fips_path_candidates_unlist(&b->table,to.transport,&to.addr,input->as.source); break;
    case FIPS_PATH_INPUT_Probe: probe(b,now,to,input->as.probe,frame,fp,fl,env,o); break;
    case FIPS_PATH_INPUT_Ack: ack(b,now,to,input->as.ack,frame,fp,fl,env,o); break;
    case FIPS_PATH_INPUT_Close: closed(b,now,to,input->as.close,o); break;
    case FIPS_PATH_INPUT_Carrier: carrier(b,now,to.transport,input->as.up,env,o); break;
    case FIPS_PATH_INPUT_SendFailed: {
        size_t i=fips_path_set_find(&b->set,to.transport,&to.addr);
        if(state(b,i)==FIPS_PATH_Live) shift(b,i,FIPS_PATH_Suspect,now,FIPS_PATH_WHY_Unreachable,o);
        break;
    }
    case FIPS_PATH_INPUT_Present:
        if(input->as.present) carrier(b,now,to.transport,true,env,o); else removed(b,to.transport,true,o);
        break;
    case FIPS_PATH_INPUT_Removed: removed(b,to.transport,false,o); break;
    case FIPS_PATH_INPUT_Connected: connected(b,now,to,input->as.ok,o); break;
    case FIPS_PATH_INPUT_Pin: b->has_pin=true; b->pin=(fips_path_pin){to.transport,input->as.has_addr,to.addr}; break;
    case FIPS_PATH_INPUT_Unpin: b->has_pin=false; break;
    case FIPS_PATH_INPUT_Report:
        if(fips_path_hold_held(&b->hold,now,b->params.cfg.switch_dwell,&ignored)) fips_path_hold_report(&b->hold);
        break;
    }
    fips_path_sent sent; if(b->capable&&fips_path_capability_take(&b->record,&sent)) lapse(b,sent);
    expire(b,now,false,o);
    if(!select_path(b,now,held,before,was,env,o)) return FIPS_OK;
    grace(b,now,o); sends(b,now,env,o); return FIPS_OK;
}
fips_instant fips_path_bundle_next_wake(const fips_path_bundle *b)
{
    if(b->orphaned) return (fips_instant){0};
    bool live=any_live(b); fips_instant wake=pt_has(b->dwell_at)?fips_deadline(b->dwell_at,b->params.cfg.switch_dwell):(fips_instant){0};
    for(size_t i=0;i<b->set.len;++i) {
        const fips_path_liveness *l=&b->set.paths[i].live;
        wake=pt_min(wake,l->trip); wake=pt_min(wake,fips_path_liveness_earliest_due(l));
        if(probed(b,i)&&fips_path_liveness_outstanding(l)<p_min(cap(b,i,false),8)) wake=pt_min(wake,pt_has(l->next_at)?l->next_at:b->origin);
        wake=pt_min(wake,ends(b,i,live));
    }
    if(b->params.cfg.enabled&&!b->capable) {
        fips_instant due=fips_path_capability_wake(&b->record,b->eligible);
        if(!pt_has(due)&&b->eligible) due=b->origin;
        wake=pt_min(wake,due);
    }
    return wake;
}
fips_path_account fips_path_bundle_account(fips_path_bundle *b,fips_instant now,fips_transport_id t,const fips_transport_addr *a,size_t len,uint8_t type)
{
    fips_path_account v={.path=FIPS_PATH_NONE}; if(b->orphaned) return v;
    bool message=signal(type);
    if(!(message&&b->params.cfg.enabled)&&single(b)) v.roam=fips_path_set_roam(&b->set,t,*a,b->entropy);
    if(v.roam.kind!=FIPS_PATH_Unmoved) b->eligible=true;
    size_t i=fips_path_set_find(&b->set,t,a); uint32_t id=0;
    if(i!=FIPS_PATH_NONE) {
        fips_peer_path *p=&b->set.paths[i]; id=p->path_id; fips_path_liveness_heard(&p->live,len,now);
        if(type==FIPS_LINK_SessionDatagram) p->live.data_at=now;
    }
    if(!message&&type!=FIPS_LINK_SessionDatagram) b->peer_active=id;
    v.path=i; v.sample=b->set.len<=1||(id&&id==b->peer_active);
    b->has_frame=message; b->frame_path=id; b->frame_len=len; return v;
}
fips_path_charge fips_path_bundle_charge(fips_path_bundle *b,fips_instant now,fips_transport_id t,const fips_transport_addr *a)
{
    if(b->orphaned||fips_path_set_classify(&b->set,t,a).kind!=FIPS_PATH_Proven) return FIPS_PATH_Nothing;
    size_t i=fips_path_set_find(&b->set,t,a); fips_path_liveness *l=&b->set.paths[i].live;
    l->fails=p_inc(l->fails); if(l->fails!=b->params.cfg.auth_limit) return FIPS_PATH_Counted;
    if(!trippable(b,i)) return FIPS_PATH_Warn;
    l->trip=now; return FIPS_PATH_Tripped;
}
void fips_path_bundle_authentic(fips_path_bundle *b,fips_transport_id t,const fips_transport_addr *a)
{ size_t i=fips_path_set_find(&b->set,t,a); if(i!=FIPS_PATH_NONE) b->set.paths[i].live.fails=0; }
fips_status fips_path_bundle_close(fips_path_bundle *b,fips_path_outputs *o)
{
    if(!ready(o)) return FIPS_ERR_RANGE;
    o->len=0; for(size_t i=0;i<b->set.len;++i) released(b,&b->set.paths[i],o);
    fips_path_set_new(&b->set); b->orphaned=true; return FIPS_OK;
}
fips_status fips_path_bundle_renew(fips_path_bundle *b,fips_instant now,const fips_path_env *env,fips_path_ref to,fips_path_outputs *o)
{
    if(!ready(o)) return FIPS_ERR_RANGE;
    o->len=0; if(!b->set.len) return FIPS_OK;
    uint32_t id; do { id=p_draw(b->entropy); } while(!id||ours(b,id)!=FIPS_PATH_NONE);
    size_t kept=fips_path_set_find(&b->set,to.transport,&to.addr);
    fips_peer_path stays={.transport_id=to.transport,.addr=to.addr};
    for(size_t i=0;i<b->set.len;++i) {
        const fips_peer_path *p=&b->set.paths[i];
        fips_path_output v={.kind=FIPS_PATH_OUTPUT_State,.to=pair(p)};
        v.as.state.path_id=p->path_id; v.as.state.from=p->state; v.as.state.next=FIPS_PATH_NoState; v.as.state.why=FIPS_PATH_WHY_Renewed; push(o,v);
        if(i==kept) stays=*p; else released(b,p,o);
    }
    uint32_t fails=stays.live.fails; fips_path_liveness_new(&stays.live); stays.live.fails=fails;
    stays.path_id=id; stays.peer_id=0; stays.state=FIPS_PATH_Live; stays.acked=true; stays.dead_since=(fips_instant){0};
    b->scope=fips_path_rules_link(env->facts,stays.transport_id,&stays.addr);
    fips_path_set_new(&b->set); b->set.paths[0]=stays; b->set.len=1; b->set.active=0;
    note(b,0,FIPS_PATH_NoState,FIPS_PATH_WHY_Renewed,o);
    b->capable=false; b->origin=now; b->peer_active=0; b->original=id;
    fips_path_candidates_new(&b->table); fips_path_capability_new(&b->record);
    b->eligible=false; b->has_frame=false; b->orphaned=false; b->dwell_at=(fips_instant){0}; b->left=0; return FIPS_OK;
}
void **fips_path_bundle_driver_mut(fips_path_bundle *b,size_t i) { return i<b->set.len?&b->set.paths[i].driver_slot:NULL; }
double fips_path_bundle_cost(const fips_path_bundle *b,fips_instant now,double live)
{ double cost; return fips_path_hold_held(&b->hold,now,b->params.cfg.switch_dwell,&cost)?cost:live; }
void fips_path_bundle_view_get(const fips_path_bundle *b,fips_instant now,fips_path_bundle_view *out)
{
    fips_path_bundle_view v={.enabled=b->params.cfg.enabled,.capable=b->capable,.has_pin=b->has_pin,.pin=b->pin,.len=b->set.len};
    double cost; v.held=fips_path_hold_held(&b->hold,now,b->params.cfg.switch_dwell,&cost);
    for(size_t i=0;i<b->set.len;++i) {
        const fips_peer_path *p=&b->set.paths[i]; const fips_path_liveness *l=&p->live;
        fips_path_view *pv=&v.paths[i]; pv->to=pair(p); pv->id=p->path_id; pv->state=p->state;
        pv->active=b->set.active==i; pv->acked=p->acked; pv->remote=l->peer;
        pv->pinned=b->has_pin&&fips_path_pin_names(&b->pin,&pv->to); v.pin_enforced|=pv->pinned&&contends(p);
        pv->has_rtt=l->has_rtt; pv->rtt=l->rtt; pv->has_least=fips_path_liveness_min_rtt(l,&pv->least);
        pv->samples=fips_path_liveness_samples(l); pv->etx=l->etx; pv->has_score=fips_path_select_score(l,&pv->score);
        pv->has_mtu=l->has_mtu; pv->mtu=l->mtu; pv->frames=l->frames; pv->bytes=l->bytes;
        pv->has_heard=pt_has(l->heard_at); if(pv->has_heard) pv->heard=pt_since(now,l->heard_at);
        pv->misses=l->misses; pv->fails=l->fails;
    }
    *out=v;
}
fips_status fips_path_output_encode(const fips_path_output *v,fips_writer *w,fips_proto_error *e)
{
    if(v->kind==FIPS_PATH_OUTPUT_Send) switch(v->as.send.type) {
    case FIPS_LINK_PathProbe: return fips_path_probe_encode(&v->as.send.as.probe,w,e);
    case FIPS_LINK_PathAck: return fips_path_ack_encode(&v->as.send.as.ack,w,e);
    case FIPS_LINK_PathClose: return fips_path_close_encode(&v->as.send.as.close,w,e);
    default: break;
    }
    if(e) *e=(fips_proto_error){.variant=FIPS_PROTO_Malformed,.as.malformed="not a path Send action"};
    return FIPS_ERR_RANGE;
}
