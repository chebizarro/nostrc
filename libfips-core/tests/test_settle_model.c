#include <fips/proto/settle/model/judge.h>
#include <fips/proto/settle/gates.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"model:%d: %s\n",__LINE__,#x); abort(); } } while (0)
void fips_test_settle_model(void)
{
    fips_settle_model_params p=fips_settle_model_params_default(); fips_settle_params core=fips_settle_params_default();
    /* Upstream gates.rs::the_default_intervals_are_the_frcs, with independent
     * implementations rather than two aliases of the production constants. */
    CHECK(p.unheard==core.unheard.secs*1000+core.unheard.nanos/1000000);
    CHECK(p.interval==core.interval.secs*1000+core.interval.nanos/1000000);
    CHECK(p.linkage==30000 && p.sessionage==10000);
    fips_settle_model_judge j,before; fips_settle_model_step out={.open=true,.disconnect=true},saved=out;
    fips_settle_model_init(&j,1,2,p);
    fips_settle_model_event e={.kind=FIPS_MODEL_DIAL,.hs=1,.link=1,.to={1,2},.binding=FIPS_MODEL_CONNECTIONLESS};
    for (unsigned i=0;i<FIPS_SETTLE_MODEL_CAPACITY;++i) { e.hs=i; e.link=i; CHECK(fips_settle_model_step_event(&j,e,&out)==FIPS_OK); }
    before=j; out=saved;
    CHECK(fips_settle_model_step_event(&j,e,&out)==FIPS_ERR_RANGE && !memcmp(&j,&before,sizeof(j)) && !memcmp(&out,&saved,sizeof(out)));
    e=(fips_settle_model_event){.kind=FIPS_MODEL_TIMEOUT,.hs=0}; CHECK(fips_settle_model_step_event(&j,e,&out)==FIPS_OK && j.attempt_count==127 && j.attempts[0].hs==1);
    fips_settle_model_init(&j,1,2,p);
    e=(fips_settle_model_event){.kind=FIPS_MODEL_MSG1,.hs=1,.bytes=1,.epoch=1,.link=1,.from={1,9},.binding=FIPS_MODEL_CONNECTIONLESS,.sent=true};
    CHECK(fips_settle_model_step_event(&j,e,&out)==FIPS_OK);
    for (unsigned i=2;i<=FIPS_SETTLE_MODEL_CAPACITY;++i) { e.hs=i; e.bytes=i*10; CHECK(fips_settle_model_step_event(&j,e,&out)==FIPS_OK && out.has_refused && out.refused==FIPS_MODEL_GATE_UNHEARD); }
    before=j; out=saved; e.hs=129; e.bytes=1290;
    CHECK(fips_settle_model_step_event(&j,e,&out)==FIPS_ERR_RANGE && !memcmp(&j,&before,sizeof(j)) && !memcmp(&out,&saved,sizeof(out)));
    fips_settle_model_init(&j,1,2,p);
    e=(fips_settle_model_event){.kind=FIPS_MODEL_ADVANCE,.ms=UINT64_MAX-1}; CHECK(fips_settle_model_step_event(&j,e,&out)==FIPS_OK);
    before=j; out=saved; e.ms=2;
    CHECK(fips_settle_model_step_event(&j,e,&out)==FIPS_ERR_RANGE && !memcmp(&j,&before,sizeof(j)) && !memcmp(&out,&saved,sizeof(out)));
    fips_settle_model_init(&j,1,2,p); before=j;
    fips_settle_model_event history[2]={{.kind=FIPS_MODEL_ADVANCE,.ms=UINT64_MAX},{.kind=FIPS_MODEL_ADVANCE,.ms=1}};
    fips_settle_model_step outputs[2]={saved,saved},snapshot[2]; memcpy(snapshot,outputs,sizeof(snapshot));
    CHECK(fips_settle_model_run(&j,history,2,outputs,2)==FIPS_ERR_RANGE && !memcmp(&j,&before,sizeof(j)) && !memcmp(outputs,snapshot,sizeof(outputs)));
    CHECK(fips_settle_model_run(&j,history,2,outputs,1)==FIPS_ERR_RANGE && !memcmp(&j,&before,sizeof(j)) && !memcmp(outputs,snapshot,sizeof(outputs)));
    e=(fips_settle_model_event){.kind=FIPS_MODEL_DIAL,.hs=1,.link=1,.to={1,2},.binding=FIPS_MODEL_CONNECTED}; CHECK(fips_settle_model_step_event(&j,e,&out)==FIPS_OK);
    fips_settle_model_need need={.link=77,.at={7,77}}; size_t count=77;
    CHECK(fips_settle_model_needs(&j,&need,0,&count)==FIPS_ERR_RANGE && need.link==77 && count==77);
    CHECK(fips_settle_model_needs(&j,&need,1,&count)==FIPS_OK && count==1 && need.link==1 && need.at.at==2);
    uint32_t hs=77; CHECK(!fips_settle_model_settled(&j,&need,&hs) && hs==77);
    e=(fips_settle_model_event){.kind=FIPS_MODEL_MSG2,.hs=1,.epoch=1,.from={1,3}}; CHECK(fips_settle_model_step_event(&j,e,&out)==FIPS_OK);
    CHECK(fips_settle_model_settled(&j,&need,&hs) && hs==1 && need.link==1 && need.at.at==3);
    CHECK(fips_settle_model_needs(&j,&need,1,&count)==FIPS_OK && need.at.at==2);
    e=fips_settle_model_frame(1,(fips_settle_model_addr){1,3}); CHECK(e.kind==FIPS_MODEL_FRAME && e.paths==1 && !e.message);
    fips_settle_model_init(&j,1,2,p);
    history[0]=(fips_settle_model_event){.kind=FIPS_MODEL_ADVANCE,.ms=1000};
    history[1]=(fips_settle_model_event){.kind=FIPS_MODEL_MSG1,.hs=10,.bytes=100,.epoch=1,.link=1,.from={1,9},.sent=true};
    CHECK(fips_settle_model_run(&j,history,2,outputs,2)==FIPS_OK);
    CHECK(j.now==1000 && j.has_peering && j.peering.hs==10 && j.peering.installed==1000 && !j.peering.heard);
    CHECK(!outputs[0].has_change && outputs[1].has_class && outputs[1].class_==FIPS_MODEL_CLASS_NOT_HELD && outputs[1].has_change && outputs[1].change==FIPS_MODEL_TRANSITION_PROMOTE);
    CHECK(fips_settle_model_run(&j,NULL,0,NULL,0)==FIPS_OK && j.now==1000);
    puts("settle model: independent defaults, vector/record bounds, transactional run/step and history queries passed");
}
