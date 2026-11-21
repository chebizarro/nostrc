#include "fakes/rekey_profile.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sodium.h>
static void invariant(rp_node *n) {
    fips_rekey_binding bindings[3];
    size_t count = fips_rekey_bindings(&n->machine, bindings);
    assert(count == n->allocated && count <= 3);
    fips_rekey_view v = fips_rekey_get_view(&n->machine);
    assert(v.candidates <= n->machine.profile->candidates);
    for (size_t i = 0; i < count; ++i) {
        assert(bindings[i].index < 256 && n->indices[bindings[i].index]);
        for (size_t j = 0; j < i; ++j) assert(bindings[i].index != bindings[j].index);
    }
    for (size_t i = 0; i < 3; ++i)
        if (!n->machine.used[i]) assert(sodium_is_zero((const uint8_t *)&n->machine.sessions[i], sizeof n->machine.sessions[i]));
}
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (!size) return 0;
    assert(sodium_init() >= 0);
    rp_node *n = calloc(2, sizeof *n); assert(n);
    fips_seeded_entropy rng = {0x72656b6579};
    const uint8_t epoch[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    for (size_t i = 0; i < 2; ++i) {
        assert(rp_init(&n[i], i, fips_seeded_entropy_adapter(&rng), epoch));
        n[i].with_coords = true;
    }
    const fips_rekey_profile *p = data[0] & 1 ? &fips_fmp_rekey_profile : &fips_fsp_rekey_profile;
    assert(rp_pair(&n[0], &n[1], p));
    uint64_t now = 600000;
    (void)rp_poll(&n[0], now);
    if (data[0] & 2) (void)rp_poll(&n[1], now);
    /* Production trigger only; no FIPS_TEST_HOOKS or stub cryptography. */
    for (size_t i = 1; i < size && i < 512; ++i) {
        size_t who = (data[i] >> 3) & 1, other = 1 - who;
        rp_node *node = &n[who];
        now += 1 + (uint64_t)(data[i] >> 4) * 1000;
        for (size_t k = 0; k < 2; ++k) if (n[k].queued > 8) n[k].queued = 8; /* loss */
        switch (data[i] & 7) {
        case 0: (void)rp_poll(node, now); break;
        case 1:
        case 2:
            if (n[other].queued) {
                size_t selected = data[i] % n[other].queued;
                rp_packet packet = n[other].queue[selected];
                memmove(n[other].queue + selected, n[other].queue + selected + 1,
                    (--n[other].queued - selected) * sizeof packet);
                if ((data[i] & 7) == 2 && packet.len) packet.bytes[data[i] % packet.len] ^= 0x80;
                (void)rp_receive(node, now, &packet);
            }
            break;
        case 3: {
            rp_packet packet = {0};
            if (rp_data(&n[other], now, &packet)) {
                (void)rp_execute(&n[other], now, NULL, 0);
                (void)rp_receive(node, now, &packet);
            }
            break;
        }
        case 4: {
            rp_packet packet = {0};
            packet.len = size - i < sizeof packet.bytes ? size - i : sizeof packet.bytes;
            memcpy(packet.bytes, data + i, packet.len);
            (void)rp_receive(node, now, &packet);
            break;
        }
        case 5:
            fips_rekey_outputs_clear(&node->out);
            assert(fips_rekey_step(&node->machine, fips_test_instant(now),
                (fips_rekey_input){.kind = FIPS_REKEY_IN_PathFacts, .reachable = (data[i] & 0x20) != 0},
                &node->env, &node->out) == FIPS_OK);
            (void)rp_execute(node, now, NULL, 0);
            break;
        case 6: if (node->queued) --node->queued; break;
        case 7: {
            fips_instant wake = fips_rekey_next_wake(&node->machine);
            if (wake.ops && wake.opaque[0] > now) now = wake.opaque[0];
            (void)rp_poll(node, now); break;
        }
        }
        invariant(&n[0]); invariant(&n[1]);
    }
    for (size_t i = 0; i < 2; ++i) rp_destroy(&n[i]);
    free(n);
    assert(!fips_rekey_leaked());
    return 0;
}
