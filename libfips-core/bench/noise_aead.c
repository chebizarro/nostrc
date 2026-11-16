/* Upstream noise_aead.rs: real XK pair (setup excluded), 16-byte AAD,
 * 64/1400-byte payloads, 1024 presealed packets and replay reset on wrap.
 * C session outputs are caller-owned; Rust's session path allocates a Vec. */
#include "bench.h"
#include "../tests/fakes/host.h"
#include <fips/noise/session.h>
#include <string.h>
#define RING 1024u
#define OK(expr) REQUIRE((expr) == FIPS_NOISE_OK)
static void finish(fips_handshake_state *hs, fips_handshake_role role, fips_noise_session *s)
{
    fips_aead_key send, recv;
    uint8_t h[32], remote[33], epoch[8];
    OK(fips_handshake_finish(hs, &send, &recv, h, remote, epoch));
    OK(fips_noise_session_from_handshake(s, role, &send, &recv, h, remote, epoch));
}
static void pair(fips_noise_session *tx, fips_noise_session *rx)
{
    fips_seeded_entropy seed = {42};
    fips_entropy entropy = fips_seeded_entropy_adapter(&seed);
    fips_secp *secp;
    fips_identity *a, *b;
    REQUIRE(fips_identity_init_secp(entropy, &secp) == FIPS_IDENTITY_OK);
    REQUIRE(fips_identity_generate(secp, entropy, &a) == FIPS_IDENTITY_OK);
    REQUIRE(fips_identity_generate(secp, entropy, &b) == FIPS_IDENTITY_OK);
    uint8_t pub[33], ea[8], eb[8], msg[128]; size_t len;
    memset(ea, 1, sizeof(ea)); memset(eb, 2, sizeof(eb));
    fips_identity_pubkey_full(b, pub);
    fips_handshake_state i, r;
    OK(fips_handshake_new_xk_initiator(&i, secp, a, pub, ea, entropy));
    OK(fips_handshake_new_xk_responder(&r, secp, b, eb, entropy));
    OK(fips_handshake_write_xk_message_1(&i, msg, sizeof(msg), &len));
    OK(fips_handshake_read_xk_message_1(&r, msg, len));
    OK(fips_handshake_write_xk_message_2(&r, msg, sizeof(msg), &len));
    OK(fips_handshake_read_xk_message_2(&i, msg, len));
    OK(fips_handshake_write_xk_message_3(&i, msg, sizeof(msg), &len));
    OK(fips_handshake_read_xk_message_3(&r, msg, len));
    finish(&i, FIPS_HANDSHAKE_INITIATOR, tx);
    finish(&r, FIPS_HANDSHAKE_RESPONDER, rx);
    fips_identity_destroy(a); fips_identity_destroy(b); fips_identity_destroy_secp(secp);
}
static void run(size_t size)
{
    uint8_t header[16], payload[1400], wire[16 + 1400 + 16], plain[1400];
    memset(header, 0x5a, sizeof(header)); memset(payload, 0xa5, sizeof(payload));
    size_t clen = size + 16, got;
    uint8_t *sealed = malloc(RING * clen); REQUIRE(sealed);
    char name[80];
    fips_noise_session tx, rx;
    pair(&tx, &rx);
    snprintf(name, sizeof(name), "session/seal/%zu", size);
    BENCH(name, {
        OK(fips_noise_session_seal(&tx, header, 16, payload, size, wire, sizeof(wire), &got));
        bench_observe(wire);
    });
    fips_noise_session_destroy(&tx); fips_noise_session_destroy(&rx);
    pair(&tx, &rx);
    for (size_t j = 0; j < RING; ++j)
        OK(fips_noise_session_seal(&tx, header, 16, payload, size, sealed + j * clen, clen, &got));
    size_t k = 0;
    snprintf(name, sizeof(name), "session/open/%zu", size);
    BENCH(name, {
        if (k == RING) { fips_noise_session_reset_replay_window(&rx); k = 0; }
        OK(fips_noise_session_open(&rx, k, header, 16, sealed + k * clen, clen, plain, sizeof(plain), &got));
        ++k; bench_observe(plain);
    });
    REQUIRE(got == size && !memcmp(plain, payload, size));
    fips_noise_session_destroy(&tx); fips_noise_session_destroy(&rx);
    pair(&tx, &rx);
    snprintf(name, sizeof(name), "worker/seal/%zu", size);
    BENCH(name, {
        fips_cipher_state key; uint64_t counter;
        OK(fips_noise_session_send_cipher_clone(&tx, &key));
        OK(fips_noise_session_take_send_counter(&tx, &counter));
        memset(wire, 0xa5, 16 + size);
        OK(fips_cipher_state_seal_with_counter(&key, counter, wire, 16,
            wire + 16, size, wire + 16, sizeof(wire) - 16, &got));
        bench_observe(wire); fips_cipher_state_destroy(&key);
    });
    fips_noise_session_destroy(&tx); fips_noise_session_destroy(&rx);
    pair(&tx, &rx);
    fips_cipher_state key, rkey;
    OK(fips_noise_session_send_cipher_clone(&tx, &key));
    for (size_t j = 0; j < RING; ++j)
        OK(fips_cipher_state_seal_with_counter(&key, j, header, 16, payload, size,
            sealed + j * clen, clen, &got));
    OK(fips_noise_session_recv_cipher_clone(&rx, &rkey));
    fips_replay_window window = rx.replay_window;
    k = 0;
    snprintf(name, sizeof(name), "worker/open/%zu", size);
    BENCH(name, {
        if (k == RING) { fips_replay_window_reset(&window); k = 0; }
        REQUIRE(fips_replay_window_check(&window, k));
        memcpy(wire, sealed + k * clen, clen);
        OK(fips_cipher_state_open(&rkey, k, header, 16, wire, clen, wire, sizeof(wire), &got));
        bench_observe(wire); REQUIRE(fips_replay_window_mark(&window, k)); ++k;
    });
    REQUIRE(got == size && !memcmp(wire, payload, size));
    fips_cipher_state_destroy(&key); fips_cipher_state_destroy(&rkey);
    fips_noise_session_destroy(&tx); fips_noise_session_destroy(&rx);
    free(sealed);
}
int main(void)
{
    run(64); run(1400);
    fips_noise_session tx, rx; pair(&tx, &rx);
    BENCH("handout/send_cipher_clone", {
        fips_cipher_state key;
        OK(fips_noise_session_send_cipher_clone(&tx, &key));
        bench_observe(&key); fips_cipher_state_destroy(&key);
    });
    fips_noise_session_destroy(&tx); fips_noise_session_destroy(&rx);
    return 0;
}
