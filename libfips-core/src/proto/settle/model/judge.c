#ifndef FIPS_TEST_HOOKS
#error "settlement model is test-only"
#endif
#include <fips/proto/settle/model/judge.h>
#include <string.h>
#define TRY(call) do { fips_status status_=(call); if (status_!=FIPS_OK) return status_; } while (0)
#define FIELD(s,name,value) do { (s)->has_##name=true; (s)->name=(value); } while (0)
typedef fips_settle_model_judge Judge;
typedef fips_settle_model_peering Peering;
typedef fips_settle_model_leg Leg;
typedef fips_settle_model_attempt Attempt;
typedef fips_settle_model_step Step;
typedef fips_settle_model_addr Addr;
static bool eq(Addr a,Addr b) { return a.transport==b.transport && a.at==b.at; }
static uint64_t age(uint64_t now,uint64_t at) { return now-at; }
fips_settle_model_params fips_settle_model_params_default(void)
{ return (fips_settle_model_params){.unheard=1000,.interval=15000,.linkage=30000,.sessionage=10000}; }
void fips_settle_model_init(Judge *j,uint8_t me,uint8_t peer,fips_settle_model_params p)
{ *j=(Judge){.me=me,.peer=peer,.params=p}; }
static void class_(Step *s,fips_settle_model_class c) { s->has_class=true; s->class_=c; }
static fips_status reply(Step *s,bool has_class,fips_settle_model_class c,fips_settle_model_reply r)
{ if (has_class) class_(s,c); FIELD(s,reply,r); return FIPS_OK; }
static fips_status change(Step *s,fips_settle_model_transition t) { FIELD(s,change,t); return FIPS_OK; }
static bool recorded(const Peering *p,uint32_t hs)
{ for (size_t i=0;i<p->record_count;++i) if (p->record[i]==hs) return true; return false; }
static fips_status record(Peering *p,uint32_t hs)
{ if (p->record_count==FIPS_SETTLE_MODEL_CAPACITY) return FIPS_ERR_RANGE; p->record[p->record_count++]=hs; return FIPS_OK; }
static bool eligible(const Judge *j,const Peering *p)
{ return age(j->now,p->linkborn)>=j->params.linkage && age(j->now,p->sessionborn)>=j->params.sessionage; }
static bool onlink(const Peering *p,Addr from)
{ return from.transport==p->setup.transport && (eq(from,p->current) || eq(from,p->setup)); }
static bool closed(const Judge *j,Addr at)
{ for (size_t i=0;i<j->closed_count;++i) if (eq(j->closed[i],at)) return true; return false; }
static bool reachable(const Judge *j,const Peering *p)
{ return p->binding==FIPS_MODEL_CONNECTIONLESS || !closed(j,p->current); }
static bool needed(const Judge *j,uint32_t except,Addr at)
{
    if (j->has_peering) {
        const Peering *p=&j->peering;
        if (p->link!=except && eq(p->setup,at)) return true;
        if (p->has_fallback && p->fallback.link!=except && eq(p->fallback.setup,at)) return true;
    }
    for (size_t i=0;i<j->attempt_count;++i) if (j->attempts[i].link!=except && eq(j->attempts[i].to,at)) return true;
    for (size_t i=0;i<j->leg_count;++i) if (j->legs[i].link!=except && eq(j->legs[i].from,at)) return true;
    return j->has_redial && j->redial.leg.link!=except && eq(j->redial.leg.from,at);
}
static void close_link(const Judge *j,uint32_t link,Addr setup,fips_settle_model_binding binding,Step *s)
{
    s->close_count=0;
    if (binding==FIPS_MODEL_CONNECTED && !needed(j,link,setup) && !closed(j,setup)) s->closes[s->close_count++]=setup;
}
static Peering answered(const Judge *j,Leg l)
{
    Peering p={.hs=l.hs,.epoch=l.epoch,.link=l.link,.binding=l.binding,.setup=l.from,.current=l.from,
        .has_msg1=true,.msg1=l.bytes,.installed=j->now,.linkborn=j->now,.sessionborn=j->now,.record_count=1};
    p.record[0]=l.hs; return p;
}
static fips_status install(Judge *j,fips_settle_model_class c,Leg l,bool sent,Step *s)
{
    if (!sent) {
        if (j->leg_count==FIPS_SETTLE_MODEL_CAPACITY) return FIPS_ERR_RANGE;
        j->legs[j->leg_count++]=l; return reply(s,true,c,FIPS_MODEL_REPLY_DEFERRED);
    }
    j->peering=answered(j,l); j->has_peering=true; class_(s,c); FIELD(s,reply,FIPS_MODEL_REPLY_MSG2); return change(s,FIPS_MODEL_TRANSITION_PROMOTE);
}
static bool gated(const Judge *j,const Peering *p,fips_settle_model_gate *gate)
{
    if (!p->heard && age(j->now,p->installed)<j->params.unheard) { *gate=FIPS_MODEL_GATE_UNHEARD; return true; }
    if (p->heard && age(j->now,p->has_last ? p->last : p->installed)<j->params.interval) { *gate=FIPS_MODEL_GATE_HEARD; return true; }
    if (j->has_stamp && age(j->now,j->stamp)<j->params.interval) { *gate=FIPS_MODEL_GATE_DAMPENED; return true; }
    return false;
}
static fips_status replace(Judge *j,fips_settle_model_class c,Leg l,bool sent,Step *s)
{
    Peering *p=&j->peering; fips_settle_model_gate gate;
    if (gated(j,p,&gate)) {
        if (c==FIPS_MODEL_CLASS_REPLACEMENT) TRY(record(p,l.hs));
        class_(s,c); FIELD(s,refused,gate); FIELD(s,reply,FIPS_MODEL_REPLY_SILENT); return FIPS_OK;
    }
    if (p->heard) { j->has_stamp=true; j->stamp=j->now; }
    j->has_peering=false; TRY(install(j,c,l,sent,s)); FIELD(s,rule,FIPS_MODEL_RULE_DAMPENER_STAMP);
    return change(s,s->has_change && s->change==FIPS_MODEL_TRANSITION_PROMOTE ? FIPS_MODEL_TRANSITION_REPLACE : FIPS_MODEL_TRANSITION_REMOVE);
}
static fips_status arm(Judge *j,Leg l,Step *s)
{
    Peering *p=&j->peering; TRY(record(p,l.hs)); p->has_pending=true;
    p->pending=(fips_settle_model_pending){.hs=l.hs,.has_armed=true,.armed=l.bytes}; FIELD(s,reply,FIPS_MODEL_REPLY_MSG2); return FIPS_OK;
}
static fips_status redialed(Judge *j,Leg l,bool sent,Step *s)
{
    fips_settle_model_gate gate=FIPS_MODEL_GATE_SLOT; bool refused=j->has_redial && age(j->now,j->redial.at)<j->params.unheard;
    if (!refused && j->has_stamp && age(j->now,j->stamp)<j->params.interval) { refused=true; gate=FIPS_MODEL_GATE_DAMPENED; }
    class_(s,FIPS_MODEL_CLASS_REDIAL); FIELD(s,rule,FIPS_MODEL_RULE_DEFERRED_REDIAL);
    if (refused) { FIELD(s,refused,gate); FIELD(s,reply,FIPS_MODEL_REPLY_SILENT); return FIPS_OK; }
    TRY(record(&j->peering,l.hs)); FIELD(s,reply,sent ? FIPS_MODEL_REPLY_MSG2 : FIPS_MODEL_REPLY_DEFERRED);
    if (j->has_redial) { Leg old=j->redial.leg; j->has_redial=false; close_link(j,old.link,old.from,old.binding,s); }
    j->has_redial=true; j->redial=(fips_settle_model_redial){.leg=l,.at=j->now,.sent=sent}; return FIPS_OK;
}
static fips_status msg1(Judge *j,Leg l,bool sent,Step *s)
{
    for (size_t i=0;i<j->leg_count;++i) if (eq(j->legs[i].from,l.from)) return reply(s,false,0,FIPS_MODEL_REPLY_STORED);
    if (j->has_redial && eq(j->redial.leg.from,l.from) && (!j->redial.sent || j->redial.leg.bytes==l.bytes)) return reply(s,false,0,FIPS_MODEL_REPLY_STORED);
    if (!j->has_peering) return install(j,FIPS_MODEL_CLASS_NOT_HELD,l,sent,s);
    Peering *p=&j->peering;
    if (l.epoch!=p->epoch) return replace(j,FIPS_MODEL_CLASS_RESTART,l,sent,s);
    if (!eligible(j,p)) {
        if (p->has_msg1 && p->msg1==l.bytes) return reply(s,true,FIPS_MODEL_CLASS_RESEND,FIPS_MODEL_REPLY_STORED);
        if (recorded(p,l.hs)) return reply(s,true,FIPS_MODEL_CLASS_COPY,FIPS_MODEL_REPLY_SILENT);
        if (p->heard && !onlink(p,l.from)) return redialed(j,l,sent,s);
        return replace(j,FIPS_MODEL_CLASS_REPLACEMENT,l,sent,s);
    }
    if (!onlink(p,l.from) && reachable(j,p)) {
        if (p->heard && !recorded(p,l.hs)) return redialed(j,l,sent,s);
        return reply(s,true,FIPS_MODEL_CLASS_OFF_LINK,FIPS_MODEL_REPLY_SILENT);
    }
    if (p->has_pending) return reply(s,true,FIPS_MODEL_CLASS_PENDING,!p->pending.initiator && p->pending.has_armed && p->pending.armed==l.bytes ? FIPS_MODEL_REPLY_STORED : FIPS_MODEL_REPLY_SILENT);
    if (recorded(p,l.hs)) return reply(s,true,FIPS_MODEL_CLASS_RECORDED,FIPS_MODEL_REPLY_SILENT);
    if (j->has_rekey) {
        if (j->me<j->peer) return reply(s,true,FIPS_MODEL_CLASS_SIMULTANEOUS,FIPS_MODEL_REPLY_SILENT);
        TRY(arm(j,l,s)); class_(s,FIPS_MODEL_CLASS_SIMULTANEOUS); s->open=true; return FIPS_OK;
    }
    TRY(arm(j,l,s)); class_(s,FIPS_MODEL_CLASS_REKEY); return FIPS_OK;
}
static void moveto(Peering *p,Attempt a,Addr from)
{ p->hs=a.hs; p->link=a.link; p->binding=a.binding; p->setup=a.to; p->dialed=true; p->current=from; p->heard=true; }
static fips_status msg2(Judge *j,uint32_t hs,uint32_t epoch,Addr from,Step *s)
{
    size_t i=0; while (i<j->attempt_count && j->attempts[i].hs!=hs) ++i; if (i==j->attempt_count) return FIPS_OK;
    Attempt a=j->attempts[i]; memmove(j->attempts+i,j->attempts+i+1,(--j->attempt_count-i)*sizeof(a));
    Peering next={.hs=a.hs,.epoch=epoch,.link=a.link,.binding=a.binding,.setup=a.to,.dialed=true,.current=from,
        .installed=j->now,.linkborn=j->now,.sessionborn=j->now,.heard=true};
    if (!j->has_peering) { j->has_peering=true; j->peering=next; return change(s,FIPS_MODEL_TRANSITION_PROMOTE); }
    Peering *p=&j->peering;
    if (epoch!=p->epoch) { *p=next; FIELD(s,rule,FIPS_MODEL_RULE_DAMPENER_STAMP); return change(s,FIPS_MODEL_TRANSITION_REPLACE); }
    if (a.aged) { FIELD(s,rule,FIPS_MODEL_RULE_AGED_LINK); s->open=true; return FIPS_OK; }
    if (p->dialed) {
        p->has_fallback=true; p->fallback=(fips_settle_model_fallback){.hs=p->hs,.link=p->link,.binding=p->binding,.setup=p->setup,.current=p->current};
        moveto(p,a,from); FIELD(s,rule,FIPS_MODEL_RULE_LATE_SECOND_DIAL); return change(s,FIPS_MODEL_TRANSITION_MOVE);
    }
    if (j->me<j->peer) {
        uint32_t link=p->link; Addr setup=p->setup; fips_settle_model_binding binding=p->binding;
        moveto(p,a,from); FIELD(s,change,FIPS_MODEL_TRANSITION_MOVE); close_link(j,link,setup,binding,s); return FIPS_OK;
    }
    FIELD(s,change,FIPS_MODEL_TRANSITION_KEEP); close_link(j,a.link,a.to,a.binding,s); return FIPS_OK;
}
static fips_status sent(Judge *j,uint32_t hs,Step *s)
{
    if (j->has_redial && j->redial.leg.hs==hs) {
        j->redial.sent=true; Leg l=j->redial.leg; FIELD(s,reply,FIPS_MODEL_REPLY_STORED);
        if (j->has_peering) { FIELD(s,rule,FIPS_MODEL_RULE_DEFERRED_REDIAL); return FIPS_OK; }
        j->has_redial=false; j->has_peering=true; j->peering=answered(j,l); return change(s,FIPS_MODEL_TRANSITION_PROMOTE);
    }
    size_t i=0; while (i<j->leg_count && j->legs[i].hs!=hs) ++i; if (i==j->leg_count) return FIPS_OK;
    Leg l=j->legs[i]; memmove(j->legs+i,j->legs+i+1,(--j->leg_count-i)*sizeof(l)); FIELD(s,reply,FIPS_MODEL_REPLY_STORED);
    if (j->has_peering) { FIELD(s,rule,FIPS_MODEL_RULE_STORED_LATE); return FIPS_OK; }
    j->has_peering=true; j->peering=answered(j,l); return change(s,FIPS_MODEL_TRANSITION_PROMOTE);
}
static fips_status timeout(Judge *j,uint32_t hs,Step *s)
{
    for (size_t i=0;i<j->attempt_count;++i) if (j->attempts[i].hs==hs) {
        Attempt a=j->attempts[i]; memmove(j->attempts+i,j->attempts+i+1,(--j->attempt_count-i)*sizeof(a)); close_link(j,a.link,a.to,a.binding,s); return FIPS_OK;
    }
    for (size_t i=0;i<j->leg_count;++i) if (j->legs[i].hs==hs) {
        Leg l=j->legs[i]; memmove(j->legs+i,j->legs+i+1,(--j->leg_count-i)*sizeof(l)); close_link(j,l.link,l.from,l.binding,s); return FIPS_OK;
    }
    if (j->has_redial && j->redial.leg.hs==hs) {
        Leg l=j->redial.leg; j->has_redial=false; close_link(j,l.link,l.from,l.binding,s); FIELD(s,rule,FIPS_MODEL_RULE_DEFERRED_REDIAL);
    }
    return FIPS_OK;
}
static fips_status frame(Judge *j,uint32_t hs,Addr from,uint8_t paths,bool message,Step *s)
{
    if (j->has_redial && j->redial.leg.hs==hs) {
        Leg l=j->redial.leg; j->has_redial=false; bool existed=j->has_peering;
        if (existed && j->peering.heard) { j->has_stamp=true; j->stamp=j->now; }
        j->peering=answered(j,l); j->has_peering=true; j->peering.heard=true; j->peering.has_last=true; j->peering.last=j->now; j->peering.current=from;
        FIELD(s,rule,FIPS_MODEL_RULE_DEFERRED_REDIAL); return change(s,existed ? FIPS_MODEL_TRANSITION_REPLACE : FIPS_MODEL_TRANSITION_PROMOTE);
    }
    if (!j->has_peering) return FIPS_OK;
    Peering *p=&j->peering;
    if (p->has_fallback && p->fallback.hs==hs) {
        fips_settle_model_fallback f=p->fallback; p->has_fallback=false; uint32_t link=p->link; Addr setup=p->setup; fips_settle_model_binding binding=p->binding;
        p->hs=f.hs; p->link=f.link; p->binding=f.binding; p->setup=f.setup; p->current=f.current; p->dialed=true;
        FIELD(s,change,FIPS_MODEL_TRANSITION_MOVE); FIELD(s,rule,FIPS_MODEL_RULE_LATE_SECOND_DIAL); close_link(j,link,setup,binding,s);
    } else if (p->hs==hs) {
        if (p->has_fallback) { fips_settle_model_fallback f=p->fallback; p->has_fallback=false; FIELD(s,rule,FIPS_MODEL_RULE_LATE_SECOND_DIAL); close_link(j,f.link,f.setup,f.binding,s); }
    } else if (p->has_pending && !p->pending.initiator && p->pending.hs==hs) {
        p->hs=hs; p->sessionborn=j->now; p->has_pending=false; FIELD(s,change,FIPS_MODEL_TRANSITION_REKEY_CUTOVER);
    } else return FIPS_OK;
    p->heard=true; p->has_last=true; p->last=j->now;
    bool roams=!paths || (paths==1 && !message && from.transport==p->current.transport);
    if (!eq(p->current,from) && !s->has_change && roams) { p->current=from; FIELD(s,change,FIPS_MODEL_TRANSITION_ROAM); }
    return FIPS_OK;
}
static fips_status step(Judge *j,fips_settle_model_event e,Step *s)
{
    if ((e.kind==FIPS_MODEL_DIAL || e.kind==FIPS_MODEL_MSG1) && (unsigned)e.binding>FIPS_MODEL_CONNECTED) return FIPS_ERR_RANGE;
    switch (e.kind) {
    case FIPS_MODEL_ADVANCE: if (e.ms>UINT64_MAX-j->now) return FIPS_ERR_RANGE; j->now+=e.ms; return FIPS_OK;
    case FIPS_MODEL_DIAL:
        if (j->attempt_count==FIPS_SETTLE_MODEL_CAPACITY) return FIPS_ERR_RANGE;
        j->attempts[j->attempt_count++]=(Attempt){.hs=e.hs,.link=e.link,.to=e.to,.binding=e.binding,.aged=j->has_peering && onlink(&j->peering,e.to) && eligible(j,&j->peering) && reachable(j,&j->peering)}; return FIPS_OK;
    case FIPS_MODEL_MSG1: return msg1(j,(Leg){.hs=e.hs,.bytes=e.bytes,.epoch=e.epoch,.link=e.link,.from=e.from,.binding=e.binding},e.sent,s);
    case FIPS_MODEL_MSG2_SENT: return sent(j,e.hs,s);
    case FIPS_MODEL_MSG2: return msg2(j,e.hs,e.epoch,e.from,s);
    case FIPS_MODEL_TIMEOUT: return timeout(j,e.hs,s);
    case FIPS_MODEL_FRAME: return frame(j,e.hs,e.from,e.paths,e.message,s);
    case FIPS_MODEL_SWITCH: if (j->has_peering) j->peering.current=e.to; return FIPS_OK;
    case FIPS_MODEL_FOLLOW: if (j->has_peering && eq(j->peering.current,e.from)) j->peering.current=e.to; return FIPS_OK;
    case FIPS_MODEL_REKEY_START: if (j->has_peering) { j->has_rekey=true; j->rekey=e.hs; } return FIPS_OK;
    case FIPS_MODEL_REKEY_MSG2:
        if (!j->has_rekey || j->rekey!=e.hs) return FIPS_OK;
        j->has_rekey=false;
        if (j->has_peering) { j->peering.has_pending=true; j->peering.pending=(fips_settle_model_pending){.hs=e.hs,.initiator=true,.has_restarted=e.epoch!=j->peering.epoch,.restarted=e.epoch}; } return FIPS_OK;
    case FIPS_MODEL_REKEY_CUTOVER: {
        if (!j->has_peering || !j->peering.has_pending || !j->peering.pending.initiator) return FIPS_OK;
        Peering *p=&j->peering; fips_settle_model_pending pd=p->pending; p->has_pending=false; p->hs=pd.hs; p->sessionborn=j->now;
        if (!pd.has_restarted) return change(s,FIPS_MODEL_TRANSITION_REKEY_CUTOVER);
        p->epoch=pd.restarted; p->linkborn=j->now; FIELD(s,rule,FIPS_MODEL_RULE_DAMPENER_STAMP); return change(s,FIPS_MODEL_TRANSITION_RESTARTED);
    }
    case FIPS_MODEL_CLOSE: if (j->closed_count==FIPS_SETTLE_MODEL_CAPACITY) return FIPS_ERR_RANGE; j->closed[j->closed_count++]=e.from; return FIPS_OK;
    case FIPS_MODEL_LOST: if (j->has_peering) { j->has_peering=false; FIELD(s,change,FIPS_MODEL_TRANSITION_REMOVE); } return FIPS_OK;
    case FIPS_MODEL_RESTART: fips_settle_model_init(j,j->me,j->peer,j->params); return FIPS_OK;
    }
    return FIPS_ERR_RANGE;
}
fips_status fips_settle_model_step_event(Judge *j,fips_settle_model_event event,Step *out)
{
    Judge copy=*j; Step result={0}; TRY(step(&copy,event,&result)); *j=copy; *out=result; return FIPS_OK;
}
fips_status fips_settle_model_run(Judge *j,const fips_settle_model_event *events,size_t count,Step *out,size_t capacity)
{
    if (count>capacity || count>FIPS_SETTLE_MODEL_CAPACITY) return FIPS_ERR_RANGE;
    Judge copy=*j; Step steps[FIPS_SETTLE_MODEL_CAPACITY];
    for (size_t i=0;i<count;++i) { steps[i]=(Step){0}; TRY(step(&copy,events[i],&steps[i])); }
    *j=copy; if (count) memcpy(out,steps,count*sizeof(*out)); return FIPS_OK;
}
bool fips_settle_model_settled(const Judge *j,fips_settle_model_need *out,uint32_t *hs)
{
    if (!j->has_peering) return false;
    *out=(fips_settle_model_need){.link=j->peering.link,.at=j->peering.current}; *hs=j->peering.hs; return true;
}
fips_status fips_settle_model_needs(const Judge *j,fips_settle_model_need *out,size_t capacity,size_t *count)
{
    size_t n=(j->has_peering ? 1u+(j->peering.has_fallback ? 1u : 0u) : 0u)+j->attempt_count+j->leg_count+(j->has_redial ? 1u : 0u);
    if (n>capacity) return FIPS_ERR_RANGE;
    size_t at=0;
    if (j->has_peering) { out[at++]=(fips_settle_model_need){j->peering.link,j->peering.setup}; if (j->peering.has_fallback) out[at++]=(fips_settle_model_need){j->peering.fallback.link,j->peering.fallback.setup}; }
    for (size_t i=0;i<j->attempt_count;++i) out[at++]=(fips_settle_model_need){j->attempts[i].link,j->attempts[i].to};
    for (size_t i=0;i<j->leg_count;++i) out[at++]=(fips_settle_model_need){j->legs[i].link,j->legs[i].from};
    if (j->has_redial) out[at++]=(fips_settle_model_need){j->redial.leg.link,j->redial.leg.from};
    *count=at; return FIPS_OK;
}
