#include <fips/proto/settle/join.h>
#include <string.h>
#include <sodium.h>
_Static_assert(FIPS_REKEY_OUTPUTS_MAX>=1,"refused shift needs one release");
_Static_assert(FIPS_PATH_OUTPUTS_MAX>=2*sizeof(((fips_path_set *)0)->paths)/sizeof(fips_peer_path),"close/renew workspace covers every path");
fips_settle_pair fips_settle_path_pair(const fips_path_ref *p)
{ return (fips_settle_pair){.transport=p->transport,.addr=p->addr}; }
fips_settle_proven fips_settle_path_proven(const fips_path_bundle *b,const fips_path_ref *p)
{
    size_t i=fips_path_set_find(&b->set,p->transport,&p->addr);
    if (i==FIPS_PATH_NONE) return FIPS_SETTLE_PROBING;
    const fips_peer_path *path=&b->set.paths[i];
    return path->state==FIPS_PATH_Live ? FIPS_SETTLE_LIVE : path->acked ? FIPS_SETTLE_ACKED : FIPS_SETTLE_PROBING;
}
static bool mirrored(fips_settle_joined *j,const fips_path_output *p,fips_settle_input *in)
{
    *in=(fips_settle_input){0};
    switch (p->kind) {
    case FIPS_PATH_OUTPUT_State:
        if (!j->has_link || p->as.state.next==FIPS_PATH_NoState) return false;
        in->kind=FIPS_SETTLE_IN_STATE; in->data.state.link=j->link; in->data.state.pair=fips_settle_path_pair(&p->to); in->data.state.proven=j->read(j->read_ctx,&p->to); return true;
    case FIPS_PATH_OUTPUT_Moved:
        if (!j->has_link) return false;
        in->kind=FIPS_SETTLE_IN_MOVED; in->data.moved.link=j->link; in->data.moved.from=fips_settle_path_pair(&p->as.moved.from); in->data.moved.to=fips_settle_path_pair(&p->to); return true;
    case FIPS_PATH_OUTPUT_Switched:
        if (!j->has_link) return false;
        in->kind=FIPS_SETTLE_IN_SWITCHED; in->data.switched.link=j->link; in->data.switched.to=fips_settle_path_pair(&p->to); in->data.switched.proven=j->read(j->read_ctx,&p->to); return true;
    case FIPS_PATH_OUTPUT_Released:
        if (!j->has_link && !j->has_released) return false;
        in->kind=FIPS_SETTLE_IN_RELEASED; in->data.released.link=j->has_released ? j->released : j->link; in->data.released.pair=fips_settle_path_pair(&p->to); in->data.released.keep=p->as.released.keep; return true;
    case FIPS_PATH_OUTPUT_Orphaned:
        if (!j->has_link) return false;
        in->kind=FIPS_SETTLE_IN_ORPHANED; in->data.peer=j->peer; return true;
    case FIPS_PATH_OUTPUT_Capable:
        if (!j->has_link) return false;
        in->kind=FIPS_SETTLE_IN_CAPABLE; in->data.link=j->link; return true;
    case FIPS_PATH_OUTPUT_NotCapable: case FIPS_PATH_OUTPUT_Refused: case FIPS_PATH_OUTPUT_Send:
    case FIPS_PATH_OUTPUT_Connect: case FIPS_PATH_OUTPUT_Proven: return false;
    }
    return false;
}
static void told(fips_settle_joined *j,fips_rekey_output *o,fips_settle_join_step *s)
{
    *s=(fips_settle_join_step){.kind=FIPS_SETTLE_STEP_REKEY,.rekey=o};
    switch (o->kind) {
    case FIPS_REKEY_OUT_PeerRestarted: s->kind=FIPS_SETTLE_STEP_CORE; s->input.kind=FIPS_SETTLE_IN_RESTARTED; s->input.data.epoch.peer=j->peer; memcpy(s->input.data.epoch.epoch,o->epoch,8); s->rekey=NULL; break;
    case FIPS_REKEY_OUT_Teardown: s->kind=FIPS_SETTLE_STEP_CORE; s->input.kind=FIPS_SETTLE_IN_ENDED; s->input.data.peer=j->peer; s->rekey=NULL; break;
    case FIPS_REKEY_OUT_ReplaceRequested: s->kind=FIPS_SETTLE_STEP_REPLACE; break;
    case FIPS_REKEY_OUT_RedialRequested: s->kind=FIPS_SETTLE_STEP_REDIAL; break;
    default: break;
    }
}
static bool valid_setup(const fips_settle_join_event *e)
{
    const fips_settle_caps *c=e->caps; const fips_settle_setup *s=e->setup;
    return c && s && c->profile && c->env && fips_rekey_profile_valid(c->profile) &&
        c->profile->indexed==s->lease.live && c->profile->indexed==s->theirs.some;
}
static fips_status build(fips_settle_joined *j,const fips_settle_join_event *e,fips_peer_key key,fips_rekey_setup_facts *facts)
{
    fips_rekey_bond bond={.kind=FIPS_REKEY_BOND_Key,.key=key};
    fips_rekey_ids ids={.lease=e->setup->lease,.theirs=e->setup->theirs,.transport=e->setup->transport};
    fips_status status=fips_rekey_established(e->rekey,e->caps->profile,e->caps->params,bond,e->caps->me,&e->setup->done,&ids,facts,e->caps->now,e->caps->env,&j->build_out);
    if (status==FIPS_OK) { e->setup->lease=(fips_rekey_index_lease){0}; *e->has_rekey=true; j->built=true; }
    return status;
}
static void refused(fips_settle_joined *j,fips_settle_setup *s)
{
    fips_rekey_output *o=&j->build_out.items[j->build_out.count++]; o->kind=FIPS_REKEY_OUT_Release; o->lease=s->lease;
    s->lease=(fips_rekey_index_lease){0}; fips_rekey_completed_clear(&s->done);
}
fips_status fips_settle_join(fips_settle_joined *j,fips_node_addr peer,const fips_settle_join_event *e)
{
    if (j->active || j->close_out.count || j->build_out.count || (unsigned)e->kind>FIPS_SETTLE_JOIN_ROAM || e->to.addr.len>FIPS_TRANSPORT_ADDR_CAPACITY || (e->has_from && e->from.addr.len>FIPS_TRANSPORT_ADDR_CAPACITY)) return FIPS_ERR_RANGE;
    if (e->kind==FIPS_SETTLE_JOIN_START || e->kind==FIPS_SETTLE_JOIN_SHIFT) {
        if (!e->rekey || !e->has_rekey || !valid_setup(e) || (e->paths && !e->path_env)) return FIPS_ERR_RANGE;
        if (e->kind==FIPS_SETTLE_JOIN_START) {
            fips_rekey_bond bond={.kind=FIPS_REKEY_BOND_Key,.key=e->key};
            if (*e->has_rekey || !e->paths || !e->has_paths || *e->has_paths || !e->facts || !e->path_env || !fips_rekey_bond_check(&bond,&e->setup->done)) return FIPS_ERR_RANGE;
        } else if (*e->has_rekey!=e->rekey->live) return FIPS_ERR_RANGE;
    }
    if (e->kind==FIPS_SETTLE_JOIN_STOP && (!e->rekey || !e->has_rekey || !e->paths || !e->has_paths || *e->has_rekey!=e->rekey->live)) return FIPS_ERR_RANGE;
    if (e->kind==FIPS_SETTLE_JOIN_REKEY && (!e->rekey_out || e->rekey_out->count>FIPS_REKEY_OUTPUTS_MAX)) return FIPS_ERR_RANGE;
    if (e->kind==FIPS_SETTLE_JOIN_PATHS && (!e->path_out || e->path_out->len>e->path_out->capacity || e->path_out->len>FIPS_PATH_OUTPUTS_MAX || (e->path_out->len && !e->path_out->items) || !e->read)) return FIPS_ERR_RANGE;
    j->active=true; j->built=false; j->has_carry=false; j->paths_first=false;
    j->mirror=false; j->has_tail=false; j->has_pending=false; j->paths=NULL;
    j->group=0; j->rekey_pos=0; j->path_pos=0; j->read=NULL; j->read_ctx=NULL;
    j->peer=peer; j->has_link=e->has_link; j->link=e->link; j->has_released=e->has_released; j->released=e->released;
    j->owned_paths=(fips_path_outputs){.items=j->path_items,.capacity=FIPS_PATH_OUTPUTS_MAX};
    j->rekeys[0]=&j->close_out; j->rekeys[1]=&j->build_out;
    switch (e->kind) {
    case FIPS_SETTLE_JOIN_START: {
        /* Complete preflight above makes established infallible here. */
        (void)build(j,e,e->key,e->facts);
        fips_path_bundle_new(e->paths,e->caps->now,e->to.transport,e->to.addr,e->path_params,e->entropy,e->path_env); *e->has_paths=true;
        j->has_tail=e->has_link; j->tail=(fips_settle_input){.kind=FIPS_SETTLE_IN_STATE,.data.state={.link=e->link,.pair=e->to,.proven=FIPS_SETTLE_LIVE}}; break;
    }
    case FIPS_SETTLE_JOIN_SHIFT: {
        if (!*e->has_rekey) { refused(j,e->setup); break; }
        fips_rekey_view v=fips_rekey_get_view(e->rekey);
        if (!v.proven || memcmp(v.identity.key.bytes,e->setup->done.remote.bytes,32)) { refused(j,e->setup); break; }
        fips_peer_key key=v.identity.key; fips_rekey_carry carry;
        bool have=fips_rekey_close_machine(e->rekey,FIPS_REKEY_CLOSE_CrossConnectionMove,&j->close_out,&carry); *e->has_rekey=false;
        if (!have) { refused(j,e->setup); break; }
        (void)build(j,e,key,&carry);
        if (e->paths) { (void)fips_path_bundle_renew(e->paths,e->caps->now,e->path_env,(fips_path_ref){.transport=e->to.transport,.addr=e->to.addr},&j->owned_paths); j->paths=&j->owned_paths; }
        break;
    }
    case FIPS_SETTLE_JOIN_STOP:
        if (*e->has_paths) { (void)fips_path_bundle_close(e->paths,&j->owned_paths); *e->has_paths=false; j->paths=&j->owned_paths; j->paths_first=true; }
        if (*e->has_rekey) { j->has_carry=fips_rekey_close_machine(e->rekey,e->why,&j->close_out,&j->carry); *e->has_rekey=false; } break;
    case FIPS_SETTLE_JOIN_REKEY: j->rekeys[0]=e->rekey_out; j->rekeys[1]=NULL; break;
    case FIPS_SETTLE_JOIN_PATHS:
        j->paths=e->path_out; j->paths_first=true; j->mirror=true; j->read=e->read; j->read_ctx=e->read_ctx;
        for (size_t i=0;i<e->path_out->len;++i) j->mirrors[i].present=mirrored(j,&e->path_out->items[i],&j->mirrors[i].input);
        break;
    case FIPS_SETTLE_JOIN_ROAM:
        j->has_tail=true;
        if (e->has_from) j->tail=(fips_settle_input){.kind=FIPS_SETTLE_IN_MOVED,.data.moved={.link=e->link,.from=e->from,.to=e->to,.roam=!e->standby}};
        else j->tail=(fips_settle_input){.kind=FIPS_SETTLE_IN_STATE,.data.state={.link=e->link,.pair=e->to,.proven=FIPS_SETTLE_LIVE}}; break;
    }
    return FIPS_OK;
}
static bool next_path(fips_settle_joined *j,fips_settle_join_step *out)
{
    if (j->has_pending) { *out=(fips_settle_join_step){.kind=FIPS_SETTLE_STEP_CORE,.input=j->pending}; j->has_pending=false; return true; }
    if (!j->paths || j->path_pos==j->paths->len) return false;
    size_t pos=j->path_pos++; fips_path_output *p=&j->paths->items[pos];
    if (j->mirror) { j->has_pending=j->mirrors[pos].present; if (j->has_pending) j->pending=j->mirrors[pos].input; }
    *out=(fips_settle_join_step){.kind=FIPS_SETTLE_STEP_PATH,.path=p}; return true;
}
bool fips_settle_join_next(fips_settle_joined *j,fips_settle_join_step *out)
{
    if (!j->active) return false;
    if (j->paths_first && next_path(j,out)) return true;
    while (j->group<2) {
        fips_rekey_outputs *r=j->rekeys[j->group];
        if (r && j->rekey_pos<r->count) { told(j,&r->items[j->rekey_pos++],out); return true; }
        ++j->group; j->rekey_pos=0;
    }
    if (!j->paths_first && next_path(j,out)) return true;
    if (j->has_tail) { *out=(fips_settle_join_step){.kind=FIPS_SETTLE_STEP_CORE,.input=j->tail}; j->has_tail=false; return true; }
    j->active=false; return false;
}
void fips_settle_join_clear(fips_settle_joined *j)
{
    fips_rekey_outputs_clear(&j->close_out); fips_rekey_outputs_clear(&j->build_out);
    if (j->owned_paths.len) sodium_memzero(j->path_items,j->owned_paths.len*sizeof(*j->path_items));
    j->owned_paths.len=0; j->active=false; j->has_carry=false;
}
