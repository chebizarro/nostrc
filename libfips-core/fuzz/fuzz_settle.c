#include <fips/proto/settle/settle.h>
#include <fips/proto/settle/join.h>
#include <fips/proto/settle/parity.h>
#include "fakes/host.h"
#include "fakes/settle_table.h"
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) abort(); } while (0)
static bool draw(void *ctx, uint32_t *out) { uint32_t *next=ctx; *out=(*next)++; return true; }
static void release(fips_rekey_index_lease *l, size_t *returned)
{
    uint32_t index; CHECK(fips_rekey_index_lease_release(l,&index)); ++*returned;
}
static fips_settle_pair pair(uint8_t n)
{
    fips_settle_pair p={.transport={(uint32_t)(n/16)}};
    CHECK(fips_transport_addr_new(&n,1,&p.addr)==FIPS_OK); return p;
}
static void fuzz_table(const uint8_t *data,size_t size)
{
    fips_node_addr me={{0}}; fips_settle_params p=fips_settle_params_default();
    fips_test_settle_table *f=fips_test_settle_table_create(me,&p,4,8,64,64); fips_settle_table *t=&f->table;
    uint32_t next=1; size_t returned=0; fips_rekey_index_source source={draw,&next}; uint64_t time=0;
    for (size_t i=0;i+3<size && i<1024;i+=4) {
        unsigned kind=data[i]%30; uint64_t link=data[i+1]%(fips_settle_table_links(t)->next+1);
        fips_node_addr peer; memset(peer.bytes,(int)(data[i+1]%4+1),16); fips_settle_pair a=pair(data[i+2]),b=pair(data[i+3]);
        fips_rekey_index_lease lease={0}; fips_settle_input in={.kind=(fips_settle_input_kind)kind};
        uint8_t epoch[8],e[32]; memset(epoch,data[i+3]%3,8); memset(e,data[i+2],32);
        if (kind<28) switch (in.kind) {
        case FIPS_SETTLE_IN_DIAL: case FIPS_SETTLE_IN_PUNCHED:
            CHECK(fips_rekey_allocate(source,&lease)); in.data.dial=(fips_settle_dial){.peer=peer,.to=a,.msg1={data+i,4},.lease=&lease,.has_link=(data[i+3]&1)!=0,.link=link,.has_cap=true,.cap=data[i+3]%8}; break;
        case FIPS_SETTLE_IN_CONNECTING: in.data.connecting.peer=peer; in.data.connecting.to=a; in.data.connecting.has_cap=true; in.data.connecting.cap=data[i+3]%8; break;
        case FIPS_SETTLE_IN_ARRIVED: in.data.arrived.from=a; in.data.arrived.bytes=(fips_settle_bytes){data+i,4}; break;
        case FIPS_SETTLE_IN_MSG1: in.data.msg1.read=(fips_settle_read){.peer=peer,.from=a,.bytes={data+i,4}}; memcpy(in.data.msg1.read.epoch,epoch,8); memcpy(in.data.msg1.read.ephemeral,e,32); in.data.msg1.reachable=(data[i+3]&1)!=0; break;
        case FIPS_SETTLE_IN_REPLACE: case FIPS_SETTLE_IN_REDIAL: in.data.read=(fips_settle_read){.peer=peer,.from=a,.bytes={data+i,4}}; memcpy(in.data.read.epoch,epoch,8); memcpy(in.data.read.ephemeral,e,32); break;
        case FIPS_SETTLE_IN_LEG_HEARD: in.data.leg_heard.link=link; in.data.leg_heard.from=a; break;
        case FIPS_SETTLE_IN_HELLO: CHECK(fips_rekey_allocate(source,&lease)); in.data.hello.link=link; in.data.hello.lease=&lease; in.data.hello.msg2=(fips_settle_bytes){data+i,4}; break;
        case FIPS_SETTLE_IN_ABANDON: in.data.abandon.link=link; in.data.abandon.authorized=(data[i+3]&1)!=0; break;
        case FIPS_SETTLE_IN_MSG2: in.data.msg2.link=link; in.data.msg2.from=a; memcpy(in.data.msg2.epoch,epoch,8); break;
        case FIPS_SETTLE_IN_RESTARTED: case FIPS_SETTLE_IN_PEER_EPOCH: in.data.epoch.peer=peer; memcpy(in.data.epoch.epoch,epoch,8); break;
        case FIPS_SETTLE_IN_HEARD: in.data.heard.peer=peer; in.data.heard.from=a; in.data.heard.kind=data[i+3]; break;
        case FIPS_SETTLE_IN_MOVED: in.data.moved.link=link; in.data.moved.from=a; in.data.moved.to=b; in.data.moved.roam=(data[i+3]&1)!=0; break;
        case FIPS_SETTLE_IN_SWITCHED: in.data.switched.link=link; in.data.switched.to=a; in.data.switched.proven=(fips_settle_proven)(data[i+3]%3); break;
        case FIPS_SETTLE_IN_RELEASED: in.data.released.link=link; in.data.released.pair=a; in.data.released.keep=(data[i+3]&1)!=0; break;
        case FIPS_SETTLE_IN_STATE: in.data.state.link=link; in.data.state.pair=a; in.data.state.proven=(fips_settle_proven)(data[i+3]%3); break;
        case FIPS_SETTLE_IN_ENDED: case FIPS_SETTLE_IN_ORPHANED: case FIPS_SETTLE_IN_DISCONNECT: case FIPS_SETTLE_IN_DEAD: case FIPS_SETTLE_IN_ADMIN: in.data.peer=peer; break;
        case FIPS_SETTLE_IN_CONNECT_FAILED: case FIPS_SETTLE_IN_SENT: case FIPS_SETTLE_IN_SEND_FAILED: case FIPS_SETTLE_IN_UNSENDABLE: case FIPS_SETTLE_IN_CAPABLE: in.data.link=link; break;
        }
        unsigned active=t->active; fips_settle_outputs previous=fips_settle_table_outputs(t); time+=(uint64_t)data[i+3]*100;
        fips_status status=kind==28 ? fips_settle_table_poll(t,fips_test_instant(time)) : kind==29 ? fips_settle_table_close(t,fips_test_instant(time)) : fips_settle_table_step(t,&in,fips_test_instant(time));
        if (status!=FIPS_OK) {
            CHECK(t->active==active && fips_settle_table_outputs(t).items==previous.items && fips_settle_table_outputs(t).count==previous.count);
            if (lease.live) release(&lease,&returned);
        } else CHECK(!lease.live);
        fips_settle_outputs out=fips_settle_table_outputs(t);
        for (size_t j=0;j<out.count;++j) if (out.items[j].lease.live) { CHECK(out.items[j].kind==FIPS_SETTLE_OUT_INSTALL || out.items[j].kind==FIPS_SETTLE_OUT_FREE); release(&out.items[j].lease,&returned); }
        size_t own=0; const fips_settle_legs *legs=fips_settle_table_legs(t);
        for (size_t j=0;j<legs->capacity;++j) if (legs->slots[j].active) { CHECK(legs->slots[j].lease.live); ++own; }
        CHECK(own+returned==next-1);
    }
    CHECK(fips_settle_table_close(t,fips_test_instant(time))==FIPS_OK); fips_settle_outputs out=fips_settle_table_outputs(t);
    for (size_t i=0;i<out.count;++i) if (out.items[i].lease.live) release(&out.items[i].lease,&returned);
    CHECK(returned==next-1); fips_test_settle_table_destroy(f,fips_test_instant(time));
}
static fips_settle_proven join_read(void *ctx,const fips_path_ref *p)
{ (void)ctx; return (fips_settle_proven)(p->addr.len ? p->addr.bytes[0]%3 : 0); }
static void fuzz_join(const uint8_t *data,size_t size)
{
    fips_settle_joined *j=calloc(1,sizeof(*j)); CHECK(j); fips_node_addr peer={{0}};
    for (size_t i=0;i+3<size && i<512;i+=4) {
        fips_settle_join_event e={0}; fips_rekey_outputs r={0};
        fips_path_output paths[2]={0}; fips_path_outputs p={paths,2,2};
        if (data[i]%3==0) {
            e.kind=FIPS_SETTLE_JOIN_REKEY; e.rekey_out=&r; r.count=1;
            r.items[0].kind=(fips_rekey_output_kind)(data[i+1]%18);
            if (r.items[0].kind==FIPS_REKEY_OUT_Release) r.items[0].lease=(fips_rekey_index_lease){1,true};
        } else if (data[i]%3==1) {
            e.kind=FIPS_SETTLE_JOIN_PATHS; e.path_out=&p; e.read=join_read;
            e.has_link=(data[i+1]&1)!=0; e.link=data[i+2]; e.has_released=(data[i+1]&2)!=0; e.released=data[i+3];
            for (unsigned k=0;k<2;++k) {
                paths[k].kind=(fips_path_output_kind)((data[i+1]+k)%11);
                fips_settle_pair to=pair(data[i+2]),from=pair(data[i+3]);
                paths[k].to=(fips_path_ref){to.transport,to.addr};
                /* These are typed host outputs, not wire bytes. Populate only
                 * the member selected by kind: State.next overlaps Released.keep. */
                switch (paths[k].kind) {
                case FIPS_PATH_OUTPUT_Send: paths[k].as.send.type=FIPS_LINK_PathProbe; break;
                case FIPS_PATH_OUTPUT_Moved: paths[k].as.moved.from=(fips_path_ref){from.transport,from.addr}; break;
                case FIPS_PATH_OUTPUT_Released: paths[k].as.released.keep=(data[i+3]&1)!=0; break;
                case FIPS_PATH_OUTPUT_State: paths[k].as.state.next=(fips_path_state)((int)(data[i+3]%5)-1); break;
                case FIPS_PATH_OUTPUT_Switched:
                    paths[k].as.switched.has_from=(data[i+3]&1)!=0;
                    paths[k].as.switched.from=(fips_path_ref){from.transport,from.addr}; break;
                case FIPS_PATH_OUTPUT_Proven: paths[k].as.mtu=1280; break;
                case FIPS_PATH_OUTPUT_Refused: case FIPS_PATH_OUTPUT_Capable:
                case FIPS_PATH_OUTPUT_NotCapable: case FIPS_PATH_OUTPUT_Orphaned:
                case FIPS_PATH_OUTPUT_Connect: break;
                }
            }
        } else {
            e.kind=FIPS_SETTLE_JOIN_ROAM; e.link=data[i+1]; e.has_from=(data[i+2]&1)!=0; e.standby=(data[i+2]&2)!=0; e.from=pair(data[i+2]); e.to=pair(data[i+3]);
        }
        CHECK(fips_settle_join(j,peer,&e)==FIPS_OK); fips_settle_join_step s; size_t steps=0;
        while (fips_settle_join_next(j,&s)) {
            CHECK(++steps<=4);
            if(s.rekey) { CHECK(s.rekey==&r.items[0]); if(s.rekey->lease.live) {uint32_t index; CHECK(fips_rekey_index_lease_release(&s.rekey->lease,&index));} }
            if(s.path) CHECK(s.path==&paths[0] || s.path==&paths[1]);
        }
        CHECK(steps>0); fips_rekey_outputs_clear(&r); fips_settle_join_clear(j);
    }
    free(j);
}
static void fuzz_parity(const uint8_t *data,size_t size)
{
    if(size>4096)return; const char *text=(const char *)data;
    fips_parity_line line={0},before=line; char out[512];size_t length;
    fips_status status=fips_settle_parity_line(text,size,&line);
    if(status!=FIPS_OK)CHECK(!memcmp(&line,&before,sizeof(line)));
    else {CHECK(fips_settle_parity_format_line(&line,out,sizeof(out),&length)==FIPS_OK);fips_parity_line again;CHECK(fips_settle_parity_line(out,length,&again)==FIPS_OK);CHECK(again.step==line.step && again.node==line.node);}
    fips_parity_diff diff={0},old=diff;status=fips_settle_parity_diff(text,size,&diff);
    if(status!=FIPS_OK)CHECK(!memcmp(&diff,&old,sizeof(diff)));
    else {CHECK(fips_settle_parity_format_diff(&diff,out,sizeof(out),&length)==FIPS_OK);fips_parity_diff again;CHECK(fips_settle_parity_diff(out,length,&again)==FIPS_OK && again.count==diff.count && again.step==diff.step && again.node==diff.node);}
    fips_parity_trace *traces=calloc(2,sizeof(*traces));CHECK(traces);size_t count=999;
    status=fips_settle_parity_read(text,size,traces,2,&count);
    if(status!=FIPS_OK){CHECK(count==999);const uint8_t *p=(const uint8_t *)traces;for(size_t i=0;i<2*sizeof(*traces);++i)CHECK(!p[i]);}
    else {CHECK(count<=2);for(size_t i=0;i<count;++i){fips_parity_line lines[FIPS_PARITY_LINES_MAX];size_t n;(void)fips_settle_parity_cored(&traces[i],lines,FIPS_PARITY_LINES_MAX,&n);}}
    free(traces);
}
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    fuzz_parity(data,size); fuzz_join(data,size); fuzz_table(data,size);
    fips_settle_links links; fips_settle_link records[4]; fips_settle_path paths[32];
    fips_settle_legs legs; fips_settle_leg slots[4];
    CHECK(fips_settle_links_init(&links,records,4,paths,8)==FIPS_OK);
    fips_settle_legs_init(&legs,slots,4);
    fips_settle_params p=fips_settle_params_default();
    uint32_t next=1; size_t returned=0;
    fips_rekey_index_source source={draw,&next}; uint64_t time=0;
    for (size_t i=0; i+3<size && i<1024; i+=4) {
        uint64_t n=data[i+1]%(links.next+1); fips_settle_pair a=pair(data[i+2]), b=pair(data[i+3]);
        switch (data[i]%10) {
        case 0: {
            fips_settle_holder h={0}; uint64_t number;
            (void)fips_settle_link_open(&links,&a,data[i+3]&1 ? FIPS_LINK_INBOUND : FIPS_LINK_OUTBOUND,h,&number); break;
        }
        case 1: (void)fips_settle_link_path(&links,n,&a,(fips_settle_proven)(data[i+3]%3)); break;
        case 2: (void)fips_settle_link_moved(&links,n,&a,&b); break;
        case 3: {
            fips_settle_pair closes[9]; size_t count;
            (void)fips_settle_link_release(&links,n,&a,data[i+3]&1,closes,9,&count); break;
        }
        case 4: fips_settle_link_capable(&links,n); break;
        case 5: {
            fips_rekey_index_lease lease; CHECK(fips_rekey_allocate(source,&lease));
            fips_settle_start start={.link=n,.msg1=data+i,.msg1_len=4,.lease=&lease,.has_cap=true,.cap=data[i+2]%8};
            fips_settle_leg old;
            fips_status status=fips_settle_leg_open(&legs,&start,data[i+3]&1,data+i,4,fips_test_instant(time),&p,&old);
            if (status!=FIPS_OK) release(&lease,&returned);
            else if (old.active) release(&old.lease,&returned);
            break;
        }
        case 6: fips_settle_leg_unsent(&legs,n,fips_test_instant(time),&p); break;
        case 7: {
            fips_settle_leg old;
            if (fips_settle_leg_take(&legs,n,&old)) release(&old.lease,&returned);
            break;
        }
        case 8: {
            fips_settle_holder h={.peer=true}; memset(h.identity.bytes,data[i+2],16);
            fips_settle_link_hold(&links,n,h); break;
        }
        default: time+=(uint64_t)data[i+2]*1000; break;
        }
        fips_settle_leg_out out[4]; fips_settle_pair closes[36]; size_t count, cn;
        CHECK(fips_settle_legs_poll(&legs,&links,fips_test_instant(time),false,out,4,&count,closes,36,&cn)==FIPS_OK);
        for (size_t j=0; j<count; ++j) {
            if (out[j].reaped) release(&out[j].lease,&returned);
            else if (data[i+3]&1) fips_settle_leg_resent(&legs,out[j].link,fips_test_instant(time),&p);
            else fips_settle_leg_unsent(&legs,out[j].link,fips_test_instant(time),&p);
        }
        size_t held=0;
        for (size_t j=0; j<4; ++j) if (slots[j].active) {
            ++held; CHECK(slots[j].lease.live && slots[j].msg1_len<=FIPS_SETTLE_MESSAGE_CAPACITY);
            for (size_t k=j+1; k<4; ++k) if (slots[k].active) CHECK(slots[j].lease.index!=slots[k].lease.index && slots[j].link!=slots[k].link);
        }
        CHECK(held+returned==next-1);
        for (size_t j=0; j<4; ++j) if (records[j].active) {
            CHECK(records[j].path_count<=8);
            fips_settle_placement at=fips_settle_place(&links,&a,records[j].number,NULL,NULL);
            if (fips_settle_link_indexed(&links,records[j].number,&a)) CHECK(at.established);
        }
    }
    fips_settle_leg_out out[4]; fips_settle_pair closes[36]; size_t count, cn;
    CHECK(fips_settle_legs_poll(&legs,&links,fips_test_instant(time),true,out,4,&count,closes,36,&cn)==FIPS_OK);
    for (size_t i=0; i<count; ++i) { CHECK(out[i].reaped); release(&out[i].lease,&returned); }
    CHECK(returned==next-1);
    return 0;
}
