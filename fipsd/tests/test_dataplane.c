#ifdef NDEBUG
#undef NDEBUG /* Test operations and correctness assertions run in optimized smoke builds too. */
#endif
#include "node/handlers/internal.h"
#include "fakes/host.h"
#include "node/crypto_worker.h"
#include "node/transport_loopback.h"
#include <assert.h>
#include <stdio.h>
#include <pthread.h>
#include <sched.h>

static void forwarding(void)
{
    fips_node_addr me = {{1}}, peer = {{2}}, remote = {{3}};
    fipsd_dp_peer peers[2] = {{.addr = peer, .mtu = 1200}, {.addr = remote, .mtu = 1300}};
    fipsd_dp_snapshot s = {.me = me, .peers = peers, .peer_count = 2};
    fips_tree_coordinate *scratch = calloc(1, sizeof *scratch); assert(scratch);
    fips_routing_peer view[2]; fips_router router; fips_router_init(&router, fips_test_instant(0));
    uint8_t bytes[256], payload[] = {0x11};
    fips_session_datagram dg; fips_session_datagram_init(&dg, peer, remote, payload, sizeof payload);
    fips_writer w; fips_writer_init(&w, bytes, sizeof bytes);
    fips_route_outcome out;
    assert(fipsd_dp_forward(&s, &router, &dg, &peer, true, fips_test_instant(0), scratch, view, &w, &out) == FIPS_OK);
    assert(out.kind == FIPS_ROUTE_Forward && out.outgoing_ce && !memcmp(out.next_hop.bytes, remote.bytes, 16));
    /* Outgoing packet folds next-hop MTU and decrements TTL exactly once. */
    fips_reader rd; fips_reader_init(&rd, bytes + 1, fips_writer_len(&w) - 1);
    fips_session_datagram forwarded;
    assert(fips_session_datagram_decode(&rd, &forwarded, NULL) == FIPS_OK);
    assert(forwarded.ttl == dg.ttl - 1 && forwarded.path_mtu == 1300);
    fips_writer_init(&w, bytes, sizeof bytes);
    assert(fipsd_dp_forward(&s, &router, &dg, &remote, false, fips_test_instant(0), scratch, view, &w, &out) == FIPS_OK && out.kind == FIPS_ROUTE_NoRoute);
    dg.ttl = 1;
    assert(fipsd_dp_forward(&s, &router, &dg, NULL, false, fips_test_instant(0), scratch, view, &w, &out) == FIPS_OK && out.kind == FIPS_ROUTE_Drop);
    dg.dest_addr = me; dg.ttl = 0;
    assert(fipsd_dp_forward(&s, &router, &dg, NULL, false, fips_test_instant(0), scratch, view, &w, &out) == FIPS_OK && out.kind == FIPS_ROUTE_DeliverLocal);
    /* Snapshot-only tree routing: a bloom miss can still use a strictly
     * closer tree neighbor, but never the arrival link or an expired cache. */
    fips_tree_coordinate *mine = calloc(1, sizeof *mine), *neighbor = calloc(1, sizeof *neighbor);
    assert(mine && neighbor);
    fips_node_addr root = {{0}}, ma[] = {me, root}, pa[] = {peer, root};
    assert(fips_tree_coordinate_from_addrs(mine, ma, 2, NULL) == FIPS_OK);
    assert(fips_tree_coordinate_from_addrs(neighbor, pa, 2, NULL) == FIPS_OK);
    s.my_coords = make_go_refptr(mine, free); peers[0].coords = make_go_refptr(neighbor, free);
    /* The published cache section, built from a core cache entry that
     * expires at 100 ms (a 100 ms TTL written at 0). */
    enum { CSLOTS = FIPS_COORD_CACHE_SLOTS(4), CWORDS = FIPS_COORD_CACHE_INDEX_WORDS(CSLOTS) };
    fips_coord_slot *cslots = calloc(CSLOTS, sizeof *cslots); uint32_t *cindex = calloc(CWORDS, sizeof *cindex);
    assert(cslots && cindex);
    fips_coord_cache cache; fips_fixed_entropy entropy = {0x33};
    assert(fips_coord_cache_init(&cache, cslots, CSLOTS, cindex, CWORDS, 4, 100, fips_test_instant(0),
        fips_fixed_entropy_adapter(&entropy)) == FIPS_OK);
    uint8_t ca[3][16]; memcpy(ca[0], remote.bytes, 16); memcpy(ca[1], peer.bytes, 16); memcpy(ca[2], root.bytes, 16);
    fips_cached_coord coords; fips_hint_outcome placed;
    assert(fips_cached_coord_from_addrs(&coords, (const uint8_t (*)[16])ca, 3) == FIPS_OK);
    assert(fips_coord_cache_insert(&cache, remote.bytes, &coords, fips_test_instant(0), &placed) == FIPS_OK);
    fipsd_dp_cache_view cv; fipsd_dp_cache_view_init(&cv, fips_fixed_entropy_adapter(&entropy));
    assert(fipsd_dp_cache_view_update(&cv, &cache) && cv.section && cv.section->count == 1);
    s.cache = cv.section; s.peer_count = 1;
    dg.dest_addr = remote; dg.ttl = 64;
    fips_writer_init(&w, bytes, sizeof bytes);
    assert(fipsd_dp_forward(&s, &router, &dg, NULL, false, fips_test_instant(0), scratch, view, &w, &out) == FIPS_OK);
    assert(out.kind == FIPS_ROUTE_Forward && !memcmp(out.next_hop.bytes, peer.bytes, 16));
    fips_writer_init(&w, bytes, sizeof bytes);
    assert(fipsd_dp_forward(&s, &router, &dg, &peer, false, fips_test_instant(0), scratch, view, &w, &out) == FIPS_OK && out.kind == FIPS_ROUTE_NoRoute);
    /* Switch-time tightening uses this same outgoing route, including transit
     * destinations. An unrelated session's inbound carrier is NOT its route. */
    handlers *h = calloc(1, sizeof *h); assert(h);
    fips_test_clock clock = {0}; h->clock = fips_test_clock_adapter(&clock); h->me = me;
    fipsd_dataplane plane = {.current = {.ptr = &s}, .coord_scratch = scratch, .route_peers = view, .clamp_capacity = 3};
    plane.clamps = calloc(3, sizeof *plane.clamps); assert(plane.clamps); h->dataplane = &plane;
    h->sessions.capacity = 3;
    h->session_entries = calloc(3, sizeof *h->session_entries); h->session_ext = calloc(3, sizeof *h->session_ext);
    assert(h->session_entries && h->session_ext);
    fips_node_addr destinations[] = {peer, remote, {{4}}};
    for (size_t i = 0; i < 3; ++i) {
        h->session_entries[i].occupied = true; h->session_entries[i].peer = destinations[i];
        h->session_ext[i].used = h->session_ext[i].mmp_live = true;
        h->session_ext[i].carrier = peer;
        fips_mmp_path_mtu_init(&h->session_ext[i].mmp.path_mtu);
        fips_mmp_path_mtu_seed_source_mtu(&h->session_ext[i].mmp.path_mtu, 1400);
    }
    fipsd_dp_mtu_set(&plane, &peer, 900); /* an already tighter destination clamp survives */
    fipsd_dp_tighten_via(h, &peer, 1100);
    assert(h->session_ext[0].mmp.path_mtu.current_mtu == 1100);
    assert(h->session_ext[1].mmp.path_mtu.current_mtu == 1100);
    assert(h->session_ext[2].mmp.path_mtu.current_mtu == 1400);
    assert(fipsd_dp_mtu_get(&plane, &peer) == 900 && fipsd_dp_mtu_get(&plane, &remote) == 1100);
    assert(!fipsd_dp_mtu_get(&plane, &destinations[2]));
    fipsd_dp_tighten_via(h, &peer, 1280);
    assert(h->session_ext[0].mmp.path_mtu.current_mtu == 1100 && h->session_ext[1].mmp.path_mtu.current_mtu == 1100);
    free(h->session_entries); free(h->session_ext); free(plane.clamps); free(h);
    /* Upstream's expiry (coord_cache.get): live through expires_at itself,
     * gone once now passes it. Snapshot time is relative to its origin. */
    s.origin = fips_test_instant(0);
    fips_writer_init(&w, bytes, sizeof bytes);
    assert(fipsd_dp_forward(&s, &router, &dg, NULL, false, fips_test_instant(100), scratch, view, &w, &out) == FIPS_OK);
    assert(out.kind == FIPS_ROUTE_Forward && !memcmp(out.next_hop.bytes, peer.bytes, 16));
    fips_writer_init(&w, bytes, sizeof bytes);
    assert(fipsd_dp_forward(&s, &router, &dg, NULL, false, fips_test_instant(101), scratch, view, &w, &out) == FIPS_OK && out.kind == FIPS_ROUTE_NoRoute);
    fipsd_dp_cache_view_free(&cv); free(cslots); free(cindex);
    go_refptr_release(&s.my_coords); go_refptr_release(&peers[0].coords);
    free(scratch);
}
static void errors(void)
{
    fips_node_addr me = {{1}}, dest = {{2}}, from = {{3}};
    fips_router router; fips_router_init(&router, fips_test_instant(0));
    fips_routing_view rv = {0};
    uint8_t bytes[256]; fips_writer w; fips_error_synth out;
    fips_writer_init(&w, bytes, sizeof bytes);
    assert(fips_router_synth_routing_error(&router, &dest, &from, &me, &rv, fips_test_instant(0), 64, &w, &out, NULL) == FIPS_OK && out.has_action);
    fips_writer_init(&w, bytes, sizeof bytes);
    assert(fips_router_synth_mtu_exceeded(&router, &dest, &from, &me, 1200, fips_test_instant(99), 64, &w, &out, NULL) == FIPS_OK && !out.has_action);
    assert(fips_writer_len(&w) == 0);
    assert(fips_router_synth_mtu_exceeded(&router, &dest, &from, &me, 1200, fips_test_instant(100), 64, &w, &out, NULL) == FIPS_OK && out.has_action);
}
static void clamps(void)
{
    fipsd_dataplane d = {.clamp_capacity = 2};
    d.clamps = calloc(2, sizeof *d.clamps); assert(d.clamps);
    fips_node_addr a = {{1}}, b = {{2}};
    fipsd_dp_mtu_set(&d, &a, 1200); fipsd_dp_mtu_set(&d, &b, 1280);
    assert(fipsd_dp_mtu_get(&d, &a) == 1200);
    fipsd_dp_mtu_release(&d, &a); /* probe teardown */
    assert(!fipsd_dp_mtu_get(&d, &a) && fipsd_dp_mtu_get(&d, &b) == 1280);
    fipsd_dp_mtu_set(&d, &a, 1400); /* no stale restrictive clamp on restart */
    assert(fipsd_dp_mtu_get(&d, &a) == 1400);
    fipsd_dp_mtu_release(&d, &a); fipsd_dp_mtu_release(&d, &b);
    assert(d.stats.mtu_releases == 3); free(d.clamps);
}
static void backpressure_and_counters(void)
{
    fipsd_runtime_config cfg; fipsd_runtime_config_default(&cfg); cfg.workers = 4; cfg.handle_signals = false;
    fipsd_runtime *rt = fipsd_runtime_create(&cfg); assert(rt && !fipsd_runtime_start(rt));
    fipsd_worker_set *workers = fipsd_worker_set_create(4, 1); assert(workers);
    assert(fipsd_worker_set_start(workers, rt) == 4);
    uint8_t raw[32] = {1}, hash[32] = {0}, pub[33] = {2}, epoch[8] = {0};
    fips_aead_key send, recv; assert(fips_aead_key_init(&send, raw) == FIPS_AEAD_OK);
    raw[0] = 2; assert(fips_aead_key_init(&recv, raw) == FIPS_AEAD_OK);
    fips_noise_session owner;
    assert(fips_noise_session_from_handshake(&owner, FIPS_HANDSHAKE_INITIATOR, &send, &recv, hash, pub, epoch) == FIPS_NOISE_OK);
    size_t accepted = 0; uint64_t reserved = 0; bool refused = false;
    for (size_t i = 0; i < 100; ++i) {
        fipsd_crypto_job *j = calloc(1, sizeof *j); assert(j);
        j->input = calloc(1, 8); j->output = malloc(24); assert(j->input && j->output);
        j->input_size = 8; j->capacity = 24;
        assert(fips_noise_session_take_send_counter(&owner, &j->counter) == FIPS_NOISE_OK); ++reserved;
        assert(fips_noise_session_send_cipher_clone(&owner, &j->key) == FIPS_NOISE_OK);
        uint64_t invalid;
        assert(fips_cipher_state_take_counter(&j->key, &invalid) == FIPS_NOISE_NonceOverflow);
        if (fipsd_worker_set_dispatch(workers, j) != 0) {
            refused = true; fipsd_crypto_job_destroy(j); break;
        }
        ++accepted;
    }
    assert(refused && accepted && reserved == accepted + 1);
    fips_replay_window untouched = owner.replay_window;
    fipsd_worker_set_stop(workers);
    bool seen[100] = {false}; size_t count = 0; void *value;
    while (go_channel_receive(fipsd_worker_set_results(workers), &value) == 0) {
        fipsd_crypto_job *j = value;
        assert(j->result == FIPS_NOISE_OK && j->counter < accepted && !seen[j->counter]);
        seen[j->counter] = true; ++count; fipsd_crypto_job_destroy(j);
    }
    assert(count == accepted && fips_noise_session_current_send_counter(&owner) == reserved);
    assert(!memcmp(&untouched, &owner.replay_window, sizeof untouched));
    fipsd_runtime_shutdown(rt); assert(fipsd_runtime_run(rt) == FIPSD_RUN_OK);
    fipsd_worker_set_destroy(workers); fipsd_runtime_destroy(rt); fips_noise_session_destroy(&owner);
}
static void authenticate_then_mark(void)
{
    uint8_t raw[32] = {7}, hash[32] = {0}, pub[33] = {2}, epoch[8] = {0};
    fips_aead_key send, recv;
    assert(fips_aead_key_init(&send, raw) == FIPS_AEAD_OK);
    assert(fips_aead_key_init(&recv, raw) == FIPS_AEAD_OK);
    fips_noise_session owner;
    assert(fips_noise_session_from_handshake(&owner, FIPS_HANDSHAKE_INITIATOR,
        &send, &recv, hash, pub, epoch) == FIPS_NOISE_OK);
    uint8_t ciphertext[24], plaintext[8] = {1}, opened[8]; size_t len;
    uint64_t counter;
    assert(fips_noise_session_take_send_counter(&owner, &counter) == FIPS_NOISE_OK);
    assert(fips_noise_session_seal(&owner, NULL, 0, plaintext, sizeof plaintext,
        ciphertext, sizeof ciphertext, &len) == FIPS_NOISE_OK);
    counter = 1; /* seal consumed the next dispatcher-owned counter */
    fips_replay_window before = owner.replay_window;
    fipsd_crypto_job j = {.operation = FIPSD_CRYPTO_OPEN, .counter = counter,
        .input = ciphertext, .input_size = len, .output = opened, .capacity = sizeof opened};
    assert(fips_noise_session_recv_cipher_clone(&owner, &j.key) == FIPS_NOISE_OK);
    fipsd_decrypt_worker_execute(&j); assert(j.result == FIPS_NOISE_OK);
    assert(!memcmp(&owner.replay_window, &before, sizeof before));
    /* Two independently authenticated copies can race; only the single owner
     * accepts one. Auth failure never marks anything, even at a fresh counter. */
    fipsd_decrypt_worker_execute(&j); assert(j.result == FIPS_NOISE_OK);
    assert(fips_noise_session_accept_replay(&owner, counter) == FIPS_NOISE_OK);
    assert(fips_noise_session_accept_replay(&owner, counter) != FIPS_NOISE_OK);
    before = owner.replay_window; j.counter = counter + 1;
    fipsd_decrypt_worker_execute(&j); assert(j.result != FIPS_NOISE_OK);
    assert(!memcmp(&owner.replay_window, &before, sizeof before));
    fips_cipher_state_destroy(&j.key); fips_noise_session_destroy(&owner);
}
static void saturated_trial_retry(void)
{
    handlers h = {0}; fipsd_dataplane d = {.packet_count = 256}; h.dataplane = &d;
    d.packets = calloc(d.packet_count, sizeof *d.packets); assert(d.packets);
    d.current = make_go_refptr(calloc(1, sizeof(fipsd_dp_snapshot)), free); assert(d.current.ptr);
    for (size_t i = 0; i < d.packet_count; ++i) {
        assert(fipsd_dp_send_ready(&d) == (i + FIPSD_DP_SEND_RESERVE < d.packet_count));
        assert(fipsd_dp_packet_take(&d));
        assert(atomic_load(&d.in_flight) == i + 1);
    }
    /* Original completion is still borrowed by the node, and must not be
     * recycled/resubmitted underneath it. Full occupancy refuses the trial,
     * counts pressure and leaves the original slot/reference for its owner. */
    assert(!fipsd_dp_open_retry(&h, &d.packets[0]));
    assert(d.stats.backpressure == 1 && atomic_load(&d.packets[0].busy));
    for (size_t i = 0; i < d.packet_count; ++i) fipsd_crypto_job_destroy(&d.packets[i].job);
    for (size_t i = 0; i < d.packet_count; ++i) assert(!atomic_load(&d.packets[i].busy));
    assert(atomic_load(&d.in_flight) == 0 && fipsd_dp_send_ready(&d));
    assert(atomic_load(d.current.ref_count) == 1);
    go_refptr_release(&d.current); free(d.packets);
}
static void delayed_transport_consumer(void)
{
    fipsd_runtime_config cfg; fipsd_runtime_config_default(&cfg); cfg.workers = 2; cfg.handle_signals = false;
    fipsd_runtime *rt = fipsd_runtime_create(&cfg); assert(rt && !fipsd_runtime_start(rt));
    fipsd_loopback_network *net = fipsd_loopback_network_create(); assert(net);
    fipsd_transport *a = fipsd_loopback_transport_create(net, (fips_transport_id){1}, "a", "loop:a", 1280);
    fipsd_transport *b = fipsd_loopback_transport_create(net, (fips_transport_id){1}, "b", "loop:b", 1280);
    GoChannel *in_a = go_channel_create(2), *in_b = go_channel_create(2); assert(a && b && in_a && in_b);
    assert(!a->ops->start_async(a, rt, NULL, in_a) && !b->ops->start_async(b, rt, NULL, in_b));
    uint8_t packet[16]; memset(packet, 0x5a, sizeof packet);
    assert(!a->ops->send(a, fipsd_loopback_transport_address(b), packet, sizeof packet));
    /* Recycle/overwrite caller's slab before destination consumes its event. */
    memset(packet, 0, sizeof packet);
    void *value; assert(!go_channel_receive(in_b, &value)); fipsd_transport_event *event = value;
    assert(event->size == sizeof packet);
    for (size_t i = 0; i < event->size; ++i) assert(event->bytes[i] == 0x5a);
    free(event); a->ops->stop(a); b->ops->stop(b);
    fipsd_runtime_shutdown(rt); assert(fipsd_runtime_run(rt) == FIPSD_RUN_OK);
    fipsd_loopback_transport_destroy(a); fipsd_loopback_transport_destroy(b); fipsd_loopback_network_destroy(net);
    go_channel_unref(in_a); go_channel_unref(in_b); fipsd_runtime_destroy(rt);
}
/* Reuse a single slab while another thread releases it. busy=false hands all
 * packet fields back to the dispatcher, including plane; the recycler may only
 * access its saved dataplane pointer after publishing that handoff. */
