/* Real IK links and encrypted MMP reports over loopback, with a virtual clock.
 * Test commands run on each node's dispatcher; mutable protocol state is never
 * read by the test thread. Channel replies/packet predicates are the barriers.
 * Link frames are sealed/opened by dataplane workers: a reply waits until the
 * node has no crypto job in flight (every reserved frame is on the wire, every
 * received frame is dispatched), and the loopback filter drops a frame by its
 * cleartext FMP counter, not by completion order. */
#include "node/handlers/internal.h"
#include "node/transport_loopback.h"
#include "fakes/host.h"
#include "select.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>

typedef enum operation { DIAL, TICK, ANNOUNCE, HEARTBEAT, REPORT, WAIT_COUNTER, WAIT_RR } operation;
#define NO_DROP UINT64_MAX
typedef struct request { operation op; uint64_t ms, value; } request;
typedef struct sample {
    uint64_t sent, next, rr, highest, sequence, counter;
    bool outstanding;
} sample;
typedef struct fixture {
    handlers *h;
    fipsd_node *node;
    fipsd_transport *transport;
    GoSwap *configs;
    GoChannel *replies;
    fipsd_node_options original;
    fips_peer_identity remote;
    const fips_transport_addr *remote_addr;
    _Atomic uint64_t ms;
    _Atomic uint64_t drop_counter; /* FMP counter the loopback filter drops once */
    request *waiting;
} fixture;
static fips_instant virtual_now(void *ctx)
{ return fips_test_instant(atomic_load(&((fixture *)ctx)->ms)); }
static fixture *owner(handlers *h) { return h->clock.ctx; }
static peer_rec *peer(handlers *h)
{
    for (size_t i = 0; i < h->cfg.peer_capacity; ++i) if (h->recs[i].used) return &h->recs[i];
    return NULL;
}
static uint64_t reserved(peer_rec *p) { return fips_rekey_sent(fipsd_active_peer_link_machine(p->peer)); }
/* A packet slab is busy from submission until the node recycles it after the
 * result callback returns, so the completion being handled is excluded. */
