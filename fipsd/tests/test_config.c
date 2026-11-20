#define _POSIX_C_SOURCE 200809L
#include "test_tmpdir.h"
#include "config/config.h"
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <go.h>
#define CHECK(e) do { if (!(e)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#e); return 1; } } while (0)
#define GET(c,p) fipsd_config_get(c,p)
#define UINT(c,p) fipsd_config_uint(GET(c,p))
static fipsd_config *parse(const char *s,fipsd_config_log *log)
{ return fipsd_config_parse(s,strlen(s),log); }
static bool logged(const fipsd_config_log *log,const char *text)
{
    for (size_t i=0;i<log->count;++i) if (strstr(fipsd_config_log_message(log,i),text)) return true;
    return false;
}
static int defaults(void)
{
    fipsd_config_log log={0};
    fipsd_config *c=parse("{}",&log);
    CHECK(c && !log.error[0]);
    CHECK(UINT(c,"node.limits.max_connections")==256 && UINT(c,"node.limits.max_sessions")==1024);
    CHECK(UINT(c,"node.rekey.after_secs")==120 && UINT(c,"node.rekey.after_messages")==65536);
    CHECK(UINT(c,"node.cache.coord_ttl_secs")==300 && UINT(c,"node.cache.coord_size")==50000);
    CHECK(UINT(c,"node.mmp.owd_window_size")==32 && UINT(c,"node.session_mmp.owd_window_size")==32);
    CHECK(UINT(c,"node.lookup.attempt_timeouts_secs.3")==8);
    CHECK(logged(&log,"applied default key=node.rekey.after_secs value=120"));
    fipsd_config_core core;
    fipsd_config_core_params(c,&core);
    CHECK(core.link_rekey.after.secs==120 && core.link_rekey.messages==65536);
    CHECK(core.link_rekey.handshake.secs==30 && core.session_rekey.handshake.secs==30);
    CHECK(core.coord_ttl_ms==300000 && core.mmp_mode==FIPS_MMP_Full);
    CHECK(core.established.burst==128 && fabs(core.established.rate-6.4)<1e-12);
    CHECK(core.session_setup.burst==64 && core.session_setup.rate==16);
    fips_rekey_params params;
    CHECK(fips_rekey_params_new(core.link_rekey,&params)==FIPS_REKEY_PARAMS_OK);
    CHECK(fips_rekey_params_new(core.session_rekey,&params)==FIPS_REKEY_PARAMS_OK);
    fipsd_config_free(c); fipsd_config_log_clear(&log);
    return 0;
}
static int floors(void)
{
    const struct { const char *yaml,*key,*warning; uint64_t value; } cases[]={
        {"node: {link_dead_timeout_secs: 0}","node.link_dead_timeout_secs","below the 30 s",30},
        {"node: {cache: {coord_ttl_secs: 1}}","node.cache.coord_ttl_secs","below the 300 s",300},
        {"node: {bloom: {update_debounce_ms: 1}}","node.bloom.update_debounce_ms","below the 500 ms",500},
        {"node: {rendezvous: {nostr: {signal_ttl_secs: 0}}}","node.rendezvous.nostr.signal_ttl_secs","below the 120 s",120},
        {"node: {rendezvous: {nostr: {attempt_timeout_secs: 1}}}","node.rendezvous.nostr.attempt_timeout_secs","below the 10 s",10},
        {"node: {rendezvous: {nostr: {max_concurrent_incoming_offers: 0}}}","node.rendezvous.nostr.max_concurrent_incoming_offers","below the 16 traversal",16},
        {"node: {rendezvous: {nostr: {max_concurrent_offers_per_npub: 0}}}","node.rendezvous.nostr.max_concurrent_offers_per_npub","below the 4 traversal",4},
        {"node: {mmp: {owd_window_size: 0}}","node.mmp.owd_window_size","below the 2 samples",2},
        {"node: {mmp: {owd_window_size: 4097}}","node.mmp.owd_window_size","above the 4096-sample",4096},
        {"node: {session_mmp: {owd_window_size: 1}}","node.session_mmp.owd_window_size","below the 2 samples",2},
        {"node: {session_mmp: {owd_window_size: 9999}}","node.session_mmp.owd_window_size","above the 4096-sample",4096},
        {"node: {log_max_size_mb: 0}","node.log_max_size_mb","below 1 MiB",1},
        {"node: {log_max_files: 0}","node.log_max_files","below 1",1},
        {"node: {log_max_files: 101}","node.log_max_files","above 100",100},
        {"dns: {ttl: 4294967295}","dns.ttl","dns.ttl is above 2147483647",2147483647}
    };
    for (size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        fipsd_config_log log={0}; fipsd_config *c=parse(cases[i].yaml,&log);
        if (!c) fprintf(stderr,"%s\n",log.error);
        CHECK(c && UINT(c,cases[i].key)==cases[i].value);
        CHECK(logged(&log,cases[i].warning) && logged(&log,cases[i].key));
        fipsd_config_free(c); fipsd_config_log_clear(&log);
    }
    const char *fprs[]={"0.19","1.0",".nan",".inf","-.inf"};
    for (size_t i=0;i<5;++i) {
        char yaml[256]; snprintf(yaml,sizeof(yaml),"node: {bloom: {max_inbound_fpr: %s}}",fprs[i]);
        fipsd_config_log log={0}; fipsd_config *c=parse(yaml,&log);
        CHECK(c && fipsd_config_number(GET(c,"node.bloom.max_inbound_fpr"))==0.2);
        CHECK(logged(&log,"outside the [0.20, 1.0) range"));
        fipsd_config_free(c); fipsd_config_log_clear(&log);
    }
    fipsd_config_log log={0};
    fipsd_config *c=parse("node: {lookup: {attempt_timeouts_secs: []}, rate_limit: {handshake_max_resends: 9, handshake_timeout_secs: 60}, cache: {coord_ttl_secs: 1, test_coord_ttl_secs: 2}}",&log);
    CHECK(c && logged(&log,"configured lookup ladder is empty") && UINT(c,"node.lookup.attempt_timeouts_secs.3")==8);
    CHECK(UINT(c,"node.rate_limit.handshake_max_resends")==9 && UINT(c,"node.rate_limit.handshake_timeout_secs")==60);
    CHECK(logged(&log,"those handshakes resend at most 5 times") && logged(&log,"link rekey attempts end within 30 s"));
    fipsd_config_core core; fipsd_config_core_params(c,&core);
#ifdef FIPSD_TEST_OVERRIDES
    CHECK(core.coord_ttl_ms==2000 && logged(&log,"test-only key honoured"));
#else
    CHECK(core.coord_ttl_ms==300000 && logged(&log,"test-only key, honoured only"));
#endif
    CHECK(core.link_rekey.handshake.secs==30 && core.session_rekey.handshake.secs==60);
    fipsd_config_free(c); fipsd_config_log_clear(&log);
    char ladder[2048]="node: {lookup: {attempt_timeouts_secs: [";
    for (size_t i=0;i<260;++i) strcat(ladder,i ? ",1" : "1");
    strcat(ladder,"]}}"); c=parse(ladder,&log);
    CHECK(c && fipsd_config_count(GET(c,"node.lookup.attempt_timeouts_secs"))==255 && logged(&log,"running its first 255"));
    fipsd_config_free(c); fipsd_config_log_clear(&log);
    return 0;
}
static int invalid(void)
{
    const struct { const char *yaml,*error; } cases[]={
        {"node: {path: {probe: {active_ms: 0}}}","invalid configuration: `node.path.probe.active_ms` must be greater than zero"},
        {"node: {path: {min_samples: 256}}","invalid configuration: `node.path.min_samples` = 256 must be at most 255"},
        {"node: {native_api: {grants: [{uid: 1,gid: 2, keys: true}]}}","invalid configuration: node.native_api.grants[0] must name exactly one of `uid` and `gid`: a grant is for one principal"},
        {"node: {native_api: {grants: [{uid: 1}]}}","invalid configuration: node.native_api.grants[0] grants nothing: it names no port and neither `keys` nor `learn`"},
        {"node: {buffers: {host_channel: 0}}","invalid configuration: node.buffers.host_channel is 0 but must be at least 1: a queue toward the host that holds nothing drops every packet its reader is not already waiting for"},
        {"node: {rekey: {after_messages: 0}}","invalid configuration: `node.rekey.after_messages` must be at least 1; 0 fires the message-count trigger on every poll instead of disabling it. Use a very large value to effectively disable the message-count trigger."},
        {"transports: {udp: {advertise_on_nostr: true}}","invalid configuration: at least one transport has `advertise_on_nostr = true`, but `node.rendezvous.nostr.enabled` is false"},
        {"peers: [{npub: test, via_nostr: true}]","invalid configuration: at least one peer has `via_nostr = true`, but `node.rendezvous.nostr.enabled` is false"},
        {"peers: [{npub: test}]","invalid configuration: peers[0] (test): must specify at least one address, or set `via_nostr = true` to resolve endpoints from the Nostr advert"},
        {"transports: {ethernet: {interface: ''}}","invalid configuration: transport `ethernet` has an empty `interface`"},
        {"transports: {ethernet: {a: {interface: eth0}, b: {interface: eth0}}}","invalid configuration: transports `a` and `b` both bind interface `eth0`"},
        {"node: {session: {default_ttl: 256}}","node.session.default_ttl: invalid value, expected unsigned integer in 0..255"},
        {"node: {native_api: {enabled: yes}}","node.native_api.enabled: invalid type, expected a boolean"},
        {"node: {limits: {max_links: -1}}","node.limits.max_links: invalid value, expected unsigned integer in 0..18446744073709551615"}
    };
    for (size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        fipsd_config_log log={0}; fipsd_config *c=parse(cases[i].yaml,&log);
        if (c || strcmp(log.error,cases[i].error)) fprintf(stderr,"case %zu: %s\n",i,log.error);
        CHECK(!c && !strcmp(log.error,cases[i].error)); fipsd_config_log_clear(&log);
    }
    const char *extra_invalid[]={
        "node: {path: {switch: {margin: .nan}}}",
        "node: {path: {switch: {margin: 0.9}}}",
        "node: {netmon: {poll_interval_secs: 0}}",
        "node: {netmon: {debounce_ms: 3750}}",
        "node: {native_api: {backlog: 0}}",
        "node: {native_api: {max_flows: 0}}",
        "node: {native_api: {pending_per_flow: 0}}",
        "node: {native_api: {pending_per_flow: 65}}",
        "node: {native_api: {grants: [{uid: 1, ports: [255]}]}}",
        "node: {native_api: {grants: [{uid: 1, ports: [257, 257]}]}}",
        "tun: {enabled: true}\nnode: {native_api: {grants: [{gid: 1, ports: [256]}]}}",
        "node: {rekey: {after_secs: 15}}",
        "node: {rate_limit: {established_handshake_burst: 0}}",
        "node: {rate_limit: {established_handshake_rate: .nan}}",
        "node: {rate_limit: {session_setup_burst: 0}}",
        "node: {rate_limit: {session_setup_rate: .inf}}",
        "node: {rendezvous: {nostr: {signal_ttl_secs: 120, replay_window_secs: 240}}}",
        "node: {rendezvous: {nostr: {enabled: true, dm_relays: []}}}\ntransports: {udp: {advertise_on_nostr: true}}",
        "node: {rendezvous: {nostr: {enabled: true, stun_servers: []}}}\ntransports: {udp: {advertise_on_nostr: true}}",
        "transports: {ethernet: {interface: abcdefghijklmnop}}",
        "transports: {udp: {interface: 'bad/name'}}",
        "transports: {udp: {bind_addr: '127.0.0.1:2121'}}\npeers: [{npub: test, addresses: [{transport: udp, addr: '192.0.2.1:2121'}]}]",
        "transports: {udp: {bind_addr: '0.0.0.0:2121'}}\npeers: [{npub: test, addresses: [{transport: udp/missing, addr: '192.0.2.1:2121'}]}]",
        "peers: [{npub: test, addresses: [{transport: tcp/backup, addr: '192.0.2.1:2121'}]}]",
        "gateway: {pool: 'fd01::/112', lan_interface: eth0, port_forwards: [{listen_port: 0, proto: tcp, target: '[fd01::1]:80'}]}",
        "gateway: {pool: 'fd01::/112', lan_interface: eth0, port_forwards: [{listen_port: 80, proto: tcp, target: '127.0.0.1:80'}]}",
        "gateway: {pool: 'fd01::/112', lan_interface: eth0, port_forwards: [{listen_port: 80, proto: tcp, target: '[fd01::1]:80'},{listen_port: 80, proto: tcp, target: '[fd01::2]:80'}]}",
        "node: {session: {default_ttl: '64'}}",
        "transports: {tor: {directory_service: {}, advertise_on_nostr: true}}",
        "---\nnode: {}\n---\n...\n---\nnode: [broken",
        "node: {log_file: null, identity: {nsec: \"secret\\0tail\"}}"
    };
    for (size_t i=0;i<sizeof(extra_invalid)/sizeof(extra_invalid[0]);++i) {
        fipsd_config_log log={0}; fipsd_config *c=parse(extra_invalid[i],&log);
        if (c) fprintf(stderr,"unexpected valid case %zu: %s\n",i,extra_invalid[i]);
        CHECK(!c && log.error[0]); fipsd_config_log_clear(&log);
    }
    fipsd_config_log log={0};
    fipsd_config *c=parse("node: {typo: 'SECRET', rekey: {enabled: false}, tick_interval_secs: 5, discovery: {ttl: 32}}",&log);
    CHECK(c && UINT(c,"node.lookup.ttl")==32 && logged(&log,"unknown configuration key `node.typo`"));
    CHECK(logged(&log,"`node.rekey.enabled: false` is removed and ignored") && logged(&log,"`node.tick_interval_secs: 5` is removed"));
    CHECK(!logged(&log,"SECRET") && !GET(c,"node.typo"));
    fipsd_config_free(c); fipsd_config_log_clear(&log);
    c=parse("transports: {udp: {bind_addr: {mtu: 1500}}}\npeers: [{npub: test, addresses: [{transport: udp/bind_addr, addr: '192.0.2.1:2121'}]}]",&log);
    CHECK(c && UINT(c,"transports.udp.bind_addr.mtu")==1500);
    fipsd_config_free(c); fipsd_config_log_clear(&log);
    c=parse("transports: {ethernet: {interface: eth0, discovery: false}}\nnode: {log_file: null}",&log);
    CHECK(c && !fipsd_config_bool(GET(c,"transports.ethernet.listen")) && !GET(c,"node.log_file"));
    fipsd_config_free(c); fipsd_config_log_clear(&log);
    return 0;
}
static int write_config(const char *path,const char *text)
{
    FILE *f=fopen(path,"w");
    if (!f) return -1;
    int rc=fputs(text,f)==EOF ? -1 : 0;
    if (fclose(f)!=0) rc=-1;
    return rc;
}
typedef struct readers { GoSwap *slot; _Atomic bool done; _Atomic unsigned seen; } readers;
static void *reader(void *arg)
{
    readers *r=arg;
    do {
        GoRefPtr ref=go_swap_load(r->slot);
        const fipsd_config *c=ref.ptr;
        if (!c || UINT(c,"node.limits.max_peers")!=128) abort();
        atomic_fetch_add(&r->seen,1); go_refptr_release(&ref);
    } while (!atomic_load(&r->done));
    return NULL;
}
static int reload(void)
{
    const char *dir=fipsd_test_tmpdir(NULL);
    char path[4096]; snprintf(path,sizeof(path),"%s/reload.yaml",dir);
    const char *initial="node: {identity: {persistent: true}}";
    CHECK(write_config(path,initial)==0);
    fipsd_config_log log={0}; fipsd_config *c=fipsd_config_load(path,&log); CHECK(c);
    GoSwap *slot=go_swap_create(c,fipsd_config_free); CHECK(slot);
    GoRefPtr retained=go_swap_load(slot);
    readers r={.slot=slot}; pthread_t threads[3];
    for (size_t i=0;i<3;++i) CHECK(pthread_create(&threads[i],NULL,reader,&r)==0);
    CHECK(write_config(path,"node: {identity: {persistent: true}, log_level: debug, log_max_files: 7, limits: {max_peers: 999}}") == 0);
    CHECK(fipsd_config_reload(slot,path,&log));
    CHECK(logged(&log,"reloaded configuration key=node.log_level"));
    CHECK(logged(&log,"requires restart; keeping active value key=node.limits.max_peers"));
    GoRefPtr ref=go_swap_load(slot); c=ref.ptr;
    CHECK(UINT(c,"node.log_max_files")==7 && UINT(c,"node.limits.max_peers")==128);
    CHECK(UINT(retained.ptr,"node.log_max_files")==4);
    go_refptr_release(&ref);
    CHECK(write_config(path,"node: [") == 0);
    fipsd_config_log_clear(&log);
    CHECK(!fipsd_config_reload(slot,path,&log) && log.error[0]);
    ref=go_swap_load(slot); CHECK(UINT(ref.ptr,"node.log_max_files")==7); go_refptr_release(&ref);
    atomic_store(&r.done,true);
    for (size_t i=0;i<3;++i) CHECK(pthread_join(threads[i],NULL)==0);
    CHECK(atomic_load(&r.seen)>0);
    go_swap_free(slot); CHECK(UINT(retained.ptr,"node.log_max_files")==4); go_refptr_release(&retained);
    fipsd_config_log_clear(&log); CHECK(unlink(path)==0);
    return 0;
}
static int layers(void)
{
    size_t count=0; char **search=fipsd_config_search_paths(&count);
    CHECK(search && count>=3 && !strcmp(search[0],"/etc/fips/fips.yaml"));
#if defined(__APPLE__) || defined(__FreeBSD__)
    CHECK(!strcmp(search[1],"/usr/local/etc/fips/fips.yaml"));
#endif
    CHECK(strstr(search[count-1],"/fips.yaml"));
    for (size_t i=0;i<count;++i) CHECK(search[i][0]=='/');
    fipsd_config_search_paths_free(search,count);
    const char *dir=fipsd_test_tmpdir(NULL);
    char a[4096], b[4096];
    snprintf(a,sizeof(a),"%s/layer-a.yaml",dir);
    snprintf(b,sizeof(b),"%s/layer-b.yaml",dir);
    CHECK(write_config(a,"node: {identity: {persistent: true}, leaf_only: true, limits: {max_peers: 512}, log_level: trace}\ntransports: {udp: {mtu: 1472}}\ndns: {enabled: false}")==0);
    CHECK(write_config(b,"node: {leaf_only: false, limits: {max_links: 512}, log_level: info}\ntransports: {udp: {bind_addr: '127.0.0.1:2'}}\ndns: {port: 1234}")==0);
    const char *paths[]={a,b};
    fipsd_config_log log={0};
    fipsd_config *c=fipsd_config_load_paths(paths,2,&log);
    CHECK(c && UINT(c,"node.limits.max_peers")==512 && UINT(c,"node.limits.max_links")==512);
    CHECK(!strcmp(fipsd_config_string(GET(c,"node.log_level")),"info"));
    CHECK(fipsd_config_bool(GET(c,"node.leaf_only")) && !fipsd_config_bool(GET(c,"dns.enabled")));
    CHECK(UINT(c,"dns.port")==1234 && UINT(c,"transports.udp.mtu")==1357);
    fipsd_config_free(c); fipsd_config_log_clear(&log);
    CHECK(write_config(a,"# deliberately blank\n")==0);
    c=fipsd_config_load(a,&log); CHECK(!c && strstr(log.error,"configures nothing"));
    fipsd_config_log_clear(&log);
    c=fipsd_config_load_paths(paths,2,&log); CHECK(c); /* Other file configures UDP. */
    fipsd_config_free(c); fipsd_config_log_clear(&log);
    CHECK(write_config(a,"node: {identity: {persistent: true}}\n")==0);
    CHECK(write_config(b,"node: {cache: {coord_ttl_secs: bad}}\n")==0);
    c=fipsd_config_load_paths(paths,2,&log); CHECK(!c && strstr(log.error,"expected unsigned integer"));
    fipsd_config_log_clear(&log);
    CHECK(unlink(a)==0 && unlink(b)==0);
    c=fipsd_config_load(a,&log); CHECK(!c && strstr(log.error,"failed to read config file"));
    fipsd_config_log_clear(&log);
    c=fipsd_config_load_paths(paths,2,&log); CHECK(c); /* No files: built-ins permitted. */
    fipsd_config_free(c); fipsd_config_log_clear(&log);
    return 0;
}
static char *read_all(const char *path,size_t *len)
{
    FILE *f=fopen(path,"rb"); if (!f) return NULL;
    if (fseek(f,0,SEEK_END)!=0) { fclose(f); return NULL; }
    long size=ftell(f); if (size<0) { fclose(f); return NULL; }
    rewind(f); char *s=malloc((size_t)size+1);
    if (!s) { fclose(f); return NULL; }
    *len=fread(s,1,(size_t)size,f); s[*len]=0; fclose(f); return s;
}
static int fixture(const char *input,const char *golden)
{
    size_t len; char *yaml=read_all(input,&len); CHECK(yaml);
    fipsd_config_log log={0}; fipsd_config *c=fipsd_config_parse(yaml,len,&log); free(yaml);
    fipsd_config_log file_log={0};
    fipsd_config *loaded=fipsd_config_load(input,&file_log);
    if (loaded) { CHECK(c); fipsd_config_free(c); c=loaded; }
    else if (c) CHECK(strstr(file_log.error,"configures nothing"));
    else CHECK(file_log.error[0]);
    fipsd_config_log_clear(&file_log);
    FILE *out=tmpfile(); CHECK(out);
    if (c) CHECK(fipsd_config_dump(c,out)==0);
    else CHECK(fputs("REJECTED\n",out)!=EOF);
    rewind(out);
    size_t glen; char *expected=read_all(golden,&glen); CHECK(expected);
    (void)glen;
    char line[8192]; char *save=NULL;
    for (char *want=strtok_r(expected,"\n",&save);want;want=strtok_r(NULL,"\n",&save)) {
        CHECK(fgets(line,sizeof(line),out)); line[strcspn(line,"\n")]=0;
        /* Default sockets depend on host directories/uid, not YAML. Their
         * resolution is tested separately. Other values compare bytewise. */
        char *auto_value=strstr(want,"=@auto@");
        if (auto_value) { *auto_value=0; CHECK(!strncmp(line,want,strlen(want)) && line[strlen(want)]=='='); }
        else if (strcmp(line,want)) { fprintf(stderr,"fixture %s\nwant %s\ngot  %s\n",input,want,line); return 1; }
    }
    CHECK(!fgets(line,sizeof(line),out));
    fclose(out); free(expected); fipsd_config_free(c); fipsd_config_log_clear(&log);
    return 0;
}
static int corpus(const char *path)
{
    size_t len; char *data=read_all(path,&len); CHECK(data);
    /* Replay each seed and all its prefixes, including embedded NUL input.
     * Valid partial documents may succeed; every outcome must free cleanly. */
    for (size_t i=0;i<=len;++i) {
        fipsd_config_log log={0}; fipsd_config *c=fipsd_config_parse(data,i,&log);
        fipsd_config_free(c); fipsd_config_log_clear(&log);
    }
    free(data); return 0;
}
/* A shipped example must load through the daemon's own loader without a
 * warning both as shipped and with every commented-out setting enabled, so
 * the schema its comments suggest is the one the daemon accepts (md-uzf). */
