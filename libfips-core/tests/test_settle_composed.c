/* C-local shell qualification, not the unported Rust world/Drive explorer. */
#include "fakes/rekey.h"
#include "fakes/settle_table.h"
#include <fips/proto/settle/join.h>
#include <fips/proto/settle/parity.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"composed:%d: %s\n",__LINE__,#x); abort(); } } while (0)
typedef struct shell {
    rk_test_node a,b;
    fips_test_settle_table *table;
    fips_path_bundle paths;
    fips_settle_joined joined;
    bool has_rekey,has_paths;
    fips_seeded_entropy entropy;
    fips_settle_caps caps;
    fips_path_params params;
    fips_path_env env;
    fips_settle_setup setup;
    fips_rekey_setup_facts facts;
    fips_instant now;
    uint64_t link;
    size_t installs,removes,resets,mirrors,returns;
} shell;
static size_t mtu(void *c,fips_transport_id t,const fips_transport_addr *a) { (void)c;(void)t;(void)a;return 1280; }
static fips_path_medium medium(void *c,fips_transport_id t,const fips_transport_addr *a) { (void)c;(void)t;(void)a;return FIPS_PATH_Unknown; }
static bool no(void *c,fips_transport_id t) { (void)c;(void)t;return false; }
static bool yes(void *c,fips_transport_id t) { (void)c;(void)t;return true; }
static fips_path_class clear(void *c,fips_transport_id t) { (void)c;(void)t;return FIPS_PATH_Clear; }
static fips_path_kind unfixed(void *c,fips_transport_id t) { (void)c;(void)t;return FIPS_PATH_Unfixed; }
static fips_path_role normal(void *c,fips_transport_id t) { (void)c;(void)t;return FIPS_PATH_Normal; }
static const fips_path_facts path_facts={NULL,mtu,medium,no,clear,unfixed,normal,yes,yes};
static fips_node_addr remote(const shell *s) { return rk_test_addr(s->b.local.key); }
static fips_settle_pair pair(unsigned port)
{
    fips_settle_pair p={.transport={1}}; char text[32]; int n=snprintf(text,sizeof(text),"10.0.0.1:%u",port);
    CHECK(n>0 && (size_t)n<sizeof(text)); CHECK(fips_transport_addr_new((const uint8_t *)text,(size_t)n,&p.addr)==FIPS_OK); return p;
}
static void release(shell *s,fips_rekey_index_lease *lease)
{
    uint32_t index; CHECK(fips_rekey_index_lease_release(lease,&index)); CHECK(index<1024 && s->a.indices[index] && s->a.allocated);
    s->a.indices[index]=false; --s->a.allocated; ++s->returns;
}
static void execute(shell *s);
static void input(shell *s,fips_settle_input in)
{ CHECK(fips_settle_table_step(&s->table->table,&in,s->now)==FIPS_OK); execute(s); }
static void joined(shell *s,fips_settle_join_event event)
{
    CHECK(fips_settle_join(&s->joined,remote(s),&event)==FIPS_OK);
    fips_settle_input pending[FIPS_PATH_OUTPUTS_MAX+1]; size_t n=0; fips_settle_join_step step;
    while (fips_settle_join_next(&s->joined,&step)) {
        if (step.kind==FIPS_SETTLE_STEP_CORE) { CHECK(n<sizeof(pending)/sizeof(*pending)); pending[n++]=step.input; ++s->mirrors; }
        else if (step.kind==FIPS_SETTLE_STEP_REKEY) {
            if (step.rekey->kind==FIPS_REKEY_OUT_Release) release(s,&step.rekey->lease);
            else CHECK(step.rekey->kind==FIPS_REKEY_OUT_Bind || step.rekey->kind==FIPS_REKEY_OUT_Unbind || step.rekey->kind==FIPS_REKEY_OUT_Installed || step.rekey->kind==FIPS_REKEY_OUT_Retired || step.rekey->kind==FIPS_REKEY_OUT_Abandoned);
        } else { CHECK(step.kind==FIPS_SETTLE_STEP_PATH && step.path); }
    }
    /* No source output owner or iterator workspace is live when core feedback runs. */
    fips_settle_join_clear(&s->joined);
    for (size_t i=0;i<n;++i) input(s,pending[i]);
}
static void execute(shell *s)
{
    fips_settle_outputs out=fips_settle_table_outputs(&s->table->table);
    /* Move owners, not a checkpoint copy: nested feedback may replace the arena. */
    fips_settle_output *batch=calloc(out.count ? out.count : 1,sizeof(*batch)); CHECK(batch); size_t n=out.count;
    for (size_t i=0;i<n;++i) { batch[i]=out.items[i]; memset(&out.items[i].lease,0,sizeof(out.items[i].lease)); }
    for (size_t i=0;i<n;++i) {
        fips_settle_output *o=&batch[i];
        switch (o->kind) {
        case FIPS_SETTLE_OUT_INSTALL: {
            CHECK(!s->setup.lease.live && o->lease.live);
            s->setup.lease=o->lease; memset(&o->lease,0,sizeof(o->lease)); s->link=o->link;
            s->caps.now=s->now;
            fips_settle_join_event e={.kind=o->build==FIPS_SETTLE_BUILD_FRESH ? FIPS_SETTLE_JOIN_START : FIPS_SETTLE_JOIN_SHIFT,
                .rekey=&s->a.machine,.has_rekey=&s->has_rekey,.paths=&s->paths,.has_paths=&s->has_paths,
                .key=s->b.local.key,.setup=&s->setup,.facts=&s->facts,.caps=&s->caps,.to=o->pair,
                .path_params=s->params,.entropy=fips_seeded_entropy_adapter(&s->entropy),.path_env=&s->env,.has_link=true,.link=o->link};
            joined(s,e); CHECK(s->has_rekey && s->has_paths && s->a.machine.live); ++s->installs; break;
        }
        case FIPS_SETTLE_OUT_REMOVE:
            joined(s,(fips_settle_join_event){.kind=FIPS_SETTLE_JOIN_STOP,.rekey=&s->a.machine,.has_rekey=&s->has_rekey,.paths=&s->paths,.has_paths=&s->has_paths,.why=FIPS_REKEY_CLOSE_Admin}); ++s->removes; break;
        case FIPS_SETTLE_OUT_FREE: release(s,&o->lease); break;
        case FIPS_SETTLE_OUT_RESET: ++s->resets; break;
        case FIPS_SETTLE_OUT_OPENED: s->link=o->link; break;
        case FIPS_SETTLE_OUT_RETIRE: case FIPS_SETTLE_OUT_RELEASE: case FIPS_SETTLE_OUT_CLOSE:
        case FIPS_SETTLE_OUT_SWAPPED: case FIPS_SETTLE_OUT_RETRY: break;
        default: CHECK(false);
        }
        CHECK(!o->lease.live);
    }
    free(batch);
}
static void dial_and_complete(shell *s,unsigned port)
{
    fips_rekey_index_lease lease={0}; CHECK(fips_rekey_allocate(s->a.env.indices,&lease)); uint32_t ours=lease.index;
    fips_rekey_hs hs={0}; fips_rekey_wire msg={0};
    CHECK(s->a.profile.initiate(&s->a.local,&s->b.local.key,(fips_rekey_index){ours,true},&hs,&msg));
    input(s,(fips_settle_input){.kind=FIPS_SETTLE_IN_DIAL,.data.dial={.peer=remote(s),.to=pair(port),.msg1={msg.bytes,msg.len},.lease=&lease}}); CHECK(!lease.live);
    uint64_t link=s->link;
    rk_test_packet first={.handshake=true,.kind=FIPS_REKEY_MSG_Msg1,.len=msg.len}; memcpy(first.bytes,msg.bytes,msg.len);
    rk_test_packet answer=rk_test_answer(&s->b.local,&first,9); fips_rekey_read read={0}; CHECK(s->a.profile.reply(&hs,&s->a.local,answer.bytes,answer.len,&read));
    s->setup=(fips_settle_setup){.done=read.done,.theirs={9,true},.transport=1}; memset(&read.done,0,sizeof(read.done)); fips_rekey_hs_clear(&hs);
    s->facts=(fips_rekey_setup_facts){.has_peer_epoch=true,.link_since=s->now,.started=s->now}; memcpy(s->facts.peer_epoch,s->setup.done.epoch,8);
    fips_settle_input in={.kind=FIPS_SETTLE_IN_MSG2,.data.msg2={.link=link,.from=pair(port)}}; memcpy(in.data.msg2.epoch,s->setup.done.epoch,8); input(s,in);
    CHECK(!s->setup.lease.live && s->a.allocated==1);
}
void fips_test_settle_composed(void)
{
    size_t leaked=fips_rekey_leaked();
    for (unsigned mode=0;mode<2;++mode) {
        shell *s=calloc(1,sizeof(*s)); CHECK(s); rk_test_node_init(&s->a,1,&rk_test_ik); rk_test_node_init(&s->b,2,&rk_test_ik);
        s->now=fips_test_instant(0); s->entropy=(fips_seeded_entropy){42}; s->env=(fips_path_env){&path_facts,1.0};
        CHECK(fips_settle_parity_path(mode!=0,&s->params)==FIPS_OK);
        s->caps=(fips_settle_caps){.profile=&s->a.profile,.me=rk_test_addr(s->a.local.key),.now=s->now,.env=&s->a.env};
        CHECK(fips_settle_parity_rekey(mode!=0,&s->caps.params)==FIPS_OK);
        fips_settle_params params=fips_settle_params_default(); s->table=fips_test_settle_table_create(s->caps.me,&params,8,8,64,64); CHECK(s->table);
        dial_and_complete(s,1); CHECK(s->installs==1 && s->a.machine.mem.link_since.opaque[0]==0);
        input(s,(fips_settle_input){.kind=FIPS_SETTLE_IN_HEARD,.data.heard={.peer=remote(s),.from=pair(1),.kind=FIPS_LINK_Heartbeat}});
        fips_node_addr peer=remote(s); const fips_settle_held *held=fips_settle_table_held(&s->table->table,&peer); CHECK(held && held->heard);
        s->now=fips_test_instant(5000); dial_and_complete(s,2); CHECK(s->installs==2 && s->returns==1 && s->a.machine.mem.link_since.opaque[0]==0);
        held=fips_settle_table_held(&s->table->table,&peer); CHECK(held && held->link==s->link && fips_settle_pair_equal(&held->current,&(fips_settle_pair){.transport={1},.addr=pair(2).addr}));
        CHECK(fips_settle_path_proven(&s->paths,&(fips_path_ref){.transport={1},.addr=pair(2).addr})==FIPS_SETTLE_LIVE);
        input(s,(fips_settle_input){.kind=FIPS_SETTLE_IN_DEAD,.data.peer=peer}); CHECK(!s->has_rekey && !s->has_paths && !s->a.allocated && s->returns==2 && s->removes==1);
        CHECK(!fips_settle_table_held(&s->table->table,&peer)); CHECK(s->mirrors>0 && s->resets>=2);
        CHECK(fips_settle_table_close(&s->table->table,s->now)==FIPS_OK); execute(s); fips_test_settle_table_destroy(s->table,s->now); free(s);
    }
    CHECK(fips_rekey_leaked()==leaked); puts("settle composed: table/join/rekey/path lifecycle and lease moves, both multipath modes passed");
}
