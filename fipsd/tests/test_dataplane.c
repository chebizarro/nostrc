#ifdef NDEBUG
#undef NDEBUG /* Test operations and correctness assertions run in optimized smoke builds too. */
#endif
#include "node/handlers/internal.h"
#include "fakes/host.h"
#include "node/crypto_worker.h"
#include "node/transport_loopback.h"
#include <assert.h>
#include <stdio.h>

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
    fipsd_dp_cached cached = {.addr = remote, .expires = 100};
    uint8_t ca[3][16]; memcpy(ca[0], remote.bytes, 16); memcpy(ca[1], peer.bytes, 16); memcpy(ca[2], root.bytes, 16);
    assert(fips_cached_coord_from_addrs(&cached.coords, (const uint8_t (*)[16])ca, 3) == FIPS_OK);
    s.cache = &cached; s.cache_count = s.peer_count = 1;
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
    cached.expires = 0;
    assert(fipsd_dp_forward(&s, &router, &dg, NULL, false, fips_test_instant(0), scratch, view, &w, &out) == FIPS_OK && out.kind == FIPS_ROUTE_NoRoute);
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
    for (size_t i = 0; i < d.packet_count; ++i) assert(fipsd_dp_packet_take(&d));
    /* Original completion is still borrowed by the node, and must not be
     * recycled/resubmitted underneath it. Full occupancy refuses the trial,
     * counts pressure and leaves the original slot/reference for its owner. */
    assert(!fipsd_dp_open_retry(&h, &d.packets[0]));
    assert(d.stats.backpressure == 1 && atomic_load(&d.packets[0].busy));
    for (size_t i = 0; i < d.packet_count; ++i) fipsd_crypto_job_destroy(&d.packets[i].job);
    for (size_t i = 0; i < d.packet_count; ++i) assert(!atomic_load(&d.packets[i].busy));
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
int main(void)
{
    forwarding(); errors(); clamps(); backpressure_and_counters(); authenticate_then_mark(); saturated_trial_retry(); delayed_transport_consumer();
    puts("dataplane: forwarding/local/TTL/CE, reject limiter, clamp release, 4-worker unique reserved counters, bounded refusal: ok");
    return 0;
}
