#ifndef FIPS_PATH_INTERNAL_H
#define FIPS_PATH_INTERNAL_H
#include <fips/proto/path/params.h>
#include <fips/transport/types.h>
#include <string.h>
#include <math.h>
#include <assert.h>
/* A zero ops pointer represents None; every real host instant has ops. */
static inline bool pt_has(fips_instant t) { return t.ops!=NULL; }
static inline int pt_cmp(fips_instant a,fips_instant b) { return a.ops->compare(a,b); }
static inline fips_instant pt_min(fips_instant a,fips_instant b) { return !pt_has(a)?b:!pt_has(b)?a:pt_cmp(a,b)<0?a:b; }
static inline fips_instant pt_max(fips_instant a,fips_instant b) { return !pt_has(a)?b:!pt_has(b)?a:pt_cmp(a,b)>0?a:b; }
static inline bool pt_due(fips_instant a,fips_instant now) { return !pt_has(a)||pt_cmp(a,now)<=0; }
static inline fips_duration pt_since(fips_instant now,fips_instant then) { return now.ops->saturating_duration_since(now,then); }
static inline int pd_cmp(fips_duration a,fips_duration b) { return a.secs!=b.secs?(a.secs>b.secs?1:-1):a.nanos!=b.nanos?(a.nanos>b.nanos?1:-1):0; }
static inline fips_duration pd_min(fips_duration a,fips_duration b) { return pd_cmp(a,b)<0?a:b; }
static inline fips_duration pd_max(fips_duration a,fips_duration b) { return pd_cmp(a,b)>0?a:b; }
static inline fips_duration pd_mul(fips_duration d,uint32_t n) {
    uint64_t nanos=(uint64_t)d.nanos*n, extra=nanos/1000000000;
    if (n && d.secs>(UINT64_MAX-extra)/n) return (fips_duration){UINT64_MAX,999999999};
    return (fips_duration){d.secs*n+extra,(uint32_t)(nanos%1000000000)};
}
static inline fips_duration pd_div(fips_duration d,uint32_t n) { return (fips_duration){d.secs/n,(uint32_t)(((d.secs%n)*1000000000+d.nanos)/n)}; }
static inline uint32_t p_inc(uint32_t n) { return n==UINT32_MAX?n:n+1; }
static inline uint32_t p_draw(fips_entropy e) { uint8_t b[4]; e.fill(e.ctx,b,4); return (uint32_t)b[0]|((uint32_t)b[1]<<8)|((uint32_t)b[2]<<16)|((uint32_t)b[3]<<24); }
static inline bool p_after(uint32_t a,uint32_t b) { uint32_t d=a-b; return d!=0 && d<UINT32_C(0x80000000); }
static inline size_t p_min(size_t a,size_t b) { return a<b?a:b; }
static inline size_t p_max(size_t a,size_t b) { return a>b?a:b; }
static inline size_t p_add(size_t a,size_t b) { return a>SIZE_MAX-b?SIZE_MAX:a+b; }
static inline bool p_pair(fips_transport_id a,const fips_transport_addr *aa,fips_transport_id b,const fips_transport_addr *ba) { return a.value==b.value && fips_transport_addr_equal(aa,ba); }
#endif
