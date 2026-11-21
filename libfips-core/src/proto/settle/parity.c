#ifndef FIPS_TEST_HOOKS
#error "Settlement parity is test-only"
#endif
#include <fips/proto/settle/parity.h>
#include <string.h>
#include <limits.h>
#include "parity_data.inc"
#define OK FIPS_OK
#define BAD FIPS_ERR_RANGE
#define TRY(x) do { if ((x)!=OK) return BAD; } while(0)
typedef struct word { const char *p; size_t n; } word;
static bool space(char c) { return c==' ' || c=='\t' || c=='\r' || c=='\n' || c=='\v' || c=='\f'; }
static bool eq(word w,const char *s) { size_t n=strlen(s); return w.n==n && !memcmp(w.p,s,n); }
static bool take(word *text,word *out)
{
    while(text->n && space(*text->p)) {++text->p;--text->n;}
    if(!text->n)return false;
    size_t n=0;while(n<text->n && !space(text->p[n]))++n;
    *out=(word){text->p,n};text->p+=n;text->n-=n;return true;
}
static bool number(word w,size_t *out)
{
    if(w.n && *w.p=='+'){++w.p;--w.n;} if(!w.n)return false;
    size_t n=0;for(size_t i=0;i<w.n;++i){unsigned d=(unsigned)(unsigned char)w.p[i]-(unsigned)'0';if(d>9 || n>(SIZE_MAX-d)/10)return false;n=n*10+d;}*out=n;return true;
}
static bool place_number(word step,word node,size_t *s,size_t *n)
{ if(!node.n || *node.p!='n')return false;++node.p;--node.n;return number(step,s)&&number(node,n); }
static const char *const transports[]={"udp","tcp","ble"};
static const char *const addresses[]={"home","second","accepted"};
static const char *const fields[]={"held","session","dir","setup","active","heard"};
static const char *const causes[]={"late-dial-split","aged-link-pending"};
static bool place_read(word w,fips_parity_place *out)
{
    const char *dash=memchr(w.p,'-',w.n);if(!dash)return false;word t={w.p,(size_t)(dash-w.p)},a={dash+1,w.n-(size_t)(dash-w.p)-1};
    unsigned i=0,j=0;while(i<3 && !eq(t,transports[i]))++i;while(j<3 && !eq(a,addresses[j]))++j;
    if(i==3 || j==3)return false;*out=(fips_parity_place){(fips_parity_transport)i,(fips_parity_at)j};return true;
}
static bool value_read(unsigned field,word w,fips_parity_value *out)
{
    fips_parity_value v={.field=(fips_parity_field)field};const char *a=NULL,*b=NULL;
    switch(field){case FIPS_PARITY_HELD:a="no";b="yes";break;case FIPS_PARITY_SESSION:a="peer";b="own";break;case FIPS_PARITY_DIR:a="in";b="out";break;case FIPS_PARITY_HEARD:a="unheard";b="heard";break;case FIPS_PARITY_SETUP:case FIPS_PARITY_ACTIVE:if(!place_read(w,&v.place))return false;*out=v;return true;default:return false;}
    if(!eq(w,a) && !eq(w,b))return false;v.flag=eq(w,b);*out=v;return true;
}
static void set(fips_parity_mark *m,fips_parity_value v)
{
    switch(v.field){case FIPS_PARITY_HELD:m->held=v.flag;break;case FIPS_PARITY_SESSION:m->own=v.flag;break;case FIPS_PARITY_DIR:m->out=v.flag;break;case FIPS_PARITY_SETUP:m->setup=v.place;break;case FIPS_PARITY_ACTIVE:m->active=v.place;break;case FIPS_PARITY_HEARD:m->heard=v.flag;break;}
}
fips_status fips_settle_parity_path(bool on,fips_path_params *out)
{ fips_path_config c=fips_path_config_draft();c.enabled=on;c.mix_kinds=true;c.heartbeat=(fips_duration){10,0};return fips_path_params_new(out,c,NULL); }
fips_status fips_settle_parity_rekey(bool on,fips_rekey_params *out)
{ fips_duration silence=fips_rekey_msg1_span(5);silence.secs+=33;fips_rekey_config c=fips_rekey_config_fmp();c.arrival=on;c.check=(fips_duration){1,0};c.resends=5;c.handshake=(fips_duration){30,0};return fips_rekey_params_new(fips_rekey_config_with_hold(c,silence),out)==FIPS_REKEY_PARAMS_OK?OK:BAD; }
const fips_parity_script *fips_settle_parity_scripts(size_t *count)
{ *count=sizeof(scripts)/sizeof(*scripts);return scripts; }
const char *const (*fips_settle_parity_unshared(size_t *count))[2]
{ *count=sizeof(unshared)/sizeof(*unshared);return unshared; }
fips_status fips_settle_parity_mark(const char *text,size_t length,fips_parity_mark *out)
{
    word rest={text,length},w;fips_parity_mark m={0};if(!take(&rest,&w))return BAD;
    if(eq(w,"none")){if(take(&rest,&w))return BAD;*out=m;return OK;}
    if(!eq(w,"held"))return BAD;m.held=true;
    for(unsigned i=1;i<6;++i){fips_parity_value v;if(!take(&rest,&w)||!value_read(i,w,&v))return BAD;set(&m,v);}
    if(take(&rest,&w))return BAD;*out=m;return OK;
}
fips_status fips_settle_parity_line(const char *text,size_t length,fips_parity_line *out)
{
    const char *one=memchr(text,' ',length);if(!one)return BAD;size_t left=length-(size_t)(one-text)-1;const char *two=memchr(one+1,' ',left);if(!two)return BAD;
    fips_parity_line l={0};if(!place_number((word){text,(size_t)(one-text)},(word){one+1,(size_t)(two-one-1)},&l.step,&l.node))return BAD;
    TRY(fips_settle_parity_mark(two+1,length-(size_t)(two-text)-1,&l.mark));*out=l;return OK;
}
fips_status fips_settle_parity_diff(const char *text,size_t length,fips_parity_diff *out)
{
    word rest={text,length},w,s,n;fips_parity_diff d={0};
    if(!take(&rest,&w)||!eq(w,"core")||!take(&rest,&s)||!take(&rest,&n)||!place_number(s,n,&d.step,&d.node))return BAD;
    unsigned mask=0;bool cause=false;
    while(take(&rest,&w)){
        const char *split=memchr(w.p,'=',w.n);if(!split)return BAD;word key={w.p,(size_t)(split-w.p)},value={split+1,w.n-(size_t)(split-w.p)-1};
        if(eq(key,"cause")){unsigned c=0;while(c<2&&!eq(value,causes[c]))++c;if(c==2 || take(&rest,&w))return BAD;d.cause=(fips_parity_cause)c;cause=true;break;}
        unsigned f=0;while(f<6&&!eq(key,fields[f]))++f;if(f==6 || (mask&(1u<<f)) || !value_read(f,value,&d.values[d.count]))return BAD;mask|=1u<<f;++d.count;
    }
    if(!cause || !d.count)return BAD;*out=d;return OK;
}
static fips_status read_pass(const char *text,size_t length,fips_parity_trace *out,size_t *count)
{
    fips_parity_trace t={0};size_t made=0;bool have=false;
    while(length){const char *end=memchr(text,'\n',length);size_t bytes=end?(size_t)(end-text):length,n=bytes;while(n&&space(text[n-1]))--n;
        if(n && *text!='#'){
            if(n>=7&&!memcmp(text,"script ",7)){
                if(have){if(out)out[made]=t;++made;}memset(&t,0,sizeof(t));word rest={text+7,n-7},w;
                if(!take(&rest,&w)||w.n>=sizeof(t.name)||memchr(w.p,0,w.n))return BAD;memcpy(t.name,w.p,w.n);
                while(take(&rest,&w)){if(t.nodes==FIPS_PARITY_NODES_MAX||(!eq(w,"off")&&!eq(w,"on")))return BAD;t.settings[t.nodes++]=eq(w,"on");}have=true;
            }else{
                if(!have)return BAD;
                if(n>=5&&!memcmp(text,"core ",5)){if(t.core_count==FIPS_PARITY_CORES_MAX)return BAD;TRY(fips_settle_parity_diff(text,n,&t.core[t.core_count]));++t.core_count;}
                else{if(t.line_count==FIPS_PARITY_LINES_MAX)return BAD;TRY(fips_settle_parity_line(text,n,&t.lines[t.line_count]));++t.line_count;}
            }
        }
        size_t used=bytes+(end?1u:0u);text+=used;length-=used;
    }
    if(have){if(out)out[made]=t;++made;}*count=made;return OK;
}
fips_status fips_settle_parity_read(const char *text,size_t length,fips_parity_trace *out,size_t capacity,size_t *count)
{ size_t n;TRY(read_pass(text,length,NULL,&n));if(n>capacity)return BAD;TRY(read_pass(text,length,out,&n));*count=n;return OK; }
static bool valid_trace(const fips_parity_trace *t)
{ return t->nodes<=FIPS_PARITY_NODES_MAX && t->line_count<=FIPS_PARITY_LINES_MAX && t->core_count<=FIPS_PARITY_CORES_MAX; }
const fips_parity_trace *fips_settle_parity_expected(const fips_parity_trace *t,size_t n,const char *name)
{ for(size_t i=0;i<n;++i)if(!strcmp(t[i].name,name))return &t[i];return NULL; }
const fips_parity_mark *fips_settle_parity_at(const fips_parity_trace *t,size_t step,size_t node)
{ if(!valid_trace(t))return NULL;for(size_t i=0;i<t->line_count;++i)if(t->lines[i].step==step&&t->lines[i].node==node)return &t->lines[i].mark;return NULL; }
static bool value_valid(fips_parity_value v)
{ return (unsigned)v.field<6 && ((v.field!=FIPS_PARITY_SETUP && v.field!=FIPS_PARITY_ACTIVE) || ((unsigned)v.place.transport<3 && (unsigned)v.place.at<3)); }
static fips_status apply(fips_parity_mark *m,const fips_parity_diff *d)
{
    if(d->count>6)return BAD;unsigned mask=m->held?63u:1u;
    for(size_t i=0;i<d->count;++i){fips_parity_value v=d->values[i];if(!value_valid(v))return BAD;set(m,v);mask|=1u<<(unsigned)v.field;}
    return !m->held || mask==63u ? OK : BAD;
}
fips_status fips_settle_parity_cored(const fips_parity_trace *t,fips_parity_line *out,size_t capacity,size_t *count)
{
    if(!valid_trace(t)||t->line_count>capacity)return BAD;fips_parity_line lines[FIPS_PARITY_LINES_MAX];memcpy(lines,t->lines,t->line_count*sizeof(*lines));
    for(size_t i=0;i<t->core_count;++i){const fips_parity_diff *d=&t->core[i];size_t k=0;while(k<t->line_count&&(lines[k].step!=d->step||lines[k].node!=d->node))++k;if(k==t->line_count)return BAD;TRY(apply(&lines[k].mark,d));}
    if(t->line_count)memcpy(out,lines,t->line_count*sizeof(*out));*count=t->line_count;return OK;
}
static bool mark_equal(fips_parity_mark a,fips_parity_mark b)
{ return a.held==b.held && (!a.held || (a.own==b.own&&a.out==b.out&&a.heard==b.heard&&a.setup.transport==b.setup.transport&&a.setup.at==b.setup.at&&a.active.transport==b.active.transport&&a.active.at==b.active.at)); }
uint32_t fips_settle_parity_strays(const fips_parity_script *s)
{
    uint32_t found=s->smaller>=s->nodes?FIPS_PARITY_SMALLER:0;
    for(size_t i=0;i<s->step_count;++i){fips_parity_step p=s->steps[i];if((unsigned)p.kind>FIPS_PARITY_REKEY){found|=FIPS_PARITY_INVALID;continue;}
        if(p.kind==FIPS_PARITY_DIAL){bool held=false;for(size_t j=0;j<s->transport_count;++j)held|=s->transports[j]==p.transport;if(!held)found|=FIPS_PARITY_TRANSPORT;if(p.from==p.to)found|=FIPS_PARITY_SELF;if(p.from>=s->nodes||p.to>=s->nodes)found|=FIPS_PARITY_NODE;}
        else if(p.kind!=FIPS_PARITY_HEAL&&p.kind!=FIPS_PARITY_ADVANCE&&p.to>=s->nodes)found|=FIPS_PARITY_NODE;
    }return found;
}
uint32_t fips_settle_parity_unmatched(const fips_parity_script *s,size_t n,const fips_parity_trace *t,size_t nt)
{
    uint32_t found=0;
    for(size_t i=0;i<nt;++i){if(!valid_trace(&t[i]))return FIPS_PARITY_INVALID;size_t same=0;bool held=false;for(size_t j=0;j<nt;++j)same+=!strcmp(t[i].name,t[j].name);for(size_t j=0;j<n;++j)held|=!strcmp(t[i].name,s[j].name);if(same>1)found|=FIPS_PARITY_DUPLICATE_BLOCK;if(!held)found|=FIPS_PARITY_NO_SCRIPT;}
    for(size_t i=0;i<n;++i){const fips_parity_trace *p=fips_settle_parity_expected(t,nt,s[i].name);if(!p){found|=FIPS_PARITY_NO_LINES;continue;}bool settings=p->nodes==s[i].nodes;if(settings)for(size_t j=0;j<p->nodes;++j)settings&=p->settings[j]==s[i].settings[j];if(!settings)found|=FIPS_PARITY_SETTINGS;
        for(size_t step=1;step<=s[i].step_count;++step)for(size_t node=0;node<s[i].nodes;++node){size_t count=0;for(size_t k=0;k<p->line_count;++k)count+=p->lines[k].step==step&&p->lines[k].node==node;if(count!=1)found|=FIPS_PARITY_LINE_COUNT;}
        for(size_t k=0;k<p->line_count;++k)if(!p->lines[k].step||p->lines[k].step>s[i].step_count||p->lines[k].node>=s[i].nodes)found|=FIPS_PARITY_PAST_SCRIPT;
        for(size_t k=0;k<p->core_count;++k){const fips_parity_diff *d=&p->core[k];const fips_parity_mark *m=fips_settle_parity_at(p,d->step,d->node);if(!m){found|=FIPS_PARITY_CORE_NO_LINE;continue;}if(d->count>6){found|=FIPS_PARITY_INVALID;continue;}for(size_t v=0;v<d->count;++v){fips_parity_mark one=*m;if(!value_valid(d->values[v])){found|=FIPS_PARITY_INVALID;continue;}set(&one,d->values[v]);/* Fields::of compares present fields even when held=false. */
                unsigned f=(unsigned)d->values[v].field;bool same=f==FIPS_PARITY_HELD?one.held==m->held:m->held&&mark_equal(one,*m);if(same)found|=FIPS_PARITY_RESTATED;}}
        fips_parity_line scratch[FIPS_PARITY_LINES_MAX];size_t count;if(fips_settle_parity_cored(p,scratch,FIPS_PARITY_LINES_MAX,&count)!=OK)found|=FIPS_PARITY_INCOMPLETE;
    }return found;
}
typedef struct buffer {char bytes[512];size_t n;bool bad;} buffer;
static void append(buffer *b,const char *s)
{ size_t n=strlen(s);if(n>sizeof(b->bytes)-b->n){b->bad=true;return;}memcpy(b->bytes+b->n,s,n);b->n+=n; }
static void decimal(buffer *b,size_t n)
{ char s[3*sizeof(size_t)+1];size_t k=0;do{s[k++]=(char)('0'+n%10);n/=10;}while(n);while(k){char c[2]={s[--k],0};append(b,c);} }
static void print_place(buffer *b,fips_parity_place p)
{ if((unsigned)p.transport>=3||(unsigned)p.at>=3){b->bad=true;return;}append(b,transports[p.transport]);append(b,"-");append(b,addresses[p.at]); }
static void print_mark(buffer *b,fips_parity_mark m)
{ if(!m.held){append(b,"none");return;}append(b,m.own?"held own ":"held peer ");append(b,m.out?"out ":"in ");print_place(b,m.setup);append(b," ");print_place(b,m.active);append(b,m.heard?" heard":" unheard"); }
static fips_status finish(buffer *b,char *out,size_t capacity,size_t *length)
{ if(b->bad || b->n>=capacity)return BAD;memcpy(out,b->bytes,b->n);out[b->n]=0;*length=b->n;return OK; }
fips_status fips_settle_parity_format_line(const fips_parity_line *line,char *out,size_t capacity,size_t *length)
{ buffer b={0};decimal(&b,line->step);append(&b," n");decimal(&b,line->node);append(&b," ");print_mark(&b,line->mark);return finish(&b,out,capacity,length); }
fips_status fips_settle_parity_format_diff(const fips_parity_diff *d,char *out,size_t capacity,size_t *length)
{
    if(d->count>6||(unsigned)d->cause>=2)return BAD;buffer b={0};append(&b,"core ");decimal(&b,d->step);append(&b," n");decimal(&b,d->node);
    for(size_t i=0;i<d->count;++i){fips_parity_value v=d->values[i];if(!value_valid(v))return BAD;append(&b," ");append(&b,fields[v.field]);append(&b,"=");switch(v.field){case FIPS_PARITY_HELD:append(&b,v.flag?"yes":"no");break;case FIPS_PARITY_SESSION:append(&b,v.flag?"own":"peer");break;case FIPS_PARITY_DIR:append(&b,v.flag?"out":"in");break;case FIPS_PARITY_SETUP:case FIPS_PARITY_ACTIVE:print_place(&b,v.place);break;case FIPS_PARITY_HEARD:append(&b,v.flag?"heard":"unheard");break;}}
    append(&b," cause=");append(&b,causes[d->cause]);return finish(&b,out,capacity,length);
}
