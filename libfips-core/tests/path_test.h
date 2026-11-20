#ifndef FIPS_PATH_TEST_H
#define FIPS_PATH_TEST_H
#include <fips/proto/path/mod.h>
#include "fakes/host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); abort(); } } while(0)
#define AT(n) fips_test_instant(n)
#define MS(n) fips_duration_from_millis(n)
#define T(n) ((fips_transport_id){n})
static inline bool duration_eq(fips_duration a,fips_duration b) { return a.secs==b.secs&&a.nanos==b.nanos; }
static inline bool instant_eq(fips_instant a,uint64_t ms) { return a.ops&&a.ops->compare(a,AT(ms))==0; }
static inline fips_transport_addr address(unsigned n) {
    char text[64]; int len=snprintf(text,sizeof(text),"10.0.0.%u:2121",n); fips_transport_addr a;
    CHECK(fips_transport_addr_from_string(text,(size_t)len,&a)==FIPS_OK); return a;
}
static inline fips_transport_addr text_addr(const char *s) {
    fips_transport_addr a; CHECK(fips_transport_addr_from_string(s,strlen(s),&a)==FIPS_OK); return a;
}
static inline void counter_fill(void *ctx,uint8_t *out,size_t n) {
    uint32_t *p=ctx; CHECK(n==4); uint32_t v=(*p)++;
    for(size_t i=0;i<4;++i) out[i]=(uint8_t)(v>>(8*i));
}
static inline fips_entropy counter_entropy(uint32_t *n) { return (fips_entropy){counter_fill,n}; }
void test_path_live(void);
void test_path_machine(void);
void test_path_net(void);
#endif
