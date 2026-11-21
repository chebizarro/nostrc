/* Independent family runner: does not alter D1's shared schema/pin scaffolding. */
#include <fips/proto/settle/gates.h>
#include <fips/proto/settle/model/judge.h>
#include "../fakes/host.h"
#include "../fakes/settle_table.h"
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
static size_t unhex(json_t *v,uint8_t *out,size_t capacity)
{
    const char *s=text(v); size_t n=strlen(s); REQUIRE(!(n&1) && n/2<=capacity);
    for (size_t i=0;i<n;i+=2) {
        const char *a=strchr("0123456789abcdef",s[i]), *b=strchr("0123456789abcdef",s[i+1]); REQUIRE(a && b);
        out[i/2]=(uint8_t)((a-"0123456789abcdef")*16+(b-"0123456789abcdef"));
    }
    return n/2;
}
static void fixed(json_t *v,uint8_t *out,size_t n) { REQUIRE(unhex(v,out,n)==n); }
static fips_node_addr full_node(json_t *v) { fips_node_addr p; fixed(v,p.bytes,16); return p; }
static json_t *node_json(fips_node_addr p) { return hex_bytes(p.bytes,16); }
static fips_settle_pair full_pair(json_t *v)
{
    uint64_t transport=at(v,0); REQUIRE(transport<=UINT32_MAX); fips_settle_pair p={.transport={(uint32_t)transport}};
    uint8_t buf[FIPS_TRANSPORT_ADDR_CAPACITY]; size_t n=unhex(json_array_get(v,1),buf,sizeof(buf));
    REQUIRE(fips_transport_addr_new(buf,n,&p.addr)==FIPS_OK); return p;
}
static json_t *pair_json(const fips_settle_pair *p)
{ json_t *a=json_array(); add(a,integer(p->transport.value)); add(a,hex_bytes(p->addr.bytes,p->addr.len)); return a; }
static fips_settle_read full_read(json_t *v,uint8_t *bytes)
{
    fips_settle_read r={.peer=full_node(json_array_get(v,0)),.from=full_pair(json_array_get(v,1))};
    r.bytes=(fips_settle_bytes){bytes,unhex(json_array_get(v,2),bytes,FIPS_SETTLE_MESSAGE_CAPACITY)};
    fixed(json_array_get(v,3),r.ephemeral,32); fixed(json_array_get(v,4),r.epoch,8); return r;
}
static fips_settle_input full_input(json_t *v,uint8_t *bytes,fips_rekey_index_lease *lease)
{
    uint64_t kind=at(v,0); REQUIRE(kind<=FIPS_SETTLE_IN_STATE); fips_settle_input in={.kind=(fips_settle_input_kind)kind};
    json_t *a=json_array_get(v,1);
    switch (in.kind) {
    case FIPS_SETTLE_IN_DIAL: case FIPS_SETTLE_IN_PUNCHED:
        in.data.dial.peer=full_node(json_array_get(a,0)); in.data.dial.to=full_pair(json_array_get(a,1));
        in.data.dial.msg1=(fips_settle_bytes){bytes,unhex(json_array_get(a,2),bytes,FIPS_SETTLE_MESSAGE_CAPACITY)};
        REQUIRE(at(a,3)<=UINT32_MAX); *lease=(fips_rekey_index_lease){.live=true,.index=(uint32_t)at(a,3)}; in.data.dial.lease=lease;
        in.data.dial.has_cap=optional(json_array_get(a,4)); if (in.data.dial.has_cap) { REQUIRE(at(a,4)<=UINT32_MAX); in.data.dial.cap=(uint32_t)at(a,4); }
        in.data.dial.has_link=optional(json_array_get(a,5)); if (in.data.dial.has_link) in.data.dial.link=at(a,5); break;
    case FIPS_SETTLE_IN_CONNECTING:
        in.data.connecting.peer=full_node(a); in.data.connecting.to=full_pair(json_array_get(v,2));
        in.data.connecting.has_cap=optional(json_array_get(v,3)); if (in.data.connecting.has_cap) { REQUIRE(at(v,3)<=UINT32_MAX); in.data.connecting.cap=(uint32_t)at(v,3); } break;
    case FIPS_SETTLE_IN_ARRIVED: in.data.arrived.from=full_pair(a); in.data.arrived.bytes=(fips_settle_bytes){bytes,unhex(json_array_get(v,2),bytes,FIPS_SETTLE_MESSAGE_CAPACITY)}; break;
    case FIPS_SETTLE_IN_MSG1: in.data.msg1.read=full_read(a,bytes); in.data.msg1.reachable=boolean(json_array_get(v,2)); break;
    case FIPS_SETTLE_IN_REPLACE: case FIPS_SETTLE_IN_REDIAL: in.data.read=full_read(a,bytes); break;
    case FIPS_SETTLE_IN_LEG_HEARD: in.data.leg_heard.link=num(a); in.data.leg_heard.from=full_pair(json_array_get(v,2)); break;
    case FIPS_SETTLE_IN_HELLO:
        in.data.hello.link=num(a); in.data.hello.msg2=(fips_settle_bytes){bytes,unhex(json_array_get(v,2),bytes,FIPS_SETTLE_MESSAGE_CAPACITY)};
        REQUIRE(at(v,3)<=UINT32_MAX); *lease=(fips_rekey_index_lease){.live=true,.index=(uint32_t)at(v,3)}; in.data.hello.lease=lease; break;
    case FIPS_SETTLE_IN_ABANDON: in.data.abandon.link=num(a); in.data.abandon.authorized=boolean(json_array_get(v,2)); break;
    case FIPS_SETTLE_IN_MSG2: in.data.msg2.link=num(a); fixed(json_array_get(v,2),in.data.msg2.epoch,8); in.data.msg2.from=full_pair(json_array_get(v,3)); break;
    case FIPS_SETTLE_IN_RESTARTED: case FIPS_SETTLE_IN_PEER_EPOCH: in.data.epoch.peer=full_node(a); fixed(json_array_get(v,2),in.data.epoch.epoch,8); break;
    case FIPS_SETTLE_IN_HEARD: in.data.heard.peer=full_node(a); in.data.heard.from=full_pair(json_array_get(v,2)); REQUIRE(at(v,3)<=UINT8_MAX); in.data.heard.kind=(uint8_t)at(v,3); break;
    case FIPS_SETTLE_IN_MOVED: in.data.moved.link=num(a); in.data.moved.from=full_pair(json_array_get(v,2)); in.data.moved.to=full_pair(json_array_get(v,3)); in.data.moved.roam=boolean(json_array_get(v,4)); break;
    case FIPS_SETTLE_IN_SWITCHED: in.data.switched.link=num(a); in.data.switched.to=full_pair(json_array_get(v,2)); REQUIRE(at(v,3)<=2); in.data.switched.proven=(fips_settle_proven)at(v,3); break;
    case FIPS_SETTLE_IN_RELEASED: in.data.released.link=num(a); in.data.released.pair=full_pair(json_array_get(v,2)); in.data.released.keep=boolean(json_array_get(v,3)); break;
    case FIPS_SETTLE_IN_STATE: in.data.state.link=num(a); in.data.state.pair=full_pair(json_array_get(v,2)); REQUIRE(at(v,3)<=2); in.data.state.proven=(fips_settle_proven)at(v,3); break;
    case FIPS_SETTLE_IN_ENDED: case FIPS_SETTLE_IN_ORPHANED: case FIPS_SETTLE_IN_DISCONNECT: case FIPS_SETTLE_IN_DEAD: case FIPS_SETTLE_IN_ADMIN: in.data.peer=full_node(a); break;
    case FIPS_SETTLE_IN_CONNECT_FAILED: case FIPS_SETTLE_IN_SENT: case FIPS_SETTLE_IN_SEND_FAILED: case FIPS_SETTLE_IN_UNSENDABLE: case FIPS_SETTLE_IN_CAPABLE: in.data.link=num(a); break;
    }
    return in;
}
static json_t *closes_json(fips_settle_outputs out,const fips_settle_output *o)
{
    REQUIRE(o->close_offset<=out.close_count && o->close_count<=out.close_count-o->close_offset);
    json_t *a=json_array(); for (size_t i=0;i<o->close_count;++i) add(a,pair_json(&out.closes[o->close_offset+i])); return a;
}
static json_t *table_outputs_json(fips_settle_table *t)
{
    static const char *transitions[]={"Promote","Replace","Move","Keep","Restarted","Roam"};
    static const char *whys[]={"Restart","Replacement","Redial","OwnRestart","Ended","Orphaned","Disconnect","Dead","Admin","Closed"};
    static const char *sources[]={"NotHeld","Restart","Replacement","Redial","OwnMsg2","RekeyMsg2"};
    static const char *refusals[]={"Accept","Unheard","Heard","Dampened","Replayed","ReplayFull","Silent","Redialed"};
    fips_settle_outputs out=fips_settle_table_outputs(t); json_t *all=json_array();
    for (size_t i=0;i<out.count;++i) {
        fips_settle_output *o=&out.items[i]; json_t *a=json_array(); add(a,integer(o->kind));
        switch (o->kind) {
        case FIPS_SETTLE_OUT_OPENED: add(a,integer(o->link)); break;
        case FIPS_SETTLE_OUT_ASK: add(a,integer(o->link)); add(a,node_json(o->peer)); break;
        case FIPS_SETTLE_OUT_ANSWER: add(a,integer(o->link)); add(a,hex_bytes(o->bytes,o->length)); add(a,json_boolean(o->stored)); break;
        case FIPS_SETTLE_OUT_RESEND: add(a,integer(o->link)); add(a,hex_bytes(o->bytes,o->length)); break;
        case FIPS_SETTLE_OUT_REKEY: add(a,node_json(o->peer)); add(a,json_boolean(o->on_link)); add(a,json_boolean(o->reachable)); add(a,json_boolean(o->heard)); break;
        case FIPS_SETTLE_OUT_INSTALL: REQUIRE(o->lease.live); add(a,node_json(o->peer)); add(a,integer(o->link)); add(a,pair_json(&o->pair)); add(a,json_boolean(o->build==FIPS_SETTLE_BUILD_FROM_CARRY)); add(a,json_boolean(o->initiator)); add(a,integer(o->lease.index)); break;
        case FIPS_SETTLE_OUT_RESET: REQUIRE((unsigned)o->transition<6); add(a,node_json(o->peer)); add(a,json_string(transitions[o->transition])); break;
        case FIPS_SETTLE_OUT_REMOVE: REQUIRE((unsigned)o->why<10); add(a,node_json(o->peer)); add(a,json_string(whys[o->why])); break;
        case FIPS_SETTLE_OUT_RETIRE: case FIPS_SETTLE_OUT_RELEASE: add(a,integer(o->link)); add(a,closes_json(out,o)); break;
        case FIPS_SETTLE_OUT_CLOSE: add(a,closes_json(out,o)); break;
        case FIPS_SETTLE_OUT_FREE: REQUIRE(o->lease.live); add(a,integer(o->lease.index)); break;
        case FIPS_SETTLE_OUT_SWAPPED: add(a,node_json(o->peer)); add(a,json_boolean(o->own)); add(a,json_boolean(o->unpin)); break;
        case FIPS_SETTLE_OUT_RETRY: add(a,node_json(o->peer)); break;
        case FIPS_SETTLE_OUT_REJECT: {
            REQUIRE((unsigned)o->denial<4 && (unsigned)o->source<6 && (unsigned)o->refusal<8);
            add(a,o->has_peer ? node_json(o->peer) : json_null()); add(a,o->has_source ? json_string(sources[o->source]) : json_null());
            json_t *d=json_array(); add(d,integer(o->denial)); if (o->denial==FIPS_SETTLE_DENIAL_GATE) add(d,json_string(refusals[o->refusal])); add(a,d); break;
        }
        }
        add(all,a);
    }
    return all;
}
static int node_cmp(const void *a,const void *b) { return memcmp(((const fips_node_addr *)a)->bytes,((const fips_node_addr *)b)->bytes,16); }
typedef struct index_row { fips_settle_pair pair; uint64_t link; } index_row;
static int index_cmp(const void *a,const void *b)
{
    const index_row *x=a,*y=b;
    if (x->pair.transport.value!=y->pair.transport.value) return x->pair.transport.value<y->pair.transport.value ? -1 : 1;
    size_t n=x->pair.addr.len<y->pair.addr.len ? x->pair.addr.len : y->pair.addr.len;
    int c=memcmp(x->pair.addr.bytes,y->pair.addr.bytes,n); if (c) return c;
    if (x->pair.addr.len!=y->pair.addr.len) return x->pair.addr.len<y->pair.addr.len ? -1 : 1;
    return x->link==y->link ? 0 : x->link<y->link ? -1 : 1;
}
static json_t *table_snapshot(fips_settle_table *t,fips_instant now)
{
    fips_settle_table_state *s=&t->states[t->active];
    json_t *root=json_object(),*records=json_array(),*index=json_array(),*peers=json_array(),*legs=json_array(),*gates=json_array(),*redials=json_array();
    size_t capacity=s->storage.capacity*(s->storage.paths_per_link+1), count=0;
    index_row *idx=fips_test_settle_array(capacity,sizeof(*idx));
    for (uint64_t n=0;n<s->links.next;++n) {
        const fips_settle_link *r=fips_settle_link_get(&s->links,n); if (!r) continue;
        json_t *a=json_array(),*paths=json_array(); add(a,integer(n)); add(a,pair_json(&r->setup)); add(a,json_boolean(r->direction==FIPS_LINK_INBOUND));
        add(a,r->holder.peer ? node_json(r->holder.identity) : json_null()); add(a,json_boolean(r->released)); add(a,json_boolean(r->capable));
        if (!r->released) idx[count++]=(index_row){r->setup,n};
        for (size_t i=0;i<r->path_count;++i) { json_t *p=json_array(); add(p,pair_json(&r->paths[i].pair)); add(p,integer(r->paths[i].state)); add(paths,p); if (r->paths[i].state!=FIPS_SETTLE_PROBING) idx[count++]=(index_row){r->paths[i].pair,n}; }
        add(a,paths); add(records,a);
    }
    REQUIRE(count<=capacity); qsort(idx,count,sizeof(*idx),index_cmp);
    for (size_t i=0;i<count;++i) if (!i || index_cmp(&idx[i-1],&idx[i])) { json_t *a=json_array(); add(a,pair_json(&idx[i].pair)); add(a,integer(idx[i].link)); add(index,a); }
    free(idx);
    fips_node_addr *known=fips_test_settle_array(2*s->storage.capacity+s->storage.gate_capacity,sizeof(*known)); size_t kn=0;
    for (size_t i=0;i<s->storage.capacity;++i) { if (s->storage.held[i].active) known[kn++]=s->storage.held[i].peer; if (s->storage.meta[i].active && s->storage.meta[i].pending_redial) known[kn++]=s->storage.meta[i].peer; }
    for (size_t i=0;i<s->gates.capacity;++i) if (s->gates.slots[i].active) known[kn++]=s->gates.slots[i].peer;
    qsort(known,kn,sizeof(*known),node_cmp);
    for (size_t i=0;i<kn;++i) {
        if (i && !node_cmp(&known[i-1],&known[i])) continue;
        fips_node_addr p=known[i]; const fips_settle_held *h=fips_settle_table_held(t,&p);
        if (h) {
            json_t *a=json_array(); add(a,node_json(p)); add(a,integer(h->link)); add(a,hex_bytes(h->epoch,8)); add(a,pair_json(&h->current)); add(a,json_boolean(h->heard));
            add(a,integer(h->installed.opaque[0])); add(a,h->has_last ? integer(h->last.opaque[0]) : json_null()); add(a,h->has_setup ? hex_bytes(h->setup,32) : json_null()); add(a,h->has_sending ? pair_json(&h->sending) : json_null()); add(peers,a);
        }
        const fips_settle_memory *m=fips_settle_gate_get(&s->gates,&p);
        if (m) {
            json_t *a=json_array(),*silence=json_null(); add(a,node_json(p)); add(a,integer(m->replay_count)); add(a,integer(m->redial_count)); add(a,m->silence.present ? integer(m->silence.count) : json_null()); add(a,m->has_stamp ? integer(m->stamp.opaque[0]) : json_null());
            fips_settle_silencing r;
            if (fips_settle_gate_refusing(&s->gates,&p,now,&r)) { silence=json_array(); add(silence,r.has_epoch ? hex_bytes(r.epoch,8) : json_null()); add(silence,integer(r.silent)); add(silence,integer(r.remaining.secs*1000+r.remaining.nanos/1000000)); }
            add(a,silence); add(gates,a);
        }
        uint64_t link; if (fips_settle_table_redialing(t,&p,&link)) { json_t *a=json_array(); add(a,node_json(p)); add(a,integer(link)); add(redials,a); }
    }
    free(known);
    for (uint64_t n=0;n<s->links.next;++n) {
        const fips_settle_leg *l=fips_settle_leg_get(&s->legs,n); if (!l) continue;
        json_t *a=json_array(); add(a,integer(n)); add(a,l->has_peer ? node_json(l->peer) : json_null()); add(a,hex_bytes(l->msg1,l->msg1_len)); add(a,l->answer ? hex_bytes(l->msg2,l->msg2_len) : json_null());
        add(a,integer(l->sent)); add(a,integer(l->resends)); add(a,l->has_next ? integer(l->next.opaque[0]) : json_null()); add(a,integer(l->deadline.opaque[0])); add(a,integer(l->lease.index)); add(a,l->has_held ? integer(l->held) : json_null()); add(legs,a);
    }
    put(root,"records",records); put(root,"index",index); put(root,"held",peers); put(root,"legs",legs); put(root,"gates",gates); put(root,"redials",redials);
    fips_instant wake; bool found=fips_settle_table_next_wake(t,&wake); put(root,"wake",found ? integer(wake.opaque[0]) : json_null()); return root;
}
static size_t check_table(json_t *trace)
{
    fips_test_settle_table *tables[1024]={0}; fips_instant last[1024];
    json_t *rows=field(trace,"rows"); REQUIRE(json_is_array(rows) && json_array_size(rows)>0); (void)text(field(trace,"test"));
    for (size_t n=0;n<json_array_size(rows);++n) {
        json_t *row=json_array_get(rows,n),*op=field(row,"op"); size_t id=(size_t)num(field(row,"id")); REQUIRE(id<1024);
        const char *kind=text(json_array_get(op,0));
        if (!strcmp(kind,"new")) {
            REQUIRE(!tables[id]); fips_settle_params p=fips_settle_params_default(); p.pending=(size_t)at(op,2); p.peers=(size_t)at(op,3); REQUIRE(at(op,4)<=UINT32_MAX); p.resends=(uint32_t)at(op,4);
            tables[id]=fips_test_settle_table_create(full_node(json_array_get(op,1)),&p,64,32,512,4096); last[id]=fips_test_instant(0); continue;
        }
        REQUIRE(tables[id]);
        if (!strcmp(kind,"drop")) { fips_test_settle_table_destroy(tables[id],last[id]); tables[id]=NULL; continue; }
        fips_settle_table *t=&tables[id]->table; fips_instant now=fips_test_instant(num(field(row,"now"))); last[id]=now;
        fips_status status=FIPS_ERR_RANGE; bool fixture=false;
        if (!strcmp(kind,"step")) {
            uint8_t bytes[FIPS_SETTLE_MESSAGE_CAPACITY]; fips_rekey_index_lease lease={0}; fips_settle_input in=full_input(json_array_get(op,1),bytes,&lease);
            status=fips_settle_table_step(t,&in,now); REQUIRE(status!=FIPS_OK || !lease.live);
        } else if (!strcmp(kind,"poll")) status=fips_settle_table_poll(t,now);
        else if (!strcmp(kind,"close")) status=fips_settle_table_close(t,now);
        else if (!strcmp(kind,"reap")) status=fips_settle_table_reap_leg(t,at(op,1),now);
        else if (!strcmp(kind,"gate_ended")) {
            /* Pinned upstream test plants one silent-session record through a
             * private writer. This fixture operation is not a production input. */
            fixture=true; fips_node_addr peer=full_node(json_array_get(op,1)); uint8_t epoch[8],e[32]; bool ep=optional(json_array_get(op,2)), setup=optional(json_array_get(op,3)), started; fips_duration length;
            if (ep) fixed(json_array_get(op,2),epoch,8); if (setup) fixed(json_array_get(op,3),e,32);
            fips_instant memory=optional(json_array_get(op,4)) ? fips_test_instant(at(op,4)) : now;
            status=fips_settle_gate_ended(&t->states[t->active].gates,peer,ep ? epoch : NULL,setup ? e : NULL,now,&memory,&t->params,&started,&length);
        }
        if (status!=FIPS_OK) fprintf(stderr,"table trace %s row %zu operation %s failed\n",text(field(trace,"test")),n,kind);
        REQUIRE(status==FIPS_OK); REQUIRE(t->states[t->active].links.next==num(field(row,"next")));
        if (!fixture) equal(table_outputs_json(t),field(row,"out"));
        equal(table_snapshot(t,now),field(row,"state")); fips_test_settle_drain(t);
    }
    for (size_t i=0;i<1024;++i) REQUIRE(!tables[i]);
    return json_array_size(rows);
}
static uint32_t number32(json_t *v) { uint64_t n=num(v); REQUIRE(n<=UINT32_MAX); return (uint32_t)n; }
static fips_settle_model_addr model_addr(json_t *v)
{ REQUIRE(at(v,0)<=UINT8_MAX && at(v,1)<=UINT16_MAX); return (fips_settle_model_addr){(uint8_t)at(v,0),(uint16_t)at(v,1)}; }
static json_t *model_addr_json(fips_settle_model_addr a)
{ json_t *v=json_array(); add(v,integer(a.transport)); add(v,integer(a.at)); return v; }
static fips_settle_model_event model_event(json_t *v)
{
    uint64_t kind=at(v,0); REQUIRE(kind<=FIPS_MODEL_RESTART); fips_settle_model_event e={.kind=(fips_settle_model_event_kind)kind};
    switch (e.kind) {
    case FIPS_MODEL_ADVANCE: e.ms=at(v,1); break;
    case FIPS_MODEL_DIAL: e.hs=number32(json_array_get(v,1)); e.link=number32(json_array_get(v,2)); e.to=model_addr(json_array_get(v,3)); e.binding=boolean(json_array_get(v,4)) ? FIPS_MODEL_CONNECTED : FIPS_MODEL_CONNECTIONLESS; break;
    case FIPS_MODEL_MSG1: e.hs=number32(json_array_get(v,1)); e.bytes=number32(json_array_get(v,2)); e.epoch=number32(json_array_get(v,3)); e.link=number32(json_array_get(v,4)); e.from=model_addr(json_array_get(v,5)); e.binding=boolean(json_array_get(v,6)) ? FIPS_MODEL_CONNECTED : FIPS_MODEL_CONNECTIONLESS; e.sent=boolean(json_array_get(v,7)); break;
    case FIPS_MODEL_MSG2_SENT: case FIPS_MODEL_TIMEOUT: case FIPS_MODEL_REKEY_START: e.hs=number32(json_array_get(v,1)); break;
    case FIPS_MODEL_MSG2: e.hs=number32(json_array_get(v,1)); e.epoch=number32(json_array_get(v,2)); e.from=model_addr(json_array_get(v,3)); break;
    case FIPS_MODEL_FRAME: e.hs=number32(json_array_get(v,1)); e.from=model_addr(json_array_get(v,2)); REQUIRE(at(v,3)<=UINT8_MAX); e.paths=(uint8_t)at(v,3); e.message=boolean(json_array_get(v,4)); break;
    case FIPS_MODEL_SWITCH: e.to=model_addr(json_array_get(v,1)); break;
    case FIPS_MODEL_FOLLOW: e.from=model_addr(json_array_get(v,1)); e.to=model_addr(json_array_get(v,2)); break;
    case FIPS_MODEL_REKEY_MSG2: e.hs=number32(json_array_get(v,1)); e.epoch=number32(json_array_get(v,2)); break;
    case FIPS_MODEL_CLOSE: e.from=model_addr(json_array_get(v,1)); break;
    case FIPS_MODEL_REKEY_CUTOVER: case FIPS_MODEL_LOST: case FIPS_MODEL_RESTART: break;
    }
    return e;
}
static json_t *name_or_null(bool has,unsigned n,const char *const *names,size_t count)
{ if (!has) return json_null(); REQUIRE(n<count); return json_string(names[n]); }
static json_t *model_step_json(const fips_settle_model_step *s)
{
    static const char *const classes[]={"NotHeld","Restart","Resend","Copy","Replacement","OffLink","Pending","Recorded","Simultaneous","Rekey","Redial"};
    static const char *const gates[]={"Unheard","Heard","Dampened","Slot"};
    static const char *const replies[]={"Silent","Msg2","Stored","Deferred"};
    static const char *const changes[]={"Promote","Replace","Move","Keep","RekeyCutover","Restarted","Roam","Remove"};
    static const char *const rules[]={"LateSecondDial","StoredLate","DampenerStamp","AgedLink","BetweenFloors","RecordScope","SessionAge","DeferredRedial"};
    json_t *a=json_array(),*closes=json_array(); REQUIRE(s->close_count<=1);
    add(a,name_or_null(s->has_class,(unsigned)s->class_,classes,11)); add(a,name_or_null(s->has_refused,(unsigned)s->refused,gates,4));
    add(a,name_or_null(s->has_reply,(unsigned)s->reply,replies,4)); add(a,name_or_null(s->has_change,(unsigned)s->change,changes,8));
    for (size_t i=0;i<s->close_count;++i) add(closes,model_addr_json(s->closes[i]));
    add(a,closes); add(a,json_boolean(s->disconnect)); add(a,name_or_null(s->has_rule,(unsigned)s->rule,rules,8)); add(a,json_boolean(s->open)); return a;
}
static json_t *model_leg_json(fips_settle_model_leg l)
{
    json_t *a=json_array(); add(a,integer(l.hs)); add(a,integer(l.bytes)); add(a,integer(l.epoch)); add(a,integer(l.link)); add(a,model_addr_json(l.from)); add(a,json_boolean(l.binding==FIPS_MODEL_CONNECTED)); return a;
}
static json_t *model_snapshot(const fips_settle_model_judge *j)
{
    json_t *root=json_object(),*params=json_array(),*p=json_null(),*attempts=json_array(),*legs=json_array(),*redial=json_null(),*closed=json_array(),*settled=json_null(),*needs=json_array();
    add(params,integer(j->params.unheard)); add(params,integer(j->params.interval)); add(params,integer(j->params.linkage)); add(params,integer(j->params.sessionage));
    if (j->has_peering) {
        const fips_settle_model_peering *h=&j->peering; p=json_array();
        add(p,integer(h->hs)); add(p,integer(h->epoch)); add(p,integer(h->link)); add(p,json_boolean(h->binding==FIPS_MODEL_CONNECTED)); add(p,model_addr_json(h->setup)); add(p,json_boolean(h->dialed));
        add(p,h->has_msg1 ? integer(h->msg1) : json_null()); add(p,model_addr_json(h->current)); add(p,integer(h->installed)); add(p,integer(h->linkborn)); add(p,integer(h->sessionborn)); add(p,json_boolean(h->heard)); add(p,h->has_last ? integer(h->last) : json_null());
        json_t *pending=json_null(),*fallback=json_null(),*record=json_array();
        if (h->has_pending) { const fips_settle_model_pending *pd=&h->pending; pending=json_array(); add(pending,integer(pd->hs)); add(pending,json_boolean(pd->initiator)); add(pending,pd->has_armed ? integer(pd->armed) : json_null()); add(pending,pd->has_restarted ? integer(pd->restarted) : json_null()); }
        if (h->has_fallback) { const fips_settle_model_fallback *f=&h->fallback; fallback=json_array(); add(fallback,integer(f->hs)); add(fallback,integer(f->link)); add(fallback,json_boolean(f->binding==FIPS_MODEL_CONNECTED)); add(fallback,model_addr_json(f->setup)); add(fallback,model_addr_json(f->current)); }
        REQUIRE(h->record_count<=FIPS_SETTLE_MODEL_CAPACITY); for (size_t i=0;i<h->record_count;++i) add(record,integer(h->record[i]));
        add(p,pending); add(p,fallback); add(p,record);
    }
    for (size_t i=0;i<j->attempt_count;++i) { const fips_settle_model_attempt *a=&j->attempts[i]; json_t *v=json_array(); add(v,integer(a->hs)); add(v,integer(a->link)); add(v,model_addr_json(a->to)); add(v,json_boolean(a->binding==FIPS_MODEL_CONNECTED)); add(v,json_boolean(a->aged)); add(attempts,v); }
    for (size_t i=0;i<j->leg_count;++i) add(legs,model_leg_json(j->legs[i]));
    if (j->has_redial) { redial=json_array(); add(redial,model_leg_json(j->redial.leg)); add(redial,integer(j->redial.at)); add(redial,json_boolean(j->redial.sent)); }
    for (size_t i=0;i<j->closed_count;++i) add(closed,model_addr_json(j->closed[i]));
    fips_settle_model_need held; uint32_t hs;
    if (fips_settle_model_settled(j,&held,&hs)) { settled=json_array(); add(settled,integer(hs)); add(settled,integer(held.link)); add(settled,model_addr_json(held.at)); }
    fips_settle_model_need all[2*FIPS_SETTLE_MODEL_CAPACITY+3]; size_t count;
    REQUIRE(fips_settle_model_needs(j,all,2*FIPS_SETTLE_MODEL_CAPACITY+3,&count)==FIPS_OK);
    for (size_t i=0;i<count;++i) { json_t *a=json_array(); add(a,integer(all[i].link)); add(a,model_addr_json(all[i].at)); add(needs,a); }
    put(root,"me",integer(j->me)); put(root,"peer",integer(j->peer)); put(root,"params",params); put(root,"now",integer(j->now)); put(root,"peering",p); put(root,"attempts",attempts); put(root,"legs",legs); put(root,"redial",redial); put(root,"rekey",j->has_rekey ? integer(j->rekey) : json_null()); put(root,"stamp",j->has_stamp ? integer(j->stamp) : json_null()); put(root,"closed",closed); put(root,"settled",settled); put(root,"needs",needs); return root;
}
typedef struct gate_model_beside {
    fips_settle_gates gates;
    fips_settle_memory memory;
    bool has_held;
    fips_settle_liveness held;
} gate_model_beside;
static void check_gate_model(gate_model_beside *b,const fips_settle_model_judge *j,fips_settle_model_event e,const fips_settle_model_step *step)
{
    fips_node_addr peer=holder(2).identity; fips_settle_params p=fips_settle_params_default(); fips_instant now=fips_test_instant(j->now);
    fips_settle_ask a={.has_held=b->has_held,.held=b->held,.has_epoch=true}; bool question=false, promote=false, own=false;
    for (unsigned i=0;i<4;++i) { a.ephemeral[i]=(uint8_t)(e.hs>>(24-8*i)); a.epoch[i]=(uint8_t)(e.epoch>>(24-8*i)); }
    if (e.kind==FIPS_MODEL_MSG1 && step->has_class) {
        switch (step->class_) {
        case FIPS_MODEL_CLASS_NOT_HELD: a.source=FIPS_SETTLE_SOURCE_NOT_HELD; question=promote=true; break;
        case FIPS_MODEL_CLASS_RESTART: a.source=FIPS_SETTLE_SOURCE_RESTART; question=promote=true; break;
        case FIPS_MODEL_CLASS_REPLACEMENT: a.source=FIPS_SETTLE_SOURCE_REPLACEMENT; question=promote=true; break;
        default: break;
        }
        a.has_ephemeral=true;
    } else if (e.kind==FIPS_MODEL_MSG2 && step->has_change) {
        if (step->change==FIPS_MODEL_TRANSITION_PROMOTE) { b->has_held=true; b->held=(fips_settle_liveness){.heard=true,.installed=now}; }
        else if (step->change==FIPS_MODEL_TRANSITION_REPLACE) { REQUIRE(b->has_held); a.source=FIPS_SETTLE_SOURCE_OWN_MSG2; question=own=true; }
    } else if (e.kind==FIPS_MODEL_REKEY_CUTOVER && step->has_change && step->change==FIPS_MODEL_TRANSITION_RESTARTED) {
        REQUIRE(b->has_held); a.source=FIPS_SETTLE_SOURCE_REKEY_MSG2; memset(a.epoch,2,8); question=true;
    } else if (e.kind==FIPS_MODEL_FRAME) {
        if (b->has_held) { b->held.heard=true; b->held.has_last=true; b->held.last=now; }
        fips_settle_gate_heard(&b->gates,&peer);
    }
    if (question) {
        fips_settle_refusal refusal; REQUIRE(fips_settle_gate(&b->gates,peer,&a,now,&p,&refusal)==FIPS_OK);
        if (refusal==FIPS_SETTLE_GATE_ACCEPT) {
            REQUIRE(!step->has_refused);
            if (promote || own) { b->has_held=true; b->held=(fips_settle_liveness){.heard=own,.installed=now}; }
            if (promote) REQUIRE(fips_settle_gate_promoted(&b->gates,peer,a.ephemeral,now,NULL,&p)==FIPS_OK);
        } else REQUIRE(step->has_refused && refusal>=FIPS_SETTLE_GATE_UNHEARD && refusal<=FIPS_SETTLE_GATE_DAMPENED && (unsigned)refusal-1==(unsigned)step->refused);
    }
    const fips_settle_memory *m=fips_settle_gate_get(&b->gates,&peer);
    uint64_t at=m && m->has_stamp ? m->stamp.opaque[0] : 0;
    bool core_stamp=m && m->has_stamp && (j->now>=at ? j->now-at : 0)<j->params.interval;
    bool model_stamp=j->has_stamp && (j->now>=j->stamp ? j->now-j->stamp : 0)<j->params.interval;
    REQUIRE(core_stamp==model_stamp && (!core_stamp || at==j->stamp));
}
static size_t check_model(json_t *trace,bool with_gates)
{
    fips_settle_model_judge *judges[1024]={0}; gate_model_beside *beside[1024]={0}; json_t *rows=field(trace,"rows"); REQUIRE(json_is_array(rows) && json_array_size(rows));
    (void)text(field(trace,"test"));
    for (size_t i=0;i<json_array_size(rows);++i) {
        json_t *row=json_array_get(rows,i),*op=field(row,"op"); size_t id=(size_t)num(field(row,"id")); REQUIRE(id<1024); const char *kind=text(json_array_get(op,0));
        if (!strcmp(kind,"drop")) { REQUIRE(judges[id]); free(judges[id]); free(beside[id]); judges[id]=NULL; beside[id]=NULL; continue; }
        if (!strcmp(kind,"new")) {
            REQUIRE(!judges[id] && at(op,1)<=UINT8_MAX && at(op,2)<=UINT8_MAX); json_t *p=json_array_get(op,3);
            fips_settle_model_params params={at(p,0),at(p,1),at(p,2),at(p,3)};
            judges[id]=fips_test_settle_array(1,sizeof(*judges[id])); fips_settle_model_init(judges[id],(uint8_t)at(op,1),(uint8_t)at(op,2),params);
            if (with_gates) { REQUIRE(judges[id]->me==1 && judges[id]->peer==2); beside[id]=fips_test_settle_array(1,sizeof(*beside[id])); REQUIRE(fips_settle_gates_init(&beside[id]->gates,&beside[id]->memory,1)==FIPS_OK); }
        } else {
            REQUIRE(!strcmp(kind,"step") && judges[id]); fips_settle_model_step result;
            fips_settle_model_event e=model_event(json_array_get(op,1));
            REQUIRE(fips_settle_model_step_event(judges[id],e,&result)==FIPS_OK); equal(model_step_json(&result),field(row,"out"));
            if (with_gates) check_gate_model(beside[id],judges[id],e,&result);
        }
        equal(model_snapshot(judges[id]),field(row,"state"));
    }
    for (size_t i=0;i<1024;++i) REQUIRE(!judges[i]); return json_array_size(rows);
}
#include "settle_join.h"
#include "settle_parity.h"
int main(int argc, char **argv)
{
    REQUIRE(argc<=2); json_error_t error;
    json_t *root=json_load_file(argc==2 ? argv[1] : FIPS_SETTLE_VECTOR,JSON_REJECT_DUPLICATES,&error);
    if (!root) { fprintf(stderr,"settle: invalid fixture: %s\n",error.text); return 1; }
    REQUIRE(json_is_object(root)); REQUIRE(num(field(root,"schema_version"))==1);
    REQUIRE(!strcmp(text(field(root,"pin")),PIN)); REQUIRE(!strcmp(text(field(root,"family")),"settle"));
    REQUIRE(!strcmp(text(field(root,"generator")),"fips-vectors")); REQUIRE(!strcmp(text(field(root,"generator_version")),"0.1.0"));
    REQUIRE(json_is_array(field(root,"gaps")));
    json_t *cases=field(root,"cases"); REQUIRE(json_is_array(cases) && json_array_size(cases)==161);
    size_t rows=0;
    for (size_t i=0; i<json_array_size(cases); ++i) {
        json_t *trace=json_array_get(cases,i); const char *kind=text(field(trace,"kind"));
        if (!strcmp(kind,"links")) rows+=check_links(trace);
        else if (!strcmp(kind,"gates")) rows+=check_gates(trace);
        else if (!strcmp(kind,"table")) rows+=check_table(trace);
        else if (!strcmp(kind,"model")) rows+=check_model(trace,false);
        else if (!strcmp(kind,"gate_model")) rows+=check_model(trace,true);
        else if (!strcmp(kind,"join")) rows+=check_join(trace);
        else if (!strcmp(kind,"parity")) rows+=check_parity(trace);
        else { REQUIRE(!strcmp(kind,"legs")); rows+=check_legs(trace); }
    }
    printf("settle: %zu checked traces, %zu checked steps, 0 skipped, 0 failed\n",json_array_size(cases),rows);
    json_decref(root); return 0;
}
