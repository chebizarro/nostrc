#define _POSIX_C_SOURCE 200809L
#include "nostr/stun.h"
#include "nostr/traversal_machine.h"
#include "nostr/runtime.h"
#include "nostr/traversal_io.h"
#include "host_os.h"
#include "nostr_relay_fixture.h"
#include "stun_fixture.h"
#include "test_tmpdir.h"
#include "transport/stats_common.h"
#include <select.h>
#include <sodium.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void *receive_at(GoChannel *c, const char *caller, unsigned line)
{
    void *p = NULL; GoSelectCase cs = {.op = GO_SELECT_RECEIVE, .chan = c, .recv_buf = &p};
    GoSelectResult r = go_select_timeout(&cs, 1, 20000);
    if (r.selected_case != 0 || !p)
        fprintf(stderr, "channel receive failed at %s:%u (selected=%d, value=%p)\n", caller, line, r.selected_case, p);
    assert(r.selected_case == 0 && p); return p;
}
#define receive(c) receive_at((c), __func__, __LINE__)
static fips_transport_addr addr(const char *s)
{ fips_transport_addr a; assert(fips_transport_addr_from_string(s, strlen(s), &a) == FIPS_OK); return a; }
static void codecs(void)
{
    static const uint8_t id[12] = {1,2,3,4,5,6,7,8,9,10,11,12};
    static const uint8_t request[20] = {0,1,0,0,0x21,0x12,0xa4,0x42,1,2,3,4,5,6,7,8,9,10,11,12};
    uint8_t out[20]; fipsd_stun_request(id, out); assert(!memcmp(out, request, 20));
    /* Upstream tests/stun.rs: 192.0.2.1:1234, XOR-MAPPED, fixed txn. */
    uint8_t response[48] = {1,1,0,12,0x21,0x12,0xa4,0x42,1,2,3,4,5,6,7,8,9,10,11,12,
        0,0x20,0,8,0,1,0x25,0xc0,0xe1,0x12,0xa6,0x43};
    fips_transport_addr mapped, want = addr("192.0.2.1:1234");
    assert(fipsd_stun_response(response, 32, id, &mapped) && fips_transport_addr_equal(&mapped, &want));
    for (size_t n = 0; n < 32; ++n) assert(!fipsd_stun_response(response, n, id, &mapped));
    response[8] ^= 1; assert(!fipsd_stun_response(response, 32, id, &mapped)); response[8] ^= 1;
    response[3] = 8; assert(!fipsd_stun_response(response, 32, id, &mapped)); response[3] = 12;
    response[23] = 255; assert(!fipsd_stun_response(response, 32, id, &mapped)); response[23] = 8;
    /* Unknown padded attribute before mapped address. */
    memmove(response + 28, response + 20, 12); memset(response + 20, 0, 8);
    response[21] = 0xff; response[23] = 1; response[24] = 7; response[3] = 20;
    assert(fipsd_stun_response(response, 40, id, &mapped) && fips_transport_addr_equal(&mapped, &want));
    static const uint8_t ipv6[44] = {1,1,0,24,0x21,0x12,0xa4,0x42,1,2,3,4,5,6,7,8,9,10,11,12,
        0,0x20,0,20,0,2,0x25,0xc0,0x01,0x13,0xa9,0xfa,1,2,3,4,5,6,7,8,9,10,11,13};
    want = addr("[2001:db8::1]:1234");
    assert(fipsd_stun_response(ipv6, sizeof ipv6, id, &mapped) && fips_transport_addr_equal(&mapped, &want));
    memcpy(response, ipv6, sizeof ipv6); response[21] = 1; response[26] = 4; response[27] = 0xd2;
    memset(response + 28, 0, 16); response[28] = 0x20; response[29] = 1; response[30] = 0xd; response[31] = 0xb8; response[43] = 1;
    assert(fipsd_stun_response(response, 44, id, &mapped) && fips_transport_addr_equal(&mapped, &want));
    assert(fipsd_stun_server("stun:[::1]:3478", &mapped));
    assert(!fipsd_stun_server("stun:example:65536", &mapped)); assert(!fipsd_stun_server("stun::0", &mapped));
    uint8_t punch[24]; fipsd_punch_packet parsed;
    fipsd_punch_build(false, 42, "session-classify-vectors", punch);
    /* SHA-256 first 16 bytes independently fixed below, not a round-trip oracle. */
    static const uint8_t hash[16] = {0x85,0xad,0x23,0x1f,0x2c,0xff,0x7f,0x8b,0xe0,0xe1,0x7f,0x4f,0x7c,0x6c,0xfc,0x53};
    assert(!memcmp(punch, "NPTC\0\0\0\x2a", 8) && !memcmp(punch + 8, hash, 16));
    assert(fipsd_punch_parse(punch, sizeof punch, &parsed) && !parsed.ack && parsed.sequence == 42);
    assert(!fipsd_punch_parse(punch, 23, &parsed));
    fipsd_punch_build(true, 3, "session-classify-vectors", punch);
    assert(fipsd_punch_parse(punch, 24, &parsed) && parsed.ack && parsed.sequence == 3);
    assert(fipsd_udp_classify(NULL, punch, 24) == FIPSD_UDP_DISCARD);
    assert(fipsd_udp_classify(NULL, (const uint8_t *)"NPTC", 4) == FIPSD_UDP_DISCARD);
    uint8_t fmp[24] = {0x10}; assert(fipsd_udp_classify(NULL, fmp, sizeof fmp) == FIPSD_UDP_FMP);
    fipsd_stun s; want = addr("127.0.0.1:3478"); fipsd_stun_begin(&s, &want, id, 100, 2000);
    assert(fipsd_stun_tick(&s, 100) == FIPSD_STUN_SEND && fipsd_stun_deadline(&s) == 600);
    assert(fipsd_stun_tick(&s, 599) == FIPSD_STUN_WAIT && fipsd_stun_tick(&s, 600) == FIPSD_STUN_SEND);
    assert(fipsd_stun_tick(&s, 1600) == FIPSD_STUN_SEND && fipsd_stun_deadline(&s) == 2100);
    assert(fipsd_stun_tick(&s, 2100) == FIPSD_STUN_FAILED);
    puts("STUN IPv4/IPv6 MAPPED/XOR, truncation/txn/length/padding; NPTC/NPTA and FMP vectors: OK");
}
static void states(void)
{
    fipsd_tr_config c; fipsd_tr_config_default(&c);
    struct row { fipsd_tr_state from; fipsd_tr_input in; uint64_t at; fipsd_tr_state to; unsigned effects; } rows[] = {
        {FIPSD_TR_IDLE,{.event=FIPSD_TR_OFFER,.initiator=true},0,FIPSD_TR_ANSWER,FIPSD_TR_SEND_OFFER},
        {FIPSD_TR_IDLE,{.event=FIPSD_TR_OFFER},0,FIPSD_TR_PUNCHING,FIPSD_TR_SEND_ANSWER},
        {FIPSD_TR_ANSWER,{.event=FIPSD_TR_ANSWERED,.accepted=true},0,FIPSD_TR_PUNCHING,0},
        {FIPSD_TR_ANSWER,{.event=FIPSD_TR_ANSWERED},0,FIPSD_TR_COOLDOWN,FIPSD_TR_FAILED|FIPSD_TR_FINISHED},
        {FIPSD_TR_ANSWER,{.event=FIPSD_TR_TIMEOUT},100,FIPSD_TR_COOLDOWN,FIPSD_TR_FAILED|FIPSD_TR_FINISHED},
        {FIPSD_TR_PUNCHING,{.event=FIPSD_TR_PUNCH,.rank=2,.probe=true},0,FIPSD_TR_ESTABLISHED,FIPSD_TR_HANDOFF|FIPSD_TR_SEND_ACK},
        {FIPSD_TR_SETTLING,{.event=FIPSD_TR_PUNCH,.rank=2},0,FIPSD_TR_ESTABLISHED,FIPSD_TR_HANDOFF},
        {FIPSD_TR_PUNCHING,{.event=FIPSD_TR_PUNCH,.rank=1,.probe=true},0,FIPSD_TR_SETTLING,FIPSD_TR_SEND_ACK},
        {FIPSD_TR_SETTLING,{.event=FIPSD_TR_PUNCH,.rank=1},0,FIPSD_TR_SETTLING,0},
        {FIPSD_TR_ESTABLISHED,{.event=FIPSD_TR_PUNCH,.rank=2,.probe=true},0,FIPSD_TR_ESTABLISHED,FIPSD_TR_SEND_ACK},
        {FIPSD_TR_SETTLING,{.event=FIPSD_TR_TIMEOUT},50,FIPSD_TR_ESTABLISHED,FIPSD_TR_HANDOFF},
        {FIPSD_TR_PUNCHING,{.event=FIPSD_TR_TIMEOUT},100,FIPSD_TR_COOLDOWN,FIPSD_TR_FAILED|FIPSD_TR_FINISHED},
        {FIPSD_TR_PUNCHING,{.event=FIPSD_TR_TIMEOUT},10,FIPSD_TR_PUNCHING,FIPSD_TR_SEND_PUNCH},
        {FIPSD_TR_SETTLING,{.event=FIPSD_TR_TIMEOUT},10,FIPSD_TR_SETTLING,FIPSD_TR_SEND_PUNCH},
        {FIPSD_TR_ESTABLISHED,{.event=FIPSD_TR_TIMEOUT},100,FIPSD_TR_IDLE,FIPSD_TR_FINISHED},
        {FIPSD_TR_COOLDOWN,{.event=FIPSD_TR_TIMEOUT},100,FIPSD_TR_IDLE,FIPSD_TR_RETRY},
        {FIPSD_TR_PUNCHING,{.event=FIPSD_TR_ANSWERED},0,FIPSD_TR_COOLDOWN,FIPSD_TR_FAILED|FIPSD_TR_FINISHED},
        {FIPSD_TR_SETTLING,{.event=FIPSD_TR_ANSWERED},0,FIPSD_TR_COOLDOWN,FIPSD_TR_FAILED|FIPSD_TR_FINISHED}
    };
    for (size_t i = 0; i < sizeof rows / sizeof *rows; ++i) {
        fipsd_tr_machine m; fipsd_tr_init(&m, &c); m.state = rows[i].from; m.initiator = true;
        m.deadline = 100; m.next_punch = 10; m.send_until = 100; m.settle_at = 50;
        unsigned a = fipsd_tr_step(&m, rows[i].in, rows[i].at); assert(a == rows[i].effects && m.state == rows[i].to);
    }
    /* Every state/event pair is defined: unlisted inputs are inert. */
    for (int state = 0; state < FIPSD_TR_STATES; ++state) for (int event = 0; event < FIPSD_TR_EVENTS; ++event) {
        fipsd_tr_machine m; fipsd_tr_init(&m, &c); m.state = (fipsd_tr_state)state;
        m.deadline = 100; m.next_punch = 10; m.send_until = 100; m.settle_at = 50;
        unsigned a = fipsd_tr_step(&m, (fipsd_tr_input){.event=(fipsd_tr_event)event}, 0);
        if (event == FIPSD_TR_CANCEL && state != FIPSD_TR_CANCELLED) assert(m.state == FIPSD_TR_CANCELLED && a == FIPSD_TR_FINISHED);
        else if (!(state == FIPSD_TR_IDLE && event == FIPSD_TR_OFFER) &&
            !((state == FIPSD_TR_ANSWER || state == FIPSD_TR_PUNCHING || state == FIPSD_TR_SETTLING) && event == FIPSD_TR_ANSWERED)) assert(!a && m.state == (fipsd_tr_state)state);
    }
    fipsd_tr_machine m; fipsd_tr_init(&m, &c);
    for (unsigned i = 0; i < 5; ++i) {
        uint64_t now = i * UINT64_C(2000000);
        assert(fipsd_tr_step(&m,(fipsd_tr_input){.event=FIPSD_TR_OFFER,.initiator=true},now)==FIPSD_TR_SEND_OFFER);
        (void)fipsd_tr_step(&m,(fipsd_tr_input){.event=FIPSD_TR_TIMEOUT},now+c.answer_ms);
        uint64_t wait = i == 4 ? c.cooldown_ms : c.retry_ms << i;
        assert(m.deadline == now+c.answer_ms+wait);
        assert(!fipsd_tr_step(&m,(fipsd_tr_input){.event=FIPSD_TR_TIMEOUT},m.deadline-1));
        assert(fipsd_tr_step(&m,(fipsd_tr_input){.event=FIPSD_TR_TIMEOUT},m.deadline)==FIPSD_TR_RETRY);
    }
    m.state = FIPSD_TR_COOLDOWN; m.attempts = 6;
    assert(!fipsd_tr_step(&m,(fipsd_tr_input){.event=FIPSD_TR_TIMEOUT},m.deadline));
    /* Repair results may follow a valid answer that consumed the actor's
     * reservation. They are nonfatal after punching begins, unlike rejection. */
    fipsd_tr_init(&m, &c);
    (void)fipsd_tr_step(&m, (fipsd_tr_input){.event=FIPSD_TR_OFFER,.initiator=true}, 0);
    (void)fipsd_tr_step(&m, (fipsd_tr_input){.event=FIPSD_TR_ANSWERED,.accepted=true}, 1);
    assert(!fipsd_tr_step(&m, (fipsd_tr_input){.event=FIPSD_TR_ANSWERED,.repair=true}, 2));
    assert(m.state == FIPSD_TR_PUNCHING);
    (void)fipsd_tr_step(&m, (fipsd_tr_input){.event=FIPSD_TR_PUNCH,.rank=1}, 3);
    assert(!fipsd_tr_step(&m, (fipsd_tr_input){.event=FIPSD_TR_ANSWERED,.repair=true}, 4));
    assert(m.state == FIPSD_TR_SETTLING);
    fipsd_tr_init(&m, &c);
    (void)fipsd_tr_step(&m, (fipsd_tr_input){.event=FIPSD_TR_OFFER,.initiator=true}, 0);
    assert(fipsd_tr_step(&m, (fipsd_tr_input){.event=FIPSD_TR_ANSWERED,.repair=true}, 1) ==
        (FIPSD_TR_FAILED | FIPSD_TR_FINISHED));
    assert(m.state == FIPSD_TR_COOLDOWN);
    /* Jump over the last punch send opportunity without expiring the attempt. */
    for (int state = FIPSD_TR_PUNCHING; state <= FIPSD_TR_SETTLING; ++state) {
        fipsd_tr_init(&m, &c); m.state = (fipsd_tr_state)state;
        m.next_punch = 10; m.send_until = 20; m.deadline = 100; m.settle_at = 80;
        assert(!fipsd_tr_step(&m, (fipsd_tr_input){.event=FIPSD_TR_TIMEOUT}, 30));
        assert(m.next_punch == UINT64_MAX);
        assert(fipsd_tr_deadline(&m) == (state == FIPSD_TR_PUNCHING ? 100u : 80u));
    }
    uint8_t a[16] = {1}, b[16] = {2}; assert(fipsd_tr_suppress(a,b,true)); assert(!fipsd_tr_suppress(a,b,false)); assert(!fipsd_tr_suppress(b,a,true));
    puts("all traversal rows, inert inputs, cancellation, remapped settle, bounded retry and 5-failure/1800s cooldown: OK");
}

