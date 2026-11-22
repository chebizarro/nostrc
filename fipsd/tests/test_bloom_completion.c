/* Completion-time FilterAnnounce refusal must not consume pending delivery.
 * Real dispatcher, IK link, seal jobs and transport; only the outcome between
 * submit and completion is faulted, on the dispatcher, without timing races. */
#include "handshake_fixture.h"
#include <errno.h>
#include <stdio.h>

static handshake_fixture *fixture;
static fipsd_dp_packet *target;
static bool (*original_result)(void *, const fipsd_crypto_job *);
static const fipsd_transport_ops *original_ops;
static fipsd_transport_ops fault_ops;
static GoChannel *done;
static int fault;
static uint64_t sent_before, failed_before, sequence, counter;

static bool announce(const fipsd_crypto_job *j)
{
    if (j->operation != FIPSD_CRYPTO_SEAL) return false;
    fipsd_dp_packet *p = j->owner;
    uint32_t ts; const uint8_t *msg; size_t n;
    return !p->session && fips_fmp_strip_inner_header(p->input, j->input_size, &ts, &msg, &n) &&
        n && msg[0] == FIPS_LINK_FilterAnnounce;
}
static uint16_t tiny_mtu(const fipsd_transport *t, const fips_transport_addr *to)
{ (void)t; (void)to; return 1; }
static int refused_send(fipsd_transport *t, const fips_transport_addr *to, const uint8_t *bytes, size_t len)
{ (void)t; (void)to; (void)bytes; (void)len; return EAGAIN; }

static bool completion(void *user, const fipsd_crypto_job *j)
{
    if (j->owner != target) return original_result(user, j);
    assert(announce(j));
    fipsd_crypto_job copy = *j;
    if (fault == 2) copy.result = FIPS_NOISE_EncryptionFailed;
    bool ok = original_result(user, &copy);
    fixture->server->ops = original_ops;
    handlers *h = fixture->h;
    fips_bloom_peer *p = fips_bloom_state_peer(&h->bloom, &fixture->remote.node_addr);
    assert(p);
    bool sent = fault == 0 || fault == 5;
    assert(h->counters.bloom_sent == sent_before + sent);
    assert(h->counters.bloom_send_failed == failed_before + !sent);
    assert(p->pending == (fault != 0));
    if (sent) {
        assert(p->sent_len && p->ack.counter == counter && p->ack.outstanding);
        assert(p->sequence >= sequence);
    }
    target = NULL;
    assert(!go_channel_send(done, h));
    return ok;
}
static void submit(handshake_fixture *f, void *arg)
{
    fault = *(int *)arg;
    handlers *h = f->h;
    sent_before = h->counters.bloom_sent; failed_before = h->counters.bloom_send_failed;
    fips_bloom_peer *p = fips_bloom_state_peer(&h->bloom, &f->remote.node_addr); assert(p);
    /* The wire peer does not run tree negotiation; supply its established
     * routing role, then exercise the production planner and send path. */
    fips_bloom_state_peer_role(&h->bloom, p, true, false, true);
    p->has_last_sent = false; p->pending = true;
    original_result = f->on_crypto;
    assert(original_result != completion);
    f->on_crypto = completion;
    hs_bloom_refresh(h, hs_now(h), false);
    size_t count = 0;
    for (size_t i = 0; i < h->dataplane->packet_count; ++i) {
        fipsd_dp_packet *packet = &h->dataplane->packets[i];
        if (atomic_load(&packet->busy) && announce(&packet->job)) { target = packet; ++count; }
    }
    assert(count == 1 && target && !target->worker_sent);
    counter = target->job.counter; sequence = p->sequence;
    assert(counter > 0);
    /* Regression: submit alone is not a successful send or a live boundary. */
    assert(h->counters.bloom_sent == sent_before && p->pending);
    hs_bloom_refresh(h, hs_now(h), false);
    size_t after = 0;
    for (size_t i = 0; i < h->dataplane->packet_count; ++i)
        if (atomic_load(&h->dataplane->packets[i].busy) && announce(&h->dataplane->packets[i].job)) ++after;
    assert(after == 1); /* no duplicate work while the first seal is outstanding */
    if (fault == 5) {
        fips_node_addr leaf = {{0x55}};
        assert(fips_bloom_state_add_leaf(&h->bloom, leaf) == FIPS_OK);
    }
    if (fault == 1) target->tag = UINT64_MAX; /* epoch retired while sealing */
    fault_ops = *original_ops;
    if (fault == 3) fault_ops.mtu = tiny_mtu;
    if (fault == 4) fault_ops.send = refused_send;
    f->server->ops = &fault_ops;
}
static void restore_result(handshake_fixture *f, void *arg)
{ (void)arg; f->on_crypto = original_result; }

int main(void)
{
    for (int failure = 1; failure <= 5; ++failure) {
        handshake_fixture f; hf_init(&f); fixture = &f;
        hf_start(&f, 1); hf_send(&f, f.client, f.msg1.bytes, f.msg1.len); hf_finish(&f); hf_heartbeat(&f);
        original_ops = f.server->ops;
        done = go_channel_create(1); assert(done);
        hf_call(&f, submit, &failure); hf_wait(done); hf_call(&f, restore_result, NULL);
        int success = 0;
        hf_call(&f, submit, &success); hf_wait(done); hf_call(&f, restore_result, NULL);
        go_channel_free(done); hf_destroy(&f);
    }
    puts("bloom: submit is provisional; stale/crypto/MTU/transport drops stay pending; successful retry records its real counter");
    return 0;
}