static void *recycle_packets(void *arg)
{
    GoChannel *jobs = arg; void *value;
    while (go_channel_receive(jobs, &value) == 0) fipsd_crypto_job_destroy(value);
    return NULL;
}
static void concurrent_recycle(void)
{
    fipsd_dataplane d = {.packet_count = 1};
    d.packets = calloc(1, sizeof *d.packets); assert(d.packets);
    d.current = make_go_refptr(calloc(1, sizeof(fipsd_dp_snapshot)), free); assert(d.current.ptr);
    GoChannel *jobs = go_channel_create(1); assert(jobs);
    pthread_t thread; assert(!pthread_create(&thread, NULL, recycle_packets, jobs));
    for (size_t i = 0; i < 10000; ++i) {
        fipsd_dp_packet *p;
        while (!(p = fipsd_dp_packet_take(&d))) sched_yield();
        assert(!go_channel_send(jobs, &p->job));
    }
    go_channel_close(jobs); assert(!pthread_join(thread, NULL));
    assert(!atomic_load(&d.packets[0].busy) && atomic_load(&d.in_flight) == 0);
    assert(atomic_load(d.current.ref_count) == 1);
    go_channel_free(jobs); go_refptr_release(&d.current); free(d.packets);
}
static bool zero(const uint8_t *b, size_t n) { for (size_t i = 0; i < n; ++i) if (b[i]) return false; return true; }
/* nostrc-u19cz.3: recycle wipes each byte a tenant can have written, once, and
 * nothing past it (it used to wipe all three 2 KB slabs after destroy had
 * already wiped them). Seal-shaped and refused-open-shaped tenants. */
