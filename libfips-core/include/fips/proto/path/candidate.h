#ifndef FIPS_PROTO_PATH_CANDIDATE_H
#define FIPS_PROTO_PATH_CANDIDATE_H
#include <fips/proto/path/rules.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef uint8_t fips_path_sources;
typedef struct fips_path_entry {
    bool used; fips_transport_id transport; fips_transport_addr addr;
    fips_path_sources sources; bool held; uint64_t seq; fips_path_rule refused;
} fips_path_entry;
typedef struct fips_path_candidates { fips_path_entry slots[8]; uint64_t seq; } fips_path_candidates;
typedef enum fips_path_report_kind { FIPS_PATH_New, FIPS_PATH_Again, FIPS_PATH_Repeat, FIPS_PATH_Full } fips_path_report_kind;
typedef struct fips_path_report { fips_path_report_kind kind; size_t slot; } fips_path_report;
FIPS_CORE_API bool fips_path_sources_has(fips_path_sources s,fips_path_source source);
FIPS_CORE_API void fips_path_candidates_new(fips_path_candidates *c);
FIPS_CORE_API size_t fips_path_candidates_find(const fips_path_candidates *c,fips_transport_id t,const fips_transport_addr *a);
FIPS_CORE_API fips_path_report fips_path_candidates_report(fips_path_candidates *c,fips_transport_id t,fips_transport_addr a,fips_path_source source,const fips_path_set *paths);
FIPS_CORE_API bool fips_path_candidates_unlist(fips_path_candidates *c,fips_transport_id t,const fips_transport_addr *a,fips_path_source source);
FIPS_CORE_API void fips_path_candidates_hold(fips_path_candidates *c,fips_transport_id t,fips_transport_addr a,const fips_path_set *paths);
FIPS_CORE_API void fips_path_candidates_absent(fips_path_candidates *c,fips_transport_id t);
FIPS_CORE_API void fips_path_candidates_mark(fips_path_candidates *c,size_t slot,fips_path_rule rule);
FIPS_CORE_API uint8_t fips_path_candidates_release(fips_path_candidates *c,fips_transport_id t);
FIPS_CORE_API void fips_path_candidates_prune(fips_path_candidates *c,uint8_t mask,const fips_path_set *paths);
FIPS_CORE_API void fips_path_candidates_forget(fips_path_candidates *c,fips_transport_id t);
#ifdef __cplusplus
}
#endif
#endif
