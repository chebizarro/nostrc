#include <fips/proto/path/candidate.h>
#include "internal.h"
bool fips_path_sources_has(fips_path_sources s,fips_path_source source) { return (s&(1u<<source))!=0; }
void fips_path_candidates_new(fips_path_candidates *c) { memset(c,0,sizeof(*c)); }
size_t fips_path_candidates_find(const fips_path_candidates *c,fips_transport_id t,const fips_transport_addr *a)
{ for(size_t i=0;i<8;++i) if(c->slots[i].used&&p_pair(c->slots[i].transport,&c->slots[i].addr,t,a)) return i; return FIPS_PATH_NONE; }
static size_t room(const fips_path_candidates *c,const fips_path_set *s)
{
    for(size_t i=0;i<8;++i) if(!c->slots[i].used) return i;
    for(unsigned pass=0;pass<2;++pass) {
        size_t best=FIPS_PATH_NONE;
        for(size_t i=0;i<8;++i) {
            const fips_path_entry *e=&c->slots[i];
            if(fips_path_set_find(s,e->transport,&e->addr)!=FIPS_PATH_NONE) continue;
            if(pass==0?(!e->held||e->sources):e->held) continue;
            if(best==FIPS_PATH_NONE||e->seq<c->slots[best].seq) best=i;
        }
        if(best!=FIPS_PATH_NONE) return best;
    }
    return FIPS_PATH_NONE;
}
fips_path_report fips_path_candidates_report(fips_path_candidates *c,fips_transport_id t,fips_transport_addr a,fips_path_source source,const fips_path_set *paths)
{
    ++c->seq; size_t slot=fips_path_candidates_find(c,t,&a);
    if(slot!=FIPS_PATH_NONE) {
        fips_path_entry *e=&c->slots[slot]; e->seq=c->seq;
        if(fips_path_sources_has(e->sources,source)&&source!=FIPS_PATH_Request) return (fips_path_report){FIPS_PATH_Repeat,slot};
        e->sources|=(uint8_t)(1u<<source); e->held=false; return (fips_path_report){FIPS_PATH_Again,slot};
    }
    slot=room(c,paths);
    if(slot==FIPS_PATH_NONE) return (fips_path_report){FIPS_PATH_Full,slot};
    c->slots[slot]=(fips_path_entry){.used=true,.transport=t,.addr=a,.sources=(uint8_t)(1u<<source),.seq=c->seq};
    return (fips_path_report){FIPS_PATH_New,slot};
}
bool fips_path_candidates_unlist(fips_path_candidates *c,fips_transport_id t,const fips_transport_addr *a,fips_path_source source)
{
    size_t i=fips_path_candidates_find(c,t,a); if(i==FIPS_PATH_NONE) return false;
    fips_path_entry *e=&c->slots[i]; e->sources&=(uint8_t)~(1u<<source);
    if(!e->sources&&!e->held) { e->used=false; return true; } return false;
}
void fips_path_candidates_hold(fips_path_candidates *c,fips_transport_id t,fips_transport_addr a,const fips_path_set *paths)
{
    size_t i=fips_path_candidates_find(c,t,&a);
    if(i!=FIPS_PATH_NONE) { c->slots[i].held=true; c->slots[i].refused=FIPS_PATH_RuleNone; return; }
    i=room(c,paths); if(i==FIPS_PATH_NONE) return;
    ++c->seq; c->slots[i]=(fips_path_entry){.used=true,.transport=t,.addr=a,.held=true,.seq=c->seq};
}
void fips_path_candidates_absent(fips_path_candidates *c,fips_transport_id t)
{ for(size_t i=0;i<8;++i) if(c->slots[i].used&&c->slots[i].transport.value==t.value) { c->slots[i].held=true; c->slots[i].refused=FIPS_PATH_RuleNone; } }
void fips_path_candidates_mark(fips_path_candidates *c,size_t i,fips_path_rule r) { if(i<8&&c->slots[i].used) c->slots[i].refused=r; }
uint8_t fips_path_candidates_release(fips_path_candidates *c,fips_transport_id t)
{
    uint8_t mask=0;
    for(size_t i=0;i<8;++i) if(c->slots[i].used&&c->slots[i].transport.value==t.value&&c->slots[i].held) { c->slots[i].held=false; mask|=(uint8_t)(1u<<i); }
    return mask;
}
void fips_path_candidates_prune(fips_path_candidates *c,uint8_t mask,const fips_path_set *s)
{ for(size_t i=0;i<8;++i) { fips_path_entry *e=&c->slots[i]; if((mask&(1u<<i))&&e->used&&!e->sources&&!e->held&&fips_path_set_find(s,e->transport,&e->addr)==FIPS_PATH_NONE) e->used=false; } }
void fips_path_candidates_forget(fips_path_candidates *c,fips_transport_id t)
{ for(size_t i=0;i<8;++i) if(c->slots[i].used&&c->slots[i].transport.value==t.value) c->slots[i].used=false; }