static void exact_wipe(void)
{
    fipsd_dataplane d = {.packet_count = 1};
    d.packets = calloc(1, sizeof *d.packets); assert(d.packets);
    d.current = make_go_refptr(calloc(1, sizeof(fipsd_dp_snapshot)), free); assert(d.current.ptr);
    for (int shape = 0; shape < 2; ++shape) {
        fipsd_dp_packet *p = fipsd_dp_packet_take(&d); assert(p && !p->body && !p->wire_size);
        size_t body = 12, in = 300, out = body + in + FIPS_NOISE_TAG_SIZE;
        memset(p->ad, 0xA5, FIPSD_DP_WIRE); memset(p->input, 0xA5, FIPSD_DP_WIRE); memset(p->output, 0xA5, FIPSD_DP_WIRE);
        p->body = body;
        if (shape == 0) p->job.input_size = in; /* seal: plaintext length */
        else p->wire_size = body + in; /* open refused before its job was set up */
        fipsd_crypto_job_destroy(&p->job);
        assert(!atomic_load(&p->busy) && atomic_load(&d.in_flight) == 0);
        assert(zero(p->ad, body) && p->ad[body] == 0xA5);
        assert(zero(p->input, in) && p->input[in] == 0xA5);
        assert(zero(p->output, out) && p->output[out] == 0xA5);
    }
    go_refptr_release(&d.current); free(d.packets);
}
static size_t gate_sends;
static int counting_send(fipsd_transport *t, const fips_transport_addr *to, const uint8_t *b, size_t n)
{ (void)t; (void)to; (void)b; assert(n == 12 + 40); ++gate_sends; return 0; }
/* nostrc-u19cz.3 worker send (9keoz.12 guard): a sealed frame leaves from the
 * executing thread only at the gate generation captured at submit; after the
 * dispatcher moves the link it comes back unsent for the completion. */
