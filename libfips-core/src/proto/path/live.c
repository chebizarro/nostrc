#include <fips/proto/path/live.h>
#include <fips/proto/path/wire.h>
#include <fips/proto/mmp/limits.h>
#include "internal.h"
void fips_path_liveness_new(fips_path_liveness *l) { memset(l,0,sizeof(*l)); l->etx=1.0; }
size_t fips_path_liveness_outstanding(const fips_path_liveness *l)
{ size_t n=0; for(size_t i=0;i<FIPS_PATH_RING;++i) n+=pt_has(l->ring[i].at); return n; }
size_t fips_path_liveness_samples(const fips_path_liveness *l)
{ size_t n=0; for(size_t i=0;i<32;++i) n+=pt_has(l->rtts[i].at); return n; }
static bool sample_extreme(const fips_path_liveness *l,bool max,fips_duration *out)
{
    bool found=false; fips_duration d={0};
    for(size_t i=0;i<32;++i) if(pt_has(l->rtts[i].at)) {
        if(!found || (max?pd_cmp(l->rtts[i].rtt,d)>0:pd_cmp(l->rtts[i].rtt,d)<0)) d=l->rtts[i].rtt;
        found=true;
    }
    if(found) *out=d;
    return found;
}
bool fips_path_liveness_min_rtt(const fips_path_liveness *l,fips_duration *out) { return sample_extreme(l,false,out); }
bool fips_path_liveness_max_rtt(const fips_path_liveness *l,fips_duration *out) { return sample_extreme(l,true,out); }
bool fips_path_liveness_base_rtt(const fips_path_liveness *l,fips_duration *out)
{
    bool found=false; fips_duration d={0};
    for(size_t i=0;i<8;++i) if(pt_has(l->spans[i].from)) {
        if(!found||pd_cmp(l->spans[i].rtt,d)<0) d=l->spans[i].rtt;
        found=true;
    }
    if(found) *out=d;
    return found;
}
fips_instant fips_path_liveness_earliest_due(const fips_path_liveness *l)
{ fips_instant at={0}; for(size_t i=0;i<8;++i) if(pt_has(l->ring[i].at)) at=pt_min(at,l->ring[i].due); return at; }
void fips_path_liveness_halt(fips_path_liveness *l,fips_instant at,bool active)
{
    if(pt_has(l->stop.free)) return;
    bool recheck=active||l->carried;
    fips_instant from=pt_has(l->quiet)?l->quiet:at;
    l->carried=false;
    l->stop.free=fips_deadline(at,FIPS_PATH_DEAD_PROBE_INTERVAL_MIN);
    l->stop.gap=l->stop.used=FIPS_PATH_DEAD_PROBE_INTERVAL_MIN;
    l->stop.until=recheck?fips_deadline(from,FIPS_PATH_DEAD_RECHECK_SPAN):(fips_instant){0};
    l->stop.owed=false;
}
void fips_path_liveness_spend(fips_path_liveness *l,fips_instant at)
{
    if(!pt_has(l->stop.free)) return;
    l->stop.used=l->stop.gap;
    l->stop.free=fips_deadline(at,l->stop.gap);
    l->stop.gap=pt_has(l->stop.until)&&pt_cmp(at,l->stop.until)<0?FIPS_PATH_DEAD_PROBE_INTERVAL_MIN:
        pd_min(pd_mul(l->stop.gap,2),FIPS_PATH_DEAD_PROBE_INTERVAL_MAX);
    l->stop.owed=false;
}
void fips_path_liveness_rebase(fips_path_liveness *l,fips_instant at,bool ok)
{
    if(!pt_has(l->stop.free)||!l->stop.owed) return;
    l->stop.owed=false;
    if(ok) l->stop.free=pt_max(l->stop.free,fips_deadline(at,l->stop.used));
}
void fips_path_liveness_resume(fips_path_liveness *l) { l->quiet=(fips_instant){0}; memset(&l->stop,0,sizeof(l->stop)); }
fips_duration fips_path_liveness_timeout(const fips_path_liveness *l,fips_duration floor,fips_duration cap)
{
    fips_duration latest=l->has_rtt?pd_mul(l->rtt,3):(fips_duration){0},largest={0},r;
    if(fips_path_liveness_max_rtt(l,&r)) largest=pd_mul(r,3);
    return pd_max(pd_max(floor,latest),pd_min(largest,cap));
}
uint32_t fips_path_liveness_take_id(fips_path_liveness *l,fips_entropy e)
{
    uint32_t id=l->has_next_id?l->next_id:p_draw(e);
    l->has_next_id=true; l->next_id=id+1; return id;
}
static bool counts(const fips_path_liveness *l,uint32_t id) { return !l->has_since||!p_after(l->since,id); }
bool fips_path_liveness_send(fips_path_liveness *l,fips_instant at,fips_instant due,size_t len,fips_entropy e,uint32_t *id)
{
    size_t slot=8; bool all=true;
    for(size_t i=0;i<8;++i) {
        if(!pt_has(l->ring[i].at)) { if(slot==8) slot=i; }
        else if(!counts(l,l->ring[i].probe_id)) all=false;
    }
    if(slot==8) return false;
    if(all) l->has_since=false;
    uint32_t probe=fips_path_liveness_take_id(l,e);
    if(!pt_has(l->quiet)) l->quiet=at;
    l->has_last=true; l->last=len;
    l->ring[slot]=(fips_path_sent){probe,at,due,len}; *id=probe; return true;
}
void fips_path_liveness_lapse(fips_path_liveness *l,fips_path_sent sent)
{ memmove(l->late,l->late+1,3*sizeof(l->late[0])); l->late[3]=sent; }
static void compact(fips_path_liveness *l)
{
    size_t w=0;
    for(size_t r=0;r<8;++r) if(pt_has(l->ring[r].at)) l->ring[w++]=l->ring[r];
    while(w<8) l->ring[w++]=(fips_path_sent){0};
}
fips_path_answer fips_path_liveness_answer(fips_path_liveness *l,uint32_t id)
{
    for(size_t i=0;i<8;++i) if(pt_has(l->ring[i].at)&&l->ring[i].probe_id==id) {
        fips_path_sent sent=l->ring[i]; l->ring[i]=(fips_path_sent){0}; compact(l); l->misses=0;
        if(!l->has_answered||p_after(id,l->answered)) { l->has_answered=true; l->answered=id; }
        return (fips_path_answer){FIPS_PATH_InTime,sent};
    }
    for(size_t i=0;i<4;++i) if(pt_has(l->late[i].at)&&l->late[i].probe_id==id) {
        fips_path_sent sent=l->late[i]; l->late[i]=(fips_path_sent){0};
        return (fips_path_answer){FIPS_PATH_Late,sent};
    }
    return (fips_path_answer){0};
}
static bool passed(fips_instant due,fips_instant now,bool overdue) { int c=pt_cmp(due,now); return c<0||(!overdue&&c==0); }
bool fips_path_liveness_expire(fips_path_liveness *l,fips_instant now,bool overdue,fips_duration cap,bool *counted)
{
    for(size_t i=0;i<8;++i) {
        fips_path_sent sent=l->ring[i];
        if(!pt_has(sent.at)||!passed(sent.due,now,overdue)) continue;
        if(pt_has(l->data_at)&&pt_cmp(l->data_at,sent.at)>0) {
            fips_instant until=fips_deadline(sent.at,cap);
            if(!passed(until,now,overdue)) { l->ring[i].due=until; continue; }
        }
        l->ring[i]=(fips_path_sent){0}; compact(l); fips_path_liveness_lapse(l,sent);
        if((!l->has_epoch||!p_after(l->epoch,sent.probe_id))&&sent.len>27) l->fallen=true;
        bool c=counts(l,sent.probe_id)&&(!l->has_answered||!p_after(l->answered,sent.probe_id));
        if(c) l->misses=p_inc(l->misses);
        *counted=c; return true;
    }
    return false;
}
void fips_path_liveness_fresh(fips_path_liveness *l)
{
    l->misses=0; l->has_epoch=l->has_next_id; l->epoch=l->next_id;
    memset(&l->seek,0,sizeof(l->seek)); l->fallen=false; l->has_last=false;
}
void fips_path_liveness_thaw(fips_path_liveness *l) { l->has_since=l->has_next_id; l->since=l->next_id; }
size_t fips_path_liveness_size(const fips_path_liveness *l,size_t full)
{
    if(!l->has_last||!l->fallen||l->last<=27) return full;
    /* Avoid overflow for caller-provided large MTUs. */
    size_t steps[]={full,full/4*3+(full%4)*3/4,full/2};
    for(size_t i=0;i<3;++i) if(steps[i]<l->last&&steps[i]>27) return steps[i];
    return 27;
}
void fips_path_liveness_clear(fips_path_liveness *l)
{ for(size_t i=0;i<8;++i) if(pt_has(l->ring[i].at)) { fips_path_liveness_lapse(l,l->ring[i]); l->ring[i]=(fips_path_sent){0}; } }
void fips_path_liveness_sample(fips_path_liveness *l,fips_instant now,fips_duration rtt,fips_duration window,fips_duration horizon)
{
    l->has_rtt=true; l->rtt=rtt;
    size_t slot=32,oldest=0;
    for(size_t i=0;i<32;++i) {
        if(pt_has(l->rtts[i].at)&&pd_cmp(pt_since(now,l->rtts[i].at),window)>0) l->rtts[i].at=(fips_instant){0};
        if(!pt_has(l->rtts[i].at)) { if(slot==32) slot=i; }
        else if(pt_has(l->rtts[oldest].at)&&pt_cmp(l->rtts[i].at,l->rtts[oldest].at)<0) oldest=i;
    }
    if(slot==32) slot=oldest;
    l->rtts[slot]=(fips_path_sample){now,rtt};
    slot=8; oldest=0; size_t newest=8;
    for(size_t i=0;i<8;++i) {
        if(pt_has(l->spans[i].from)&&pd_cmp(pt_since(now,l->spans[i].to),horizon)>0) l->spans[i].from=(fips_instant){0};
        if(!pt_has(l->spans[i].from)) { if(slot==8) slot=i; }
        else {
            if(newest==8||pt_cmp(l->spans[i].from,l->spans[newest].from)>0) newest=i;
            if(pt_has(l->spans[oldest].from)&&pt_cmp(l->spans[i].from,l->spans[oldest].from)<0) oldest=i;
        }
    }
    if(newest<8&&pd_cmp(pt_since(now,l->spans[newest].from),pd_div(horizon,6))<0) {
        l->spans[newest].to=now; l->spans[newest].rtt=pd_min(l->spans[newest].rtt,rtt); return;
    }
    if(slot==8) slot=oldest;
    l->spans[slot]=(fips_path_span){now,now,rtt};
}
void fips_path_liveness_fold(fips_path_liveness *l,bool delivered)
{
    double ratio=fmin(1.0,fmax(0.01,1.0/l->etx));
    double next=ratio+FIPS_MMP_EWMA_LONG_ALPHA*((delivered?1.0:0.0)-ratio);
    l->etx=fmin(100.0,fmax(1.0,1.0/fmax(next,0.01)));
}
bool fips_path_liveness_prove(fips_path_liveness *l,size_t mtu)
{ bool changed=!l->has_mtu||l->mtu!=mtu; l->has_mtu=true; l->mtu=mtu; return changed; }
bool fips_path_liveness_seek_size(const fips_path_liveness *l,size_t full,fips_instant now,size_t *out)
{
    if(l->seek.len||!pt_due(l->seek.at,now)||!l->has_mtu) return false;
    full=p_min(full,UINT16_MAX);
    if(l->mtu>=full) return false;
    if(!l->seek.lost) { *out=full; return true; }
    if(l->seek.lost>p_add(l->mtu,32)) { *out=l->mtu+(l->seek.lost-l->mtu)/2; return true; }
    return false;
}
uint32_t fips_path_liveness_seek_send(fips_path_liveness *l,fips_instant at,size_t len,fips_entropy e)
{
    uint32_t id=fips_path_liveness_take_id(l,e);
    l->seek.at=at; l->seek.probe_id=id; l->seek.len=(uint16_t)p_min(len,UINT16_MAX); return id;
}
static size_t unseek(fips_path_liveness *l,fips_duration gap)
{
    size_t len=l->seek.len; l->seek.len=0;
    if(pt_has(l->seek.at)) l->seek.at=fips_deadline(l->seek.at,gap);
    return len;
}
bool fips_path_liveness_seek_answer(fips_path_liveness *l,uint32_t id,fips_duration gap,size_t *out)
{
    if(!l->seek.len||l->seek.probe_id!=id) return false;
    *out=unseek(l,gap); return true;
}
static void seek_end(fips_path_liveness *l,fips_instant now)
{
    size_t proven=l->has_mtu?l->mtu:27;
    if(l->seek.lost&&l->seek.lost<=p_add(proven,32)) {
        l->seek.lost=0; l->seek.at=fips_deadline(now,(fips_duration){600,0});
    }
}
static void lose(fips_path_liveness *l,size_t len,fips_instant now)
{
    uint16_t n=(uint16_t)p_min(len,UINT16_MAX);
    l->seek.lost=l->seek.lost?(uint16_t)p_min(l->seek.lost,n):n; seek_end(l,now);
}
bool fips_path_liveness_seek_proved(fips_path_liveness *l,size_t sent,size_t acked,fips_instant now)
{
    size_t size=p_min(sent,acked); bool raised=size>27&&(!l->has_mtu||size>l->mtu);
    if(raised) { l->has_mtu=true; l->mtu=size; }
    if(acked<sent) lose(l,sent,now); else seek_end(l,now);
    return raised;
}
void fips_path_liveness_seek_judge(fips_path_liveness *l,fips_instant now,fips_duration timeout,fips_duration gap)
{
    if(!l->seek.len||!pt_has(l->seek.at)||!pt_due(fips_deadline(l->seek.at,timeout),now)||
        !l->has_answered||!p_after(l->answered,l->seek.probe_id)) return;
    size_t len=unseek(l,gap); lose(l,len,now);
}
void fips_path_liveness_heard(fips_path_liveness *l,size_t len,fips_instant now)
{ ++l->frames; l->bytes+=(uint32_t)len; l->heard_at=now; l->fails=0; }
bool fips_path_liveness_settle(fips_path_liveness *l,bool ok)
{
    if(l->connect!=FIPS_PATH_Pending) return false;
    l->connect=ok?FIPS_PATH_Up:FIPS_PATH_Down;
    if(!ok) l->misses=p_inc(l->misses);
    return true;
}
