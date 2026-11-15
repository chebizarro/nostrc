#include <fips/proto/path/set.h>
#include "internal.h"
void fips_path_set_new(fips_path_set *s) { memset(s,0,sizeof(*s)); s->active=FIPS_PATH_NONE; }
const fips_peer_path *fips_path_set_get(const fips_path_set *s,size_t i) { return i<s->len?&s->paths[i]:NULL; }
fips_peer_path *fips_path_set_path_mut(fips_path_set *s,size_t i) { return i<s->len?&s->paths[i]:NULL; }
const fips_peer_path *fips_path_set_active(const fips_path_set *s) { return fips_path_set_get(s,s->active); }
size_t fips_path_set_find(const fips_path_set *s,fips_transport_id t,const fips_transport_addr *a)
{ for(size_t i=0;i<s->len;++i) if(p_pair(s->paths[i].transport_id,&s->paths[i].addr,t,a)) return i; return FIPS_PATH_NONE; }
size_t fips_path_set_find_by_peer(const fips_path_set *s,fips_transport_id t,uint32_t id)
{ for(size_t i=0;i<s->len;++i) if(id&&s->paths[i].transport_id.value==t.value&&s->paths[i].peer_id==id) return i; return FIPS_PATH_NONE; }
fips_path_match fips_path_set_classify(const fips_path_set *s,fips_transport_id t,const fips_transport_addr *a)
{
    size_t i=fips_path_set_find(s,t,a);
    if(i==FIPS_PATH_NONE) return (fips_path_match){.kind=FIPS_PATH_MatchNone};
    const fips_peer_path *p=&s->paths[i];
    return (fips_path_match){p->acked||p->state==FIPS_PATH_Live?FIPS_PATH_Proven:FIPS_PATH_Unproven,s->active==i,p->state};
}
bool fips_path_set_learn_peer(fips_path_set *s,size_t i,uint32_t id)
{ if(!id||i>=s->len) return false; s->paths[i].peer_id=id; return true; }
void fips_path_set_mark_acked(fips_path_set *s,size_t i) { if(i<s->len) s->paths[i].acked=true; }
void fips_path_set_transition(fips_path_set *s,size_t i,fips_path_state state,fips_instant now)
{
    if(i>=s->len) return;
    fips_peer_path *p=&s->paths[i];
    if(state==FIPS_PATH_Dead) { if(p->state!=FIPS_PATH_Dead) p->dead_since=now; }
    else p->dead_since=(fips_instant){0};
    p->state=state;
}
bool fips_path_set_readdress(fips_path_set *s,size_t i,fips_transport_addr addr,fips_transport_addr *old)
{
    if(i>=s->len||fips_path_set_find(s,s->paths[i].transport_id,&addr)!=FIPS_PATH_NONE) return false;
    *old=s->paths[i].addr; s->paths[i].addr=addr; return true;
}
bool fips_path_set_discard(fips_path_set *s,size_t i,fips_peer_path *out)
{
    if(i>=s->len) return false;
    *out=s->paths[i]; --s->len;
    memmove(&s->paths[i],&s->paths[i+1],(s->len-i)*sizeof(s->paths[0]));
    memset(&s->paths[s->len],0,sizeof(s->paths[0]));
    if(s->active==i) s->active=FIPS_PATH_NONE;
    else if(s->active!=FIPS_PATH_NONE&&s->active>i) --s->active;
    return true;
}
bool fips_path_set_activate(fips_path_set *s,size_t i)
{ if(i>=s->len||!s->paths[i].acked) return false; s->active=i; return true; }
static size_t victim(const fips_path_set *s,bool only,fips_transport_id t)
{
    bool live=false; size_t acked=0,best=FIPS_PATH_NONE;
    for(size_t i=0;i<s->len;++i) { live|=s->paths[i].state==FIPS_PATH_Live; acked+=s->paths[i].state==FIPS_PATH_Dead&&s->paths[i].acked; }
    for(size_t i=0;i<s->len;++i) {
        const fips_peer_path *p=&s->paths[i];
        if(p->state!=FIPS_PATH_Dead||i==s->active||(only&&p->transport_id.value!=t.value)||(p->acked&&!live&&acked==1)) continue;
        if(best==FIPS_PATH_NONE||p->acked<s->paths[best].acked||
            (p->acked==s->paths[best].acked&&(!pt_has(p->dead_since)||
            (pt_has(s->paths[best].dead_since)&&pt_cmp(p->dead_since,s->paths[best].dead_since)<0)))) best=i;
    }
    return best;
}
fips_status fips_path_set_admit(fips_path_set *s,fips_transport_id t,fips_transport_addr a,fips_path_medium medium,fips_entropy e,fips_path_admission *out,fips_path_refused *error)
{
    size_t known=fips_path_set_find(s,t,&a);
    if(known!=FIPS_PATH_NONE) { *out=(fips_path_admission){.known=true,.index=known}; return FIPS_OK; }
    size_t count=0; for(size_t i=0;i<s->len;++i) count+=s->paths[i].transport_id.value==t.value;
    bool full=count>=(medium==FIPS_PATH_Ble?1:4);
    fips_path_admission admission={0};
    if(full||s->len==8) {
        size_t i=victim(s,full,t);
        if(i==FIPS_PATH_NONE) { if(error) *error=full?FIPS_PATH_TransportFull:FIPS_PATH_PeerFull; return FIPS_ERR_RANGE; }
        admission.has_evicted=fips_path_set_discard(s,i,&admission.evicted);
    }
    uint32_t id;
    for(;;) {
        id=p_draw(e); bool used=!id;
        for(size_t i=0;i<s->len;++i) used|=s->paths[i].path_id==id;
        if(!used) break;
    }
    admission.index=s->len;
    fips_peer_path *p=&s->paths[s->len++];
    *p=(fips_peer_path){.transport_id=t,.addr=a,.path_id=id,.state=FIPS_PATH_Probing};
    fips_path_liveness_new(&p->live); *out=admission; return FIPS_OK;
}
static fips_status bind(fips_path_set *s,fips_transport_id t,fips_transport_addr a,fips_path_medium medium,fips_entropy e,fips_path_admission *out,fips_path_refused *error)
{
    fips_path_admission v;
    if(fips_path_set_admit(s,t,a,medium,e,&v,error)!=FIPS_OK) return FIPS_ERR_RANGE;
    fips_peer_path *p=&s->paths[v.index]; p->state=FIPS_PATH_Live; p->dead_since=(fips_instant){0}; p->acked=true; s->active=v.index;
    *out=v; return FIPS_OK;
}
fips_status fips_path_set_bind_original(fips_path_set *s,fips_transport_id t,fips_transport_addr a,fips_entropy e,fips_path_admission *out,fips_path_refused *error)
{ return bind(s,t,a,FIPS_PATH_Unknown,e,out,error); }
fips_path_roam fips_path_set_roam(fips_path_set *s,fips_transport_id t,fips_transport_addr a,fips_entropy e)
{
    if(!s->len) {
        fips_path_admission v; fips_status status=bind(s,t,a,FIPS_PATH_Unknown,e,&v,NULL);
        return (fips_path_roam){.kind=status==FIPS_OK?FIPS_PATH_Bound:FIPS_PATH_Unmoved};
    }
    fips_peer_path *p=&s->paths[0];
    if(s->len!=1||p->transport_id.value!=t.value||fips_transport_addr_equal(&p->addr,&a)) return (fips_path_roam){0};
    fips_path_ref from={t,p->addr}; p->addr=a;
    return (fips_path_roam){.kind=s->active==0?FIPS_PATH_Roamed:FIPS_PATH_Standby,.from=from,.index=0};
}
fips_path_rebind fips_path_set_rebind(fips_path_set *s,fips_transport_id t,fips_transport_addr a,fips_path_medium medium,fips_entropy e,fips_path_admission *out)
{
    *out=(fips_path_admission){0}; size_t known=fips_path_set_find(s,t,&a);
    if(known!=FIPS_PATH_NONE&&known==s->active) return FIPS_PATH_Unchanged;
    if(known!=FIPS_PATH_NONE) {
        if(!s->paths[known].acked) return FIPS_PATH_Refused;
        s->active=known; return FIPS_PATH_Switched;
    }
    if(s->active==FIPS_PATH_NONE) return bind(s,t,a,medium,e,out,NULL)==FIPS_OK?FIPS_PATH_Switched:FIPS_PATH_Refused;
    fips_path_set_admit(s,t,a,medium,e,out,NULL); return FIPS_PATH_Refused;
}