static void worker_send_gate(void)
{
    static const fipsd_transport_ops ops = {.send = counting_send};
    fipsd_transport t = {.ops = &ops};
    handlers h = {0}; peer_rec recs[2] = {{0}};
    fipsd_dataplane d = {.packet_count = 2, .gate_count = 2}; h.dataplane = &d; h.recs = recs;
    d.packets = calloc(2, sizeof *d.packets); d.gates = calloc(2, sizeof *d.gates); assert(d.packets && d.gates);
    for (size_t i = 0; i < 2; ++i) assert(!pthread_mutex_init(&d.gates[i].mu, NULL));
    d.current = make_go_refptr(calloc(1, sizeof(fipsd_dp_snapshot)), free); assert(d.current.ptr);
    for (int moved = 0; moved < 2; ++moved) {
        fipsd_dp_packet *p = fipsd_dp_packet_take(&d); assert(p && !p->worker_sent && !p->via);
        p->via = &t; p->gate = &d.gates[1]; p->body = 12;
        p->gate_generation = atomic_load(&p->gate->generation);
        p->job.result = FIPS_NOISE_OK; p->job.output_size = 40;
        if (moved) fipsd_dp_link_moved(&h, &recs[1]);
        size_t before = gate_sends;
        fipsd_dp_worker_send(&p->job);
        assert(p->worker_sent == !moved && gate_sends == before + !moved);
        fipsd_crypto_job_destroy(&p->job);
    }
    fipsd_dp_link_moved(&h, &recs[0]); /* other slots are independent */
    assert(atomic_load(&d.gates[0].generation) == 1 && atomic_load(&d.gates[1].generation) == 1);
    fipsd_dp_packet *p = fipsd_dp_packet_take(&d); assert(p);
    p->via = &t; p->gate = &d.gates[1]; p->gate_generation = 1; p->job.result = FIPS_NOISE_EncryptionFailed;
    fipsd_dp_worker_send(&p->job); assert(!p->worker_sent); /* nothing sealed, nothing sent */
    fipsd_crypto_job_destroy(&p->job);
    for (size_t i = 0; i < 2; ++i) pthread_mutex_destroy(&d.gates[i].mu);
    go_refptr_release(&d.current); free(d.packets); free(d.gates);
}
/* nostrc-u19cz.3 inbound fusion: a link open's after-step opens the session
 * frame it carries only when it is addressed to this node, with the packet
 * snapshot's session keys, and accepts nothing (fipsd_dp_preopened does, on
 * the dispatcher). Opened / wrong key / transit. */
