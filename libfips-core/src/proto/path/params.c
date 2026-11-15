#include "internal.h"
fips_path_config fips_path_config_draft(void)
{
    return (fips_path_config){.enabled=true,.switch_margin=1.3,.switch_dwell={2,0},.min_samples=2,
        .active_interval={0,200000000},.standby_interval={1,0},.mix_kinds=false,
        .timeout={0,400000000},.verdict_cap={3,0},.horizon={60,0},.suspect=3,.retire=10,.discovery=8,
        .heartbeat={10,0},.dead_interval={30,0},.grace={600,0},.auth_limit=20,.ring=8,.budget=3,.span={30,0},.cap={600,0}};
}
fips_status fips_path_params_new(fips_path_params *out,fips_path_config cfg,fips_path_params_error *e)
{
    const struct { fips_duration d; const char *name; } spans[]={
        {cfg.switch_dwell,"the switch dwell"},{cfg.active_interval,"the active probe interval"},
        {cfg.standby_interval,"the standby probe interval"},{cfg.timeout,"the probe timeout floor"},
        {cfg.verdict_cap,"the verdict cap"},{cfg.horizon,"the horizon"},{cfg.heartbeat,"the heartbeat interval"},
        {cfg.dead_interval,"the Dead probe interval cap"},{cfg.grace,"the Dead grace"},
        {cfg.span,"the capability probe span"},{cfg.cap,"the capability probe interval cap"}};
    for (size_t i=0;i<sizeof(spans)/sizeof(spans[0]);++i) if (!spans[i].d.secs && !spans[i].d.nanos) {
        if(e) *e=(fips_path_params_error){FIPS_PATH_PARAMS_Zero,spans[i].name}; return FIPS_ERR_RANGE;
    }
    const struct { uint8_t n; const char *name; } counts[]={
        {cfg.min_samples,"the minimum samples"},{cfg.suspect,"the Suspect count"},{cfg.retire,"the retire count"},
        {cfg.discovery,"the discovery probe count"},{cfg.auth_limit,"the authentication failure limit"},
        {cfg.ring,"the probe ring"},{cfg.budget,"the capability probe budget"}};
    for(size_t i=0;i<sizeof(counts)/sizeof(counts[0]);++i) if(!counts[i].n) {
        if(e) *e=(fips_path_params_error){FIPS_PATH_PARAMS_Zero,counts[i].name}; return FIPS_ERR_RANGE;
    }
    if(!isfinite(cfg.switch_margin)||cfg.switch_margin<1.0) {
        if(e) *e=(fips_path_params_error){FIPS_PATH_PARAMS_Margin,NULL}; return FIPS_ERR_RANGE;
    }
    if(cfg.ring>FIPS_PATH_RING) {
        if(e) *e=(fips_path_params_error){FIPS_PATH_PARAMS_Ring,NULL}; return FIPS_ERR_RANGE;
    }
    *out=(fips_path_params){cfg,pd_max(pd_mul(pd_mul(cfg.standby_interval,cfg.min_samples),2),(fips_duration){30,0})};
    return FIPS_OK;
}
fips_path_params fips_path_params_draft(void) { fips_path_params p; fips_path_params_new(&p,fips_path_config_draft(),NULL); return p; }
