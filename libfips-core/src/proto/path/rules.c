#include <fips/proto/path/rules.h>
#include "internal.h"
/* Rust core::net parsing, without libc sockets, DNS, allocation or locale. */
static bool decimal(const uint8_t *s,size_t n,uint32_t max,uint32_t *out)
{
    if(!n) return false;
    uint32_t v=0;
    for(size_t i=0;i<n;++i) { if(s[i]<'0'||s[i]>'9') return false; unsigned d=s[i]-'0'; if(v>(max-d)/10) return false; v=v*10+d; }
    *out=v; return true;
}
static bool ipv4(const uint8_t *s,size_t n,uint8_t out[4])
{
    size_t start=0;
    for(size_t i=0;i<4;++i) {
        size_t end=start; while(end<n&&s[end]!='.') ++end;
        uint32_t v;
        if((end-start>1&&s[start]=='0')||!decimal(s+start,end-start,255,&v)) return false;
        out[i]=(uint8_t)v;
        if(i==3) return end==n;
        if(end==n) return false;
        start=end+1;
    }
    return false;
}
static int hex(uint8_t c) { return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1; }
static bool ipv6(const uint8_t *s,size_t n,uint8_t out[16])
{
    uint16_t words[8]={0}; size_t count=0,i=0; size_t gap=SIZE_MAX;
    if(n>=2&&s[0]==':'&&s[1]==':') { gap=0; i=2; }
    while(i<n) {
        if(count==8) return false;
        size_t end=i; bool dot=false; while(end<n&&s[end]!=':') { dot|=s[end]=='.'; ++end; }
        if(dot) {
            uint8_t v[4]; if(count>6||!ipv4(s+i,n-i,v)) return false;
            words[count++]=(uint16_t)((v[0]<<8)|v[1]); words[count++]=(uint16_t)((v[2]<<8)|v[3]); i=n; break;
        }
        if(end==i||end-i>4) return false;
        uint16_t v=0;
        for(size_t j=i;j<end;++j) { int d=hex(s[j]); if(d<0) return false; v=(uint16_t)(v*16+(unsigned)d); }
        words[count++]=v; i=end;
        if(i<n) {
            ++i;
            if(i<n&&s[i]==':') { if(gap!=SIZE_MAX) return false; gap=count; ++i; }
            else if(i==n) return false;
        }
    }
    if(gap==SIZE_MAX) { if(count!=8) return false; }
    else {
        if(count==8) return false;
        size_t zeros=8-count;
        memmove(words+gap+zeros,words+gap,(count-gap)*sizeof(words[0]));
        memset(words+gap,0,zeros*sizeof(words[0]));
    }
    for(i=0;i<8;++i) { out[2*i]=(uint8_t)(words[i]>>8); out[2*i+1]=(uint8_t)words[i]; }
    return true;
}
static fips_path_kind near(const uint8_t *v)
{ return v[0]==127||v[0]==10||(v[0]==169&&v[1]==254)||(v[0]==172&&v[1]>=16&&v[1]<=31)||(v[0]==192&&v[1]==168)?FIPS_PATH_Local:FIPS_PATH_Internet; }
fips_path_kind fips_path_rules_scope(const fips_transport_addr *a)
{
    const uint8_t *s=a->bytes; size_t n=a->len; uint8_t v[16]; uint32_t port;
    if(!n) return FIPS_PATH_Internet;
    if(s[0]=='[') {
        size_t close=1; while(close<n&&s[close]!=']') ++close;
        if(close+2>n||s[close+1]!=':'||!decimal(s+close+2,n-close-2,65535,&port)) return FIPS_PATH_Internet;
        ++s; n=close-1;
        size_t scope=0; while(scope<n&&s[scope]!='%') ++scope;
        if(scope<n) { uint32_t id; if(!decimal(s+scope+1,n-scope-1,UINT32_MAX,&id)) return FIPS_PATH_Internet; n=scope; }
        if(!ipv6(s,n,v)) return FIPS_PATH_Internet;
    } else {
        size_t colon=0,at=0; for(size_t i=0;i<n;++i) if(s[i]==':') { ++colon; at=i; }
        if(colon==1) {
            if(!decimal(s+at+1,n-at-1,65535,&port)||!ipv4(s,at,v)) return FIPS_PATH_Internet;
            return near(v);
        }
        if(ipv4(s,n,v)) return near(v);
        if(!ipv6(s,n,v)) return FIPS_PATH_Internet;
    }
    static const uint8_t zero[16]={0};
    if(!memcmp(v,zero,10)&&v[10]==255&&v[11]==255) return near(v+12);
    if((!memcmp(v,zero,15)&&v[15]==1)||(v[0]==254&&(v[1]&192)==128)||(v[0]&254)==252) return FIPS_PATH_Local;
    return FIPS_PATH_Internet;
}
fips_path_kind fips_path_rules_kind(const fips_path_facts *f,fips_transport_id t,const fips_transport_addr *a)
{ fips_path_kind k=f->kind(f->ctx,t); return k==FIPS_PATH_Unfixed?fips_path_rules_scope(a):k; }
fips_path_scope fips_path_rules_link(const fips_path_facts *f,fips_transport_id t,const fips_transport_addr *a)
{ fips_path_class c=f->class_(f->ctx,t); fips_path_kind k=fips_path_rules_kind(f,t,a); return (fips_path_scope){c,k}; }
fips_path_rule fips_path_rules_moves(const fips_path_facts *f,fips_path_scope link,bool mix,fips_transport_id t,const fips_transport_addr *a)
{
    if(f->class_(f->ctx,t)!=link.class_) return FIPS_PATH_Class;
    if(!mix&&fips_path_rules_kind(f,t,a)!=link.kind) return FIPS_PATH_Kind;
    if(f->role(f->ctx,t)==FIPS_PATH_Never) return FIPS_PATH_Role;
    return FIPS_PATH_RuleNone;
}
fips_path_rule fips_path_rules_probe(const fips_path_facts *f,fips_path_scope link,bool mix,fips_transport_id t,const fips_transport_addr *a)
{ fips_path_rule r=fips_path_rules_moves(f,link,mix,t,a); return r!=FIPS_PATH_RuleNone?r:!f->dials(f->ctx,t)?FIPS_PATH_Dial:FIPS_PATH_RuleNone; }
fips_path_rule fips_path_rules_admit(const fips_path_facts *f,fips_path_scope link,bool mix,fips_transport_id t,const fips_transport_addr *a)
{ fips_path_rule r=fips_path_rules_moves(f,link,mix,t,a); return r!=FIPS_PATH_RuleNone?r:!f->accepts(f->ctx,t)?FIPS_PATH_Accept:FIPS_PATH_RuleNone; }