typedef struct {
    fipsd_nostr_node_driver driver; /* callbacks' user is this first member */
    GoChannel *live, *published, *advertised, *cached, *fetched, *dialed;
    bool (*dial)(void *, const char *, const fipsd_nostr_advert *); /* the production driver's */
    fipsd_config *config; GoSwap *configs; fipsd_node *node; fipsd_node_handlers *handlers;
    fipsd_transport *udp; fipsd_nostr_runtime *relay; fipsd_netmon_source netmon;
} node;
static fipsd_network_snapshot *network(void *arg)
{ (void)arg; fipsd_network_snapshot *s = calloc(1,sizeof *s); assert(s); return s; }
static void live(void *arg, const char *url, bool ready, const char *reason)
{ (void)url; (void)reason; node *n = arg; if (ready) assert(!go_channel_try_send(n->live, n)); }
static void published(void *arg, const char *url, int kind, bool ok)
{
    (void)url; node *n = arg;
    if (kind == NOSTR_KIND_FIPS_TRAVERSAL && ok) assert(!go_channel_try_send(n->published, n));
    if (kind == NOSTR_KIND_FIPS_OVERLAY_ADVERT && ok) assert(!go_channel_try_send(n->advertised, n));
}
/* Observers around the production driver: what it was offered and whether it
 * dialled (true: traversal or a direct connect was queued). */
