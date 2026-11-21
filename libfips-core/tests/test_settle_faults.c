#include <fips/proto/settle/faults.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"faults:%d: %s\n",__LINE__,#x);abort();}}while(0)
static void *independent(void *ctx)
{ bool *ok=ctx;*ok=!SETTLE_FAULT(IntervalTen) && fips_settle_params_default().interval.secs==15;return NULL; }
void fips_test_settle_faults_api(void)
{
    fips_settle_faults *a=calloc(1,sizeof(*a)),*b=calloc(1,sizeof(*b));CHECK(a && b);
    for(unsigned i=0;i<FIPS_SETTLE_FAULT_COUNT;++i){fips_settle_fault f;const char *name=fips_settle_fault_name((fips_settle_fault)i);CHECK(name && fips_settle_fault_named(name,&f) && (unsigned)f==i && fips_settle_fault_boundary(f));}
    fips_settle_fault f=FIPS_SETTLE_FAULT_COUNT;CHECK(!fips_settle_fault_named("not-a-fault",&f) && f==FIPS_SETTLE_FAULT_COUNT);CHECK(!fips_settle_fault_name(f));
    CHECK(fips_settle_fault_select(a,FIPS_SETTLE_FAULT_IndexStale,true)==FIPS_ERR_RANGE && !a->bits[0]);
    a->bits[FIPS_SETTLE_FAULT_COUNT/64]=UINT64_C(1)<<63;fips_settle_faults *old=(fips_settle_faults *)(uintptr_t)1;
    CHECK(fips_settle_fault_enter(a,&old)==FIPS_ERR_RANGE && old==(fips_settle_faults *)(uintptr_t)1);memset(a->bits,0,sizeof(a->bits));
    CHECK(fips_settle_fault_select(a,FIPS_SETTLE_FAULT_IntervalTen,true)==FIPS_OK && fips_settle_fault_enter(a,&old)==FIPS_OK && !old);CHECK(fips_settle_params_default().interval.secs==10);
    CHECK(fips_settle_fault_enter(b,&old)==FIPS_OK && old==a && fips_settle_params_default().interval.secs==15);CHECK(fips_settle_fault_enter(old,NULL)==FIPS_OK && fips_settle_params_default().interval.secs==10);
    pthread_t worker;bool ok=false;CHECK(!pthread_create(&worker,NULL,independent,&ok));CHECK(fips_settle_params_default().interval.secs==10);CHECK(!pthread_join(worker,NULL) && ok);
    CHECK(fips_settle_fault_enter(NULL,&old)==FIPS_OK && old==a && fips_settle_params_default().interval.secs==15);
    free(a);free(b);puts("settle faults: catalog, fail-closed selection, nested caller scopes and thread isolation passed");
}
