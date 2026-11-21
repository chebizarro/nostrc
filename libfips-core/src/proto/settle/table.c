#include <fips/proto/settle/table.h>
#include <fips/proto/link.h>
#include <string.h>
#define TRY(call) do { fips_status status_=(call); if (status_!=FIPS_OK) return status_; } while (0)
typedef struct context { fips_settle_table *table; fips_settle_table_state *s; fips_instant now; } context;
static bool eq(const fips_node_addr *a,const fips_node_addr *b) { return !memcmp(a->bytes,b->bytes,16); }
static bool pair_eq(const fips_settle_pair *a,const fips_settle_pair *b) { return fips_settle_pair_equal(a,b); }
static fips_settle_table_state *current(const fips_settle_table *t) { return (fips_settle_table_state *)&t->states[t->active]; }
static fips_settle_held *held(fips_settle_table_state *s,const fips_node_addr *peer)
{
    for (size_t i=0;i<s->storage.capacity;++i) if (s->storage.held[i].active && eq(&s->storage.held[i].peer,peer)) return &s->storage.held[i];
    return NULL;
}
static fips_settle_held *hold_slot(fips_settle_table_state *s,fips_node_addr peer)
{
    fips_settle_held *h=held(s,&peer); if (h) return h;
    for (size_t i=0;i<s->storage.capacity;++i) if (!s->storage.held[i].active) return &s->storage.held[i];
    return NULL;
}
static fips_settle_meta *meta(fips_settle_table_state *s,uint64_t link)
{
    for (size_t i=0;i<s->storage.capacity;++i) if (s->storage.meta[i].active && s->storage.meta[i].link==link) return &s->storage.meta[i];
    return NULL;
}
static fips_settle_meta *redial(fips_settle_table_state *s,const fips_node_addr *peer)
{
    for (size_t i=0;i<s->storage.capacity;++i) if (s->storage.meta[i].active && s->storage.meta[i].pending_redial && eq(&s->storage.meta[i].peer,peer)) return &s->storage.meta[i];
    return NULL;
}
static void clear_meta(fips_settle_table_state *s,uint64_t link)
{ fips_settle_meta *m=meta(s,link); if (m) memset(m,0,sizeof(*m)); }
static fips_settle_output *emit(context *c,fips_settle_output_kind kind)
{
    fips_settle_table_state *s=c->s; if (s->output_count==s->storage.output_capacity) return NULL;
    fips_settle_output *o=&s->storage.outputs[s->output_count++]; *o=(fips_settle_output){.kind=kind}; return o;
}
#define OUT(name,kind) fips_settle_output *name=emit(c,kind); if (!(name)) return FIPS_ERR_RANGE
static fips_status peer_out(context *c,fips_settle_output_kind kind,fips_node_addr peer)
{ OUT(o,kind); o->peer=peer; o->has_peer=true; return FIPS_OK; }
static fips_status reset(context *c,fips_node_addr peer,fips_settle_transition transition)
{ TRY(peer_out(c,FIPS_SETTLE_OUT_RESET,peer)); c->s->storage.outputs[c->s->output_count-1].transition=transition; return FIPS_OK; }
static fips_status free_lease(context *c,fips_rekey_index_lease *lease)
{ OUT(o,FIPS_SETTLE_OUT_FREE); o->lease=*lease; *lease=(fips_rekey_index_lease){0}; return FIPS_OK; }
static fips_status reject(context *c,const fips_node_addr *peer,const fips_settle_source *source,fips_settle_denial denial,fips_settle_refusal refusal)
{
    OUT(o,FIPS_SETTLE_OUT_REJECT); o->has_peer=peer!=NULL; if (peer) o->peer=*peer;
    o->has_source=source!=NULL; if (source) o->source=*source; o->denial=denial; o->refusal=refusal; return FIPS_OK;
}
static fips_status open_link(context *c,const fips_settle_pair *pair,fips_link_direction direction,uint64_t *link)
{
    TRY(fips_settle_link_open(&c->s->links,pair,direction,(fips_settle_holder){0},link));
    const fips_settle_link *r=fips_settle_link_get(&c->s->links,*link);
    size_t slot=(size_t)(r-c->s->storage.links);
    c->s->storage.meta[slot]=(fips_settle_meta){.active=true,.link=*link}; return FIPS_OK;
}
static fips_status retire(context *c,uint64_t link,bool output,bool keep)
{
    fips_settle_table_state *s=c->s; size_t offset=s->close_count,n=0;
    TRY(fips_settle_link_retire(&s->links,link,s->storage.closes ? s->storage.closes+offset : NULL,s->storage.close_capacity-offset,&n));
    clear_meta(s,link);
    if (!keep) s->close_count+=n;
    if (output) { OUT(o,FIPS_SETTLE_OUT_RETIRE); o->link=link; o->close_offset=offset; o->close_count=keep ? 0 : n; }
    return FIPS_OK;
}
static fips_settle_liveness liveness(const fips_settle_held *h)
{ return (fips_settle_liveness){.heard=h->heard,.has_last=h->has_last,.installed=h->installed,.last=h->last}; }
static bool unkept(const fips_settle_link *r,const fips_settle_pair *sending,const fips_settle_pair *p)
{
    if (!r || !r->capable || (sending && pair_eq(sending,p))) return false;
    for (size_t i=0;i<r->path_count;++i) if (pair_eq(&r->paths[i].pair,p)) return true;
    return false;
}
static fips_status remove_peer(context *c,fips_node_addr peer,fips_settle_why why)
{
    fips_settle_held *slot=held(c->s,&peer); if (!slot) return FIPS_OK;
    fips_settle_held h=*slot;
    if (!h.has_last) {
        bool started; fips_duration length;
        TRY(fips_settle_gate_ended(&c->s->gates,peer,h.epoch,h.has_setup ? h.setup : NULL,c->now,NULL,&c->table->params,&started,&length));
    }
    memset(slot,0,sizeof(*slot));
    const fips_settle_link *r=fips_settle_link_get(&c->s->links,h.link);
    fips_settle_link old=r ? *r : (fips_settle_link){0};
    size_t offset=c->s->close_count; TRY(retire(c,h.link,false,false)); size_t end=c->s->close_count;
    if (why!=FIPS_SETTLE_WHY_CLOSED) {
        size_t n=offset;
        for (size_t i=offset;i<end;++i) if (unkept(r ? &old : NULL,h.has_sending ? &h.sending : NULL,&c->s->storage.closes[i])) c->s->storage.closes[n++]=c->s->storage.closes[i];
        c->s->close_count=n;
        if (why==FIPS_SETTLE_WHY_ADMIN) {
            bool found=false; for (size_t i=offset;i<n;++i) if (pair_eq(&c->s->storage.closes[i],&h.current)) found=true;
            if (!found) {
                if (n==c->s->storage.close_capacity) return FIPS_ERR_RANGE;
                memmove(c->s->storage.closes+offset+1,c->s->storage.closes+offset,(n-offset)*sizeof(h.current));
                c->s->storage.closes[offset]=h.current; ++c->s->close_count;
            }
        }
    }
    TRY(peer_out(c,FIPS_SETTLE_OUT_REMOVE,peer)); c->s->storage.outputs[c->s->output_count-1].why=why;
    OUT(o,FIPS_SETTLE_OUT_RETIRE); o->link=h.link; o->close_offset=offset; o->close_count=c->s->close_count-offset;
    if (why!=FIPS_SETTLE_WHY_ENDED && why!=FIPS_SETTLE_WHY_ADMIN && why!=FIPS_SETTLE_WHY_CLOSED) TRY(peer_out(c,FIPS_SETTLE_OUT_RETRY,peer));
    return FIPS_OK;
}
static fips_status abandon(context *c,uint64_t link)
{
    fips_settle_meta *m=meta(c->s,link);
    if (m && m->connecting) return retire(c,link,true,true);
    fips_settle_leg leg;
    if (!fips_settle_leg_take(&c->s->legs,link,&leg)) return FIPS_OK;
    TRY(free_lease(c,&leg.lease)); return retire(c,link,true,false);
}
static bool capped(context *c,const fips_node_addr *peer)
{
    if (!c->table->params.peers || held(c->s,peer)) return false;
    size_t n=0; for (size_t i=0;i<c->s->storage.capacity;++i) n+=c->s->storage.held[i].active;
    return n>=c->table->params.peers;
}
static bool full(context *c)
{
    size_t n=fips_settle_legs_answering(&c->s->legs);
    for (size_t i=0;i<c->s->storage.capacity;++i) n+=c->s->storage.meta[i].active && c->s->storage.meta[i].asked;
    return n>=c->table->params.pending;
}
static fips_status ask_answer(context *c,const fips_settle_read *read,bool replaces,bool redialed)
{
    uint64_t link; TRY(open_link(c,&read->from,FIPS_LINK_INBOUND,&link)); fips_settle_meta *m=meta(c->s,link);
    m->peer=read->peer; m->asked=true; m->replaces=replaces; m->redial=redialed;
    memcpy(m->ephemeral,read->ephemeral,32); memcpy(m->epoch,read->epoch,8);
    m->msg1_len=read->bytes.length; if (m->msg1_len) memcpy(m->msg1,read->bytes.bytes,m->msg1_len);
    OUT(o,FIPS_SETTLE_OUT_ASK); o->link=link; o->peer=read->peer; o->has_peer=true; return FIPS_OK;
}
static fips_status admit(context *c,fips_settle_source source,const fips_settle_read *read)
{
    fips_settle_held *h=held(c->s,&read->peer);
    if (!h && capped(c,&read->peer)) return reject(c,&read->peer,NULL,FIPS_SETTLE_DENIAL_CAP,FIPS_SETTLE_GATE_ACCEPT);
    if (!h) source=FIPS_SETTLE_SOURCE_NOT_HELD;
    if (full(c)) return reject(c,&read->peer,&source,FIPS_SETTLE_DENIAL_FULL,FIPS_SETTLE_GATE_ACCEPT);
    fips_settle_ask a={.source=source,.has_held=h!=NULL,.has_ephemeral=true,.has_epoch=true};
    if (h) a.held=liveness(h);
    memcpy(a.ephemeral,read->ephemeral,32); memcpy(a.epoch,read->epoch,8); fips_settle_refusal refusal;
    TRY(fips_settle_gate(&c->s->gates,read->peer,&a,c->now,&c->table->params,&refusal));
    if (refusal!=FIPS_SETTLE_GATE_ACCEPT) return reject(c,&read->peer,&source,FIPS_SETTLE_DENIAL_GATE,refusal);
    bool replaces=h!=NULL;
    if (replaces) TRY(remove_peer(c,read->peer,source==FIPS_SETTLE_SOURCE_RESTART ? FIPS_SETTLE_WHY_RESTART : FIPS_SETTLE_WHY_REPLACEMENT));
    return ask_answer(c,read,replaces,false);
}
static fips_status redial_read(context *c,const fips_settle_read *read)
{
    fips_settle_held *h=held(c->s,&read->peer);
    if (!h) return admit(c,FIPS_SETTLE_SOURCE_NOT_HELD,read);
    fips_settle_source source=FIPS_SETTLE_SOURCE_REDIAL;
    if (full(c)) return reject(c,&read->peer,&source,FIPS_SETTLE_DENIAL_FULL,FIPS_SETTLE_GATE_ACCEPT);
    if (fips_settle_gate_silenced(&c->s->gates,&read->peer,read->epoch,c->now)) return reject(c,&read->peer,&source,FIPS_SETTLE_DENIAL_GATE,FIPS_SETTLE_GATE_SILENT);
    fips_settle_meta *pending=redial(c->s,&read->peer);
    if (pending) {
        fips_duration d=c->now.ops->saturating_duration_since(c->now,pending->armed), floor=c->table->params.unheard;
        if (d.secs<floor.secs || (d.secs==floor.secs && d.nanos<floor.nanos)) return reject(c,&read->peer,&source,FIPS_SETTLE_DENIAL_HELD,FIPS_SETTLE_GATE_ACCEPT);
    }
    fips_settle_ask a={.source=source,.has_held=true,.held=liveness(h),.has_ephemeral=true,.has_epoch=true};
    memcpy(a.ephemeral,read->ephemeral,32); memcpy(a.epoch,read->epoch,8); fips_settle_refusal refusal;
    TRY(fips_settle_gate(&c->s->gates,read->peer,&a,c->now,&c->table->params,&refusal));
    if (refusal!=FIPS_SETTLE_GATE_ACCEPT) return reject(c,&read->peer,&source,FIPS_SETTLE_DENIAL_GATE,refusal);
    return ask_answer(c,read,false,true);
}
static fips_status install(context *c,fips_settle_held value,fips_settle_build build,bool initiator,fips_rekey_index_lease *lease,fips_settle_transition transition)
{
    fips_settle_held *slot=hold_slot(c->s,value.peer); if (!slot) return FIPS_ERR_RANGE;
    value.active=true; *slot=value; fips_settle_holder holder={.peer=true,.identity=value.peer}; fips_settle_link_hold(&c->s->links,value.link,holder);
    OUT(o,FIPS_SETTLE_OUT_INSTALL); o->has_peer=true; o->peer=value.peer; o->link=value.link; o->pair=value.current; o->build=build; o->initiator=initiator;
    o->lease=*lease; *lease=(fips_rekey_index_lease){0}; return reset(c,value.peer,transition);
}
static fips_status refuse_leg(context *c,fips_node_addr peer,fips_settle_leg *leg)
{
    TRY(reject(c,&peer,NULL,FIPS_SETTLE_DENIAL_CAP,FIPS_SETTLE_GATE_ACCEPT)); TRY(free_lease(c,&leg->lease)); return retire(c,leg->link,true,false);
}
static fips_status sent(context *c,uint64_t link)
{
    const fips_settle_leg *found=fips_settle_leg_get(&c->s->legs,link); if (!found) return FIPS_OK;
    if (!found->answer) { fips_settle_leg_resent(&c->s->legs,link,c->now,&c->table->params); return FIPS_OK; }
    if (!found->has_peer) return FIPS_OK;
    fips_node_addr peer=found->peer; fips_settle_meta *m=meta(c->s,link);
    if (held(c->s,&peer)) { if (m && m->answered) m->sent=true; return FIPS_OK; }
    if (m) m->pending_redial=false;
    fips_settle_leg leg;
    if (capped(c,&peer)) { if (fips_settle_leg_take(&c->s->legs,link,&leg)) return refuse_leg(c,peer,&leg); return FIPS_OK; }
    const fips_settle_link *r=fips_settle_link_get(&c->s->links,link);
    if (!m || !m->answered || !r) return FIPS_OK;
    fips_settle_meta a=*m; if (!fips_settle_leg_take(&c->s->legs,link,&leg)) return FIPS_OK;
    memset(m,0,sizeof(*m)); fips_settle_held h={.peer=peer,.link=link,.current=r->setup,.installed=c->now,.has_setup=true};
    memcpy(h.epoch,a.epoch,8); memcpy(h.setup,a.ephemeral,32);
    TRY(fips_settle_gate_promoted(&c->s->gates,peer,a.ephemeral,c->now,NULL,&c->table->params));
    return install(c,h,FIPS_SETTLE_BUILD_FRESH,false,&leg.lease,a.replaces ? FIPS_SETTLE_TRANSITION_REPLACE : FIPS_SETTLE_TRANSITION_PROMOTE);
}
static fips_status hello(context *c,uint64_t link,fips_settle_bytes msg2,fips_rekey_index_lease *lease)
{
    fips_settle_meta *m=meta(c->s,link); if (!m || !m->asked) return free_lease(c,lease);
    fips_settle_meta a=*m;
    if (a.redial) {
        fips_settle_meta *old=redial(c->s,&a.peer); if (old) TRY(abandon(c,old->link));
        TRY(fips_settle_gate_redialed(&c->s->gates,a.peer,a.ephemeral,c->now,&c->table->params));
    }
    fips_settle_start start={.link=link,.has_peer=true,.peer=a.peer,.msg1=a.msg1,.msg1_len=a.msg1_len,.lease=lease}; fips_settle_leg old;
    TRY(fips_settle_leg_open(&c->s->legs,&start,true,msg2.bytes,msg2.length,c->now,&c->table->params,&old));
    if (old.active) TRY(free_lease(c,&old.lease));
    m=meta(c->s,link); *m=a; m->asked=false; m->answered=true; m->pending_redial=a.redial; m->armed=c->now;
    OUT(o,FIPS_SETTLE_OUT_ANSWER); o->link=link; o->length=msg2.length; if (o->length) memcpy(o->bytes,msg2.bytes,o->length); return FIPS_OK;
}
static fips_status dial(context *c,const fips_settle_dial *d)
{
    uint64_t link=d->link; bool opened=!d->has_link; fips_settle_meta connect={.peer=d->peer,.has_cap=d->has_cap,.cap=d->cap};
    fips_settle_held *h=held(c->s,&d->peer); if (h) { connect.has_held=true; connect.held=h->link; }
    if (d->has_link) {
        fips_settle_meta *m=meta(c->s,link);
        if (m && m->connecting) {
            connect=*m; memset(m,0,sizeof(*m));
            if (!fips_settle_link_get(&c->s->links,link)) opened=true;
        } else if (!fips_settle_link_get(&c->s->links,link)) return free_lease(c,d->lease);
    }
    if (opened) TRY(open_link(c,&d->to,FIPS_LINK_OUTBOUND,&link));
    fips_settle_start start={.link=link,.has_peer=true,.peer=connect.peer,.has_cap=connect.has_cap,.cap=connect.cap,
        .has_held=connect.has_held,.held=connect.held,.msg1=d->msg1.bytes,.msg1_len=d->msg1.length,.lease=d->lease};
    fips_settle_leg old; TRY(fips_settle_leg_open(&c->s->legs,&start,false,NULL,0,c->now,&c->table->params,&old));
    if (old.active) TRY(free_lease(c,&old.lease));
    if (opened) { OUT(o,FIPS_SETTLE_OUT_OPENED); o->link=link; }
    return FIPS_OK;
}
static bool expects(void *ctx,uint64_t link,fips_node_addr *peer)
{
    fips_settle_table_state *s=ctx; const fips_settle_leg *leg=fips_settle_leg_get(&s->legs,link);
    if (!leg || !leg->has_peer) return false;
    *peer=leg->peer; return true;
}
static const fips_settle_leg *stored(context *c,const fips_settle_pair *from,fips_settle_bytes bytes)
{
    const fips_settle_leg *best=NULL;
    for (size_t i=0;i<c->s->legs.capacity;++i) {
        const fips_settle_leg *leg=&c->s->legs.slots[i];
        if (!leg->active || !leg->answer || !fips_settle_link_indexed(&c->s->links,leg->link,from)) continue;
        fips_settle_meta *m=meta(c->s,leg->link);
        bool any=m && m->answered && !m->sent;
        if (!any && (leg->msg1_len!=bytes.length || (bytes.length && memcmp(leg->msg1,bytes.bytes,bytes.length)))) continue;
        if (!best || leg->link<best->link) best=leg;
    }
    return best;
}
static fips_status answer_stored(context *c,const fips_settle_leg *leg)
{
    OUT(o,FIPS_SETTLE_OUT_ANSWER); o->link=leg->link; o->stored=true; o->length=leg->msg2_len;
    if (o->length) memcpy(o->bytes,leg->msg2,o->length); return FIPS_OK;
}
static fips_status dialed(context *c,fips_node_addr peer,uint64_t link,const uint8_t epoch[8],const fips_settle_pair *from,fips_settle_transition transition)
{
    fips_settle_leg leg; if (!fips_settle_leg_take(&c->s->legs,link,&leg)) return FIPS_OK;
    if (capped(c,&peer)) return refuse_leg(c,peer,&leg);
    clear_meta(c->s,link); fips_settle_held h={.peer=peer,.link=link,.current=*from,.heard=true,.installed=c->now}; memcpy(h.epoch,epoch,8);
    return install(c,h,FIPS_SETTLE_BUILD_FRESH,true,&leg.lease,transition);
}
static fips_status moveto(context *c,fips_node_addr peer,uint64_t link,const fips_settle_pair *from,bool own)
{
    fips_settle_held *h=held(c->s,&peer); fips_settle_leg leg;
    if (!h || !fips_settle_leg_take(&c->s->legs,link,&leg)) return FIPS_OK;
    uint64_t old=h->link; bool unpin=own || (leg.has_held && leg.held==old);
    bool has_sending=h->has_sending; fips_settle_pair sending=h->sending; h->has_sending=false;
    const fips_settle_link *newrec=fips_settle_link_get(&c->s->links,link);
    bool answered=newrec && pair_eq(&newrec->setup,from), first=!h->heard;
    h->link=link; h->current=*from; h->heard=true; clear_meta(c->s,link);
    fips_settle_link_hold(&c->s->links,link,(fips_settle_holder){.peer=true,.identity=peer});
    TRY(fips_settle_link_path(&c->s->links,link,from,FIPS_SETTLE_LIVE));
    if (first) fips_settle_gate_forget(&c->s->gates,&peer);
    OUT(o,FIPS_SETTLE_OUT_INSTALL); o->has_peer=true; o->peer=peer; o->link=link; o->pair=*from; o->build=FIPS_SETTLE_BUILD_FROM_CARRY; o->initiator=true; o->lease=leg.lease; leg.lease=(fips_rekey_index_lease){0};
    TRY(reset(c,peer,FIPS_SETTLE_TRANSITION_MOVE));
    OUT(sw,FIPS_SETTLE_OUT_SWAPPED); sw->has_peer=true; sw->peer=peer; sw->own=own; sw->unpin=unpin;
    const fips_settle_link *oldrec=fips_settle_link_get(&c->s->links,old); fips_settle_link before=oldrec ? *oldrec : (fips_settle_link){0};
    size_t offset=c->s->close_count; TRY(retire(c,old,false,false)); size_t n=offset;
    for (size_t i=offset;i<c->s->close_count;++i) {
        fips_settle_pair *p=&c->s->storage.closes[i];
        if (!oldrec || pair_eq(&before.setup,p) || unkept(&before,has_sending ? &sending : NULL,p)) c->s->storage.closes[n++]=*p;
    }
    c->s->close_count=n;
    if (!answered) {
        newrec=fips_settle_link_get(&c->s->links,link);
        if (newrec && !fips_settle_link_in_use(&c->s->links,&newrec->setup,old)) {
            bool found=false; for (size_t i=offset;i<n;++i) if (pair_eq(&c->s->storage.closes[i],&newrec->setup)) found=true;
            if (!found) { if (n==c->s->storage.close_capacity) return FIPS_ERR_RANGE; c->s->storage.closes[c->s->close_count++]=newrec->setup; }
        }
    }
    OUT(ret,FIPS_SETTLE_OUT_RETIRE); ret->link=old; ret->close_offset=offset; ret->close_count=c->s->close_count-offset; return FIPS_OK;
}
static fips_status msg2(context *c,uint64_t link,const uint8_t epoch[8],const fips_settle_pair *from)
{
    const fips_settle_leg *leg=fips_settle_leg_get(&c->s->legs,link);
    if (!leg || leg->answer || !leg->has_peer) return FIPS_OK;
    fips_node_addr peer=leg->peer; fips_settle_held *h=held(c->s,&peer);
    if (!h) return dialed(c,peer,link,epoch,from,FIPS_SETTLE_TRANSITION_PROMOTE);
    if (memcmp(h->epoch,epoch,8)) {
        fips_settle_ask a={.source=FIPS_SETTLE_SOURCE_OWN_MSG2,.has_held=true,.held=liveness(h),.has_epoch=true}; memcpy(a.epoch,epoch,8); fips_settle_refusal refusal;
        TRY(fips_settle_gate(&c->s->gates,peer,&a,c->now,&c->table->params,&refusal));
        if (refusal!=FIPS_SETTLE_GATE_ACCEPT) { TRY(reject(c,&peer,&a.source,FIPS_SETTLE_DENIAL_GATE,refusal)); return abandon(c,link); }
        TRY(remove_peer(c,peer,FIPS_SETTLE_WHY_OWN_RESTART)); return dialed(c,peer,link,epoch,from,FIPS_SETTLE_TRANSITION_REPLACE);
    }
    const fips_settle_link *r=fips_settle_link_get(&c->s->links,h->link); bool own=r && r->direction==FIPS_LINK_OUTBOUND;
    if (own || memcmp(c->table->me.bytes,peer.bytes,16)<0) return moveto(c,peer,link,from,own);
    fips_settle_leg gone; if (!fips_settle_leg_take(&c->s->legs,link,&gone)) return FIPS_OK;
    TRY(free_lease(c,&gone.lease)); TRY(retire(c,link,true,false)); return reset(c,peer,FIPS_SETTLE_TRANSITION_KEEP);
}
static fips_status swap(context *c,uint64_t link,const fips_settle_pair *from)
{
    fips_settle_meta *m=meta(c->s,link); if (!m || !m->pending_redial) return FIPS_OK;
    const fips_settle_link *r=fips_settle_link_get(&c->s->links,link); if (!r || !m->answered) return FIPS_OK;
    fips_settle_meta a=*m; fips_settle_leg leg; if (!fips_settle_leg_take(&c->s->legs,link,&leg)) return FIPS_OK;
    fips_settle_pair pair=from ? *from : r->setup; clear_meta(c->s,link);
    fips_settle_held *old=held(c->s,&a.peer); bool existed=old!=NULL, heard_before=old && old->heard;
    if (existed) TRY(remove_peer(c,a.peer,FIPS_SETTLE_WHY_REDIAL));
    TRY(fips_settle_gate_swapped(&c->s->gates,a.peer,heard_before,c->now));
    fips_settle_held h={.peer=a.peer,.link=link,.current=pair,.installed=c->now,.has_setup=true}; memcpy(h.epoch,a.epoch,8); memcpy(h.setup,a.ephemeral,32);
    TRY(fips_settle_gate_promoted(&c->s->gates,a.peer,a.ephemeral,c->now,NULL,&c->table->params));
    return install(c,h,FIPS_SETTLE_BUILD_FRESH,false,&leg.lease,existed ? FIPS_SETTLE_TRANSITION_REPLACE : FIPS_SETTLE_TRANSITION_PROMOTE);
}
static fips_status run(context *c,const fips_settle_input *in)
{
    fips_settle_table_state *s=c->s;
    switch (in->kind) {
    case FIPS_SETTLE_IN_DIAL: case FIPS_SETTLE_IN_PUNCHED: return dial(c,&in->data.dial);
    case FIPS_SETTLE_IN_CONNECTING: {
        uint64_t link; TRY(open_link(c,&in->data.connecting.to,FIPS_LINK_OUTBOUND,&link)); fips_settle_meta *m=meta(s,link);
        m->connecting=true; m->peer=in->data.connecting.peer; m->has_cap=in->data.connecting.has_cap; m->cap=in->data.connecting.cap;
        fips_settle_held *h=held(s,&m->peer); if (h) { m->has_held=true; m->held=h->link; }
        OUT(o,FIPS_SETTLE_OUT_OPENED); o->link=link; return FIPS_OK;
    }
    case FIPS_SETTLE_IN_CONNECT_FAILED: return abandon(c,in->data.link);
    case FIPS_SETTLE_IN_ARRIVED: {
        const fips_settle_leg *leg=stored(c,&in->data.arrived.from,in->data.arrived.bytes);
        if (leg) return answer_stored(c,leg);
        return full(c) ? reject(c,NULL,NULL,FIPS_SETTLE_DENIAL_FULL,FIPS_SETTLE_GATE_ACCEPT) : FIPS_OK;
    }
    case FIPS_SETTLE_IN_MSG1: {
        const fips_settle_read *read=&in->data.msg1.read; const fips_settle_leg *leg=stored(c,&read->from,read->bytes); if (leg) return answer_stored(c,leg);
        if (capped(c,&read->peer)) return reject(c,&read->peer,NULL,FIPS_SETTLE_DENIAL_CAP,FIPS_SETTLE_GATE_ACCEPT);
        fips_settle_held *h=held(s,&read->peer);
        if (h && !memcmp(h->epoch,read->epoch,8)) {
            fips_settle_placement place=fips_settle_place(&s->links,&read->from,h->link,expects,s);
            OUT(o,FIPS_SETTLE_OUT_REKEY); o->has_peer=true; o->peer=read->peer; o->on_link=place.on_link; o->reachable=in->data.msg1.reachable; o->heard=h->heard; return FIPS_OK;
        }
        return admit(c,h ? FIPS_SETTLE_SOURCE_RESTART : FIPS_SETTLE_SOURCE_NOT_HELD,read);
    }
    case FIPS_SETTLE_IN_REPLACE: return admit(c,FIPS_SETTLE_SOURCE_REPLACEMENT,&in->data.read);
    case FIPS_SETTLE_IN_REDIAL: return redial_read(c,&in->data.read);
    case FIPS_SETTLE_IN_LEG_HEARD: return swap(c,in->data.leg_heard.link,&in->data.leg_heard.from);
    case FIPS_SETTLE_IN_HELLO: return hello(c,in->data.hello.link,in->data.hello.msg2,in->data.hello.lease);
    case FIPS_SETTLE_IN_ABANDON: {
        fips_settle_meta *m=meta(s,in->data.abandon.link); if (!m || !m->asked) return FIPS_OK;
        if (in->data.abandon.authorized && m->redial) TRY(fips_settle_gate_redialed(&s->gates,m->peer,m->ephemeral,c->now,&c->table->params));
        return retire(c,m->link,true,false);
    }
    case FIPS_SETTLE_IN_MSG2: return msg2(c,in->data.msg2.link,in->data.msg2.epoch,&in->data.msg2.from);
    case FIPS_SETTLE_IN_RESTARTED: case FIPS_SETTLE_IN_PEER_EPOCH: {
        fips_settle_held *h=held(s,&in->data.epoch.peer); if (!h) return FIPS_OK;
        if (in->kind==FIPS_SETTLE_IN_RESTARTED) {
            fips_settle_ask a={.source=FIPS_SETTLE_SOURCE_REKEY_MSG2,.has_held=true,.held=liveness(h),.has_epoch=true}; memcpy(a.epoch,in->data.epoch.epoch,8); fips_settle_refusal refusal;
            TRY(fips_settle_gate(&s->gates,h->peer,&a,c->now,&c->table->params,&refusal));
            if (refusal!=FIPS_SETTLE_GATE_ACCEPT) return reject(c,&h->peer,&a.source,FIPS_SETTLE_DENIAL_GATE,refusal);
        }
        memcpy(h->epoch,in->data.epoch.epoch,8);
        return in->kind==FIPS_SETTLE_IN_RESTARTED ? reset(c,h->peer,FIPS_SETTLE_TRANSITION_RESTARTED) : FIPS_OK;
    }
    case FIPS_SETTLE_IN_ENDED: return remove_peer(c,in->data.peer,FIPS_SETTLE_WHY_ENDED);
    case FIPS_SETTLE_IN_ORPHANED: return remove_peer(c,in->data.peer,FIPS_SETTLE_WHY_ORPHANED);
    case FIPS_SETTLE_IN_DISCONNECT: return remove_peer(c,in->data.peer,FIPS_SETTLE_WHY_DISCONNECT);
    case FIPS_SETTLE_IN_DEAD: return remove_peer(c,in->data.peer,FIPS_SETTLE_WHY_DEAD);
    case FIPS_SETTLE_IN_ADMIN: {
        bool was_held=held(s,&in->data.peer)!=NULL; TRY(remove_peer(c,in->data.peer,FIPS_SETTLE_WHY_ADMIN));
        fips_settle_meta *pending=redial(s,&in->data.peer); return was_held && pending ? abandon(c,pending->link) : FIPS_OK;
    }
    case FIPS_SETTLE_IN_HEARD: {
        fips_settle_held *h=held(s,&in->data.heard.peer); if (!h) return FIPS_OK;
        uint8_t kind=in->data.heard.kind;
        if (kind!=FIPS_LINK_SessionDatagram && kind!=FIPS_LINK_PathProbe && kind!=FIPS_LINK_PathAck && kind!=FIPS_LINK_PathClose) {
            const fips_settle_link *r=fips_settle_link_get(&s->links,h->link); h->has_sending=false;
            if (r) for (size_t i=0;i<r->path_count;++i) if (pair_eq(&r->paths[i].pair,&in->data.heard.from)) { h->has_sending=true; h->sending=in->data.heard.from; break; }
        }
        bool unheard=!h->heard, framed=h->has_last; h->heard=true; h->has_last=true; h->last=c->now;
        if (unheard) fips_settle_gate_heard(&s->gates,&h->peer); else if (!framed) fips_settle_gate_framed(&s->gates,&h->peer);
        return FIPS_OK;
    }
    case FIPS_SETTLE_IN_SENT: return sent(c,in->data.link);
    case FIPS_SETTLE_IN_SEND_FAILED: {
        const fips_settle_leg *leg=fips_settle_leg_get(&s->legs,in->data.link);
        if (leg && !leg->answer) fips_settle_leg_unsent(&s->legs,in->data.link,c->now,&c->table->params);
        else { fips_settle_meta *m=meta(s,in->data.link); if (m && m->answered) m->replaces=false; }
        return FIPS_OK;
    }
    case FIPS_SETTLE_IN_UNSENDABLE: {
        const fips_settle_leg *leg=fips_settle_leg_get(&s->legs,in->data.link); if (!leg || !leg->answer) return FIPS_OK;
        fips_settle_leg gone; if (!fips_settle_leg_take(&s->legs,in->data.link,&gone)) return FIPS_OK;
        TRY(free_lease(c,&gone.lease)); return retire(c,in->data.link,true,true);
    }
    case FIPS_SETTLE_IN_MOVED: {
        TRY(fips_settle_link_moved(&s->links,in->data.moved.link,&in->data.moved.from,&in->data.moved.to));
        for (size_t i=0;i<s->storage.capacity;++i) {
            fips_settle_held *h=&s->storage.held[i]; if (!h->active || h->link!=in->data.moved.link || !pair_eq(&h->current,&in->data.moved.from)) continue;
            h->current=in->data.moved.to; return in->data.moved.roam ? reset(c,h->peer,FIPS_SETTLE_TRANSITION_ROAM) : FIPS_OK;
        }
        return FIPS_OK;
    }
    case FIPS_SETTLE_IN_SWITCHED: {
        TRY(fips_settle_link_path(&s->links,in->data.switched.link,&in->data.switched.to,in->data.switched.proven));
        for (size_t i=0;i<s->storage.capacity;++i) if (s->storage.held[i].active && s->storage.held[i].link==in->data.switched.link) { s->storage.held[i].current=in->data.switched.to; break; }
        return FIPS_OK;
    }
    case FIPS_SETTLE_IN_RELEASED: {
        size_t offset=s->close_count,n;
        TRY(fips_settle_link_release(&s->links,in->data.released.link,&in->data.released.pair,in->data.released.keep,s->storage.closes ? s->storage.closes+offset : NULL,s->storage.close_capacity-offset,&n));
        s->close_count+=n; OUT(o,FIPS_SETTLE_OUT_RELEASE); o->link=in->data.released.link; o->close_offset=offset; o->close_count=n; return FIPS_OK;
    }
    case FIPS_SETTLE_IN_CAPABLE: fips_settle_link_capable(&s->links,in->data.link); return FIPS_OK;
    case FIPS_SETTLE_IN_STATE: return fips_settle_link_path(&s->links,in->data.state.link,&in->data.state.pair,in->data.state.proven);
    }
    return FIPS_ERR_RANGE;
}
static bool storage_valid(const fips_settle_table_storage *s)
{
    size_t n=s->capacity;
    return n<=SIZE_MAX/sizeof(*s->links) && n<=SIZE_MAX/sizeof(*s->legs) &&
        n<=SIZE_MAX/sizeof(*s->meta) && n<=SIZE_MAX/sizeof(*s->held) &&
        (!n || s->paths_per_link<=SIZE_MAX/sizeof(*s->paths)/n) &&
        s->gate_capacity<=SIZE_MAX/sizeof(*s->memory) && s->output_capacity<=SIZE_MAX/sizeof(*s->outputs) &&
        s->close_capacity<=SIZE_MAX/sizeof(*s->closes);
}
fips_status fips_settle_table_init(fips_settle_table *t,fips_node_addr me,const fips_settle_params *p,const fips_settle_table_storage storage[2])
{
    if (!storage_valid(&storage[0]) || !storage_valid(&storage[1]) || p->replays>FIPS_SETTLE_GATE_EPHEMERALS_MAX ||
        !p->counted || p->counted>FIPS_SETTLE_GATE_EPHEMERALS_MAX) return FIPS_ERR_RANGE;
    const fips_settle_table_storage *a=&storage[0], *b=&storage[1];
    if (a->capacity!=b->capacity || a->paths_per_link!=b->paths_per_link || a->gate_capacity!=b->gate_capacity ||
        a->output_capacity!=b->output_capacity || a->close_capacity!=b->close_capacity) return FIPS_ERR_RANGE;
    fips_settle_table value={.me=me,.params=*p};
    for (unsigned i=0;i<2;++i) {
        fips_settle_table_state *s=&value.states[i]; s->storage=storage[i];
        TRY(fips_settle_links_init(&s->links,s->storage.links,s->storage.capacity,s->storage.paths,s->storage.paths_per_link));
        fips_settle_legs_init(&s->legs,s->storage.legs,s->storage.capacity);
        TRY(fips_settle_gates_init(&s->gates,s->storage.memory,s->storage.gate_capacity));
        if (s->storage.capacity) { memset(s->storage.meta,0,s->storage.capacity*sizeof(*s->storage.meta)); memset(s->storage.held,0,s->storage.capacity*sizeof(*s->storage.held)); }
    }
    *t=value; return FIPS_OK;
}
static fips_status begin(fips_settle_table *t,fips_instant now,context *c)
{
    fips_settle_table_state *a=current(t), *b=&t->states[t->active^1u];
    for (size_t i=0;i<a->output_count;++i) if (a->storage.outputs[i].lease.live) return FIPS_ERR_RANGE;
    size_t n=a->storage.capacity;
    if (n) {
        memcpy(b->storage.links,a->storage.links,n*sizeof(*a->storage.links));
        memcpy(b->storage.legs,a->storage.legs,n*sizeof(*a->storage.legs));
        memcpy(b->storage.meta,a->storage.meta,n*sizeof(*a->storage.meta));
        memcpy(b->storage.held,a->storage.held,n*sizeof(*a->storage.held));
        if (a->storage.paths_per_link) memcpy(b->storage.paths,a->storage.paths,n*a->storage.paths_per_link*sizeof(*a->storage.paths));
        for (size_t i=0;i<n;++i) b->storage.links[i].paths=b->storage.paths_per_link ? b->storage.paths+i*b->storage.paths_per_link : NULL;
    }
    if (a->storage.gate_capacity) memcpy(b->storage.memory,a->storage.memory,a->storage.gate_capacity*sizeof(*a->storage.memory));
    b->links.next=a->links.next; b->output_count=0; b->close_count=0;
    *c=(context){.table=t,.s=b,.now=now}; return FIPS_OK;
}
static void commit(fips_settle_table *t) { t->active^=1u; }
static bool pair_valid(const fips_settle_pair *p) { return p->addr.len<=FIPS_TRANSPORT_ADDR_CAPACITY; }
static bool read_valid(const fips_settle_read *r) { return r->bytes.length<=FIPS_SETTLE_MESSAGE_CAPACITY && pair_valid(&r->from); }
static bool input_valid(const fips_settle_input *i)
{
    switch (i->kind) {
    case FIPS_SETTLE_IN_DIAL: case FIPS_SETTLE_IN_PUNCHED: return i->data.dial.msg1.length<=FIPS_SETTLE_MESSAGE_CAPACITY && pair_valid(&i->data.dial.to) && i->data.dial.lease && i->data.dial.lease->live;
    case FIPS_SETTLE_IN_HELLO: return i->data.hello.msg2.length<=FIPS_SETTLE_MESSAGE_CAPACITY && i->data.hello.lease && i->data.hello.lease->live;
    case FIPS_SETTLE_IN_MSG1: return read_valid(&i->data.msg1.read);
    case FIPS_SETTLE_IN_REPLACE: case FIPS_SETTLE_IN_REDIAL: return read_valid(&i->data.read);
    case FIPS_SETTLE_IN_ARRIVED: return pair_valid(&i->data.arrived.from) && i->data.arrived.bytes.length<=FIPS_SETTLE_MESSAGE_CAPACITY;
    case FIPS_SETTLE_IN_CONNECTING: return pair_valid(&i->data.connecting.to);
    case FIPS_SETTLE_IN_LEG_HEARD: return pair_valid(&i->data.leg_heard.from);
    case FIPS_SETTLE_IN_MSG2: return pair_valid(&i->data.msg2.from);
    case FIPS_SETTLE_IN_HEARD: return pair_valid(&i->data.heard.from);
    case FIPS_SETTLE_IN_MOVED: return pair_valid(&i->data.moved.from) && pair_valid(&i->data.moved.to);
    case FIPS_SETTLE_IN_SWITCHED: return pair_valid(&i->data.switched.to) && (unsigned)i->data.switched.proven<=FIPS_SETTLE_PROBING;
    case FIPS_SETTLE_IN_RELEASED: return pair_valid(&i->data.released.pair);
    case FIPS_SETTLE_IN_STATE: return pair_valid(&i->data.state.pair) && (unsigned)i->data.state.proven<=FIPS_SETTLE_PROBING;
    case FIPS_SETTLE_IN_CONNECT_FAILED: case FIPS_SETTLE_IN_ABANDON: case FIPS_SETTLE_IN_RESTARTED:
    case FIPS_SETTLE_IN_PEER_EPOCH: case FIPS_SETTLE_IN_ENDED: case FIPS_SETTLE_IN_ORPHANED:
    case FIPS_SETTLE_IN_DISCONNECT: case FIPS_SETTLE_IN_DEAD: case FIPS_SETTLE_IN_ADMIN:
    case FIPS_SETTLE_IN_SENT: case FIPS_SETTLE_IN_SEND_FAILED: case FIPS_SETTLE_IN_UNSENDABLE: case FIPS_SETTLE_IN_CAPABLE: return true;
    }
    return false;
}
fips_status fips_settle_table_step(fips_settle_table *t,const fips_settle_input *input,fips_instant now)
{
    if (!input_valid(input)) return FIPS_ERR_RANGE;
    context c; TRY(begin(t,now,&c)); fips_settle_input in=*input; fips_rekey_index_lease *owner=NULL, local={0};
    if (in.kind==FIPS_SETTLE_IN_DIAL || in.kind==FIPS_SETTLE_IN_PUNCHED) { owner=in.data.dial.lease; local=*owner; in.data.dial.lease=&local; }
    if (in.kind==FIPS_SETTLE_IN_HELLO) { owner=in.data.hello.lease; local=*owner; in.data.hello.lease=&local; }
    TRY(run(&c,&in)); commit(t); if (owner && !local.live) *owner=(fips_rekey_index_lease){0}; return FIPS_OK;
}
static fips_settle_leg *first_leg(fips_settle_legs *legs,bool timed,fips_instant now,const uint64_t *only)
{
    fips_settle_leg *best=NULL;
    for (size_t i=0;i<legs->capacity;++i) {
        fips_settle_leg *l=&legs->slots[i]; if (!l->active || (only && l->link!=*only)) continue;
        if (timed && l->deadline.ops->compare(l->deadline,now)>0) continue;
        if (!best || l->link<best->link) best=l;
    }
    return best;
}
static fips_status reap(context *c,const uint64_t *only)
{
    fips_settle_leg *found;
    while ((found=first_leg(&c->s->legs,true,c->now,only))!=NULL) {
        uint64_t link=found->link; fips_settle_leg leg; (void)fips_settle_leg_take(&c->s->legs,link,&leg);
        TRY(free_lease(c,&leg.lease)); TRY(retire(c,link,true,false));
    }
    return FIPS_OK;
}
fips_status fips_settle_table_poll(fips_settle_table *t,fips_instant now)
{
    context c; TRY(begin(t,now,&c)); TRY(reap(&c,NULL));
    /* Reaps precede resends; both groups are sorted by link number. */
    uint64_t previous=0; bool first=true;
    for (;;) {
        const fips_settle_leg *best=NULL;
        for (size_t i=0;i<c.s->legs.capacity;++i) {
            const fips_settle_leg *l=&c.s->legs.slots[i];
            if (!l->active || (!first && l->link<=previous) || !(l->retry || (l->has_next && l->next.ops->compare(l->next,now)<=0))) continue;
            if (!best || l->link<best->link) best=l;
        }
        if (!best) break;
        previous=best->link; first=false;
        context *cp=&c; fips_settle_output *o=emit(cp,FIPS_SETTLE_OUT_RESEND); if (!o) return FIPS_ERR_RANGE;
        o->link=best->link; o->length=best->msg1_len; if (o->length) memcpy(o->bytes,best->msg1,o->length);
    }
    commit(t); return FIPS_OK;
}
fips_status fips_settle_table_reap_leg(fips_settle_table *t,uint64_t link,fips_instant now)
{ context c; TRY(begin(t,now,&c)); TRY(reap(&c,&link)); commit(t); return FIPS_OK; }
fips_status fips_settle_table_close(fips_settle_table *t,fips_instant now)
{
    context c; TRY(begin(t,now,&c)); fips_settle_leg *found;
    while ((found=first_leg(&c.s->legs,false,now,NULL))!=NULL) {
        uint64_t link=found->link; fips_settle_leg leg; (void)fips_settle_leg_take(&c.s->legs,link,&leg);
        TRY(free_lease(&c,&leg.lease)); TRY(retire(&c,link,false,false));
    }
    for (unsigned pass=0;pass<2;++pass) for (;;) {
        fips_settle_meta *best=NULL;
        for (size_t i=0;i<c.s->storage.capacity;++i) {
            fips_settle_meta *m=&c.s->storage.meta[i];
            if (m->active && (pass==0 ? m->connecting : m->asked) && (!best || m->link<best->link)) best=m;
        }
        if (!best) break;
        TRY(retire(&c,best->link,false,false));
    }
    if (c.s->close_count) {
        fips_settle_output *o=emit(&c,FIPS_SETTLE_OUT_CLOSE); if (!o) return FIPS_ERR_RANGE;
        o->close_count=c.s->close_count;
    }
    for (;;) {
        fips_settle_held *best=NULL;
        for (size_t i=0;i<c.s->storage.capacity;++i) {
            fips_settle_held *h=&c.s->storage.held[i];
            if (h->active && (!best || memcmp(h->peer.bytes,best->peer.bytes,16)<0)) best=h;
        }
        if (!best) break;
        TRY(remove_peer(&c,best->peer,FIPS_SETTLE_WHY_CLOSED));
    }
    commit(t); return FIPS_OK;
}
fips_settle_outputs fips_settle_table_outputs(fips_settle_table *t)
{
    fips_settle_table_state *s=current(t); return (fips_settle_outputs){.items=s->storage.outputs,.count=s->output_count,.closes=s->storage.closes,.close_count=s->close_count};
}
const fips_settle_held *fips_settle_table_held(const fips_settle_table *t,const fips_node_addr *peer) { return held(current(t),peer); }
const fips_settle_links *fips_settle_table_links(const fips_settle_table *t) { return &current(t)->links; }
const fips_settle_legs *fips_settle_table_legs(const fips_settle_table *t) { return &current(t)->legs; }
const fips_settle_gates *fips_settle_table_gates(const fips_settle_table *t) { return &current(t)->gates; }
bool fips_settle_table_redialing(const fips_settle_table *t,const fips_node_addr *peer,uint64_t *link)
{ fips_settle_meta *m=redial(current(t),peer); if (!m) return false; *link=m->link; return true; }
bool fips_settle_table_next_wake(const fips_settle_table *t,fips_instant *out) { return fips_settle_legs_next_wake(&current(t)->legs,out); }
bool fips_settle_table_count_refused(fips_settle_table *t,const fips_node_addr *peer,uint32_t *out)
{ return fips_settle_gate_count_refused(&current(t)->gates,peer,out); }
uint32_t fips_settle_table_take_refused(fips_settle_table *t,const fips_node_addr *peer)
{ return fips_settle_gate_take_refused(&current(t)->gates,peer); }
fips_status fips_settle_table_open_link(fips_settle_table *t,const fips_settle_pair *setup,fips_link_direction direction,uint64_t *link)
{
    context c; TRY(begin(t,(fips_instant){0},&c)); uint64_t value; TRY(open_link(&c,setup,direction,&value)); commit(t); *link=value; return FIPS_OK;
}
fips_status fips_settle_table_drop_leg(fips_settle_table *t,uint64_t link,bool *found,fips_rekey_index_lease *lease)
{
    context c; TRY(begin(t,(fips_instant){0},&c)); fips_settle_leg leg; bool got=fips_settle_leg_take(&c.s->legs,link,&leg);
    if (got) clear_meta(c.s,link);
    commit(t); *found=got; if (got) *lease=leg.lease; return FIPS_OK;
}
fips_status fips_settle_table_unadopt(fips_settle_table *t,const fips_node_addr *peer,bool *found,fips_settle_held *out)
{
    context c; TRY(begin(t,(fips_instant){0},&c)); fips_settle_held *h=held(c.s,peer), value={0}; bool got=h!=NULL;
    if (h) { value=*h; memset(h,0,sizeof(*h)); }
    commit(t); *found=got; if (got) *out=value; return FIPS_OK;
}