static void cached(void *arg, const char *key, bool live)
{ (void)key; node *n = arg; if (live) assert(!go_channel_try_send(n->cached, n)); }
static void fetched(void *arg, const char *key, fipsd_nostr_refetch_outcome o)
{ (void)key; node *n = arg; assert(!go_channel_try_send(n->fetched, (void *)(uintptr_t)(o + 1))); }
static bool dialed(void *arg, const char *key, const fipsd_nostr_advert *a)
{
    node *n = arg; bool queued = n->dial(arg, key, a);
    assert(!go_channel_try_send(n->dialed, (void *)(uintptr_t)(queued ? 2 : 1))); return queued;
}
static void init_node(node *n, fipsd_runtime *rt, const char *yaml, bool relay)
{
    *n = (node){0}; n->live = go_channel_create(8); n->published = go_channel_create(16); n->advertised = go_channel_create(16);
    n->cached = go_channel_create(16); n->fetched = go_channel_create(16); n->dialed = go_channel_create(64);
    n->config = fipsd_config_parse(yaml,strlen(yaml),NULL); assert(n->config);
    n->configs = go_swap_create(n->config,fipsd_config_free);
    fipsd_node_handlers_config hc; fipsd_node_handlers_config_default(&hc); hc.peer_capacity=4; hc.notice_capacity=256;
    n->handlers = fipsd_node_handlers_create(&hc); assert(n->handlers);
    n->netmon = (fipsd_netmon_source){.read=network,.injected=true};
    fipsd_node_options options={.workers=2,.peer_capacity=4,.netmon_source=&n->netmon};
    fipsd_node_handlers_install(n->handlers,&options); n->node=fipsd_node_create(n->configs,&options); assert(n->node);
    fipsd_udp_config uc; fipsd_udp_config_default(&uc); uc.bind_addr="127.0.0.1:0";
    n->udp=fipsd_udp_create((fips_transport_id){1},"udp",&uc); assert(n->udp);
    assert(!fipsd_transport_register(fipsd_node_transports(n->node),n->udp));
    assert(!fipsd_runtime_register(rt,fipsd_node_subsystem(n->node)));
    if (relay) {
        fipsd_rendezvous_driver d=fipsd_nostr_node_driver_init(&n->driver,fipsd_node_get_context(n->node),n->handlers,fipsd_node_transports(n->node));
        d.boundary=live; d.publish_result=published;
        n->dial=d.dial_candidate; d.dial_candidate=dialed; d.advert_cached=cached; d.refetch_result=fetched;
        n->relay=fipsd_nostr_runtime_create(n->config,NULL,fipsd_node_get_context(n->node),d); assert(n->relay);
        assert(!fipsd_runtime_register(rt,fipsd_nostr_runtime_subsystem(n->relay)));
    }
}
static void destroy_node(node *n)
{
    fipsd_nostr_runtime_destroy(n->relay); fipsd_node_destroy(n->node); fipsd_node_handlers_destroy(n->handlers);
    fipsd_udp_destroy(n->udp); go_swap_free(n->configs); go_channel_unref(n->live); go_channel_unref(n->published); go_channel_unref(n->advertised);
    go_channel_unref(n->cached); go_channel_unref(n->fetched); go_channel_unref(n->dialed);
}
static fipsd_runtime *runtime(void)
{
    fipsd_runtime_config c; fipsd_runtime_config_default(&c); c.workers=2;c.handle_signals=false;c.grace_ms=20000;
    fipsd_runtime *r=fipsd_runtime_create(&c);assert(r);return r;
}
typedef struct { fips_transport_addr from; size_t n; uint8_t bytes[64]; } reply;
static fipsd_udp_class collect(void *arg,const uint8_t *p,size_t n,const fips_transport_addr *from)
{
    if (!fipsd_stun_packet(p,n)) return FIPSD_UDP_FMP;
    assert(n<=64); reply *r=calloc(1,sizeof *r);assert(r);r->from=*from;r->n=n;memcpy(r->bytes,p,n);
    assert(!go_channel_try_send(arg,r));return FIPSD_UDP_DISCARD;
}
static void stun_io(void)
{
    fipsd_runtime *rt=runtime();node n;
    init_node(&n,rt,"node: {control: {enabled: false}}",false);
    GoChannel *responses=go_channel_create(16);assert(fipsd_udp_register_datagram_classifier(n.udp,"stun-test",collect,responses));
    assert(!fipsd_runtime_start(rt));(void)receive(fipsd_udp_bound(n.udp));
    fips_transport_addr bound;assert(fipsd_udp_address(n.udp,&bound));
    for (unsigned scenario=0;scenario<3;++scenario) {
        stun_fixture server;stun_fixture_start(&server,scenario==1?1:0,scenario==2);
        uint8_t id[12];randombytes_buf(id,sizeof id);fipsd_stun s;
        fipsd_stun_begin(&s,&server.address,id,fipsd_transport_now_ms(),FIPSD_STUN_TRAVERSAL_MS);
        unsigned sends=0;
        while (s.active) {
            fipsd_stun_action action=fipsd_stun_tick(&s,fipsd_transport_now_ms());
            if (action==FIPSD_STUN_FAILED) break;
            if (action==FIPSD_STUN_SEND) {assert(!fipsd_stun_send(&s,n.udp));++sends;}
            uint64_t now=fipsd_transport_now_ms(),deadline=fipsd_stun_deadline(&s);void *value=NULL;
            GoSelectCase c={.op=GO_SELECT_RECEIVE,.chan=responses,.recv_buf=&value};
            GoSelectResult result=go_select_timeout(&c,1,deadline>now?deadline-now:0);
            if (result.selected_case==0) {
                reply *p=value;fips_transport_addr wrong=addr("127.0.0.1:1");
                assert(!fipsd_stun_receive(&s,&wrong,p->bytes,p->n));
                assert(fipsd_stun_receive(&s,&p->from,p->bytes,p->n));free(p);
            }
        }
        assert(s.complete==(scenario!=2));assert(sends==(scenario==0?1u:scenario==1?2u:3u));
        if(s.complete)assert(fips_transport_addr_equal(&s.mapped,&bound));
        for(unsigned i=0;i<sends;++i){stun_request *q=receive(server.requests);assert(fips_transport_addr_equal(&q->source,&bound));assert(!memcmp(q->request,s.request,20));free(q);}
        stun_fixture_stop(&server);
    }
    fipsd_runtime_shutdown(rt);assert(fipsd_runtime_run(rt)==FIPSD_RUN_OK);fipsd_runtime_destroy(rt);
    destroy_node(&n);go_channel_unref(responses);
    puts("real UDP listener: binding success, first-request loss/retry, bounded give-up, exact source and stable txn/local port: OK");
}
static void link_up(node *n)
{
    for (;;) { fipsd_node_notice *e=receive(fipsd_node_handlers_notices(n->handlers));
        assert(e->kind!=FIPSD_NOTICE_HANDSHAKE_FAILED);bool done=e->kind==FIPSD_NOTICE_LINK_UP;free(e);if(done)return; }
}
static void traverse(void)
{
    fixture *f=fixture_start();stun_fixture server;stun_fixture_start(&server,1,false);
    fipsd_runtime *rt=runtime();node nodes[2];
    for(unsigned i=0;i<2;++i){char yaml[2048];
        snprintf(yaml,sizeof yaml,"node:\n  identity: {nsec: '%s', persistent: true}\n  control: {enabled: false}\n  rendezvous:\n    nostr:\n      enabled: true\n      policy: open\n      advertise: true\n      advert_relays: ['%s']\n      dm_relays: ['%s']\n      stun_servers: ['%s']\n      share_local_candidates: false\ntransports: {udp: {bind_addr: '127.0.0.1:0', advertise_on_nostr: true}}\n",
            i?"2222222222222222222222222222222222222222222222222222222222222222":"1111111111111111111111111111111111111111111111111111111111111111",fixture_url(f,0),fixture_url(f,0),server.url);
        init_node(&nodes[i],rt,yaml,true);
    }
    assert(!fipsd_runtime_start(rt));
    (void)receive(nodes[0].live);(void)receive(nodes[1].live);
    /* Both nodes discover udp:nat from real signed relay adverts. No direct
     * connect(), fake driver offer callback or injected traversal packet. */
    link_up(&nodes[0]);link_up(&nodes[1]);
    (void)receive(nodes[0].published);(void)receive(nodes[1].published);
    for(unsigned i=0;i<2;++i)assert(fipsd_udp_connected_count(nodes[i].udp)==1);
    fips_transport_addr bound[2];for(unsigned i=0;i<2;++i)assert(fipsd_udp_address(nodes[i].udp,&bound[i]));
    bool saw[2]={false,false};for(unsigned j=0;j<3;++j){stun_request *q=receive(server.requests);for(unsigned i=0;i<2;++i)if(fips_transport_addr_equal(&q->source,&bound[i]))saw[i]=true;free(q);}assert(saw[0]&&saw[1]);
    /* nostrc-tbcl6: node 1's advert refreshed twice while the IK link is up.
     * Each cycle evicts node 0's cached copy (refetch against an empty relay)
     * and re-delivers node 1's signed advert live, so it reaches the dial
     * decision again. The driver must decline: no CONNECT, so no STUN, offer
     * or punch, while the refreshed advert stays cached for a reconnect. */
    fips_xonly_pubkey peer_key=fips_identity_pubkey(fipsd_node_identity(fipsd_node_get_context(nodes[1].node)));
    char peer_hex[65];sodium_bin2hex(peer_hex,sizeof peer_hex,peer_key.bytes,32);
    size_t stored;char *peer_advert=fixture_latest(f,0,NOSTR_KIND_FIPS_OVERLAY_ADVERT,peer_hex,&stored);assert(peer_advert);
    void *v;while(!go_channel_try_receive(nodes[0].dialed,&v)){}
    while(!go_channel_try_receive(nodes[0].cached,&v)){}
    while(!go_channel_try_receive(nodes[0].published,&v)){}
    for(unsigned cycle=0;cycle<2;++cycle){
        fixture_call(f,CLEAR,0,NULL);assert(fipsd_nostr_runtime_refetch(nodes[0].relay,peer_hex));
        assert((uintptr_t)receive(nodes[0].fetched)==FIPSD_REFETCH_EVICTED+1);
        fixture_call(f,INJECT,0,peer_advert);
        (void)receive(nodes[0].cached);assert((uintptr_t)receive(nodes[0].dialed)==1);
    }
    free(peer_advert);
    /* dial() declined synchronously: nothing was queued to traversal_io. */
    assert(go_channel_try_receive(server.requests,&v)&&go_channel_try_receive(nodes[0].published,&v));
    assert(fipsd_udp_connected_count(nodes[0].udp)==1);
    puts("advert refresh for an IK-linked peer, two cycles: dial declined, zero STUN/offer activity: OK");
    fipsd_runtime_shutdown(rt);assert(fipsd_runtime_run(rt)==FIPSD_RUN_OK);fipsd_runtime_destroy(rt);
    for(unsigned i=0;i<2;++i)destroy_node(&nodes[i]);
    stun_fixture_stop(&server);fixture_destroy(f);
    puts("two nodes: STUN on shared UDP tuple -> Nostr advert/offer/answer -> NPTC/NPTA -> authenticated IK + connected UDP: OK (loopback, not NAT)");
}
typedef struct { GoChannel *live, *signals; } signal_observer;
static void signal_ready(void *arg, const char *url, bool ready, const char *reason)
{
    (void)url; (void)reason; signal_observer *o = arg;
    if (ready) assert(!go_channel_try_send(o->live, o));
}
static bool record_signal(void *arg, const fipsd_nostr_traversal *p)
{
    signal_observer *o = arg; fipsd_nostr_traversal *copy = malloc(sizeof *copy); assert(copy);
    *copy = *p; assert(!go_channel_try_send(o->signals, copy)); return true;
}
static void guards(void)
{
    fixture *f = fixture_start(); stun_fixture server; stun_fixture_start(&server, 0, false);
    fipsd_runtime *rt = runtime(); node n; char yaml[2048];
    snprintf(yaml, sizeof yaml,
        "node:\n  control: {enabled: false}\n  retry: {base_interval_secs: 3600, max_backoff_secs: 3600}\n"
        "  rendezvous:\n    nostr:\n      enabled: true\n      policy: open\n      advertise: true\n"
        "      open_discovery_max_pending: 1\n      max_concurrent_incoming_offers: 1\n"
        "      advert_relays: ['%s']\n      dm_relays: ['%s']\n      stun_servers: ['%s']\n"
        "transports: {udp: {bind_addr: '127.0.0.1:0', advertise_on_nostr: true}}\n",
        fixture_url(f, 0), fixture_url(f, 0), server.url);
    init_node(&n, rt, yaml, true);
    fips_secp *secp; assert(fips_identity_init_secp(fipsd_host_os_entropy(), &secp) == FIPS_IDENTITY_OK);
    fips_identity *identities[3];
    for (unsigned i = 0; i < 3; ++i)
        assert(fips_identity_generate(secp, fipsd_host_os_entropy(), &identities[i]) == FIPS_IDENTITY_OK);
    signal_observer observers[2]; fipsd_nostr_runtime *remotes[2];
    snprintf(yaml, sizeof yaml,
        "node:\n  rendezvous:\n    nostr:\n      enabled: true\n      policy: open\n      advertise: false\n"
        "      advert_relays: ['%s']\n      dm_relays: ['%s']\n", fixture_url(f, 0), fixture_url(f, 0));
    fipsd_config *config = fipsd_config_parse(yaml, strlen(yaml), NULL); assert(config);
    for (unsigned i = 0; i < 2; ++i) {
        observers[i] = (signal_observer){go_channel_create(8), go_channel_create(8)};
        fipsd_rendezvous_driver driver = {.user=&observers[i], .boundary=signal_ready,
            .traversal_offer=record_signal, .traversal_answer=record_signal};
        remotes[i] = fipsd_nostr_runtime_create(config, identities[i], NULL, driver); assert(remotes[i]);
        assert(!fipsd_runtime_register(rt, fipsd_nostr_runtime_subsystem(remotes[i])));
    }
    fipsd_config_free(config); assert(!fipsd_runtime_start(rt));
    (void)receive(n.live); (void)receive(n.advertised); /* STUN observation published, not merely sent. */
    for (unsigned i = 0; i < 2; ++i) (void)receive(observers[i].live);
    const fips_identity *ours = fipsd_node_identity(fipsd_node_get_context(n.node));
    fips_xonly_pubkey key = fips_identity_pubkey(ours); char our_key[65];
    sodium_bin2hex(our_key, sizeof our_key, key.bytes, 32);
    uint64_t now = (uint64_t)time(NULL) * 1000;
    fipsd_nostr_traversal offer = {.issued_at=now, .expires_at=now+120000};
    assert(fips_identity_npub(ours, offer.recipient_npub, sizeof offer.recipient_npub) == FIPS_IDENTITY_OK);
    assert(fips_identity_npub(identities[0], offer.sender_npub, sizeof offer.sender_npub) == FIPS_IDENTITY_OK);
    strcpy(offer.session_id, "reject-without-targets"); strcpy(offer.nonce, "reject-nonce");
    assert(fipsd_nostr_runtime_send_signal(remotes[0], our_key, &offer));
    fipsd_nostr_traversal *answer = receive(observers[0].signals);
    assert(answer->answer && !answer->accepted && answer->has_reason && answer->reason[0]);
    fipsd_nostr_freshness fresh;
    assert(fipsd_nostr_traversal_answer_for_offer(&offer, answer, now, 120000, &fresh) == FIPSD_NOSTR_OK);
    free(answer);
    /* Exercise the authenticated-signal worker seam with another identity.
     * FIFO admission precedes CONNECT; the retained inbound cooldown must not
     * consume the one outbound slot. The first rejection above used real relay
     * admission and publication; this second input isolates worker accounting. */
    assert(fips_identity_npub(identities[2], offer.sender_npub, sizeof offer.sender_npub) == FIPS_IDENTITY_OK);
    strcpy(offer.session_id, "incoming-over-quota"); strcpy(offer.nonce, "quota-nonce");
    assert(fipsd_traversal_io_signal(n.driver.traversal, &offer));
    key = fips_identity_pubkey(identities[1]); char remote_key[65];
    sodium_bin2hex(remote_key, sizeof remote_key, key.bytes, 32);
    assert(fipsd_traversal_io_connect(n.driver.traversal, remote_key));
    fipsd_nostr_traversal *outbound = receive(observers[1].signals);
    assert(!outbound->answer && outbound->has_reflexive); free(outbound);
    fipsd_runtime_shutdown(rt); assert(fipsd_runtime_run(rt) == FIPSD_RUN_OK); fipsd_runtime_destroy(rt);
    for (unsigned i = 0; i < 2; ++i) {
        fipsd_nostr_runtime_destroy(remotes[i]);
        go_channel_unref(observers[i].live); go_channel_unref(observers[i].signals);
    }
    destroy_node(&n);
    for (unsigned i = 0; i < 3; ++i) fips_identity_destroy(identities[i]);
    fips_identity_destroy_secp(secp); stun_fixture_stop(&server); fixture_destroy(f);
    puts("real relay negative answer validation; retained inbound cooldown cannot consume outbound worker quota: OK");
}
int main(int argc,char **argv)
{
    (void)fipsd_test_tmpdir(NULL);assert(sodium_init()>=0);assert(argc==2);
    if(!strcmp(argv[1],"codecs"))codecs();else if(!strcmp(argv[1],"states"))states();
    else if(!strcmp(argv[1],"stun"))stun_io();else if(!strcmp(argv[1],"traverse"))traverse();
    else if(!strcmp(argv[1],"guards"))guards();else abort();
    return 0;
}