static void preopen(void)
{
    fips_node_addr me = {{9}}, src = {{8}}, other = {{7}};
    uint8_t raw[32] = {5}, wrong_raw[32] = {6};
    fips_aead_key key, wrong;
    assert(fips_aead_key_init(&key, raw) == FIPS_AEAD_OK && fips_aead_key_init(&wrong, wrong_raw) == FIPS_AEAD_OK);
    fipsd_dp_epoch epoch = {.peer = src, .session = true, .tag = 77};
    fipsd_dp_snapshot *s = calloc(1, sizeof *s); assert(s);
    s->me = me; s->epochs = &epoch; s->epoch_count = 1;
    fipsd_dataplane d = {.packet_count = 1};
    d.packets = calloc(1, sizeof *d.packets); assert(d.packets);
    d.current = make_go_refptr(s, free); assert(d.current.ptr);
    uint8_t plain[40]; memset(plain, 0x3c, sizeof plain);
    for (int c = 0; c < 3; ++c) {
        fips_cipher_state_init(&epoch.recv, c == 1 ? &wrong : &key);
        uint8_t frame[FIPS_FRAMING_FSP_HEADER_SIZE + sizeof plain + FIPS_NOISE_TAG_SIZE]; size_t sealed;
        fips_fsp_build_header(5, 0, sizeof plain, frame);
        assert(fips_aead_seal(&key, 5, frame, FIPS_FRAMING_FSP_HEADER_SIZE, plain, sizeof plain,
            frame + FIPS_FRAMING_FSP_HEADER_SIZE, sizeof frame - FIPS_FRAMING_FSP_HEADER_SIZE, &sealed) == FIPS_AEAD_OK);
        fips_session_datagram dg; fips_session_datagram_init(&dg, src, c == 2 ? other : me, frame, sizeof frame);
        uint8_t enc[256]; fips_writer w; fips_writer_init(&w, enc, sizeof enc);
        assert(fips_session_datagram_encode(&dg, &w, NULL) == FIPS_OK);
        fipsd_dp_packet *p = fipsd_dp_packet_take(&d); assert(p && !p->fsp_tried);
        p->body = FIPS_FRAMING_FMP_ESTABLISHED_HEADER_SIZE;
        fips_writer inner; fips_writer_init(&inner, p->output + p->body, FIPSD_DP_WIRE - p->body);
        assert(fips_fmp_prepend_inner_header(1, enc, fips_writer_len(&w), &inner, NULL) == FIPS_OK);
        p->job.result = FIPS_NOISE_OK; p->job.output_size = fips_writer_len(&inner);
        p->job.input_size = p->job.output_size + FIPS_NOISE_TAG_SIZE; /* the link ciphertext it came from */
        fipsd_dp_worker_preopen(&p->job);
        assert(p->fsp_tried == (c != 2) && p->fsp_ok == (c == 0));
        if (c == 0) assert(p->fsp_plain == sizeof plain && !memcmp(p->input, plain, sizeof plain) && p->fsp_tag == 77 &&
            p->fsp_counter == 5 && p->fsp_body == FIPS_FRAMING_FSP_HEADER_SIZE && p->fsp_frame == sizeof frame &&
            !p->fsp_k && !memcmp(p->fsp_src.bytes, src.bytes, 16));
        fipsd_crypto_job_destroy(&p->job);
        fips_cipher_state_destroy(&epoch.recv);
    }
    fips_aead_key_destroy(&key); fips_aead_key_destroy(&wrong);
    go_refptr_release(&d.current); free(d.packets);
}
typedef struct blocked_send { handlers *h; GoChannel *cancel, *started; int result; } blocked_send;
static void *send_until_cancelled(void *arg)
{
    blocked_send *s = arg; fips_node_addr dest = {{2}};
    assert(!go_channel_send(s->started, s));
    s->result = fipsd_node_handlers_send_wait(s->h, &dest, 1, 2, (const uint8_t *)"blocked", 7, s->cancel);
    return NULL;
}
static void cancelled_mailbox(void)
{
    fipsd_node_handlers_config cfg; fipsd_node_handlers_config_default(&cfg); cfg.command_capacity = 1;
    handlers *h = fipsd_node_handlers_create(&cfg); assert(h);
    fips_node_addr dest = {{2}};
    assert(!fipsd_node_handlers_send_wait(h, &dest, 1, 2, NULL, 0, NULL));
    blocked_send s = {.h = h, .cancel = go_channel_create(1), .started = go_channel_create(1)};
    assert(s.cancel && s.started);
    pthread_t thread; assert(!pthread_create(&thread, NULL, send_until_cancelled, &s));
    void *v; assert(!go_channel_receive(s.started, &v) && v == &s);
    go_channel_close(s.cancel); assert(!pthread_join(thread, NULL));
    assert(s.result == -1 && go_channel_get_depth(h->commands) == 1);
    go_channel_free(s.cancel); go_channel_free(s.started); fipsd_node_handlers_destroy(h);
}
/* Stop the real dispatcher before driving its retained handler state with an
 * injected clock: there must still be exactly one writer. */
