#ifndef FIPS_TEST_HOOKS
#error "settlement model is test-only"
#endif
#include <fips/proto/settle/model/history.h>
fips_settle_model_event fips_settle_model_frame(uint32_t hs,fips_settle_model_addr from)
{ return (fips_settle_model_event){.kind=FIPS_MODEL_FRAME,.hs=hs,.from=from,.paths=1}; }