static bool drained(handlers *h, const fipsd_dp_packet *completing)
{
    fipsd_dataplane *d = h->dataplane;
    for (size_t i = 0; i < d->packet_count; ++i)
        if (&d->packets[i] != completing && atomic_load(&d->packets[i].busy)) return false;
    return true;
}
static void reply_if_ready(handlers *h, const fipsd_dp_packet *completing)
{
    fixture *f = owner(h);
    if (!f->waiting || !drained(h, completing)) return;
    peer_rec *p = peer(h);
    fips_mmp_peer_state *mmp = p ? fipsd_active_peer_mmp(p->peer) : NULL;
    if (f->waiting->op == WAIT_COUNTER && (!mmp || !mmp->receiver.has_last_recv ||
        mmp->receiver.highest_counter < f->waiting->value)) return;
    if (f->waiting->op == WAIT_RR && h->counters.mmp_rr_received < f->waiting->value) return;
    sample *s = calloc(1, sizeof *s); assert(s);
    s->sent = h->counters.tree_sent; s->rr = h->counters.mmp_rr_received;
    if (p) {
        s->next = fips_rekey_sent(fipsd_active_peer_link_machine(p->peer));
        s->outstanding = fips_tree_state_announce_outstanding(h->tree, p->addr);
        (void)fips_tree_state_outstanding_seq(h->tree, p->addr, &s->sequence);
        (void)fips_tree_state_outstanding_counter(h->tree, p->addr, &s->counter);
    }
    if (mmp) s->highest = mmp->receiver.highest_counter;
    free(f->waiting); f->waiting = NULL;
    assert(go_channel_try_send(f->replies, s) == 0);
}
static bool transport_event(void *user, const fipsd_transport_event *event)
{
    handlers *h = user;
    bool ok = owner(h)->original.transport_event(user, event);
    reply_if_ready(h, NULL);
    return ok;
}
static bool crypto_result(void *user, const fipsd_crypto_job *job)
{
    handlers *h = user;
    bool ok = owner(h)->original.crypto_result(user, job);
    reply_if_ready(h, job->recycle ? job->owner : NULL);
    return ok;
}
static bool command(void *user, void *message)
{
    handlers *h = user; fixture *f = owner(h); request *r = message;
    assert(!f->waiting);
    atomic_store(&f->ms, r->ms);
    peer_rec *p = peer(h);
    uint64_t before = p ? reserved(p) : 0;
    switch (r->op) {
    case DIAL: assert(hs_dial(h, &f->remote, (fips_transport_id){1}, f->remote_addr, false)); break;
    case TICK: hs_tree_tick(h, hs_now(h)); break;
    case ANNOUNCE: assert(p && hs_tree_send_announce(h, p, false)); break;
    case HEARTBEAT: {
        uint8_t hb = FIPS_LINK_Heartbeat;
        assert(p && hs_link_send(h, p, &hb, 1)); break;
    }
    case REPORT: {
        assert(p);
        fips_mmp_peer_state *mmp = fipsd_active_peer_mmp(p->peer); assert(mmp);
        fips_mmp_receiver_report rr;
        assert(fips_mmp_receiver_build_report(&mmp->receiver, hs_now_ms(h), &rr));
        uint8_t bytes[1 + FIPS_MMP_RECEIVER_REPORT_SIZE];
        fips_writer w; fips_writer_init(&w, bytes, sizeof bytes);
        assert(fips_mmp_receiver_report_encode(&rr, &w, NULL) == FIPS_OK);
        assert(hs_link_send(h, p, bytes, fips_writer_len(&w))); break;
    }
    case WAIT_COUNTER: case WAIT_RR: break;
    }
    /* Drop exactly the announce this command reserved, whatever else is in flight. */
    if ((r->op == ANNOUNCE || r->op == TICK) && r->value) {
        assert(p && reserved(p) == before + 1);
        atomic_store(&f->drop_counter, before);
    }
    f->waiting = r; reply_if_ready(h, NULL);
    return true;
}
static sample call(fixture *f, operation op, uint64_t ms, uint64_t value)
{
    request *r = malloc(sizeof *r); assert(r); *r = (request){op, ms, value};
    assert(go_channel_try_send(f->h->commands, r) == 0);
    void *out = NULL;
    GoSelectCase cases[] = {{GO_SELECT_RECEIVE, f->replies, NULL, &out}};
    GoSelectResult selected = go_select_timeout(cases, 1, 10000);
    if (selected.selected_case != 0) fprintf(stderr, "operation %d at %llu waiting for %llu\n", op,
        (unsigned long long)ms, (unsigned long long)value);
    assert(selected.selected_case == 0 && out);
    sample s = *(sample *)out; free(out); return s;
}
static fipsd_network_snapshot *network(void *user)
{ (void)user; fipsd_network_snapshot *s = calloc(1, sizeof *s); assert(s); return s; }
static void init(fixture *f, fipsd_loopback_network *net, const char *address, char digit)
{
    memset(f, 0, sizeof *f); atomic_init(&f->ms, 0); atomic_init(&f->drop_counter, NO_DROP);
    char yaml[256], key[65]; memset(key, digit, 64); key[64] = 0;
    snprintf(yaml, sizeof yaml, "node: {identity: {nsec: '%s', persistent: true}, control: {enabled: false}}", key);
    fipsd_config *cfg = fipsd_config_parse(yaml, strlen(yaml), NULL); assert(cfg);
    f->configs = go_swap_create(cfg, fipsd_config_free); assert(f->configs);
    fipsd_node_handlers_config hc; fipsd_node_handlers_config_default(&hc);
    hc.peer_capacity = hc.tree_peer_capacity = hc.session_capacity = 2; hc.cache_entries = 16;
    hc.clock = (fips_clock){virtual_now, f}; hc.tick_ms = 1000000;
    hc.tree_reeval_ms = 0; hc.path_enabled = false; hc.notice_capacity = 256;
    f->h = fipsd_node_handlers_create(&hc); assert(f->h);
    f->replies = go_channel_create(1); assert(f->replies);
    fipsd_netmon_source nm = {.read = network, .injected = true};
    fipsd_node_options o = {.workers = 2, .peer_capacity = 2, .netmon_source = &nm};
    fipsd_node_handlers_install(f->h, &o); f->original = o;
    o.command = command; o.transport_event = transport_event; o.crypto_result = crypto_result;
    f->node = fipsd_node_create(f->configs, &o); assert(f->node);
    f->transport = fipsd_loopback_transport_create(net, (fips_transport_id){1}, address, address, 1280);
    assert(f->transport && fipsd_transport_register(fipsd_node_transports(f->node), f->transport) == 0);
}
static void identity(fixture *f, fixture *other)
{
    const fipsd_node_context *ctx = fipsd_node_get_context(other->node);
    fips_xonly_pubkey key = fips_identity_pubkey(fipsd_node_identity(ctx));
    assert(fips_peer_identity_from_pubkey(fipsd_node_secp(ctx), &key, &f->remote) == FIPS_IDENTITY_OK);
    f->remote_addr = fipsd_loopback_transport_address(other->transport);
}
static void link_up(fixture *f)
{
    void *out = NULL;
    GoSelectCase cases[] = {{GO_SELECT_RECEIVE, fipsd_node_handlers_notices(f->h), NULL, &out}};
    for (;;) {
        GoSelectResult s = go_select_timeout(cases, 1, 10000); assert(s.selected_case == 0 && out);
        bool up = ((fipsd_node_notice *)out)->kind == FIPSD_NOTICE_LINK_UP;
        free(out); out = NULL; if (up) return;
    }
}
typedef struct drop_state { const fips_transport_addr *from; bool all; _Atomic uint64_t *counter; } drop_state;
static bool drop_filter(void *user, const fips_transport_addr *from, const fips_transport_addr *to,
                        const uint8_t *bytes, size_t len)
{
    (void)to; drop_state *d = user;
    if (!fips_transport_addr_equal(from, d->from)) return true;
    if (d->all) return false;
    fips_fmp_encrypted_header header;
    if (!fips_fmp_encrypted_header_parse(bytes, len, &header) || header.counter != atomic_load(d->counter)) return true;
    atomic_store(d->counter, NO_DROP); return false;
}
static sample heartbeat(fixture *a, fixture *b, uint64_t ms)
{
    sample s = call(a, HEARTBEAT, ms, 0);
    (void)call(b, WAIT_COUNTER, ms, s.next - 1); return s;
}
static sample report(fixture *a, fixture *b, uint64_t ms)
{
    sample s = call(a, TICK, ms, 0);
    (void)call(b, REPORT, ms, 0);
    (void)call(a, WAIT_RR, ms, s.rr + 1);
    return call(a, TICK, ms, 0);
}
static void destroy(fixture *f)
{
    fipsd_node_destroy(f->node); fipsd_node_handlers_destroy(f->h);
    fipsd_loopback_transport_destroy(f->transport); go_swap_free(f->configs);
    go_channel_free(f->replies);
}
int main(int argc, char **argv)
{
    bool no_reports = argc == 2 && !strcmp(argv[1], "no-reports");
    assert(argc == 1 || no_reports);
    fipsd_runtime_config rc; fipsd_runtime_config_default(&rc); rc.workers = 2; rc.handle_signals = false;
    fipsd_runtime *rt = fipsd_runtime_create(&rc); assert(rt);
    fipsd_loopback_network *net = fipsd_loopback_network_create(); assert(net);
    fixture a, b; init(&a, net, "loop:stp-a", '1'); init(&b, net, "loop:stp-b", '2');
    assert(fipsd_runtime_register(rt, fipsd_node_subsystem(a.node)) == 0);
    assert(fipsd_runtime_register(rt, fipsd_node_subsystem(b.node)) == 0);
    assert(fipsd_runtime_start(rt) == 0); identity(&a, &b); identity(&b, &a);
    (void)call(&a, DIAL, 0, 0); link_up(&a); link_up(&b);
    (void)heartbeat(&a, &b, 0); (void)heartbeat(&b, &a, 0);
    (void)call(&a, TICK, 500, 0); (void)call(&b, TICK, 500, 0);
    (void)heartbeat(&a, &b, 500); (void)heartbeat(&b, &a, 500);
    sample acked = report(&a, &b, 500); assert(!acked.outstanding);
    assert(call(&a, TICK, 999, 0).sent == acked.sent);
    assert(call(&a, TICK, 30000, 0).sent == acked.sent);
    puts("acked: no resend at +499 ms (500 ms floor), or at +29500 ms");

    drop_state drop = {.from = fipsd_loopback_transport_address(no_reports ? b.transport : a.transport), .all = no_reports, .counter = &a.drop_counter};
    fipsd_loopback_network_set_filter(net, drop_filter, &drop);
    sample sent = call(&a, ANNOUNCE, 30000, no_reports ? 0 : 1); assert(sent.sent == acked.sent + 1 && sent.outstanding);
    assert(sent.counter == sent.next - 1);
    if (no_reports) {
        /* Drop the real receiver report, not the transport send result. */
        (void)heartbeat(&a, &b, 30000);
        (void)call(&b, REPORT, 30000, 0);
        assert(fipsd_loopback_transport_dropped(b.transport) >= 1);
        sample waiting = call(&a, TICK, 59999, 0);
        assert(waiting.sent == sent.sent && waiting.rr == sent.rr && waiting.outstanding);
        sample retry = call(&a, TICK, 60000, 0);
        assert(retry.sent == sent.sent + 1 && retry.outstanding);
        assert(call(&a, TICK, 90000, 0).sent == retry.sent);
        assert(call(&a, TICK, 180000, 0).sent == retry.sent);
        puts("receiver report dropped: exactly one retry at 60000 ms (+30 s), none through 180000 ms");
    } else {
        uint64_t sequence = sent.sequence;
        /* Each dropped announce is followed by a delivered frame, so a real RR
         * proves a hole. The loss budget is three; the gaps are 1 s then 2 s. */
        const uint64_t times[] = {30500, 31500, 33500};
        for (size_t i = 0; i < 3; ++i) {
            uint64_t observed_at = i ? times[i] - 1 : times[i];
            (void)heartbeat(&a, &b, observed_at);
            (void)call(&b, REPORT, observed_at, 0);
            (void)call(&a, WAIT_RR, observed_at, sent.rr + 1);
            if (i) {
                sample early = call(&a, TICK, observed_at, 0); assert(early.sent == sent.sent);
            }
            sample retry = call(&a, TICK, times[i], 1);
            assert(retry.sent == sent.sent + 1 && retry.outstanding && retry.sequence == sequence);
            printf("loss retry %zu at %llu ms\n", i + 1, (unsigned long long)times[i]); sent = retry;
        }
        (void)heartbeat(&a, &b, 33500);
        sample exhausted = report(&a, &b, 35000); assert(exhausted.sent == sent.sent);
        assert(call(&a, TICK, 63499, 0).sent == sent.sent);
        sample timeout = call(&a, TICK, 63500, 1); assert(timeout.sent == sent.sent + 1);
        assert(call(&a, TICK, 93500, 0).sent == timeout.sent);
        assert(call(&a, TICK, 180000, 0).sent == timeout.sent);
        puts("unverified retry at 63500 ms (+30 s); budgets exhausted: no further resend through 180000 ms");
    }
    fipsd_runtime_shutdown(rt); assert(fipsd_runtime_run(rt) == FIPSD_RUN_OK);
    fipsd_runtime_destroy(rt); destroy(&a); destroy(&b); fipsd_loopback_network_destroy(net);
    return 0;
}
