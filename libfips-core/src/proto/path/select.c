#include <fips/proto/path/select.h>
#include <fips/proto/mmp/algorithms.h>
#include <fips/proto/framing.h>
#include "internal.h"
bool fips_path_pin_names(const fips_path_pin *pin,const fips_path_ref *to)
{ return pin->transport.value==to->transport.value&&(!pin->has_addr||fips_transport_addr_equal(&pin->addr,&to->addr)); }
bool fips_path_heeded(bool oriented) { return !oriented; }
size_t fips_path_carried(size_t link,bool has_proven,size_t proven,bool oriented)
{ return has_proven&&fips_path_heeded(oriented)?p_min(link,p_add(proven,FIPS_FRAMING_FMP_FRAME_OVERHEAD)):link; }
bool fips_path_select_score(const fips_path_liveness *l,double *out)
{ fips_duration d; if(!fips_path_liveness_min_rtt(l,&d)) return false; *out=fips_mmp_quality_index(l->etx,((double)d.secs+(double)d.nanos/1e9)*1000.0); return true; }
bool fips_path_select_steady(const fips_path_liveness *l,double *out)
{ fips_duration d; if(!fips_path_liveness_base_rtt(l,&d)) return false; *out=fips_mmp_quality_index(l->etx,((double)d.secs+(double)d.nanos/1e9)*1000.0); return true; }
static int speed(fips_path_medium m) { return m==FIPS_PATH_Wired?0:m==FIPS_PATH_Wireless?1:m==FIPS_PATH_Ble?2:-1; }
bool fips_path_select_quicker(fips_path_medium a,fips_path_medium b) { int x=speed(a),y=speed(b); return x>=0&&y>=0&&x<y; }
bool fips_path_select_held(const fips_path_rival *to,const fips_path_rival *from,double margin)
{
    bool kept=fips_path_select_quicker(from->medium,to->medium)&&from->has_steady&&to->has_steady&&from->steady<=to->steady*margin;
    bool faster=!kept&&to->has_base&&from->has_base&&pd_cmp(to->base,from->base)<0;
    bool beats=to->has_score&&from->steady>to->score*margin;
    return from->has_steady&&!faster&&!beats;
}
bool fips_path_select_waits(const fips_path_rival *to,const fips_path_rival *from,double margin)
{ return from->has_steady&&to->has_score&&to->score>from->steady*margin; }
uint8_t fips_path_select_tier(fips_path_role role) { return role==FIPS_PATH_Never?0:role==FIPS_PATH_Backup?1:2; }
static uint8_t ranked(const fips_path_rival f[8])
{
    bool pinned=false,fit=false; uint8_t top=0,mask=0;
    for(size_t i=0;i<8;++i) if(f[i].used) pinned|=f[i].pinned;
    for(size_t i=0;i<8;++i) if(f[i].used&&(!pinned||f[i].pinned)&&f[i].tier>top) top=f[i].tier;
    for(size_t i=0;i<8;++i) if(f[i].used&&(!pinned||f[i].pinned)&&f[i].tier==top) fit|=f[i].fits;
    for(size_t i=0;i<8;++i) if(f[i].used&&(!pinned||f[i].pinned)&&f[i].tier==top&&(!fit||f[i].fits)) mask|=(uint8_t)(1u<<i);
    return mask;
}
/* Scores from MMP are finite. Preserve Rust total_cmp for exposed field tests. */
static int total(double a,double b)
{
    uint64_t x,y; memcpy(&x,&a,8); memcpy(&y,&b,8);
    x=(x>>63)?~x:x^UINT64_C(0x8000000000000000);
    y=(y>>63)?~y:y^UINT64_C(0x8000000000000000);
    return x<y?-1:x>y?1:0;
}
static bool less(const fips_path_rival *a,const fips_path_rival *b)
{ int c=total(a->score,b->score); return c<0||(c==0&&a->index<b->index); }
bool fips_path_select_best(const fips_path_rival f[8],double *out)
{
    uint8_t mask=ranked(f); bool found=false; double best=0;
    for(size_t i=0;i<8;++i) if((mask&(1u<<i))&&f[i].has_score&&(!found||total(f[i].score,best)<0)) { best=f[i].score; found=true; }
    if(found) *out=best;
    return found;
}
size_t fips_path_select_choose(const fips_path_rival f[8],double margin)
{
    uint8_t mask=ranked(f); size_t best=FIPS_PATH_NONE;
    for(size_t i=0;i<8;++i) if((mask&(1u<<i))&&f[i].has_score&&(best==FIPS_PATH_NONE||less(&f[i],&f[best]))) best=i;
    if(best!=FIPS_PATH_NONE) {
        double score=f[best].score; size_t preferred=FIPS_PATH_NONE;
        for(size_t i=0;i<8;++i) if((mask&(1u<<i))&&f[i].has_score&&f[i].preferred&&f[i].score<=score*margin&&
            (preferred==FIPS_PATH_NONE||less(&f[i],&f[preferred]))) preferred=i;
        size_t pick=preferred==FIPS_PATH_NONE?best:preferred,quick=FIPS_PATH_NONE;
        for(size_t i=0;i<8;++i) if((mask&(1u<<i))&&f[i].has_score&&f[i].score<=score*margin&&
            (preferred==FIPS_PATH_NONE||f[i].preferred)&&fips_path_select_quicker(f[i].medium,f[pick].medium)) {
            if(quick==FIPS_PATH_NONE||speed(f[i].medium)<speed(f[quick].medium)||
                (speed(f[i].medium)==speed(f[quick].medium)&&less(&f[i],&f[quick]))) quick=i;
        }
        return f[quick==FIPS_PATH_NONE?pick:quick].index;
    }
    for(size_t i=0;i<8;++i) if(mask&(1u<<i)) {
        if(best==FIPS_PATH_NONE||f[i].has_rtt>f[best].has_rtt||
            (f[i].has_rtt==f[best].has_rtt&&((f[i].has_rtt&&pd_cmp(f[i].rtt,f[best].rtt)<0)||
            ((!f[i].has_rtt||pd_cmp(f[i].rtt,f[best].rtt)==0)&&f[i].index<f[best].index)))) best=i;
    }
    return best==FIPS_PATH_NONE?best:f[best].index;
}
fips_path_hold fips_path_hold_new(double cost,fips_instant at) { return (fips_path_hold){cost,at,2}; }
void fips_path_hold_report(fips_path_hold *h) { if(h->owed) --h->owed; }
bool fips_path_hold_held(const fips_path_hold *h,fips_instant now,fips_duration dwell,double *out)
{ if(!pt_has(h->at)||(!h->owed&&pt_cmp(now,fips_deadline(h->at,dwell))>=0)) return false; *out=h->cost; return true; }
