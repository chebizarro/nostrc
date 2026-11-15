#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "control/control.h"
#include "control/probe.h"
#include "privilege.h"
#include "host_os.h"
#include <fips/identity/local.h>
#include <fips/identity/address.h>
#include <fips/identity/node_addr.h>
#include <libgo/fiber.h>
#include <go.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <time.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s (errno %d)\n",__FILE__,__LINE__,#x,errno); abort(); } } while (0)
static unsigned golden_count;
static const char *const volatile_keys[] = {
    "version", "pid", "exe_path", "control_socket", "tun_name", "allow_file", "deny_file", "uptime_secs",
    "started_at_ms", "session_start_ms", "authenticated_at_ms", "last_seen_ms", "last_activity_ms", "last_recv_ms",
    "created_at_ms", "initiated_ms", "last_sent_ms", "age_ms", "last_used_ms", "idle_ms", "first_seen_secs_ago",
    "last_contact_secs_ago", "since_secs"
};
static void normalize(json_t *v)
{
    if (json_is_array(v)) {
        size_t i; json_t *item;
        json_array_foreach(v,i,item) normalize(item);
    } else if (json_is_object(v)) {
        const char *k; json_t *item;
        json_object_foreach(v,k,item) {
            bool redact = false;
            for (size_t i=0;i<sizeof(volatile_keys)/sizeof(*volatile_keys);++i)
                if (!strcmp(k,volatile_keys[i])) redact=true;
            if (redact) json_object_set_new(v,k,json_string("<redacted>")); else normalize(item);
        }
    }
}
static json_t *golden(const char *name)
{
    char path[4096]; snprintf(path,sizeof(path),"%s/%s.json",FIPSD_CONTROL_GOLDEN,name);
    json_error_t e; json_t *v=json_load_file(path,JSON_DECODE_ANY,&e);
    if (!v) fprintf(stderr,"%s: %s\n",path,e.text);
    CHECK(v); return v;
}
static void compare_golden(const char *name, json_t *actual, bool response)
{
    CHECK(actual); normalize(actual);
    json_t *expected=golden(name);
    /* Fixture roots are serde_json::Value maps (data,status); the wire is a
     * serde Response struct (status,data). Canonicalize only that envelope;
     * lexical nested-map ordering is checked as bytes, never json_equal(). */
    char *a=response ? fipsd_control_encode(actual) : json_dumps(actual,JSON_COMPACT|JSON_SORT_KEYS);
    char *e=response ? fipsd_control_encode(expected) : json_dumps(expected,JSON_COMPACT|JSON_SORT_KEYS);
    CHECK(a && e);
    if (strcmp(a,e)) fprintf(stderr,"golden %s\nactual: %s\nexpect: %s\n",name,a,e);
    CHECK(!strcmp(a,e)); ++golden_count;
    free(a); free(e); json_decref(expected); json_decref(actual);
}
static GoRefPtr acquire(void *user) { return go_swap_load(user); }
static json_t *request(const fipsd_control_provider *p, const char *command, json_t *params)
{
    json_t *r=json_pack("{s:s,s:o}","command",command,"params",params ? params : json_null());
    json_t *out=fipsd_control_dispatch(p,r); json_decref(r); CHECK(out); return out;
}
static void expect_error(json_t *response, const char *message)
{
    CHECK(!strcmp(json_string_value(json_object_get(response,"status")),"error"));
    CHECK(!strcmp(json_string_value(json_object_get(response,"message")),message));
    CHECK(!json_object_get(response,"data")); json_decref(response);
}
static void free_snapshot(void *arg)
{
    fipsd_control_snapshot *s=arg;
    json_decref((json_t *)s->tree.my_coords);
    free(s);
}
static GoSwap *fixture(void)
{
    static char npub[64], hex[33], ipv6[64], short_hex[12];
    uint8_t seed[32]; memset(seed,0xab,sizeof(seed));
    fips_secp *secp=NULL; fips_identity *identity=NULL;
    fips_entropy entropy=fipsd_host_os_entropy();
    CHECK(fips_identity_init_secp(entropy,&secp)==FIPS_IDENTITY_OK);
    CHECK(fips_identity_from_secret_bytes(secp,entropy,seed,&identity)==FIPS_IDENTITY_OK);
    CHECK(fips_identity_npub(identity,npub,sizeof(npub))==FIPS_IDENTITY_OK);
    fips_node_addr node=fips_identity_node_addr(identity);
    fips_ipv6_address address=fips_identity_address(identity);
    CHECK(fips_node_addr_format(&node,hex,sizeof(hex))==FIPS_IDENTITY_OK);
    CHECK(fips_node_addr_short_hex(&node,short_hex,sizeof(short_hex))==FIPS_IDENTITY_OK);
    CHECK(fips_ipv6_address_format(&address,ipv6,sizeof(ipv6))==FIPS_IDENTITY_OK);
    fips_identity_destroy(identity); fips_identity_destroy_secp(secp);
    fipsd_control_snapshot *s=calloc(1,sizeof(*s)); CHECK(s);
    s->npub=npub; s->node_addr=hex; s->ipv6_addr=ipv6; s->state="created";
    s->version="test"; s->exe_path="/test/fipsd"; s->control_socket="/test/control.sock";
    s->pid=42; s->is_root=true;
    s->tree.my_node_addr=hex; s->tree.root=hex; s->tree.parent=hex;
    s->tree.root_npub=npub; s->tree.is_root=true; s->tree.declaration_sequence=1;
    s->tree.declaration_signed=true; s->tree.parent_display_name=short_hex;
    s->tree.my_coords=json_pack("[s]",hex);
    s->bloom.own_node_addr=hex;
    s->cache.max_entries=50000; s->cache.default_ttl_ms=300000; s->identity_max_entries=10000;
    s->acl.allow_file="/nonexistent/allow"; s->acl.deny_file="/nonexistent/deny";
    s->acl.default_decision="allow"; s->acl.effective_mode="default_open";
    GoSwap *slot=go_swap_create(s,free_snapshot); CHECK(slot); return slot;
}
static void test_query_goldens(void)
{
    GoSwap *slot=fixture(); fipsd_control_provider p={.user=slot,.acquire=acquire};
    size_t n; const char *const *commands=fipsd_control_commands(&n);
    CHECK(n==20);
    for (size_t i=0;i<n;++i) {
        if (!strcmp(commands[i],"show_metrics")) continue; /* no upstream golden */
        json_t *params=NULL;
        if (!strcmp(commands[i],"show_stats_history")) params=json_pack("{s:s,s:s}","metric","mesh_size","window","10s");
        if (!strcmp(commands[i],"show_stats_all_history")) params=json_pack("{s:s}","window","10s");
        if (!strcmp(commands[i],"show_stats_history_all_peers")) params=json_pack("{s:s,s:s}","metric","srtt_ms","window","10s");
        compare_golden(commands[i],request(&p,commands[i],params),true);
    }
    /* Non-empty typed interface fixture; no fixture JSON is used as state. */
    GoRefPtr ref=go_swap_load(slot); fipsd_control_snapshot with=*(fipsd_control_snapshot *)ref.ptr;
    fipsd_control_interface interface={.name="fips-absent-x0",.presence="absent",.policy="required"};
    fipsd_control_transport transport={.transport_id=1,.type="ethernet",.state="up",.name="lab",.mtu=1499,.interface=&interface};
    with.transports=&transport; with.transports_count=1;
    compare_golden("show_transports_with_interface",fipsd_control_query(&with,"show_transports",NULL),true);
    /* These TWO goldens are upstream CLIENT composition fixtures, not daemon
     * replies. Assert the daemon omits adapter fields, then exercise the same
     * additive merge/fallback contract, keeping them out of server output. */
    for (unsigned fallback=0;fallback<2;++fallback) {
        json_t *r=request(&p,"show_status",NULL), *d=json_object_get(r,"data");
        CHECK(!json_object_get(d,"tun_state") && !json_object_get(d,"effective_ipv6_mtu"));
        json_object_set_new(d,"tun_state",json_string("disabled"));
        json_object_set_new(d,"tun_name",json_string("-"));
        json_object_set_new(d,"effective_ipv6_mtu",json_integer(fallback ? 0 : 1203));
        compare_golden(fallback ? "show_status_fallback" : "show_status_composed",r,true);
    }
    go_refptr_release(&ref); go_swap_free(slot);
}
static void test_probe_golden(void)
{
    fips_probe_snapshot *s=calloc(1,sizeof(*s)); CHECK(s);
    s->overall=FIPS_PROBE_OVERALL_Ok; s->tick_ms=1000;
    fips_probe_stage_record ok={.verdict=FIPS_PROBE_VERDICT_Ok,.has_elapsed_ms=true};
    s->bloom=ok; s->discovery=ok; s->discovery.elapsed_ms=2000; s->path=ok;
    s->session=ok; s->session.elapsed_ms=1000; s->rtt=ok; s->rtt.elapsed_ms=1000;
    s->has_lookup_fanout=true; s->lookup_fanout=3; s->has_resolve_source=true; s->resolve_source=FIPS_PROBE_RESOLVE_Lookup;
    s->session_established=true; s->has_path_mtu=true; s->path_mtu=1420;
    s->has_rtt_ms=true; s->rtt_ms=18; s->has_srtt_ms=true; s->srtt_ms=18.0;
    s->counters.reports_seen=1; s->counters.samples=1; s->torn_down=true; s->lookup_issued=true; s->warmups_sent=1;
    json_t *ours=json_pack("[s,s]","4d2201ff","91b40c6e"), *theirs=json_pack("[s,s]","4a1f9c22","91b40c6e");
    fipsd_control_probe_path path={.our_coords=ours,.their_coords=theirs,.our_depth=1,.their_depth=1,.same_root=true,
        .lca="91b40c6e",.hops_up=1,.hops_down=1,.distance=2,.next_hop_addr="91b40c6e",.next_hop_display_name="relay-a",.next_hop_class="tree_up"};
    const uint64_t timeouts[]={1,2,4,8}; uint8_t attempts=2;
    fipsd_control_probe_report report={.snapshot=s,.probe_id=7,.elapsed_ms=4000,.npub="npub1qq8s4f7x",
        .node_addr="4a1f9c22e08b5d3714aa06fb92c1de55",.display_name="hydra",.ipv6_addr="fd00:4a1f:9c22:e08b:5d37:14aa:06fb:92c1",
        .path=&path,.attempt_timeouts_secs=timeouts,.timeout_count=4,.attempts=&attempts};
    compare_golden("probe_report",fipsd_control_probe_render(&report),false);
    report.path=NULL; s->has_path_mtu=false; s->has_rtt_ms=false; s->has_srtt_ms=false;
    json_t *absent=fipsd_control_probe_render(&report);
    CHECK(!json_object_get(json_object_get(absent,"session"),"path_mtu"));
    CHECK(!json_object_get(json_object_get(absent,"rtt"),"rtt_ms"));
    CHECK(json_is_null(json_object_get(json_object_get(absent,"path"),"lca")));
    json_decref(absent); json_decref(ours); json_decref(theirs); free(s);
}
static json_t *path_provider(void *user, const char *name, const json_t *params,
                             const fipsd_control_request_context *context)
{
    (void)context;
    unsigned *count=user; ++*count;
    CHECK(!strcmp(json_string_value(json_object_get(params,"npub")),"<npub>"));
    if (!strcmp(name,"path_pin")) {
        CHECK(!strcmp(json_string_value(json_object_get(params,"transport")),"2"));
        CHECK(!strcmp(json_string_value(json_object_get(params,"address")),"<addr>"));
    } else CHECK(!strcmp(name,"path_show") || !strcmp(name,"path_unpin"));
    return fipsd_control_ok(json_object());
}
static void test_path_requests(void)
{
    json_t *fixtures=golden("fipsctl_path_requests"); unsigned count=0;
    fipsd_control_provider p={.user=&count,.command=path_provider};
    CHECK(json_array_size(fixtures)==3);
    for (size_t i=0;i<3;++i) {
        const json_t *entry=json_array_get(fixtures,i);
        json_t *r=fipsd_control_dispatch(&p,json_object_get(entry,"request"));
        CHECK(!strcmp(json_string_value(json_object_get(r,"status")),"ok")); json_decref(r);
    }
    CHECK(count==3); ++golden_count; json_decref(fixtures);
}
static void test_validation(void)
{
    expect_error(request(NULL,"show_status",NULL),"node not yet initialized");
    expect_error(request(NULL,"show_metrics",NULL),"subsystem not started");
    const char *live[]={"connect","disconnect","probe_start","probe_poll","probe_cancel","path_show","path_pin","path_unpin"};
    for (size_t i=0;i<sizeof(live)/sizeof(*live);++i) {
        json_t *p=json_pack("{s:s,s:s,s:s,s:i}","npub","test","address","host:1","transport","udp","probe_id",7);
        expect_error(request(NULL,live[i],p),"subsystem not started");
    }
    expect_error(request(NULL,"connect",NULL),"missing params for connect");
    expect_error(request(NULL,"connect",json_object()),"missing 'npub' parameter");
    expect_error(request(NULL,"connect",json_pack("{s:s}","npub","x")),"missing 'address' parameter");
    expect_error(request(NULL,"path_pin",json_pack("{s:s}","npub","x")),"missing 'transport' parameter");
    expect_error(request(NULL,"probe_poll",json_pack("{s:i}","probe_id",-1)),"missing 'probe_id' parameter");
    expect_error(request(NULL,"show_stats_history",NULL),"missing params for show_stats_history");
    expect_error(request(NULL,"show_stats_history",json_object()),"missing 'metric' parameter");
    expect_error(request(NULL,"show_stats_history",json_pack("{s:s}","metric","garbage")),"unknown metric: garbage");
    expect_error(request(NULL,"show_stats_all_history",json_pack("{s:s}","window","")),"empty duration");
    expect_error(request(NULL,"show_stats_all_history",json_pack("{s:s}","window","10d")),"unknown duration unit: d (expected s, m, h)");
    expect_error(request(NULL,"show_stats_all_history",json_pack("{s:s}","window","18446744073709551615h")),"invalid duration: 18446744073709551615h");
    expect_error(request(NULL,"show_stats_all_history",json_pack("{s:s}","granularity","2s")),"unknown granularity: 2s");
    expect_error(request(NULL,"no_such_command",NULL),"unknown command: no_such_command");
    const double values[]={NAN,INFINITY,-INFINITY,1.25};
    fips_stats_series series={.metric="mesh_size",.unit="nodes",.granularity_seconds=1,.values=values,.len=4};
    json_t *r=fipsd_control_series(&series), *a=json_object_get(r,"values");
    CHECK(json_array_size(a)==4);
    for (size_t i=0;i<3;++i) CHECK(json_is_null(json_array_get(a,i)));
    CHECK(json_real_value(json_array_get(a,3))==1.25); json_decref(r);
    const char *valid="{\"command\":\"show_peers\",\"extra\":true}";
    r=fipsd_control_parse(NULL,valid,strlen(valid)); CHECK(json_object_get(r,"data"));
    char *wire=fipsd_control_encode(r); CHECK(!strcmp(wire,"{\"status\":\"ok\",\"data\":{\"peers\":[]}}")); free(wire); json_decref(r);
}
static size_t fail_at, alloc_index, outstanding_allocations;
static bool allocation_failed;
static void *fault_alloc(size_t n)
{
    if (alloc_index++ == fail_at) { allocation_failed=true; return NULL; }
    max_align_t *p=malloc(n+sizeof(*p));
    if (!p) return NULL;
    ++outstanding_allocations; return p+1;
}
static void fault_free(void *p)
{
    if (!p) return;
    CHECK(outstanding_allocations>0); --outstanding_allocations;
    free((max_align_t *)p-1);
}
static void test_render_failures(void)
{
    const fipsd_control_snapshot empty={0};
    json_t *good=fipsd_control_query(&empty,"show_routing",NULL);
    char *expected=fipsd_control_encode(good); CHECK(expected); json_decref(good);
    json_malloc_t old_malloc; json_free_t old_free;
    json_get_alloc_funcs(&old_malloc,&old_free);
    for (size_t n=0;n<512;++n) {
        fail_at=n; alloc_index=0; allocation_failed=false;
        json_set_alloc_funcs(fault_alloc,fault_free);
        json_t *result=fipsd_control_query(&empty,"show_routing",NULL);
        fail_at=SIZE_MAX; /* fault injection stops before assertion encoding */
        if (result && json_object_get(result,"data")) {
            char *actual=fipsd_control_encode(result); CHECK(actual && !strcmp(actual,expected)); free(actual);
        } else if (result) CHECK(!strcmp(json_string_value(json_object_get(result,"status")),"error"));
        json_decref(result);
        json_set_alloc_funcs(old_malloc,old_free);
        CHECK(outstanding_allocations==0);
        if (!allocation_failed) break;
        CHECK(n+1<512);
    }
    free(expected);
    const uint64_t bad_values[]={UINT64_C(9223372036854775808),UINT64_MAX};
    for (size_t i=0;i<2;++i) {
        fipsd_control_counter counter={"received_packets",bad_values[i]};
        fipsd_control_snapshot s={.forwarding={&counter,1}};
        expect_error(fipsd_control_query(&s,"show_routing",NULL),"value exceeds JSON integer range");
    }
    fipsd_control_counter limit={"received_packets",INT64_MAX};
    fipsd_control_snapshot s={.forwarding={&limit,1}};
    json_t *result=fipsd_control_query(&s,"show_routing",NULL);
    json_t *fwd=json_object_get(json_object_get(result,"data"),"forwarding");
    CHECK(json_integer_value(json_object_get(fwd,"received_packets"))==INT64_MAX); json_decref(result);
}
static void test_paths(const char *tmp)
{
    fipsd_control_path_env e={.xdg_runtime_dir=tmp}; char p[4096], expected[4096];
    snprintf(expected,sizeof(expected),"%s/fips/control.sock",tmp);
    CHECK(fipsd_control_resolve_path(NULL,&e,p,sizeof(p))==0 && !strcmp(p,expected));
    e.freebsd=true; e.var_run_exists=true;
    CHECK(fipsd_control_resolve_path(NULL,&e,p,sizeof(p))==0 && !strcmp(p,"/var/run/fips/control.sock"));
    e.run_exists=true;
    CHECK(fipsd_control_resolve_path(NULL,&e,p,sizeof(p))==0 && !strcmp(p,"/run/fips/control.sock"));
    CHECK(fipsd_control_resolve_path("/custom/ctl",&e,p,sizeof(p))==0 && !strcmp(p,"/custom/ctl"));
    e=(fipsd_control_path_env){.macos=true,.privileged=true};
    CHECK(fipsd_control_resolve_path(NULL,&e,p,sizeof(p))==0 && !strcmp(p,"/var/run/fips/control.sock"));
    e.macos=false; e.freebsd=true;
    CHECK(fipsd_control_resolve_path(NULL,&e,p,sizeof(p))==0 && !strcmp(p,"/tmp/fips-control.sock"));
    e=(fipsd_control_path_env){0};
    CHECK(fipsd_control_resolve_path(NULL,&e,p,sizeof(p))==0 && !strcmp(p,"/tmp/fips-control.sock"));
    CHECK(fipsd_control_resolve_path("",&e,p,sizeof(p))<0);
    CHECK(fipsd_control_resolve_path("too-long",&e,p,2)<0);
}
typedef struct client_case {
    fipsd_runtime *rt;
    const char *path, *line, *expected;
    size_t len;
    bool eof, slow;
    GoChannel *done, *ready, *release;
} client_case;
static void client_fiber(void *arg)
{
    client_case *c=arg;
    int fd=socket(AF_UNIX,SOCK_STREAM,0); CHECK(fd>=0);
#ifdef SO_NOSIGPIPE
    int one=1; CHECK(setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&one,sizeof(one))==0);
