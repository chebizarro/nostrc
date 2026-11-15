#ifndef FIPS_PROTO_PATH_CAPABLE_H
#define FIPS_PROTO_PATH_CAPABLE_H
#include <fips/proto/path/live.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct fips_path_capability { uint32_t sent,path; fips_path_sent out; fips_instant next_at; bool told; } fips_path_capability;
FIPS_CORE_API void fips_path_capability_new(fips_path_capability *c);
FIPS_CORE_API fips_duration fips_path_capability_gap(uint32_t sent,const fips_path_config *cfg);
FIPS_CORE_API bool fips_path_capability_due(const fips_path_capability *c,fips_instant now);
FIPS_CORE_API void fips_path_capability_send(fips_path_capability *c,fips_instant now,uint32_t path,uint32_t id,fips_duration timeout,const fips_path_config *cfg);
FIPS_CORE_API bool fips_path_capability_expire(fips_path_capability *c,fips_instant now,bool overdue,const fips_path_config *cfg,fips_path_sent *out,bool *round);
FIPS_CORE_API bool fips_path_capability_answer(fips_path_capability *c,uint32_t path,uint32_t id,fips_path_sent *out);
FIPS_CORE_API bool fips_path_capability_take(fips_path_capability *c,fips_path_sent *out);
FIPS_CORE_API fips_instant fips_path_capability_wake(const fips_path_capability *c,bool eligible);
#ifdef __cplusplus
}
#endif
#endif