static size_t lead(const char *line) { return strspn(line," "); }
/* Length of "key:" when @s starts with a lowercase key followed by ':' and a
 * space or the end of the line, else 0. */
static size_t key_len(const char *s)
{
    size_t n=strspn(s,"abcdefghijklmnopqrstuvwxyz_");
    return n && s[n]==':' && (s[n+1]==' ' || !s[n+1]) ? n+1 : 0;
}
/* The line with its comment marker removed when it comments out a setting
 * ("# key: ...", "#   key:", "#   - item"); NULL for prose and live lines. */
static char *uncomment(const char *line)
{
    size_t at=lead(line);
    if (line[at]!='#') return NULL;
    const char *body=line+at+1; if (*body==' ') ++body;
    const char *item=body+lead(body);
    if (!key_len(item) && strncmp(item,"- ",2)) return NULL;
    size_t n=strlen(body); char *out=malloc(at+n+1); if (!out) return NULL;
    memset(out,' ',at); memcpy(out+at,body,n+1); return out;
}
static int load_clean(const char *path,const char *what)
{
    fipsd_config_log log={0};
    fipsd_config *c=fipsd_config_load(path,&log);
    if (!c) fprintf(stderr,"%s: %s\n",what,log.error);
    for (size_t i=0;i<log.count;++i) if (fipsd_config_log_level(&log,i)<=FIPSD_LOG_WARN)
        fprintf(stderr,"%s: %s\n",what,fipsd_config_log_message(&log,i));
    bool clean=c!=NULL;
    for (size_t i=0;i<log.count;++i) clean=clean && fipsd_config_log_level(&log,i)>FIPSD_LOG_WARN;
    fipsd_config_free(c); fipsd_config_log_clear(&log);
    CHECK(clean);
    return 0;
}
static int example(const char *path)
{
    CHECK(load_clean(path,"as shipped")==0);
    size_t len; char *text=read_all(path,&len); CHECK(text);
    size_t count=1; for (const char *p=text;*p;++p) count+=*p=='\n';
    char **lines=calloc(count,sizeof(*lines)); bool *on=calloc(count,sizeof(*on));
    CHECK(lines && on);
    size_t n=0, enabled=0;
    for (char *save=NULL,*l=strtok_r(text,"\n",&save);l;l=strtok_r(NULL,"\n",&save),++n) {
        char *u=uncomment(l);
        lines[n]=u ? u : strdup(l); on[n]=u!=NULL; enabled+=on[n];
        CHECK(lines[n]);
    }
    CHECK(enabled>0);
    char enabled_path[4096];
    snprintf(enabled_path,sizeof(enabled_path),"%s/fips.yaml",fipsd_test_tmpdir(NULL));
    FILE *out=fopen(enabled_path,"w"); CHECK(out);
    for (size_t i=0;i<n;++i) {
        /* A live empty placeholder ("peers: []") yields to the enabled
         * example of the same key at the same indentation. */
        size_t at=lead(lines[i]), k=on[i] ? 0 : key_len(lines[i]+at);
        const char *v=k ? lines[i]+at+k : NULL;
        bool replaced=false;
        if (v && (!strcmp(v," []") || !strcmp(v," {}")))
            for (size_t j=0;j<n && !replaced;++j)
                replaced=on[j] && lead(lines[j])==at && !strncmp(lines[j]+at,lines[i]+at,k);
        if (!replaced) CHECK(fprintf(out,"%s\n",lines[i])>=0);
        free(lines[i]);
    }
    CHECK(fclose(out)==0);
    free(lines); free(on); free(text);
    return load_clean(enabled_path,"commented settings enabled");
}
static int alias_bytes(void)
{
    size_t scalar=128u*1024u, cap=scalar+1024;
    char *yaml=malloc(cap); CHECK(yaml);
    size_t n=(size_t)snprintf(yaml,cap,"anchor: &a '");
    memset(yaml+n,'s',scalar); n+=scalar;
    n+=(size_t)snprintf(yaml+n,cap-n,"'\nitems: [");
    for (size_t i=0;i<65;++i) n+=(size_t)snprintf(yaml+n,cap-n,"%s*a",i ? "," : "");
    n+=(size_t)snprintf(yaml+n,cap-n,"]\n"); CHECK(n<cap);
    fipsd_config_log log={0};
    fipsd_config *c=fipsd_config_parse(yaml,n,&log);
    CHECK(!c && strstr(log.error,"expanded scalar limit"));
    fipsd_config_log_clear(&log); free(yaml);
    cap=150000; yaml=malloc(cap); CHECK(yaml); n=0;
    for (unsigned i=0;i<5000;++i)
        n+=(size_t)snprintf(yaml+n,cap-n,"unknown_%u: x\n",i);
    CHECK(n<cap);
    c=fipsd_config_parse(yaml,n,&log);
    CHECK(!c && log.count==4096 && strstr(log.error,"diagnostic limit"));
    fipsd_config_log_clear(&log); free(yaml); return 0;
}
int main(int argc,char **argv)
{
    if (argc==4 && !strcmp(argv[1],"fixture")) return fixture(argv[2],argv[3]);
    if (argc==3 && !strcmp(argv[1],"corpus")) return corpus(argv[2]);
    if (argc==3 && !strcmp(argv[1],"example")) return example(argv[2]);
    if (argc!=1) return 2;
    CHECK(alias_bytes()==0);
    CHECK(defaults()==0); CHECK(floors()==0); CHECK(invalid()==0); CHECK(reload()==0); CHECK(layers()==0);
    fipsd_config *c=fipsd_config_parse("{}",2,NULL); CHECK(c); fipsd_config_free(c);
    return 0;
}
