#include <fips/proto/settle/parity.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"parity:%d: %s\n",__LINE__,#x);abort();}}while(0)
static char *source(void)
{ FILE *f=fopen(FIPS_SETTLE_PARITY_TRACE,"rb");CHECK(f && fseek(f,0,SEEK_END)==0);long n=ftell(f);CHECK(n>=0 && fseek(f,0,SEEK_SET)==0);char *s=malloc((size_t)n+1);CHECK(s && fread(s,1,(size_t)n,f)==(size_t)n);s[n]=0;CHECK(!fclose(f));return s; }
static char *plant(const char *text,const char *after,const char *extra)
{ const char *at=strstr(text,after);CHECK(at);at+=strlen(after);size_t first=(size_t)(at-text),n=strlen(text),m=strlen(extra);char *s=malloc(n+m+1);CHECK(s);memcpy(s,text,first);memcpy(s+first,extra,m);memcpy(s+first+m,at,n-first+1);return s; }
static fips_parity_trace *read_all(const char *text,size_t *count)
{ fips_parity_trace *t=calloc(64,sizeof(*t));CHECK(t && fips_settle_parity_read(text,strlen(text),t,64,count)==FIPS_OK);return t; }
static void a_trace_reads_back_as_it_was_written(const char *text)
{
    const char *p=text;
    while(*p){const char *end=strchr(p,'\n');size_t n=end?(size_t)(end-p):strlen(p);while(n && (p[n-1]=='\r'||p[n-1]==' '||p[n-1]=='\t'))--n;
        if(n && *p!='#' && strncmp(p,"script ",7)){
            char out[512];size_t made=999;
            if(n>=5 && !memcmp(p,"core ",5)){fips_parity_diff d;CHECK(fips_settle_parity_diff(p,n,&d)==FIPS_OK && fips_settle_parity_format_diff(&d,out,sizeof(out),&made)==FIPS_OK);}
            else{fips_parity_line l;CHECK(fips_settle_parity_line(p,n,&l)==FIPS_OK && fips_settle_parity_format_line(&l,out,sizeof(out),&made)==FIPS_OK);}
            CHECK(made==n && !memcmp(out,p,n));
        }p=end?end+1:p+strlen(p);
    }
    const char *words[]={"5","n0","held","own","out","udp-home","udp-home","heard"};
    for(size_t gone=3;gone<8;++gone){char bad[128]={0};for(size_t i=0;i<8;++i)if(i!=gone){if(*bad)strcat(bad," ");strcat(bad,words[i]);}fips_parity_line l;CHECK(fips_settle_parity_line(bad,strlen(bad),&l)==FIPS_ERR_RANGE);}
    fips_parity_line l;const char *bad="5 n0 held out own udp-home udp-home heard";CHECK(fips_settle_parity_line(bad,strlen(bad),&l)==FIPS_ERR_RANGE);
    fips_parity_mark m;CHECK(fips_settle_parity_mark("none",4,&m)==FIPS_OK && !m.held);CHECK(fips_settle_parity_mark("none heard",10,&m)==FIPS_ERR_RANGE);
}
static void a_script_names_no_node_or_transport_it_does_not_hold(void)
{
    size_t n;const fips_parity_script *scripts=fips_settle_parity_scripts(&n);for(size_t i=0;i<n;++i)CHECK(!fips_settle_parity_strays(&scripts[i]));
    bool settings[]={false,false};fips_parity_transport udp=FIPS_PARITY_UDP;
    fips_parity_step bad[]={ {.kind=FIPS_PARITY_DIAL,.from=0,.to=2,.transport=FIPS_PARITY_UDP}, {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_TCP}, {.kind=FIPS_PARITY_DIAL,.from=1,.to=1,.transport=FIPS_PARITY_UDP}, {.kind=FIPS_PARITY_CONNECTED,.to=3}, {.kind=FIPS_PARITY_RESTART,.to=2}, {.kind=FIPS_PARITY_HEAL} };
    uint32_t want[]={FIPS_PARITY_NODE,FIPS_PARITY_TRANSPORT,FIPS_PARITY_SELF,FIPS_PARITY_NODE,FIPS_PARITY_NODE,FIPS_PARITY_SMALLER};
    for(size_t i=0;i<6;++i){fips_parity_script s={"planted",settings,2,i==5?2u:0u,&udp,1,&bad[i],1};CHECK(fips_settle_parity_strays(&s)&want[i]);}
    fips_parity_step good[]={ {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_UDP}, {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.nth=1}, {.kind=FIPS_PARITY_ADVANCE,.millis=1000}, {.kind=FIPS_PARITY_REKEY,.to=1} };
    fips_parity_script s={"planted",settings,2,1,&udp,1,good,4};CHECK(!fips_settle_parity_strays(&s));
}
static void every_script_has_its_lines_and_every_line_its_script(const char *text)
{
    size_t n,ns;fips_parity_trace *t=read_all(text,&n);const fips_parity_script *scripts=fips_settle_parity_scripts(&ns);CHECK(n==ns && !fips_settle_parity_unmatched(scripts,ns,t,n));
    for(size_t i=0;i<ns;++i)CHECK(fips_settle_parity_expected(t,n,scripts[i].name));
    size_t gone=0;while(gone<t[0].line_count && (t[0].lines[gone].step!=3 || t[0].lines[gone].node!=1))++gone;CHECK(gone<t[0].line_count);fips_parity_line saved=t[0].lines[gone];memmove(t[0].lines+gone,t[0].lines+gone+1,(t[0].line_count-gone-1)*sizeof(saved));--t[0].line_count;CHECK(fips_settle_parity_unmatched(scripts,ns,t,n)&FIPS_PARITY_LINE_COUNT);memmove(t[0].lines+gone+1,t[0].lines+gone,(t[0].line_count-gone)*sizeof(saved));t[0].lines[gone]=saved;++t[0].line_count;
    fips_parity_script *more=malloc((ns+1)*sizeof(*more));CHECK(more);memcpy(more,scripts,ns*sizeof(*more));more[ns]=scripts[0];more[ns].name="planted-with-no-lines";CHECK(fips_settle_parity_unmatched(more,ns+1,t,n)==FIPS_PARITY_NO_LINES);free(more);
    memset(&t[n],0,sizeof(*t));strcpy(t[n].name,"planted-with-no-script");t[n].nodes=2;t[n].line_count=1;t[n].lines[0].step=1;CHECK(fips_settle_parity_unmatched(scripts,ns,t,n+1)&FIPS_PARITY_NO_SCRIPT);
    t[n]=t[0];CHECK(fips_settle_parity_unmatched(scripts,ns,t,n+1)&FIPS_PARITY_DUPLICATE_BLOCK);
    t[0].settings[0]=t[0].settings[1]=true;CHECK(fips_settle_parity_unmatched(scripts,ns,t,n)&FIPS_PARITY_SETTINGS);t[0].settings[0]=t[0].settings[1]=false;
    const char *core="core 5 n0 dir=in cause=late-dial-split";CHECK(fips_settle_parity_diff(core,strlen(core),&t[0].core[0])==FIPS_OK);t[0].core_count=1;CHECK(!fips_settle_parity_unmatched(scripts,ns,t,n));
    fips_parity_line out[FIPS_PARITY_LINES_MAX];size_t count=0;CHECK(fips_settle_parity_cored(&t[0],out,FIPS_PARITY_LINES_MAX,&count)==FIPS_OK && count==t[0].line_count);bool seen=false;for(size_t i=0;i<count;++i)if(out[i].step==5 && out[i].node==0){CHECK(!out[i].mark.out && fips_settle_parity_at(&t[0],5,0)->out);seen=true;}CHECK(seen);
    t[0].core[0].values[0].flag=true;CHECK(fips_settle_parity_unmatched(scripts,ns,t,n)&FIPS_PARITY_RESTATED);t[0].core[0].step=500;CHECK(fips_settle_parity_unmatched(scripts,ns,t,n)&FIPS_PARITY_CORE_NO_LINE);
    free(t);
}
static void a_core_line_names_a_listed_cause(const char *text)
{
    const char *causes[]={"late-dial-split","aged-link-pending"};
    for(size_t i=0;i<2;++i){char line[128];int n=snprintf(line,sizeof(line),"core 5 n0 dir=out cause=%s",causes[i]);CHECK(n>0);fips_parity_diff d;CHECK(fips_settle_parity_diff(line,(size_t)n,&d)==FIPS_OK && (size_t)d.cause==i);}
    const char *bad[]={"core 5 n0 dir=out cause=the-node-is-wrong","core 5 n0 dir=out","core 5 n0 dir=out cause=","core 5 n0 cause=late-dial-split"};
    fips_parity_trace *out=calloc(64,sizeof(*out)),*old=malloc(64*sizeof(*old));CHECK(out && old);memcpy(old,out,64*sizeof(*out));
    for(size_t i=0;i<4;++i){fips_parity_diff d;CHECK(fips_settle_parity_diff(bad[i],strlen(bad[i]),&d)==FIPS_ERR_RANGE);char extra[256];snprintf(extra,sizeof(extra),"\n%s",bad[i]);char *fixture=plant(text,"5 n0 held own out udp-home udp-home heard",extra);size_t count=999;CHECK(fips_settle_parity_read(fixture,strlen(fixture),out,64,&count)==FIPS_ERR_RANGE && count==999 && !memcmp(old,out,64*sizeof(*out)));free(fixture);}
    free(old);free(out);
}
void fips_test_settle_parity(void)
{
    char *text=source();a_trace_reads_back_as_it_was_written(text);a_script_names_no_node_or_transport_it_does_not_hold();every_script_has_its_lines_and_every_line_its_script(text);a_core_line_names_a_listed_cause(text);
    fips_parity_trace sentinel={0};size_t n=999;CHECK(fips_settle_parity_read(text,strlen(text),&sentinel,1,&n)==FIPS_ERR_RANGE && n==999 && !sentinel.nodes && !sentinel.line_count);
    fips_parity_line line={.step=1};char out[4]={'x','x','x','x'};n=999;CHECK(fips_settle_parity_format_line(&line,out,4,&n)==FIPS_ERR_RANGE && n==999 && !memcmp(out,"xxxx",4));
    free(text);puts("settle parity: original reader/formatter, schedule scans, core overrides and planted failures passed");
}