#endif
    struct sockaddr_un sa={.sun_family=AF_UNIX}; CHECK(strlen(c->path)<sizeof(sa.sun_path));
    memcpy(sa.sun_path,c->path,strlen(c->path)+1);
    CHECK(gof_connect(fd,(struct sockaddr *)&sa,sizeof(sa),5000)==0);
    if (c->slow) {
        CHECK(gof_write(fd,"{",1)==1); CHECK(go_channel_send(c->ready,c)==0);
        void *token; CHECK(go_channel_receive(c->release,&token)==0);
    }
    size_t sent=0;
    while (sent<c->len) {
        ssize_t n=gof_write(fd,c->line+sent,c->len-sent); CHECK(n>0); sent+=(size_t)n;
    }
    if (c->eof) CHECK(shutdown(fd,SHUT_WR)==0);
    char out[8192]; size_t used=0;
    for (;;) {
        ssize_t n=gof_read(fd,out+used,sizeof(out)-1-used);
        if (n==0 || (n<0 && errno==ECONNRESET)) break;
        CHECK(n>0); used+=(size_t)n; CHECK(used<sizeof(out)-1);
    }
    out[used]=0;
    CHECK(!strcmp(out,c->expected));
    CHECK(close(fd)==0); CHECK(go_channel_send(c->done,c)==0);
}
static void run_client(fipsd_runtime *rt, const char *path, const char *line, size_t len,
                        bool eof, const char *expected)
{
    GoChannel *done=go_channel_create(1); CHECK(done);
    client_case c={.rt=rt,.path=path,.line=line,.len=len,.eof=eof,.expected=expected,.done=done};
    CHECK(fipsd_runtime_spawn(rt,client_fiber,&c)==0);
    void *token; CHECK(go_channel_receive(done,&token)==0 && token==&c); go_channel_free(done);
}
static json_t *deadline_provider(void *user, const char *name, const json_t *params,
                                 const fipsd_control_request_context *context)
{
    (void)user; (void)params;
    CHECK(!strcmp(name,"path_show") && context && context->shutdown);
    struct timespec t; CHECK(clock_gettime(CLOCK_MONOTONIC,&t)==0);
    uint64_t now=(uint64_t)t.tv_sec*UINT64_C(1000000000)+(uint64_t)t.tv_nsec;
    uint64_t ms=context->deadline_ns>now ? (context->deadline_ns-now+999999)/1000000 : 0;
    GoChannel *never=go_channel_create(0); CHECK(never);
    void *token=NULL;
    GoSelectCase cases[]={
        {.op=GO_SELECT_RECEIVE,.chan=never,.recv_buf=&token},
        {.op=GO_SELECT_RECEIVE,.chan=go_context_done(context->shutdown),.recv_buf=&token}
    };
    GoSelectResult result=go_select_timeout(cases,2,ms);
    go_channel_free(never);
    CHECK(result.selected_case==-1 || result.selected_case==1);
    return fipsd_control_error(result.selected_case==-1 ? "query timeout" : "node shutting down");
}
static void test_socket(const char *tmp)
{
    char xdg[4096], path[4096]; snprintf(xdg,sizeof(xdg),"%s/runtime",tmp); CHECK(mkdir(xdg,0700)==0);
    CHECK(setenv("XDG_RUNTIME_DIR",xdg,1)==0);
    snprintf(path,sizeof(path),"%s/runtime/fips/control.sock",tmp);
    char yaml[8192]; snprintf(yaml,sizeof(yaml),"node:\n  control:\n    socket_path: %s\n",path);
    fipsd_config_log log={0}; fipsd_config *config=fipsd_config_parse(yaml,strlen(yaml),&log); CHECK(config);
    fipsd_config_log_clear(&log);
    GoSwap *configs=go_swap_create(config,fipsd_config_free); CHECK(configs);
    GoSwap *state=fixture();
    fipsd_control_provider provider={.user=state,.acquire=acquire,.command=deadline_provider};
    fipsd_control_service service; fipsd_control_service_init(&service,configs,&provider);
    /* A stale socket from a dead daemon is reclaimable, not an active one. */
    char parent_before[4096]; snprintf(parent_before,sizeof(parent_before),"%s/runtime/fips",tmp);
    CHECK(mkdir(parent_before,0777)==0);
    int stale=socket(AF_UNIX,SOCK_STREAM,0); CHECK(stale>=0);
    struct sockaddr_un old={.sun_family=AF_UNIX}; CHECK(strlen(path)<sizeof(old.sun_path));
    memcpy(old.sun_path,path,strlen(path)+1);
    CHECK(bind(stale,(struct sockaddr *)&old,sizeof(old))==0); CHECK(close(stale)==0);
    fipsd_runtime_config cfg; fipsd_runtime_config_default(&cfg); cfg.handle_signals=false; cfg.workers=2;
    fipsd_runtime *rt=fipsd_runtime_create(&cfg); CHECK(rt);
    CHECK(fipsd_runtime_register(rt,&service.subsystem)==0 && fipsd_runtime_start(rt)==0);
    fipsd_control_service collision; fipsd_control_service_init(&collision,configs,NULL);
    CHECK(collision.subsystem.start(&collision.subsystem,rt)<0);
    fipsd_control_service_clear(&collision);
    struct stat st; CHECK(lstat(path,&st)==0 && S_ISSOCK(st.st_mode) && (st.st_mode&0777)==0770);
    gid_t gid; char err[256]; int found=fipsd_privilege_resolve_group("fips",&gid,err,sizeof(err)); CHECK(found>=0);
    if (found) CHECK(st.st_gid==gid); else puts("SKIP fips group assertion: group absent (mode still checked)");
    char parent[4096]; snprintf(parent,sizeof(parent),"%s/runtime/fips",tmp);
    CHECK(stat(parent,&st)==0 && (st.st_mode&0777)==0750); if (found) CHECK(st.st_gid==gid);
    CHECK(stat(xdg,&st)==0 && (st.st_mode&0777)==0700); /* shared ancestor unchanged */
    const char *ok="{\"status\":\"ok\",\"data\":{\"peers\":[]}}\n";
    const char *req="{\"command\":\"show_peers\"}\n";
    run_client(rt,path,req,strlen(req),false,ok);
    run_client(rt,path,"{\"command\":\"bad\"}\n",18,false,"{\"status\":\"error\",\"message\":\"unknown command: bad\"}\n");
    run_client(rt,path,"",0,true,"{\"status\":\"error\",\"message\":\"empty request\"}\n");
    /* EOF without delimiter follows the actual Rust serve implementation: a
     * complete final JSON value is accepted; incomplete JSON is refused. */
    run_client(rt,path,req,strlen(req)-1,true,ok);
    json_t *malformed=fipsd_control_parse(NULL,"{\n",2); char *bad=fipsd_control_encode(malformed); CHECK(bad);
    char bad_line[1024]; snprintf(bad_line,sizeof(bad_line),"%s\n",bad); free(bad); json_decref(malformed);
    run_client(rt,path,"{\n",2,false,bad_line);
    char boundary[4097]; memset(boundary,' ',sizeof(boundary)); memcpy(boundary,req,strlen(req)-1);
    boundary[4095]='\n'; run_client(rt,path,boundary,4096,false,ok);
    boundary[4095]=' '; boundary[4096]='\n';
    run_client(rt,path,boundary,4097,false,"{\"status\":\"error\",\"message\":\"read error: request too large\"}\n");
    char twice[256]; snprintf(twice,sizeof(twice),"%s{\"command\":\"bad\"}\n",req);
    run_client(rt,path,twice,strlen(twice),false,ok);
    /* A partially-written first connection must not serialize accept/dispatch
     * of independent connections. Channels are readiness barriers, not sleeps. */
    GoChannel *done=go_channel_create(32), *ready=go_channel_create(1), *release=go_channel_create(1);
    CHECK(done && ready && release);
    client_case slow={.rt=rt,.path=path,.line=req+1,.len=strlen(req)-1,.expected=ok,.slow=true,.done=done,.ready=ready,.release=release};
    CHECK(fipsd_runtime_spawn(rt,client_fiber,&slow)==0);
    void *token; CHECK(go_channel_receive(ready,&token)==0 && token==&slow);
    client_case cases[24];
    for (size_t i=0;i<24;++i) {
        cases[i]=(client_case){.rt=rt,.path=path,.line=req,.len=strlen(req),.expected=ok,.done=done};
        CHECK(fipsd_runtime_spawn(rt,client_fiber,&cases[i])==0);
    }
    /* Concurrently publish new immutable generations while readers retain
     * their old snapshots. This never touches a published object. */
    for (size_t i=0;i<32;++i) {
        fipsd_control_snapshot *next=calloc(1,sizeof(*next)); CHECK(next);
        next->peer_count=i; CHECK(go_swap_store(state,next));
    }
    for (size_t i=0;i<24;++i) CHECK(go_channel_receive(done,&token)==0 && token!=&slow);
    CHECK(go_channel_send(release,&slow)==0); CHECK(go_channel_receive(done,&token)==0 && token==&slow);
    go_channel_free(done); go_channel_free(ready); go_channel_free(release);
    /* A completed read has a separate execution deadline: a provider waiting
     * for a reply must yield query timeout, never a spurious read timeout. */
    const char *execute="{\"command\":\"path_show\",\"params\":{\"npub\":\"test\"}}\n";
    run_client(rt,path,execute,strlen(execute),false,"{\"status\":\"error\",\"message\":\"query timeout\"}\n");
    /* Wait for the REAL protocol deadline, not a synchronization sleep. */
    run_client(rt,path,"",0,false,"{\"status\":\"error\",\"message\":\"read timeout\"}\n");
    /* Shutdown must not depend on reconnecting to the path: remove it while
     * accept is live, replace it with a file, and preserve that replacement. */
    CHECK(unlink(path)==0);
    int replacement=creat(path,0600); CHECK(replacement>=0 && close(replacement)==0);
    fipsd_runtime_shutdown(rt); CHECK(fipsd_runtime_run(rt)==FIPSD_RUN_OK); fipsd_runtime_destroy(rt);
    fipsd_control_service_clear(&service); CHECK(lstat(path,&st)==0 && S_ISREG(st.st_mode));
    CHECK(unlink(path)==0);
    go_swap_free(state);
    /* A later replacement is not ours to unlink. */
    int file=creat(path,0600); CHECK(file>=0 && close(file)==0);
    service.bound=true; fipsd_control_service_clear(&service); CHECK(stat(path,&st)==0 && S_ISREG(st.st_mode));
    CHECK(unlink(path)==0);
    go_swap_free(configs); CHECK(rmdir(parent)==0); CHECK(rmdir(xdg)==0);
}
int main(void)
{
    char tmp[4096]; snprintf(tmp,sizeof(tmp),"%s/control-XXXXXX",FIPSD_CONTROL_TMP); CHECK(mkdtemp(tmp));
    test_paths(tmp); test_validation(); test_render_failures(); test_query_goldens(); test_probe_golden(); test_path_requests();
    CHECK(golden_count==24); printf("goldens: %u/24 (22 responses/compositions, 1 report, 1 request catalog)\n",golden_count);
    test_socket(tmp); CHECK(rmdir(tmp)==0);
    puts("control: protocol bounds, JSON errors, EOF, one response, fiber concurrency, deadlines, paths and permissions passed");
    return 0;
}
