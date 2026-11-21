#ifndef FIPS_TEST_HOOKS
#error "Settlement fault switches are test-only"
#endif
#include <fips/proto/settle/faults.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "faults_data.inc"
_Static_assert(sizeof(faults)/sizeof(*faults)==FIPS_SETTLE_FAULT_COUNT,"pinned fault catalog");
static _Thread_local fips_settle_faults *active;
const char *fips_settle_fault_name(fips_settle_fault f)
{ return (unsigned)f<FIPS_SETTLE_FAULT_COUNT ? faults[f].name : NULL; }
bool fips_settle_fault_named(const char *name,fips_settle_fault *out)
{ for(unsigned f=0;f<FIPS_SETTLE_FAULT_COUNT;++f)if(!strcmp(name,faults[f].name)){*out=(fips_settle_fault)f;return true;}return false; }
bool fips_settle_fault_supported(fips_settle_fault f)
{ return (unsigned)f<FIPS_SETTLE_FAULT_COUNT && faults[f].supported; }
const char *fips_settle_fault_boundary(fips_settle_fault f)
{ return (unsigned)f<FIPS_SETTLE_FAULT_COUNT ? faults[f].boundary : "unknown fault"; }
fips_status fips_settle_fault_select(fips_settle_faults *set,fips_settle_fault f,bool enabled)
{
    if((unsigned)f>=FIPS_SETTLE_FAULT_COUNT || (enabled&&!fips_settle_fault_supported(f)))return FIPS_ERR_RANGE;
    uint64_t bit=UINT64_C(1)<<((unsigned)f%64);if(enabled)set->bits[(unsigned)f/64]|=bit;else set->bits[(unsigned)f/64]&=~bit;return FIPS_OK;
}
fips_status fips_settle_fault_enter(fips_settle_faults *set,fips_settle_faults **previous)
{
    if(set){unsigned tail=FIPS_SETTLE_FAULT_COUNT%64;if(tail && (set->bits[FIPS_SETTLE_FAULT_COUNT/64]>>tail))return FIPS_ERR_RANGE;
        for(unsigned f=0;f<FIPS_SETTLE_FAULT_COUNT;++f)if((set->bits[f/64]&(UINT64_C(1)<<(f%64)))&&!faults[f].supported)return FIPS_ERR_RANGE;}
    if(previous)*previous=active;active=set;return FIPS_OK;
}
bool fips_settle_fault_on(fips_settle_fault f)
{ return active && (unsigned)f<FIPS_SETTLE_FAULT_COUNT && (active->bits[(unsigned)f/64]&(UINT64_C(1)<<((unsigned)f%64))); }
void fips_settle_fault_resends_begin(void)
{ if(active)active->next_resend=0; }
const uint8_t *fips_settle_fault_resend(const uint8_t *bytes,size_t length,size_t *written)
{
    /* Test scopes bound one poll to64 outputs; exceeding a mutant's scratch
     * arena aborts rather than permitting memory corruption. Production never
     * calls this function and retains its original borrowed byte storage. */
    if(!active || length>FIPS_SETTLE_MESSAGE_CAPACITY || active->next_resend==FIPS_SETTLE_FAULT_RESENDS_MAX)abort();
    uint8_t *out=active->resends[active->next_resend++];if(length)memcpy(out,bytes,length);out[length]=0;*written=length+1;return out;
}
fips_instant fips_settle_fault_deadline(fips_instant now,fips_duration after)
{
    if(SETTLE_FAULT(DeadlineWraps)){fips_instant out;if(now.ops->checked_add(now,after,&out)!=FIPS_OK){assert(!"unchecked settle deadline");abort();}return out;}
    return fips_deadline(now,after);
}
