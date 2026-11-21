#include <fips/proto/settle/parity.h>
static const char *const parity_transports[]={"udp","tcp","ble"};
static json_t *parity_line_json(fips_parity_line l)
{ char s[512];size_t n;REQUIRE(fips_settle_parity_format_line(&l,s,sizeof(s),&n)==FIPS_OK);return json_stringn(s,n); }
static json_t *parity_diff_json(fips_parity_diff d)
{ char s[512];size_t n;REQUIRE(fips_settle_parity_format_diff(&d,s,sizeof(s),&n)==FIPS_OK);return json_stringn(s,n); }
static json_t *parity_scripts_json(void)
{
    size_t n;const fips_parity_script *scripts=fips_settle_parity_scripts(&n);json_t *all=json_array();
    for(size_t i=0;i<n;++i){const fips_parity_script *s=&scripts[i];REQUIRE(!fips_settle_parity_strays(s));json_t *v=json_object(),*settings=json_array(),*transports=json_array(),*steps=json_array();
        for(size_t j=0;j<s->nodes;++j)add(settings,json_boolean(s->settings[j]));
        for(size_t j=0;j<s->transport_count;++j){REQUIRE((unsigned)s->transports[j]<3);add(transports,json_string(parity_transports[s->transports[j]]));}
        for(size_t j=0;j<s->step_count;++j){fips_parity_step p=s->steps[j];json_t *a=json_array();switch(p.kind){
            case FIPS_PARITY_DIAL:add(a,json_string("Dial"));add(a,integer(p.from));add(a,integer(p.to));REQUIRE((unsigned)p.transport<3);add(a,json_string(parity_transports[p.transport]));break;
            case FIPS_PARITY_CONNECTED:add(a,json_string("Connected"));add(a,integer(p.to));break;
            case FIPS_PARITY_HANDSHAKE:add(a,json_string("Handshake"));add(a,integer(p.to));add(a,json_string(p.msg2?"Msg2":"Msg1"));add(a,integer(p.nth));break;
            case FIPS_PARITY_HEAL:add(a,json_string("Heal"));break;
            case FIPS_PARITY_ADVANCE:add(a,json_string("Advance"));add(a,integer(p.millis));break;
            case FIPS_PARITY_RESTART:add(a,json_string("Restart"));add(a,integer(p.to));break;
            case FIPS_PARITY_REKEY:add(a,json_string("Rekey"));add(a,integer(p.to));break;
        }add(steps,a);}
        put(v,"name",json_string(s->name));put(v,"settings",settings);put(v,"smaller",integer(s->smaller));put(v,"transports",transports);put(v,"steps",steps);add(all,v);
    }return all;
}
static json_t *parity_read_json(const char *text,size_t length)
{
    fips_parity_trace *traces=fips_test_settle_array(64,sizeof(*traces));size_t n=0;
    if(fips_settle_parity_read(text,length,traces,64,&n)!=FIPS_OK){free(traces);return json_null();}
    json_t *all=json_array();for(size_t i=0;i<n;++i){fips_parity_trace *t=&traces[i];json_t *v=json_object(),*settings=json_array(),*lines=json_array(),*core=json_array(),*cored=json_null();
        for(size_t j=0;j<t->nodes;++j)add(settings,json_boolean(t->settings[j]));
        for(size_t j=0;j<t->line_count;++j)add(lines,parity_line_json(t->lines[j]));
        for(size_t j=0;j<t->core_count;++j)add(core,parity_diff_json(t->core[j]));
        fips_parity_line corrected[FIPS_PARITY_LINES_MAX];size_t count;if(fips_settle_parity_cored(t,corrected,FIPS_PARITY_LINES_MAX,&count)==FIPS_OK){cored=json_array();for(size_t j=0;j<count;++j)add(cored,parity_line_json(corrected[j]));}
        put(v,"name",json_string(t->name));put(v,"settings",settings);put(v,"lines",lines);put(v,"core",core);put(v,"cored",cored);add(all,v);
    }free(traces);return all;
}
static json_t *parity_settings_json(void)
{
    json_t *all=json_array();for(unsigned on=0;on<2;++on){fips_rekey_params r;fips_path_params p;REQUIRE(fips_settle_parity_rekey(on!=0,&r)==FIPS_OK && fips_settle_parity_path(on!=0,&p)==FIPS_OK);json_t *v=json_object(),*rekey=json_object(),*path=json_object();
        put(rekey,"after",integer(r.cfg.after.secs*1000+r.cfg.after.nanos/1000000));put(rekey,"check",integer(r.cfg.check.secs*1000+r.cfg.check.nanos/1000000));put(rekey,"resends",integer(r.cfg.resends));put(rekey,"handshake",integer(r.cfg.handshake.secs*1000+r.cfg.handshake.nanos/1000000));put(rekey,"hold",integer(r.cfg.hold.secs*1000+r.cfg.hold.nanos/1000000));put(rekey,"arrival",json_boolean(r.cfg.arrival));put(rekey,"floor_link",integer(r.cfg.floor_link.secs*1000+r.cfg.floor_link.nanos/1000000));put(rekey,"floor_session",integer(r.cfg.floor_session.secs*1000+r.cfg.floor_session.nanos/1000000));
        put(path,"enabled",json_boolean(p.cfg.enabled));put(path,"mix_kinds",json_boolean(p.cfg.mix_kinds));put(path,"heartbeat",integer(p.cfg.heartbeat.secs*1000+p.cfg.heartbeat.nanos/1000000));put(v,"on",json_boolean(on));put(v,"rekey",rekey);put(v,"path",path);add(all,v);
    }return all;
}
static size_t check_parity(json_t *trace)
{
    equal(parity_scripts_json(),field(trace,"scripts"));equal(parity_settings_json(),field(trace,"settings"));
    size_t n;const char *const (*unshared)[2]=fips_settle_parity_unshared(&n);json_t *u=json_array();for(size_t i=0;i<n;++i){json_t *a=json_array();add(a,json_string(unshared[i][0]));add(a,json_string(unshared[i][1]));add(u,a);}equal(u,field(trace,"unshared"));
    json_t *rows=field(trace,"rows");REQUIRE(json_is_array(rows));
    for(size_t i=0;i<json_array_size(rows);++i){json_t *row=json_array_get(rows,i),*op=field(row,"op"),*value=json_array_get(op,1);const char *kind=text(json_array_get(op,0)),*s=text(value);size_t length=json_string_length(value);json_t *out=json_null();
        if(!strcmp(kind,"read"))out=parity_read_json(s,length);
        else if(!strcmp(kind,"line")){fips_parity_line l;if(fips_settle_parity_line(s,length,&l)==FIPS_OK)out=parity_line_json(l);}
        else if(!strcmp(kind,"diff")){fips_parity_diff d;if(fips_settle_parity_diff(s,length,&d)==FIPS_OK)out=parity_diff_json(d);}
        else{REQUIRE(!strcmp(kind,"mark"));fips_parity_line l={0};if(fips_settle_parity_mark(s,length,&l.mark)==FIPS_OK){json_t *full=parity_line_json(l);const char *p=json_string_value(full);REQUIRE(!strncmp(p,"0 n0 ",5));out=json_string(p+5);json_decref(full);}}
        equal(out,field(row,"out"));
    }return json_array_size(rows);
}
