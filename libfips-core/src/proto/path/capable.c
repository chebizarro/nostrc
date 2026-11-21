#include <fips/proto/path/capable.h>
#include <fips/proto/path/wire.h>
#include "internal.h"
void fips_path_capability_new(fips_path_capability *c) { memset(c,0,sizeof(*c)); }
fips_duration fips_path_capability_gap(uint32_t sent,const fips_path_config *cfg)
{ return sent<cfg->budget?pd_div(cfg->span,cfg->budget):fips_backoff(sent-cfg->budget,cfg->span,cfg->cap); }
bool fips_path_capability_due(const fips_path_capability *c,fips_instant now) { return !pt_has(c->out.at)&&pt_due(c->next_at,now); }
void fips_path_capability_send(fips_path_capability *c,fips_instant now,uint32_t path,uint32_t id,fips_duration timeout,const fips_path_config *cfg)
{
    c->sent=p_inc(c->sent); c->path=path;
    c->out=(fips_path_sent){id,now,fips_deadline(now,timeout),FIPS_PATH_PROBE_MIN_SIZE,timeout};
    c->next_at=fips_deadline(now,fips_path_capability_gap(c->sent,cfg));
}
bool fips_path_capability_take(fips_path_capability *c,fips_path_sent *out)
{ if(!pt_has(c->out.at)) return false; *out=c->out; c->out=(fips_path_sent){0}; return true; }
bool fips_path_capability_expire(fips_path_capability *c,fips_instant now,bool overdue,const fips_path_config *cfg,fips_path_sent *out,bool *round)
{
    if(!pt_has(c->out.at)||pt_cmp(c->out.due,now)>0||(overdue&&pt_cmp(c->out.due,now)==0)) return false;
    fips_path_capability_take(c,out); *round=!c->told&&c->sent>=cfg->budget; c->told|=*round; return true;
}
bool fips_path_capability_answer(fips_path_capability *c,uint32_t path,uint32_t id,fips_path_sent *out)
{ return c->path==path&&c->out.probe_id==id&&fips_path_capability_take(c,out); }
fips_instant fips_path_capability_wake(const fips_path_capability *c,bool eligible)
{ return pt_has(c->out.at)?c->out.due:eligible?c->next_at:(fips_instant){0}; }
