/* Ported helpers/graphs/snapshots model cases from upstream fipstop. */
#include "model.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
int main(void)
{
    char b[FT_TEXT];
    ft_bytes(0,b); assert(!strcmp(b,"0B"));
    ft_bytes(1024,b); assert(!strcmp(b,"1.0KB"));
    ft_bytes(1048576,b); assert(!strcmp(b,"1.0MB"));
    ft_bytes(1073741824,b); assert(!strcmp(b,"1.00GB"));
    ft_throughput(-1,b); assert(!strcmp(b,"0 B/s"));
    ft_throughput(42,b); assert(!strcmp(b,"42.0 B/s"));
    ft_throughput(1234,b); assert(!strcmp(b,"1.23 KB/s"));
    ft_throughput(123456789,b); assert(!strcmp(b,"123 MB/s"));
    ft_duration(42,b); assert(!strcmp(b,"42ms"));
    ft_duration(3200,b); assert(!strcmp(b,"3.2s"));
    ft_duration(300000,b); assert(!strcmp(b,"5.0m"));
    ft_elapsed(0,1000,b); assert(!strcmp(b,"-"));
    ft_elapsed(2000,1000,b); assert(!strcmp(b,"-"));
    ft_uptime(93784,b); assert(!strcmp(b,"1d 2h 3m 4s"));
    char *hex=ft_truncate_hex("0123456789abcdef",8); assert(!strcmp(hex,"01234...")); free(hex);
    char *t=ft_truncate("abcdef",4,true); assert(!strcmp(t,"abc…")); free(t);
    t=ft_truncate("éλabc",3,false); assert(!strcmp(t,"éλ…")); free(t);
    t=ft_truncate("a",4,true); assert(!strcmp(t,"a   ")); free(t);
    t=ft_truncate("a",0,true); assert(!strcmp(t,"")); free(t);
    assert(ft_clamp_scroll(65535,30,10)==20); assert(ft_clamp_scroll(2,1,5)==0);
    json_error_t e; json_t *a=json_loads("[{\"mmp\":{\"lqi\":1}},{\"is_parent\":true,\"mmp\":{\"lqi\":9}},{\"is_child\":true}]",0,&e);
    assert(a); size_t n=0; json_t **p=ft_peer_order(a,&n);
    assert(n==3 && ft_group(p[0])==0 && ft_group(p[1])==1 && ft_group(p[2])==2);
    free(p); json_decref(a);
    a=json_loads("[null,1,2,3,null]",0,&e); double *v=NULL; n=ft_series(a,&v);
    assert(n==5 && isnan(v[0]) && isnan(v[4]));
    ft_summary s=ft_summarize(v,n); assert(s.min==1 && s.max==3 && s.last==3 && s.n==5);
    double out[10]; ft_resample(v,n,out,10); assert(isnan(out[0]) && fabs(out[3]-1.33333333333333)<1e-10 && isnan(out[9]));
    char *spark=ft_spark(a); assert(!strcmp(spark," ▁▅█ ")); free(spark); free(v); json_decref(a);
    a=json_loads("[7,7,7]",0,&e); spark=ft_spark(a); assert(!strcmp(spark,"▄▄▄")); free(spark); json_decref(a);
    assert(!strcmp(ft_windows[1],"10m") && !strcmp(ft_granularities[3],"1m"));
    puts("fipstop model: formatting, UTF-8 truncation, stable role/LQI ordering, scroll, history rates/nulls, resampling, sparklines passed");
    return 0;
}
