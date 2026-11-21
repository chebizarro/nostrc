#include "fakes/rekey.h"
#include <sodium.h>
#include <string.h>
/* Two peers, deterministic time, arbitrary delivery/drop/reorder/corruption.
 * The stub is only a handshake oracle; slot encryption is real Noise AEAD. */
static rk_test_node nodes[2];
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (sodium_init() < 0 || size == 0)
        return 0;
    const fips_rekey_profile *profile = (data[0] & 1) ? &rk_test_ik : &rk_test_xk;
    fips_rekey_config cfg =
        profile->messages == 2 ? fips_rekey_config_fmp() : fips_rekey_config_fsp();
    cfg.messages = 8;
    cfg.soft = 16;
    cfg.hard = 32;
    rk_test_pair(&nodes[0], &nodes[1], profile, cfg);
    uint64_t now = 0;
    for (size_t i = 1; i + 2 < size && i < 769; i += 3) {
        rk_test_node *n = &nodes[data[i] & 1], *peer = &nodes[(data[i] & 1) ^ 1];
        now += (uint64_t)data[i + 1] * 1000;
        unsigned op = (data[i] >> 1) % 7;
        if (!n->machine.live) {
            fips_rekey_outputs out = {0};
            fips_rekey_input in = {.kind = FIPS_REKEY_IN_Poll};
            assert(fips_rekey_step(&n->machine, fips_test_instant(now), in, &n->env, &out) ==
                   FIPS_ERR_RANGE);
            assert(sodium_is_zero((const uint8_t *)&n->machine, sizeof n->machine) && !out.count);
            continue;
        }
        fips_rekey_view before = fips_rekey_get_view(&n->machine);
        uint64_t proofs = rk_test_completions();
        /* Bound the harness's network independently of the machine's state. */
        if (n->wires > RK_TEST_WIRE - 8)
            n->wires = 0;
        if (op == 0)
            rk_test_poll(n, now);
        else if (op == 1 && peer->wires) {
            size_t at = data[i + 2] % peer->wires;
            rk_test_packet p = peer->wire[at];
            memmove(peer->wire + at, peer->wire + at + 1, (peer->wires - at - 1) * sizeof p);
            --peer->wires;
            if ((data[i + 2] & 0x80) && p.len)
                p.bytes[data[i + 1] % p.len] ^= 1;
            rk_test_recv(n, now, &p);
        } else if (op == 2) {
            fips_rekey_sealed s = {0};
            /* data() reserves the counter itself; inspect the count first. */
            if (fips_rekey_sent(&n->machine) < cfg.hard && before.current.present) {
                const uint8_t payload[] = {0x52, 1, 2, 3, 4, 5};
                rk_test_packet p = rk_test_data(n, now, payload, sizeof payload);
                n->wire[n->wires++] = p;
            } else {
                assert(fips_rekey_seal(&n->machine, fips_test_instant(now), &s) !=
                       FIPS_REKEY_SEAL_OK);
            }
        } else if (op == 3) {
            rk_test_step(n, now,
                         (fips_rekey_input){.kind = FIPS_REKEY_IN_PathFacts,
                                            .reachable = (data[i + 2] & 1) != 0,
                                            .has_srtt = true,
                                            .srtt = {data[i + 2] % 30, 0}});
        } else if (op == 4)
            rk_test_step(n, now, (fips_rekey_input){.kind = FIPS_REKEY_IN_DataSent});
        else if (op == 5) {
            if (peer->wires)
                --peer->wires;
        } else if (op == 6)
            rk_test_close(n);
        if (n->machine.live) {
            fips_rekey_view after = fips_rekey_get_view(&n->machine);
            if (after.current.present && after.current.tag != before.current.tag) {
                /* Frame/timer adoption can only install an already completed
                 * pending; reversion can only restore the held previous. */
                assert(rk_test_completions() > proofs ||
                       (before.pending.present && after.current.tag == before.pending.tag) ||
                       (before.previous.present && after.current.tag == before.previous.tag));
            }
            if (after.pending.present &&
                (!before.pending.present || after.pending.tag != before.pending.tag))
                assert(rk_test_completions() > proofs ||
                       (before.current.present && after.pending.tag == before.current.tag));
        }
        rk_test_conserved(n);
    }
    rk_test_close(&nodes[0]);
    rk_test_close(&nodes[1]);
    return 0;
}
