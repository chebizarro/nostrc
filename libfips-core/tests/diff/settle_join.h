/* Pinned join routing trace replay; helpers are private to settle.c. */
#include <fips/proto/settle/join.h>
static fips_path_ref join_path(unsigned port)
{ fips_path_ref p={.transport={1}}; uint8_t a=(uint8_t)port; REQUIRE(fips_transport_addr_new(&a,1,&p.addr)==FIPS_OK); return p; }
static fips_settle_proven join_read(void *ctx,const fips_path_ref *p)
{ (void)ctx; return p->addr.bytes[0]==2 ? FIPS_SETTLE_ACKED : FIPS_SETTLE_LIVE; }
static const char *const join_rekey_names[]={"Send","Prompt","Deliver","Bind","Unbind","Delegate","Revoke","Release","Installed","Retired","Reverted","PeerRestarted","Rekeyed","ReplaceRequested","Reject","Abandoned","Teardown","RedialRequested"};
static const char *const join_path_names[]={"Send","Moved","Released","State","Capable","NotCapable","Refused","Switched","Orphaned","Connect","Proven"};
static size_t join_paths(fips_path_output *out)
{
    fips_path_output items[]={
        {.kind=FIPS_PATH_OUTPUT_Send,.to=join_path(1)},
        {.kind=FIPS_PATH_OUTPUT_Moved,.to=join_path(3),.as.moved={.from=join_path(1)}},
        {.kind=FIPS_PATH_OUTPUT_Released,.to=join_path(4),.as.released={.keep=true}},
        {.kind=FIPS_PATH_OUTPUT_State,.to=join_path(2),.as.state={.next=FIPS_PATH_Live}},
        {.kind=FIPS_PATH_OUTPUT_State,.to=join_path(5),.as.state={.next=FIPS_PATH_Live}},
        {.kind=FIPS_PATH_OUTPUT_State,.to=join_path(6),.as.state={.next=FIPS_PATH_NoState}},
        {.kind=FIPS_PATH_OUTPUT_Capable},{.kind=FIPS_PATH_OUTPUT_NotCapable},
        {.kind=FIPS_PATH_OUTPUT_Refused,.to=join_path(1)},
        {.kind=FIPS_PATH_OUTPUT_Switched,.to=join_path(5)},
        {.kind=FIPS_PATH_OUTPUT_Switched,.to=join_path(2)},
        {.kind=FIPS_PATH_OUTPUT_Orphaned},{.kind=FIPS_PATH_OUTPUT_Connect,.to=join_path(1)},
        {.kind=FIPS_PATH_OUTPUT_Proven,.to=join_path(1)}};
    memcpy(out,items,sizeof(items)); return sizeof(items)/sizeof(*items);
}
static json_t *join_input(const fips_settle_input *i)
{
    json_t *a=json_array(); const fips_node_addr *peer=NULL; uint64_t link=0;
    switch(i->kind) {
    case FIPS_SETTLE_IN_RESTARTED: add(a,json_string("Restarted")); add(a,hex_bytes(i->data.epoch.epoch,8)); peer=&i->data.epoch.peer; break;
    case FIPS_SETTLE_IN_ENDED: add(a,json_string("Ended")); peer=&i->data.peer; break;
    case FIPS_SETTLE_IN_ORPHANED: add(a,json_string("Orphaned")); peer=&i->data.peer; break;
    case FIPS_SETTLE_IN_STATE: add(a,json_string("State")); link=i->data.state.link; add(a,integer(link)); add(a,integer(i->data.state.pair.addr.bytes[0])); add(a,json_string(i->data.state.proven==FIPS_SETTLE_LIVE ? "Live" : i->data.state.proven==FIPS_SETTLE_ACKED ? "Acked" : "Probing")); break;
    case FIPS_SETTLE_IN_MOVED: add(a,json_string("Moved")); link=i->data.moved.link; add(a,integer(link)); add(a,integer(i->data.moved.from.addr.bytes[0])); add(a,integer(i->data.moved.to.addr.bytes[0])); add(a,json_boolean(i->data.moved.roam)); break;
    case FIPS_SETTLE_IN_SWITCHED: add(a,json_string("Switched")); link=i->data.switched.link; add(a,integer(link)); add(a,integer(i->data.switched.to.addr.bytes[0])); add(a,json_string(i->data.switched.proven==FIPS_SETTLE_LIVE ? "Live" : "Acked")); break;
    case FIPS_SETTLE_IN_RELEASED: add(a,json_string("Released")); link=i->data.released.link; add(a,integer(link)); add(a,integer(i->data.released.pair.addr.bytes[0])); add(a,json_boolean(i->data.released.keep)); break;
    case FIPS_SETTLE_IN_CAPABLE: add(a,json_string("Capable")); add(a,integer(i->data.link)); break;
    default: REQUIRE(false);
    }
    if (peer) { uint8_t want[16]; memset(want,2,16); REQUIRE(!memcmp(want,peer->bytes,16)); }
    return a;
}
static size_t check_join(json_t *trace)
{
    json_t *rows=field(trace,"rows"); REQUIRE(json_is_array(rows) && json_array_size(rows));
    fips_settle_joined *j=fips_test_settle_array(1,sizeof(*j)); fips_node_addr peer; memset(peer.bytes,2,16);
    for (size_t n=0;n<json_array_size(rows);++n) {
        json_t *r=json_array_get(rows,n),*op=field(r,"op"); const char *kind=text(json_array_get(op,0));
        fips_settle_join_event e={0}; fips_rekey_outputs rekey={0}; fips_path_output items[14]; fips_path_outputs paths={items,14,0};
        if (!strcmp(kind,"rekey")) {
            const char *name=text(json_array_get(op,1)); unsigned k=0; while(k<18 && strcmp(name,join_rekey_names[k])) ++k; REQUIRE(k<18);
            rekey.count=1; rekey.items[0].kind=(fips_rekey_output_kind)k; memset(rekey.items[0].epoch,6,8);
            if (k==FIPS_REKEY_OUT_Release) rekey.items[0].lease=(fips_rekey_index_lease){4,true};
            e.kind=FIPS_SETTLE_JOIN_REKEY; e.rekey_out=&rekey;
        } else if (!strcmp(kind,"paths") || !strcmp(kind,"path_sequence")) {
            paths.len=join_paths(items); e.kind=FIPS_SETTLE_JOIN_PATHS; e.path_out=&paths; e.read=join_read;
            if (!strcmp(kind,"paths")) { size_t i=(size_t)at(op,1); REQUIRE(i<paths.len); items[0]=items[i]; paths.len=1; json_t *link=json_array_get(op,2),*released=json_array_get(op,3); e.has_link=!json_is_null(link); e.has_released=!json_is_null(released); if(e.has_link)e.link=num(link); if(e.has_released)e.released=num(released); }
            else { e.has_link=true; e.link=7; e.has_released=true; e.released=3; }
        } else {
            REQUIRE(!strcmp(kind,"roam")); e.kind=FIPS_SETTLE_JOIN_ROAM; e.has_from=json_is_true(json_array_get(op,1)); e.standby=json_is_true(json_array_get(op,2)); e.link=7; fips_path_ref from=join_path(1),to=join_path(2); e.from=fips_settle_path_pair(&from); e.to=fips_settle_path_pair(&to);
        }
        REQUIRE(fips_settle_join(j,peer,&e)==FIPS_OK); json_t *got=json_array(); fips_settle_join_step s;
        while(fips_settle_join_next(j,&s)) {
            json_t *v=json_array();
            if(s.kind==FIPS_SETTLE_STEP_CORE) {add(v,json_string("core"));add(v,join_input(&s.input));}
            else if(s.kind==FIPS_SETTLE_STEP_REKEY) {REQUIRE((unsigned)s.rekey->kind<18);add(v,json_string("rekey"));add(v,json_string(join_rekey_names[s.rekey->kind]));if(s.rekey->lease.live){uint32_t index; REQUIRE(fips_rekey_index_lease_release(&s.rekey->lease,&index));}}
            else if(s.kind==FIPS_SETTLE_STEP_PATH) {REQUIRE((unsigned)s.path->kind<11);add(v,json_string("path"));add(v,json_string(join_path_names[s.path->kind]));}
            else {add(v,json_string(s.kind==FIPS_SETTLE_STEP_REPLACE ? "replace" : "redial"));}
            add(got,v);
        }
        equal(got,field(r,"out")); fips_rekey_outputs_clear(&rekey); fips_settle_join_clear(j);
    }
    free(j); return json_array_size(rows);
}
