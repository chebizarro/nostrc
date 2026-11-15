#include "fakes/rekey_profile.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sodium.h>
#define VIEW(n) fips_rekey_get_view(&(n)->machine)
static const uint8_t epoch[8] = {1, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
static rp_node *nodes;
static fips_seeded_entropy rng;
static void init(void) {
    rng.state = 0x72656b6579;
    nodes = calloc(2, sizeof *nodes); assert(nodes);
    for (size_t k = 0; k < 2; ++k)
        assert(rp_init(&nodes[k], k, fips_seeded_entropy_adapter(&rng), epoch));
    nodes[0].remote = nodes[1].me; nodes[1].remote = nodes[0].me;
    nodes[0].with_coords = nodes[1].with_coords = true;
}
static void pair(const fips_rekey_profile *p) { init(); assert(rp_pair(&nodes[0], &nodes[1], p)); }
static void finish(void) {
    for (size_t k = 0; k < 2; ++k) {
        rp_destroy(&nodes[k]);
        assert(sodium_is_zero((const uint8_t *)&nodes[k], sizeof nodes[k]));
    }
    free(nodes); nodes = NULL;
    assert(fips_rekey_leaked() == 0);
}
static rp_packet pop(rp_node *n) {
    assert(n->queued);
    rp_packet p = n->queue[0];
    --n->queued;
    memmove(n->queue, n->queue + 1, n->queued * sizeof p);
    return p;
}
static void invariant(rp_node *n) {
    assert(fips_rekey_cleared_slots_are_zero(&n->machine));
    fips_rekey_binding bindings[3];
    size_t count = fips_rekey_bindings(&n->machine, bindings);
    assert(count == n->allocated);
    for (size_t i = 0; i < count; ++i) {
        assert(bindings[i].index < 256 && n->indices[bindings[i].index]);
        for (size_t j = 0; j < i; ++j) assert(bindings[i].index != bindings[j].index);
    }
}
static void pump(uint64_t *now) {
    size_t delivered = 0;
    while (nodes[0].queued || nodes[1].queued) {
        for (size_t k = 0; k < 2; ++k) while (nodes[k].queued) {
            assert(++delivered < 64);
            rp_packet p = pop(&nodes[k]);
            *now += 10;
            assert(rp_receive(&nodes[1-k], *now, &p));
            invariant(&nodes[1-k]);
        }
    }
}
static void start(rp_node *n, uint64_t now) {
    fips_rekey_outputs_clear(&n->out);
    fips_rekey_start_now(&n->machine, fips_test_instant(now), &n->env, &n->out);
    assert(rp_execute(n, now, NULL, 0));
    invariant(n);
}
static bool before(const rp_node *n, fips_rekey_output_kind a, fips_rekey_output_kind b) {
    size_t ai = SIZE_MAX, bi = SIZE_MAX;
    for (size_t i = 0; i < n->order_count; ++i) {
        if (n->order[i] == a && ai == SIZE_MAX) ai = i;
        if (n->order[i] == b && bi == SIZE_MAX) bi = i;
    }
    return ai < bi && bi != SIZE_MAX;
}
static void cycle(const fips_rekey_profile *p, size_t initiator) {
    pair(p);
    rp_node *a = &nodes[initiator], *b = &nodes[1-initiator];
    const fips_noise_session *old[2] = {fips_rekey_keys(&a->machine, FIPS_REKEY_Current),
                                       fips_rekey_keys(&b->machine, FIPS_REKEY_Current)};
    uint64_t now = 600000;
    start(a, now);
    rp_packet m1 = pop(a); now += 10; assert(rp_receive(b, now, &m1));
    rp_packet m2 = pop(b); now += 10; assert(rp_receive(a, now, &m2));
    const fips_noise_session *pending = fips_rekey_keys(&a->machine, FIPS_REKEY_Pending);
    assert(pending && VIEW(a).pending.k);
    if (p->indexed) {
        assert(before(a, FIPS_REKEY_OUT_Unbind, FIPS_REKEY_OUT_Bind));
        now += 1; assert(rp_poll(a, now));
        assert(before(a, FIPS_REKEY_OUT_Installed, FIPS_REKEY_OUT_Prompt));
    } else {
        /* Data cannot select pending while it waits for evidence. */
        fips_rekey_sealed s = {0};
        assert(fips_rekey_seal(&a->machine, fips_test_instant(now), &s) == FIPS_REKEY_SEAL_OK);
        assert(!s.k && s.tag == 0); fips_rekey_sealed_clear(&s);
        assert(sodium_is_zero((const uint8_t *)&s, sizeof s));
    }
    pump(&now);
    assert(VIEW(a).current.k && VIEW(b).current.k && a->installed == 1 && b->installed == 1);
    assert(fips_rekey_keys(&a->machine, FIPS_REKEY_Current) == pending);
    assert(fips_rekey_keys(&a->machine, FIPS_REKEY_Previous) == old[0]);
    for (size_t k = 0; k < 2; ++k) {
        rp_packet data; now += 10; assert(rp_data(&nodes[k], now, &data));
        assert(rp_execute(&nodes[k], now, NULL, 0));
        size_t deliveries = nodes[1-k].deliveries;
        now += 10; assert(rp_receive(&nodes[1-k], now, &data));
        assert(nodes[1-k].deliveries == deliveries + 1);
    }
    now += 11000;
    for (size_t k = 0; k < 2; ++k) { assert(rp_poll(&nodes[k], now)); invariant(&nodes[k]); }
    assert(!VIEW(a).previous.present && !VIEW(b).previous.present);
    for (size_t k = 0; k < 2; ++k) assert(sodium_is_zero((const uint8_t *)old[k], sizeof *old[k]));
    /* Replace/drain another full epoch; addresses remain arena-stable. */
    now = 1200000; start(b, now); pump(&now);
    if (p->indexed) { ++now; assert(rp_poll(b, now)); pump(&now); }
    assert(!VIEW(a).current.k && !VIEW(b).current.k);
    finish();
}
static void crossing(const fips_rekey_profile *p) {
    pair(p); uint64_t now = 600000;
    start(&nodes[0], now); start(&nodes[1], now); pump(&now);
    for (unsigned i = 0; i < 12 && (!VIEW(&nodes[0]).current.k || !VIEW(&nodes[1]).current.k); ++i) {
        now = 606000 + i * 1000;
        for (size_t k = 0; k < 2; ++k) assert(rp_poll(&nodes[k], now));
        pump(&now);
    }
    assert(VIEW(&nodes[0]).current.k && VIEW(&nodes[1]).current.k);
    rp_packet data; ++now; assert(rp_data(&nodes[0], now, &data));
    assert(rp_execute(&nodes[0], now, NULL, 0));
    ++now; size_t d = nodes[1].deliveries; assert(rp_receive(&nodes[1], now, &data));
    assert(nodes[1].deliveries == d + 1);
    finish();
}
static void loss(const fips_rekey_profile *p) {
    pair(p); uint64_t now = 600000;
    start(&nodes[0], now); rp_packet orig = pop(&nodes[0]);
    const unsigned rungs[] = {1000, 3000, 7000, 15000};
    for (size_t i = 0; i < sizeof rungs / sizeof *rungs; ++i) {
        assert(rp_poll(&nodes[0], now + rungs[i]));
        assert(nodes[0].queued == 1);
        rp_packet retry = pop(&nodes[0]);
        assert(retry.len == orig.len && !memcmp(retry.bytes, orig.bytes, orig.len));
        invariant(&nodes[0]);
    }
    assert(rp_poll(&nodes[0], now + 30000));
    assert(VIEW(&nodes[0]).cycle == FIPS_REKEY_Idle && !nodes[0].queued);
    assert(nodes[0].allocated == (p->indexed ? 1u : 0u));
    /* Backoff is machine-owned, and a later retry uses fresh entropy. */
    fips_instant wake = fips_rekey_next_wake(&nodes[0].machine);
    assert(wake.ops && wake.opaque[0] > now + 30000);
    assert(rp_poll(&nodes[0], wake.opaque[0]));
    assert(nodes[0].queued == 1);
    rp_packet fresh = pop(&nodes[0]);
    assert(fresh.len == orig.len && memcmp(fresh.bytes, orig.bytes, orig.len));
    finish();
}
static void immutable_forks(void) {
    init();
    const fips_rekey_profile *profiles[] = {&fips_fmp_rekey_profile, &fips_fsp_rekey_profile};
    for (size_t k = 0; k < 2; ++k) {
        const fips_rekey_profile *p = profiles[k];
        assert(fips_rekey_profile_valid(p));
        fips_rekey_hs hs = {0}, responder = {0}, snapshot;
        fips_rekey_wire msg1 = {0}, msg2 = {0};
        assert(p->initiate(&nodes[0].local, &nodes[1].peer, (fips_rekey_index){12, true}, &hs, &msg1));
        snapshot = hs; /* Test-only byte snapshot; destroyed as an independent owner. */
        fips_fmp_ik_msg1 token = {0};
        fips_rekey_completed done = {0};
        if (p->indexed) {
            assert(fips_fmp_rekey_read_msg1(&nodes[1].local, msg1.bytes, msg1.len, &token));
            assert(p->msg1_answer(&token, &nodes[1].local, (fips_rekey_index){13, true}, &done, &msg2));
            fips_rekey_completed_clear(&done);
        } else assert(p->answer(&nodes[1].local, msg1.bytes, msg1.len, &responder, &msg2));
        uint8_t original = msg2.bytes[msg2.len - 1]; msg2.bytes[msg2.len - 1] ^= 1;
        fips_rekey_read read = {0};
        assert(!p->reply(&hs, &nodes[0].local, msg2.bytes, msg2.len, &read));
        assert(!memcmp(&snapshot, &hs, sizeof hs));
        fips_rekey_completed_clear(&read.done); sodium_memzero(&read, sizeof read);
        msg2.bytes[msg2.len - 1] = original;
        assert(p->reply(&hs, &nodes[0].local, msg2.bytes, msg2.len, &read));
        assert(!memcmp(&snapshot, &hs, sizeof hs));
        if (!p->indexed) {
            fips_rekey_hs rsnapshot = responder;
            read.fin.bytes[read.fin.len - 1] ^= 1;
            assert(!p->complete(&responder, read.fin.bytes, read.fin.len, &done));
            assert(!memcmp(&responder, &rsnapshot, sizeof responder));
            read.fin.bytes[read.fin.len - 1] ^= 1;
            assert(p->complete(&responder, read.fin.bytes, read.fin.len, &done));
            fips_rekey_hs_clear(&rsnapshot); fips_rekey_completed_clear(&done);
        }
        fips_rekey_completed_clear(&read.done); sodium_memzero(&read, sizeof read);
        fips_fmp_rekey_msg1_clear(&token); assert(sodium_is_zero((const uint8_t *)&token, sizeof token));
        fips_rekey_hs_clear(&hs); fips_rekey_hs_clear(&snapshot); fips_rekey_hs_clear(&responder);
        assert(sodium_is_zero((const uint8_t *)&hs, sizeof hs));
    }
    finish();
}
static void ik_rewritten_index(void) {
    pair(&fips_fmp_rekey_profile); start(&nodes[0], 600000);
    rp_packet orig = pop(&nodes[0]), rewritten = orig;
    assert(rp_receive(&nodes[1], 600010, &orig)); nodes[1].queued = 0;
    rewritten.bytes[4] ^= 0x20; /* wire index is outside Noise; ephemeral proof is not */
    fips_fmp_ik_msg1 token = {0}, token2 = {0};
    assert(fips_fmp_rekey_read_msg1(&nodes[1].local, orig.bytes, orig.len, &token));
    assert(fips_fmp_rekey_read_msg1(&nodes[1].local, rewritten.bytes, rewritten.len, &token2));
    assert(!memcmp(token.proof.ephemeral, token2.proof.ephemeral, 32));
    assert(memcmp(token.proof.digest, token2.proof.digest, 32));
    fips_rekey_record record;
    assert(fips_fmp_rekey_setup_record(orig.bytes, orig.len, NULL, &record));
    fips_rekey_digest ephemeral;
    memcpy(ephemeral.bytes, token.proof.ephemeral, 32);
    assert(fips_rekey_record_holds(&record, ephemeral));
    assert(rp_receive(&nodes[1], 600020, &rewritten));
    assert(!nodes[1].queued && nodes[1].allocated == 2);
    fips_fmp_rekey_msg1_clear(&token); fips_fmp_rekey_msg1_clear(&token2);
    finish();
}
static void restart(const fips_rekey_profile *p) {
    pair(p);
    const fips_noise_session *old = fips_rekey_keys(&nodes[0].machine, FIPS_REKEY_Current);
    nodes[1].local.epoch[0] = 42;
    uint64_t now = 600000; start(&nodes[0], now);
    rp_packet setup = pop(&nodes[0]); assert(rp_receive(&nodes[1], now + 10, &setup));
    rp_packet reply = pop(&nodes[1]); assert(rp_receive(&nodes[0], now + 20, &reply));
    if (p->indexed) assert(rp_poll(&nodes[0], now + 21));
    assert(before(&nodes[0], FIPS_REKEY_OUT_Retired, FIPS_REKEY_OUT_Installed));
    assert(before(&nodes[0], FIPS_REKEY_OUT_Installed, FIPS_REKEY_OUT_PeerRestarted));
    assert(VIEW(&nodes[0]).peer_epoch[0] == 42 && !VIEW(&nodes[0]).previous.present);
    assert(sodium_is_zero((const uint8_t *)old, sizeof *old)); invariant(&nodes[0]);
    finish();
}
static void coords_on_resend(void) {
    pair(&fips_fsp_rekey_profile); start(&nodes[0], 600000);
    rp_packet original = pop(&nodes[0]);
    uint8_t saved = nodes[0].remote.bytes[0]; nodes[0].remote.bytes[0] ^= 1;
    assert(rp_poll(&nodes[0], 601000));
    rp_packet retry = pop(&nodes[0]); nodes[0].remote.bytes[0] = saved;
    assert(original.len == retry.len && memcmp(original.bytes, retry.bytes, original.len));
    fips_reader r1, r2;
    fips_reader_init(&r1, original.bytes + 4, original.len - 4);
    fips_reader_init(&r2, retry.bytes + 4, retry.len - 4);
    fips_fsp_session_setup a, b;
    assert(fips_fsp_session_setup_decode_strict(&r1, &a, NULL) == FIPS_OK);
    assert(fips_fsp_session_setup_decode_strict(&r2, &b, NULL) == FIPS_OK);
    assert(a.handshake_payload.len == b.handshake_payload.len &&
        !memcmp(a.handshake_payload.data, b.handshake_payload.data, a.handshake_payload.len));
    assert(memcmp(a.dest_coords.addrs, b.dest_coords.addrs, 16));
    finish();
}
static void close_and_failed_send(void) {
    pair(&fips_fmp_rekey_profile); start(&nodes[0], 600000);
    /* Abandon at timeout still unbinds/releases when all network sends fail. */
    nodes[0].queued = 0; nodes[0].fail_send = true;
    assert(!rp_poll(&nodes[0], 601000)); invariant(&nodes[0]);
    assert(rp_poll(&nodes[0], 630000)); invariant(&nodes[0]);
    assert(nodes[0].allocated == 1);
    finish();
}
static void table_first_handshake(void) {
    init();
    fips_session_entry *storage = calloc(2, sizeof *storage); assert(storage);
    fips_session_table tables[2];
    fips_session_entry *entry[2] = {0};
    for (size_t k = 0; k < 2; ++k) fips_session_table_init(&tables[k], &storage[k], 1);
    assert(fips_session_table_open(&tables[0], nodes[1].peer, nodes[0].me,
        fips_rekey_params_fsp(), fips_test_instant(0), &nodes[0].env, &nodes[0].out, &entry[0]) == FIPS_OK);
    assert(tables[0].count == 1 && fips_session_table_by_peer(&tables[0], nodes[1].me) == entry[0]);
    assert(fips_session_table_half_open(&tables[0]) == 0); /* own attempts aren't untrusted setups */
    assert(rp_execute_machine(&nodes[0], &entry[0]->machine, 0, NULL, 0));
    rp_packet setup = pop(&nodes[0]);
    fips_reader r; fips_reader_init(&r, setup.bytes + 4, setup.len - 4);
    fips_fsp_session_setup msg;
    assert(fips_fsp_session_setup_decode_strict(&r, &msg, NULL) == FIPS_OK);
    fips_rekey_input in = {.kind = FIPS_REKEY_IN_Setup, .bytes = msg.handshake_payload.data,
        .len = msg.handshake_payload.len, .carrier = nodes[0].me, .route = 1};
    assert(fips_session_table_answer(&tables[1], nodes[0].me, nodes[1].me,
        fips_rekey_params_fsp(), fips_test_instant(10), in, &nodes[1].env, &nodes[1].out, &entry[1]) == FIPS_OK);
    assert(fips_session_table_half_open(&tables[1]) == 1);
    assert(rp_execute_machine(&nodes[1], &entry[1]->machine, 10, NULL, 0));
    assert(!fips_session_table_evict(&tables[1], fips_test_instant(100), &nodes[1].out, &(fips_rekey_carry){0}));
    fips_session_entry *sentinel = (fips_session_entry *)(uintptr_t)1;
    fips_peer_key other = {{0x22}};
    assert(fips_session_table_open(&tables[0], other, nodes[0].me, fips_rekey_params_fsp(),
        fips_test_instant(10), &nodes[0].env, &nodes[0].out, &sentinel) == FIPS_ERR_RANGE);
    assert(sentinel == (fips_session_entry *)(uintptr_t)1 && tables[0].count == 1 && !nodes[0].out.count);
    uint64_t now = 10;
    unsigned delivered = 0;
    while (nodes[0].queued || nodes[1].queued) for (size_t k = 0; k < 2; ++k) while (nodes[k].queued) {
        assert(++delivered < 16); rp_packet packet = pop(&nodes[k]); now += 10;
        rp_node *n = &nodes[1-k];
        assert(fips_fsp_rekey_receive(&entry[1-k]->machine, fips_test_instant(now), nodes[k].me, 1,
            packet.bytes, packet.len, &n->env, &n->out) == FIPS_OK);
        assert(rp_execute_machine(n, &entry[1-k]->machine, now, packet.bytes, packet.len));
    }
    for (size_t k = 0; k < 2; ++k) {
        assert(entry[k] == &storage[k] && fips_rekey_get_view(&entry[k]->machine).current.present);
        assert(fips_session_table_half_open(&tables[k]) == 0);
        assert(fips_rekey_cleared_slots_are_zero(&entry[k]->machine));
        fips_session_note_activity(entry[k], fips_test_instant(1000));
        assert(!fips_session_table_evict(&tables[k], fips_test_instant(999), &nodes[k].out, &(fips_rekey_carry){0}));
        assert(fips_session_table_evict(&tables[k], fips_test_instant(1000), &nodes[k].out, &(fips_rekey_carry){0}));
        assert(rp_execute_machine(&nodes[k], &storage[k].machine, now, NULL, 0));
        assert(!tables[k].count && sodium_is_zero((const uint8_t *)&storage[k], sizeof storage[k]));
    }
    free(storage); finish();
}
static void table_indices(void) {
    init(); fips_rekey_completed done[2] = {0};
    assert(rp_handshake(&nodes[0], &nodes[1], &fips_fmp_rekey_profile, done));
    fips_session_entry *storage = calloc(2, sizeof *storage); assert(storage);
    fips_session_table t; fips_session_table_init(&t, storage, 2);
    fips_rekey_ids ids = {.transport = 17, .theirs = done[0].theirs};
    assert(fips_rekey_allocate(nodes[0].env.indices, &ids.lease));
    uint32_t index = ids.lease.index;
    fips_rekey_setup_facts facts = {.has_peer_epoch = true,
        .started = fips_test_instant(0), .link_since = fips_test_instant(0)};
    memcpy(facts.peer_epoch, epoch, 8);
    fips_session_entry *entry = NULL;
    assert(fips_session_table_established(&t, &fips_fmp_rekey_profile, nodes[0].me, fips_rekey_params_fmp(),
        &done[0], &ids, &facts, fips_test_instant(0), &nodes[0].env, &nodes[0].out, &entry) == FIPS_OK);
    assert(rp_execute_machine(&nodes[0], &entry->machine, 0, NULL, 0));
    assert(fips_session_table_by_index(&t, 17, index) == entry && !fips_session_table_by_index(&t, 18, index));
    fips_session_entry *unchanged = NULL;
    assert(fips_session_table_open(&t, nodes[1].peer, nodes[0].me, fips_rekey_params_fsp(),
        fips_test_instant(1), &nodes[0].env, &nodes[0].out, &unchanged) == FIPS_ERR_RANGE);
    assert(!unchanged && !nodes[0].out.count);
    fips_rekey_start_now(&entry->machine, fips_test_instant(600000), &nodes[0].env, &nodes[0].out);
    assert(rp_execute_machine(&nodes[0], &entry->machine, 600000, NULL, 0));
    fips_rekey_binding bindings[3]; size_t count = fips_rekey_bindings(&entry->machine, bindings);
    assert(count == 2);
    for (size_t i = 0; i < count; ++i) assert(fips_session_table_by_index(&t, bindings[i].transport, bindings[i].index) == entry);
    assert(!fips_session_table_evict(&t, fips_test_instant(700000), &nodes[0].out, &(fips_rekey_carry){0}));
    /* Establish the real responder and follow every index through attempt,
     * pending, cutover/previous and retirement without a parallel index cache. */
    fips_rekey_ids theirs = {.transport = 7, .theirs = {index, true}};
    nodes[1].next_index = 20;
    assert(fips_rekey_allocate(nodes[1].env.indices, &theirs.lease));
    fips_rekey_setup_facts peer_facts = {.has_peer_epoch = true,
        .started = fips_test_instant(0), .link_since = fips_test_instant(0)};
    memcpy(peer_facts.peer_epoch, epoch, 8);
    assert(fips_rekey_established(&nodes[1].machine, &fips_fmp_rekey_profile, fips_rekey_params_fmp(),
        (fips_rekey_bond){.kind = FIPS_REKEY_BOND_Key, .key = nodes[0].peer}, nodes[1].me,
        &done[1], &theirs, &peer_facts, fips_test_instant(0), &nodes[1].env, &nodes[1].out) == FIPS_OK);
    assert(rp_execute(&nodes[1], 0, NULL, 0));
    rp_packet msg1 = pop(&nodes[0]); assert(rp_receive(&nodes[1], 600010, &msg1));
    rp_packet msg2 = pop(&nodes[1]);
    assert(rp_receive_machine(&nodes[0], &entry->machine, 600020, &msg2));
    count = fips_rekey_bindings(&entry->machine, bindings); assert(count == 2);
    for (size_t i = 0; i < count; ++i) assert(fips_session_table_by_index(&t, bindings[i].transport, bindings[i].index) == entry);
    assert(fips_rekey_step(&entry->machine, fips_test_instant(600021),
        (fips_rekey_input){.kind = FIPS_REKEY_IN_Poll}, &nodes[0].env, &nodes[0].out) == FIPS_OK);
    assert(rp_execute_machine(&nodes[0], &entry->machine, 600021, NULL, 0));
    assert(fips_session_table_by_index(&t, 17, index) == entry); /* Previous binding */
    uint64_t now = 600021;
    while (nodes[0].queued || nodes[1].queued) {
        if (nodes[0].queued) { rp_packet p = pop(&nodes[0]); now += 10; assert(rp_receive(&nodes[1], now, &p)); }
        if (nodes[1].queued) { rp_packet p = pop(&nodes[1]); now += 10; assert(rp_receive_machine(&nodes[0], &entry->machine, now, &p)); }
    }
    assert(fips_rekey_step(&entry->machine, fips_test_instant(611000),
        (fips_rekey_input){.kind = FIPS_REKEY_IN_Poll}, &nodes[0].env, &nodes[0].out) == FIPS_OK);
    assert(rp_execute_machine(&nodes[0], &entry->machine, 611000, NULL, 0));
    assert(!fips_session_table_by_index(&t, 17, index));
    assert(fips_session_table_remove(&t, entry, FIPS_REKEY_CLOSE_Shutdown, &nodes[0].out, &(fips_rekey_carry){0}));
    assert(rp_execute_machine(&nodes[0], &entry->machine, 611000, NULL, 0));
    assert(!nodes[0].allocated && !fips_session_table_by_index(&t, 17, index));
    assert(sodium_is_zero((const uint8_t *)entry, sizeof *entry));
    for (size_t k = 0; k < 2; ++k) fips_rekey_completed_clear(&done[k]);
    free(storage); finish();
}
static void table_oldest_and_reuse(void) {
    init();
    rp_node *third = calloc(1, sizeof *third); assert(third);
    assert(rp_init(third, 2, fips_seeded_entropy_adapter(&rng), epoch));
    fips_session_entry *storage = calloc(2, sizeof *storage); assert(storage);
    fips_session_table table; fips_session_table_init(&table, storage, 2);
    rp_node *peers[2] = {&nodes[1], third};
    fips_session_entry *entries[2] = {0};
    for (size_t k = 0; k < 2; ++k) {
        fips_rekey_completed done[2] = {0}; assert(rp_handshake(&nodes[0], peers[k], &fips_fsp_rekey_profile, done));
        fips_rekey_ids ids = {0};
        fips_rekey_setup_facts facts = {.started = fips_test_instant(0), .link_since = fips_test_instant(0)};
        assert(fips_session_table_established(&table, &fips_fsp_rekey_profile, nodes[0].me,
            fips_rekey_params_fsp(), &done[0], &ids, &facts, fips_test_instant(0),
            &nodes[0].env, &nodes[0].out, &entries[k]) == FIPS_OK);
        assert(rp_execute_machine(&nodes[0], &entries[k]->machine, 0, NULL, 0));
        fips_session_note_activity(entries[k], fips_test_instant(k ? 10 : 20));
        for (size_t j = 0; j < 2; ++j) fips_rekey_completed_clear(&done[j]);
    }
    assert(table.count == 2);
    assert(fips_session_table_evict(&table, fips_test_instant(10), &nodes[0].out, &(fips_rekey_carry){0}));
    assert(rp_execute_machine(&nodes[0], &storage[1].machine, 10, NULL, 0));
    assert(entries[0]->occupied && !entries[1]->occupied && table.count == 1);
    assert(!fips_session_table_by_peer(&table, third->me));
    assert(fips_session_table_by_peer(&table, nodes[1].me) == entries[0]);
    fips_session_entry *reused = NULL;
    assert(fips_session_table_open(&table, third->peer, nodes[0].me, fips_rekey_params_fsp(),
        fips_test_instant(21), &nodes[0].env, &nodes[0].out, &reused) == FIPS_OK);
    assert(reused == entries[1] && table.count == 2);
    assert(rp_execute_machine(&nodes[0], &reused->machine, 21, NULL, 0));
    for (size_t k = 0; k < 2; ++k) {
        assert(fips_session_table_remove(&table, &storage[k], FIPS_REKEY_CLOSE_Shutdown,
            &nodes[0].out, &(fips_rekey_carry){0}));
        assert(rp_execute_machine(&nodes[0], &storage[k].machine, 21, NULL, 0));
        assert(sodium_is_zero((const uint8_t *)&storage[k], sizeof storage[k]));
    }
    assert(!table.count); free(storage); rp_destroy(third); free(third); finish();
}
int main(void) {
    assert(sodium_init() >= 0);
    immutable_forks();
    for (size_t k = 0; k < 2; ++k) {
        const fips_rekey_profile *p = k ? &fips_fsp_rekey_profile : &fips_fmp_rekey_profile;
        cycle(p, 0); cycle(p, 1); crossing(p); loss(p); restart(p);
    }
    coords_on_resend(); ik_rewritten_index(); close_and_failed_send(); table_first_handshake(); table_indices(); table_oldest_and_reuse();
    puts("real IK/XK rekey profiles: 17 scenarios passed");
    return 0;
}