static void coalesced_snapshots(void)
{
    fips_test_clock clock = {0};
    fipsd_node_handlers_config cfg; fipsd_node_handlers_config_default(&cfg);
    cfg.peer_capacity = cfg.tree_peer_capacity = cfg.session_capacity = 4; cfg.cache_entries = 8;
    cfg.clock = fips_test_clock_adapter(&clock);
    handlers *h = fipsd_node_handlers_create(&cfg); assert(h);
    const char *yaml = "node: {identity: {persistent: false}}";
    fipsd_config *config = fipsd_config_parse(yaml, strlen(yaml), NULL); assert(config);
    GoSwap *configs = go_swap_create(config, fipsd_config_free); assert(configs);
    fipsd_node_options o = {0}; fipsd_node_handlers_install(h, &o);
    fipsd_node *node = fipsd_node_create(configs, &o); assert(node);
    fipsd_runtime_config rc; fipsd_runtime_config_default(&rc); rc.handle_signals = false; rc.workers = 4;
    fipsd_runtime *rt = fipsd_runtime_create(&rc); assert(rt);
    assert(!fipsd_runtime_register(rt, fipsd_node_subsystem(node)) && !fipsd_runtime_start(rt));
    fipsd_runtime_shutdown(rt); assert(fipsd_runtime_run(rt) == FIPSD_RUN_OK);
    h->running = true; h->publish_allowed = fips_test_instant(0);
    h->next_tick = fips_test_instant(1000);
    uint64_t generation = h->snapshot_generation;
    hs_mark_dirty(h); assert(hs_poll(h, fips_test_instant(0)));
    assert(h->snapshot_generation == ++generation && !h->dirty);
    for (uint64_t i = 1; i < 50; ++i) {
        clock.millis = i; hs_mark_dirty(h); assert(hs_poll(h, hs_now(h)));
        assert(h->snapshot_generation == generation && h->dirty);
        assert(hs_wait_ms(h, hs_now(h)) == 50 - i);
    }
    /* A state-change notice bypasses the coalescing deadline. */
    hs_notice(h, FIPSD_NOTICE_TREE_CHANGED, NULL, NULL, 0);
    assert(h->snapshot_generation == ++generation && !h->dirty);
    void *notice; assert(!go_channel_try_receive(h->notices, &notice)); fipsd_node_notice_free(notice);
    hs_mark_dirty(h); clock.millis = 50; assert(hs_poll(h, hs_now(h)));
    assert(h->snapshot_generation == ++generation && !h->dirty);
    /* The actual installed command callback must inspect kind before freeing
     * its owned command (ASan catches the previous post-free dirty check). */
    fips_peer_identity identity = {0};
    assert(!fipsd_node_handlers_learn_identity(h, &identity));
    void *command; assert(!go_channel_try_receive(h->commands, &command));
    assert(o.command(o.user, command) && h->dirty);
    clock.millis = 100; assert(hs_poll(h, hs_now(h)));
    assert(h->snapshot_generation == ++generation && !h->dirty);
    h->running = false;
    fipsd_runtime_destroy(rt); fipsd_node_destroy(node); fipsd_node_handlers_destroy(h); go_swap_free(configs);
}
int main(void)
{
    concurrent_recycle(); cancelled_mailbox(); exact_wipe(); worker_send_gate(); preopen();
    forwarding(); errors(); clamps(); backpressure_and_counters(); authenticate_then_mark(); saturated_trial_retry(); delayed_transport_consumer(); coalesced_snapshots();
    puts("dataplane: forwarding/local/TTL/CE, reject limiter, clamp release, 4-worker unique reserved counters, bounded refusal: ok");
    return 0;
}
