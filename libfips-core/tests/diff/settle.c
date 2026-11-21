/* Independent family runner: does not alter D1's shared schema/pin scaffolding. */
#include <fips/proto/settle/gates.h>
#include "../fakes/host.h"
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr,"settle:%d: %s\n",__LINE__,#x); exit(1); } } while (0)
#define PIN "64f5b4d482cfe14abad108ffc2d92128f7658311"
static json_t *field(json_t *o, const char *key) { json_t *v=json_object_get(o,key); REQUIRE(v); return v; }
static uint64_t num(json_t *v) { REQUIRE(json_is_integer(v) && json_integer_value(v)>=0); return (uint64_t)json_integer_value(v); }
static uint64_t at(json_t *a, size_t i) { REQUIRE(json_is_array(a)); return num(json_array_get(a,i)); }
static bool boolean(json_t *v) { REQUIRE(json_is_boolean(v)); return json_is_true(v); }
static const char *text(json_t *v) { REQUIRE(json_is_string(v)); return json_string_value(v); }
static json_t *integer(uint64_t n) { REQUIRE(n<=INT64_MAX); return json_integer((json_int_t)n); }
static void add(json_t *a, json_t *v) { REQUIRE(v && json_array_append_new(a,v)==0); }
static void put(json_t *o, const char *key, json_t *v) { REQUIRE(v && json_object_set_new(o,key,v)==0); }
static void equal(json_t *a, json_t *b)
{
    if (!json_equal(a,b)) {
        char *actual=json_dumps(a,JSON_COMPACT|JSON_SORT_KEYS), *want=json_dumps(b,JSON_COMPACT|JSON_SORT_KEYS);
        fprintf(stderr,"actual: %s\nexpected: %s\n",actual?actual:"<null>",want?want:"<null>");
        free(actual); free(want); exit(1);
    }
    json_decref(a);
}
static fips_settle_pair pair(uint64_t n)
{
    REQUIRE(n<16); fips_settle_pair p={.transport={(uint32_t)(n/8+1)}}; uint8_t b=(uint8_t)(n%8);
    REQUIRE(fips_transport_addr_new(&b,1,&p.addr)==FIPS_OK); return p;
}
static uint64_t pn(const fips_settle_pair *p) { return (uint64_t)(p->transport.value-1)*8+p->addr.bytes[0]; }
static fips_settle_holder holder(uint64_t n)
{
    REQUIRE(n<256); fips_settle_holder h={.peer=n!=0}; memset(h.identity.bytes,(int)n,16); return h;
}
static bool expects(void *ctx, uint64_t n, fips_node_addr *out)
{
    const fips_settle_link *r=fips_settle_link_get(ctx,n);
    if (!r || pn(&r->setup)==7) return false;
    memset(out->bytes,3,16); return true;
}
static json_t *snapshot(fips_settle_links *s)
{
    json_t *o=json_object(), *records=json_array(), *index=json_array(), *places=json_array();
    for (uint64_t n=0; n<s->next; ++n) {
        const fips_settle_link *r=fips_settle_link_get(s,n); if (!r) continue;
        json_t *row=json_array(), *paths=json_array();
        add(row,integer(n)); add(row,integer(pn(&r->setup))); add(row,json_boolean(r->direction==FIPS_LINK_INBOUND));
        add(row,integer(r->holder.peer ? r->holder.identity.bytes[0] : 0)); add(row,json_boolean(r->released)); add(row,json_boolean(r->capable));
        for (size_t i=0; i<r->path_count; ++i) {
            json_t *path=json_array(); add(path,integer(pn(&r->paths[i].pair))); add(path,integer(r->paths[i].state)); add(paths,path);
        }
        add(row,paths); add(records,row);
        for (uint64_t p=0; p<16; ++p) {
            fips_settle_pair from=pair(p); fips_settle_placement place=fips_settle_place(s,&from,n,expects,s);
            row=json_array(); add(row,integer(n)); add(row,integer(p)); add(row,json_boolean(place.established));
            add(row,place.has_expected ? integer(place.expected.bytes[0]) : json_null());
            add(row,place.has_unanswered ? integer(place.unanswered) : json_null());
            add(row,json_boolean(place.on_link)); add(places,row);
        }
    }
    for (uint64_t p=0; p<16; ++p) for (uint64_t n=0; n<s->next; ++n) {
        fips_settle_pair from=pair(p);
        if (fips_settle_link_indexed(s,n,&from)) { json_t *row=json_array(); add(row,integer(p)); add(row,integer(n)); add(index,row); }
    }
    put(o,"records",records); put(o,"index",index); put(o,"places",places); return o;
}
static size_t check_links(json_t *trace)
{
    fips_settle_links links; fips_settle_link slots[8]; fips_settle_path paths[8*256];
    REQUIRE(fips_settle_links_init(&links,slots,8,paths,256)==FIPS_OK);
    json_t *rows=field(trace,"rows"); REQUIRE(json_is_array(rows) && json_array_size(rows)>0);
    for (size_t i=0; i<json_array_size(rows); ++i) {
        json_t *row=json_array_get(rows,i), *op=field(row,"op");
        uint64_t type=at(op,0), n=at(op,1); REQUIRE(json_array_size(op)==(type<=3 ? 4 : type==6 ? 3 : 2));
        fips_settle_pair p={0}, q={0}, closes[257]; size_t count=0;
        if (type==0) {
            p=pair(n); uint64_t opened;
            REQUIRE(fips_settle_link_open(&links,&p,boolean(json_array_get(op,2)) ? FIPS_LINK_INBOUND : FIPS_LINK_OUTBOUND,holder(at(op,3)),&opened)==FIPS_OK);
        } else {
            REQUIRE(n<links.next && fips_settle_link_get(&links,n));
            if (type<=3) p=pair(at(op,2));
            switch (type) {
            case 1: REQUIRE(at(op,3)<=2); REQUIRE(fips_settle_link_path(&links,n,&p,(fips_settle_proven)at(op,3))==FIPS_OK); break;
            case 2: q=pair(at(op,3)); REQUIRE(fips_settle_link_moved(&links,n,&p,&q)==FIPS_OK); break;
            case 3: REQUIRE(fips_settle_link_release(&links,n,&p,boolean(json_array_get(op,3)),closes,257,&count)==FIPS_OK); break;
            case 4: fips_settle_link_capable(&links,n); break;
            case 5: REQUIRE(fips_settle_link_retire(&links,n,closes,257,&count)==FIPS_OK); break;
            case 6: fips_settle_link_hold(&links,n,holder(at(op,2))); break;
            default: REQUIRE(false);
            }
        }
        json_t *got=json_array(); for (size_t j=0; j<count; ++j) add(got,integer(pn(&closes[j])));
        equal(got,field(row,"closes")); equal(snapshot(&links),field(row,"want"));
    }
    return json_array_size(rows);
}
static json_t *hex_bytes(const uint8_t *b, size_t n)
{
    static const char digits[]="0123456789abcdef"; REQUIRE(n<=FIPS_SETTLE_MESSAGE_CAPACITY);
    char hex[FIPS_SETTLE_MESSAGE_CAPACITY*2+1];
    for (size_t i=0; i<n; ++i) { hex[2*i]=digits[b[i]>>4]; hex[2*i+1]=digits[b[i]&15]; }
    hex[2*n]=0; return json_string(hex);
}
static size_t check_legs(json_t *trace)
{
    fips_settle_links links; fips_settle_link records[1]; fips_settle_path paths[1];
    fips_settle_legs legs; fips_settle_leg slots[1], old;
    REQUIRE(fips_settle_links_init(&links,records,1,paths,1)==FIPS_OK); fips_settle_legs_init(&legs,slots,1);
    fips_settle_params p=fips_settle_params_default(); p.handshake.secs=120;
    const char *bits=text(field(trace,"multiplier_bits")); REQUIRE(strlen(bits)==16);
    p.backoff=0;
    for (size_t i=0; i<16; ++i) { const char *c=strchr("0123456789abcdef",bits[i]); REQUIRE(c); p.backoff=p.backoff*16+(uint64_t)(c-"0123456789abcdef"); }
    p.resend=fips_duration_from_millis(num(field(trace,"resend")));
    fips_settle_pair addr=pair(1); uint64_t link;
    REQUIRE(fips_settle_link_open(&links,&addr,FIPS_LINK_OUTBOUND,holder(0),&link)==FIPS_OK);
    fips_rekey_index_lease lease={.index=42,.live=true};
    static const uint8_t msg1[]="msg1 of the dial", msg2[]="msg2 of the answer";
    fips_settle_start start={.link=link,.has_peer=true,.peer={{0}},.msg1=msg1,.msg1_len=sizeof(msg1)-1,
        .lease=&lease,.has_held=true,.held=99}; memset(start.peer.bytes,2,16);
    json_t *cap=field(trace,"cap"); start.has_cap=!json_is_null(cap);
    if (start.has_cap) { uint64_t c=num(cap); REQUIRE(c<=UINT32_MAX); start.cap=(uint32_t)c; }
    REQUIRE(fips_settle_leg_open(&legs,&start,boolean(field(trace,"answer")),msg2,sizeof(msg2)-1,fips_test_instant(0),&p,&old)==FIPS_OK);
    REQUIRE(!old.active && !lease.live);
    json_t *rows=field(trace,"rows"); REQUIRE(json_is_array(rows) && json_array_size(rows)>0);
    size_t attempts=0;
    for (size_t i=0; i<json_array_size(rows); ++i) {
        json_t *row=json_array_get(rows,i), *events=json_array();
        fips_instant now=fips_test_instant(num(field(row,"now")));
        fips_settle_leg_out out[1]; fips_settle_pair closes[2]; size_t count, cn;
        REQUIRE(fips_settle_legs_poll(&legs,&links,now,false,out,1,&count,closes,2,&cn)==FIPS_OK);
        for (size_t j=0; j<count; ++j) {
            json_t *event=json_array(); add(event,integer(out[j].reaped ? 1 : 0)); add(event,integer(out[j].link));
            if (out[j].reaped) {
                add(event,out[j].has_peer ? integer(out[j].peer.bytes[0]) : json_null()); add(event,json_boolean(out[j].dialed));
                uint32_t index=0; REQUIRE(fips_rekey_index_lease_release(&out[j].lease,&index)); add(event,integer(index));
                json_t *c=json_array(); for (size_t k=0; k<out[j].close_count; ++k) add(c,integer(pn(&closes[out[j].close_offset+k]))); add(event,c);
            } else {
                add(event,hex_bytes(out[j].bytes,out[j].length));
                if (boolean(field(trace,"retry")) && attempts==0) fips_settle_leg_unsent(&legs,link,now,&p);
                else fips_settle_leg_resent(&legs,link,now,&p);
                ++attempts;
            }
            add(events,event);
        }
        equal(events,field(row,"events"));
        const fips_settle_leg *leg=fips_settle_leg_get(&legs,link);
        json_t *view=json_null();
        if (leg) {
            view=json_array(); add(view,integer(leg->sent)); add(view,integer(leg->resends));
            add(view,leg->has_next ? integer(leg->next.opaque[0]) : json_null()); add(view,integer(leg->deadline.opaque[0]));
            add(view,integer(leg->lease.index)); add(view,leg->has_held ? integer(leg->held) : json_null());
        }
        equal(view,field(row,"leg")); fips_instant wake;
        bool has_wake=fips_settle_legs_next_wake(&legs,&wake);
        equal(has_wake ? integer(wake.opaque[0]) : json_null(),field(row,"wake"));
        equal(integer(fips_settle_legs_answering(&legs)),field(row,"answering"));
        size_t len=0; const uint8_t *stored=fips_settle_leg_stored(&legs,link,msg1,sizeof(msg1)-1,&len);
        equal(stored ? hex_bytes(stored,len) : json_null(),field(row,"stored")); equal(snapshot(&links),field(row,"links"));
    }
    REQUIRE(!fips_settle_leg_get(&legs,link));
    return json_array_size(rows);
}
static bool optional(json_t *v) { REQUIRE(v); return !json_is_null(v); }
static size_t check_gates(json_t *trace)
{
    fips_settle_memory *slots=calloc(8,sizeof(*slots)); REQUIRE(slots);
    fips_settle_gates g; REQUIRE(fips_settle_gates_init(&g,slots,8)==FIPS_OK);
    fips_settle_params p=fips_settle_params_default();
    json_t *rows=field(trace,"rows"); REQUIRE(json_is_array(rows) && json_array_size(rows)>0);
    for (size_t row=0;row<json_array_size(rows);++row) {
        json_t *r=json_array_get(rows,row), *op=field(r,"op"), *result=json_null();
        uint64_t type=at(op,0), id=at(op,1); REQUIRE(id<256); fips_node_addr peer=holder(id).identity;
        fips_instant now=fips_test_instant(num(field(r,"now"))), memory;
        uint8_t e[32], epoch[8];
        switch (type) {
        case 0: {
            REQUIRE(at(op,2)<=5 && at(op,3)<=2);
            fips_settle_ask a={.source=(fips_settle_source)at(op,2),.has_held=at(op,3)!=0,
                .held={.heard=at(op,3)==2,.installed=fips_test_instant(at(op,4)),.has_last=optional(json_array_get(op,5))},
                .has_ephemeral=optional(json_array_get(op,6)),.has_epoch=optional(json_array_get(op,7)),.has_memory=optional(json_array_get(op,9))};
            if (a.held.has_last) a.held.last=fips_test_instant(at(op,5));
            if (a.has_ephemeral) memset(a.ephemeral,(int)at(op,6),32);
            if (a.has_epoch) memset(a.epoch,(int)at(op,7),8);
            if (a.has_memory) a.memory=fips_test_instant(at(op,9));
            REQUIRE(at(op,8)==now.opaque[0]); fips_settle_refusal refusal;
            REQUIRE(fips_settle_gate(&g,peer,&a,now,&p,&refusal)==FIPS_OK); result=integer(refusal); break;
        }
        case 1:
            memset(e,(int)at(op,2),32); memory=optional(json_array_get(op,4)) ? fips_test_instant(at(op,4)) : now;
            REQUIRE(fips_settle_gate_promoted(&g,peer,e,now,&memory,&p)==FIPS_OK); break;
        case 2: {
            bool started; fips_duration length; bool ep=optional(json_array_get(op,2)), setup=optional(json_array_get(op,3));
            if (ep) memset(epoch,(int)at(op,2),8);
            if (setup) memset(e,(int)at(op,3),32);
            memory=optional(json_array_get(op,5)) ? fips_test_instant(at(op,5)) : now;
            REQUIRE(fips_settle_gate_ended(&g,peer,ep ? epoch : NULL,setup ? e : NULL,now,&memory,&p,&started,&length)==FIPS_OK);
            result=started ? integer(length.secs*1000+length.nanos/1000000) : json_null(); break;
        }
        case 3: fips_settle_gate_heard(&g,&peer); break;
        case 4: fips_settle_gate_framed(&g,&peer); break;
        case 5: fips_settle_gate_forget(&g,&peer); break;
        case 6: memset(e,(int)at(op,2),32); REQUIRE(fips_settle_gate_redialed(&g,peer,e,now,&p)==FIPS_OK); break;
        case 7: REQUIRE(fips_settle_gate_swapped(&g,peer,boolean(json_array_get(op,2)),now)==FIPS_OK); break;
        case 8: { uint32_t count; result=fips_settle_gate_count_refused(&g,&peer,&count) ? integer(count) : json_null(); break; }
        case 9: result=integer(fips_settle_gate_take_refused(&g,&peer)); break;
        default: REQUIRE(false);
        }
        equal(result,field(r,"result")); json_t *view=json_array();
        for (unsigned n=0;n<256;++n) {
            fips_node_addr who=holder(n).identity; const fips_settle_memory *m=fips_settle_gate_get(&g,&who); if (!m) continue;
            json_t *v=json_array(); add(v,integer(n)); add(v,integer(m->replay_count)); add(v,integer(m->redial_count));
            add(v,m->silence.present ? integer(m->silence.count) : json_null()); add(v,m->has_stamp ? integer(m->stamp.opaque[0]) : json_null());
            fips_settle_silencing silence;
            json_t *s=json_null();
            if (fips_settle_gate_refusing(&g,&who,now,&silence)) {
                s=json_array(); add(s,silence.has_epoch ? integer(silence.epoch[0]) : json_null()); add(s,integer(silence.silent));
                add(s,integer(silence.remaining.secs*1000+silence.remaining.nanos/1000000));
            }
            add(v,s); add(view,v);
        }
        equal(view,field(r,"view"));
    }
    free(slots); return json_array_size(rows);
}
int main(int argc, char **argv)
{
    REQUIRE(argc<=2); json_error_t error;
    json_t *root=json_load_file(argc==2 ? argv[1] : FIPS_SETTLE_VECTOR,JSON_REJECT_DUPLICATES,&error);
    if (!root) { fprintf(stderr,"settle: invalid fixture: %s\n",error.text); return 1; }
    REQUIRE(json_is_object(root)); REQUIRE(num(field(root,"schema_version"))==1);
    REQUIRE(!strcmp(text(field(root,"pin")),PIN)); REQUIRE(!strcmp(text(field(root,"family")),"settle"));
    REQUIRE(!strcmp(text(field(root,"generator")),"fips-vectors")); REQUIRE(!strcmp(text(field(root,"generator_version")),"0.1.0"));
    REQUIRE(json_is_array(field(root,"gaps")));
    json_t *cases=field(root,"cases"); REQUIRE(json_is_array(cases) && json_array_size(cases)==13);
    size_t rows=0;
    for (size_t i=0; i<json_array_size(cases); ++i) {
        json_t *trace=json_array_get(cases,i); const char *kind=text(field(trace,"kind"));
        if (!strcmp(kind,"links")) rows+=check_links(trace);
        else if (!strcmp(kind,"gates")) rows+=check_gates(trace);
        else { REQUIRE(!strcmp(kind,"legs")); rows+=check_legs(trace); }
    }
    printf("settle: %zu checked traces, %zu checked steps, 0 skipped, 0 failed\n",json_array_size(cases),rows);
    json_decref(root); return 0;
}
