#ifndef FIPS_PROTO_PATH_SELECT_H
#define FIPS_PROTO_PATH_SELECT_H
#include <fips/proto/path/rules.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct fips_path_pin { fips_transport_id transport; bool has_addr; fips_transport_addr addr; } fips_path_pin;
typedef struct fips_path_rival {
    bool used; size_t index;
    bool has_score; double score;
    bool has_rtt; fips_duration rtt;
    bool has_steady; double steady;
    bool has_base; fips_duration base;
    uint8_t tier; bool preferred,pinned,fits; fips_path_medium medium;
} fips_path_rival;
typedef struct fips_path_hold { double cost; fips_instant at; uint8_t owed; } fips_path_hold;
FIPS_CORE_API bool fips_path_pin_names(const fips_path_pin *pin,const fips_path_ref *to);
FIPS_CORE_API bool fips_path_heeded(bool oriented);
FIPS_CORE_API size_t fips_path_carried(size_t link,bool has_proven,size_t proven,bool oriented);
FIPS_CORE_API bool fips_path_select_score(const fips_path_liveness *l,double *out);
FIPS_CORE_API bool fips_path_select_steady(const fips_path_liveness *l,double *out);
FIPS_CORE_API bool fips_path_select_held(const fips_path_rival *to,const fips_path_rival *from,double margin);
FIPS_CORE_API bool fips_path_select_waits(const fips_path_rival *to,const fips_path_rival *from,double margin);
FIPS_CORE_API uint8_t fips_path_select_tier(fips_path_role role);
FIPS_CORE_API bool fips_path_select_quicker(fips_path_medium a,fips_path_medium b);
/* Field indexed by rival.index; ties use that index. NONE when empty. */
FIPS_CORE_API size_t fips_path_select_choose(const fips_path_rival field[8],double margin);
FIPS_CORE_API bool fips_path_select_best(const fips_path_rival field[8],double *out);
FIPS_CORE_API fips_path_hold fips_path_hold_new(double cost,fips_instant at);
FIPS_CORE_API void fips_path_hold_report(fips_path_hold *h);
FIPS_CORE_API bool fips_path_hold_held(const fips_path_hold *h,fips_instant now,fips_duration dwell,double *out);
#ifdef __cplusplus
}
#endif
#endif
