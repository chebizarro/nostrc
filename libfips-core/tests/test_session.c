#include <fips/noise/session.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); return 1; \
} } while (0)

static bool filled(const void *data, size_t size, uint8_t value)
{
    const uint8_t *p = data;
    for (size_t i = 0; i < size; ++i) if (p[i] != value) return false;
    return true;
}

static int make_pair(fips_noise_session *i, fips_noise_session *r)
{
    uint8_t a[32], b[32], h[32], pub[33], epoch[8];
    memset(a, 0xa1, sizeof(a)); memset(b, 0xb2, sizeof(b));
    memset(h, 0xc3, sizeof(h)); memset(pub, 0xd4, sizeof(pub));
    memset(epoch, 0xe5, sizeof(epoch));
    fips_aead_key is, ir, rs, rr;
    CHECK(fips_aead_key_init(&is, a) == FIPS_AEAD_OK);
    CHECK(fips_aead_key_init(&rr, a) == FIPS_AEAD_OK);
    CHECK(fips_aead_key_init(&rs, b) == FIPS_AEAD_OK);
    CHECK(fips_aead_key_init(&ir, b) == FIPS_AEAD_OK);
    CHECK(fips_noise_session_from_handshake(i, FIPS_HANDSHAKE_INITIATOR,
        &is, &ir, h, pub, epoch) == FIPS_NOISE_OK);
    CHECK(fips_noise_session_from_handshake(r, FIPS_HANDSHAKE_RESPONDER,
        &rs, &rr, h, pub, epoch) == FIPS_NOISE_OK);
    CHECK(filled(&is, sizeof(is), 0) && filled(&ir, sizeof(ir), 0));
    CHECK(filled(&rs, sizeof(rs), 0) && filled(&rr, sizeof(rr), 0));
    CHECK(i->role == FIPS_HANDSHAKE_INITIATOR && r->role == FIPS_HANDSHAKE_RESPONDER);
    CHECK(!memcmp(i->handshake_hash, h, sizeof(h)) && !memcmp(r->handshake_hash, h, sizeof(h)));
    CHECK(!memcmp(i->remote_static, pub, sizeof(pub)) && !memcmp(i->remote_epoch, epoch, sizeof(epoch)));
    CHECK(!memcmp(i->send_cipher.key.bytes, a, 32) && !memcmp(i->recv_cipher.key.bytes, b, 32));
    return 0;
}

static int window_edges(void)
{
    fips_replay_window w, before;
    fips_replay_window_init(&w);
    CHECK(sizeof(w.bitmap) == 1024 && FIPS_REPLAY_WINDOW_SIZE == 8192);
    CHECK(fips_replay_window_highest(&w) == 0 && fips_replay_window_check(&w, 0));
    CHECK(fips_replay_window_mark(&w, 0));
    CHECK(!fips_replay_window_check(&w, 0));
    before = w;
    CHECK(!fips_replay_window_mark(&w, 0) && !memcmp(&w, &before, sizeof(w)));
    fips_replay_window_reset(&w);
    CHECK(filled(&w, sizeof(w), 0));
    CHECK(fips_replay_window_mark(&w, FIPS_REPLAY_WINDOW_SIZE - 1));
    CHECK(fips_replay_window_check(&w, 0)); /* Oldest inclusive, not initially seen. */
    CHECK(fips_replay_window_mark(&w, 0));
    CHECK(fips_replay_window_mark(&w, FIPS_REPLAY_WINDOW_SIZE));
    CHECK(!fips_replay_window_check(&w, 0) && fips_replay_window_check(&w, 1));
    CHECK(fips_replay_window_mark(&w, FIPS_REPLAY_WINDOW_SIZE + 1));
    CHECK(!fips_replay_window_check(&w, 1) && fips_replay_window_check(&w, 2));
    CHECK(fips_replay_window_mark(&w, UINT64_C(1) << 48));
    CHECK(!fips_replay_window_check(&w, FIPS_REPLAY_WINDOW_SIZE));
    before = w;
    CHECK(!fips_replay_window_mark(&w, 0) && !memcmp(&w, &before, sizeof(w)));
    CHECK(!fips_replay_window_check(&w, UINT64_MAX));
    CHECK(!fips_replay_window_mark(&w, UINT64_MAX) && !memcmp(&w, &before, sizeof(w)));
    CHECK(fips_replay_window_mark(&w, UINT64_MAX - 1));
    CHECK(fips_replay_window_check(&w, UINT64_MAX - FIPS_REPLAY_WINDOW_SIZE));
    CHECK(!fips_replay_window_check(&w, UINT64_MAX - FIPS_REPLAY_WINDOW_SIZE - 1));
    CHECK(fips_replay_window_mark(&w, UINT64_MAX - 2));
    CHECK(!fips_replay_window_check(&w, UINT64_MAX - 2));
    return 0;
}

/* Independent set-of-counters oracle: exercises carries across all bitmap words,
 * whole-word shifts, exact resets and large (not size_t-truncated) distances. */
static int window_shifts(void)
{
    static const uint64_t trace[] = {0, 1, 63, 64, 65, 127, 128, 8191, 8192, 8193,
        8256, 8257, 16383, 24575, 32768, UINT64_C(1) << 40, UINT64_MAX - 1};
    uint64_t accepted[sizeof(trace) / sizeof(trace[0])], high = 0;
    size_t count = 0;
    fips_replay_window w;
    fips_replay_window_init(&w);
    for (size_t i = 0; i < sizeof(trace) / sizeof(trace[0]); ++i) {
        CHECK(fips_replay_window_mark(&w, trace[i]));
        accepted[count++] = trace[i];
        high = trace[i];
        CHECK(fips_replay_window_highest(&w) == high);
        for (uint64_t age = 0; age <= FIPS_REPLAY_WINDOW_SIZE && age <= high; ++age) {
            uint64_t candidate = high - age;
            bool expected = age < FIPS_REPLAY_WINDOW_SIZE;
            for (size_t j = 0; j < count; ++j) if (accepted[j] == candidate) expected = false;
            CHECK(fips_replay_window_check(&w, candidate) == expected);
        }
    }
    return 0;
}

static int cipher_bounds_overflow(void)
{
    uint8_t bytes[32] = {1}, plain[FIPS_NOISE_MAX_MESSAGE_SIZE] = {0};
    uint8_t wire[FIPS_NOISE_MAX_MESSAGE_SIZE + 1], opened[FIPS_NOISE_MAX_MESSAGE_SIZE];
    fips_aead_key key;
    CHECK(fips_aead_key_init(&key, bytes) == FIPS_AEAD_OK);
    fips_cipher_state c;
    fips_cipher_state_init(&c, &key);
    fips_aead_key_destroy(&key);
    size_t len = SIZE_MAX;
    memset(wire, 0xa5, sizeof(wire));
    CHECK(fips_cipher_state_seal(&c, NULL, 0, plain, sizeof(plain), wire,
        sizeof(wire), &len) == FIPS_NOISE_MessageTooLarge);
    CHECK(fips_cipher_state_seal(&c, NULL, 0, plain, SIZE_MAX, wire,
        sizeof(wire), &len) == FIPS_NOISE_MessageTooLarge);
    CHECK(fips_cipher_state_seal(&c, NULL, 0, plain, 1, wire,
        FIPS_NOISE_TAG_SIZE, &len) == FIPS_NOISE_MessageTooLarge);
    CHECK(len == SIZE_MAX && fips_cipher_state_counter(&c) == 0 && filled(wire, sizeof(wire), 0xa5));
    size_t max_plain = FIPS_NOISE_MAX_MESSAGE_SIZE - FIPS_NOISE_TAG_SIZE;
    CHECK(fips_cipher_state_seal(&c, NULL, 0, plain, max_plain, wire,
        sizeof(wire) - 1, &len) == FIPS_NOISE_OK && len == FIPS_NOISE_MAX_MESSAGE_SIZE);
    CHECK(wire[sizeof(wire) - 1] == 0xa5 && fips_cipher_state_counter(&c) == 1);
    size_t got = SIZE_MAX;
    CHECK(fips_cipher_state_open(&c, 0, NULL, 0, wire, len, opened, max_plain, &got) == FIPS_NOISE_OK);
    CHECK(got == max_plain && !memcmp(plain, opened, got) && fips_cipher_state_counter(&c) == 1);
    got = SIZE_MAX;
    memset(opened, 0xa5, sizeof(opened));
    CHECK(fips_cipher_state_open(&c, 0, NULL, 0, wire, len, opened, max_plain - 1,
        &got) == FIPS_NOISE_MessageTooLarge);
    CHECK(fips_cipher_state_open(&c, 0, NULL, 0, wire, sizeof(wire), opened,
        sizeof(opened), &got) == FIPS_NOISE_MessageTooLarge);
    CHECK(fips_cipher_state_open(&c, 0, NULL, 0, wire, 15, opened,
        sizeof(opened), &got) == FIPS_NOISE_MessageTooShort);
    CHECK(got == SIZE_MAX && filled(opened, sizeof(opened), 0xa5));
    CHECK(fips_cipher_state_seal(&c, NULL, 0, NULL, 0, wire, sizeof(wire), &len) == FIPS_NOISE_OK);
    CHECK(len == FIPS_NOISE_TAG_SIZE);
    CHECK(fips_cipher_state_open(&c, 1, NULL, 0, wire, len, opened, 0, &got) == FIPS_NOISE_OK && got == 0);

    fips_cipher_state_test_set_counter(&c, UINT64_MAX - 1);
    CHECK(fips_cipher_state_seal(&c, NULL, 0, plain, 1, wire, sizeof(wire), &len) == FIPS_NOISE_OK);
    CHECK(fips_cipher_state_counter(&c) == UINT64_MAX);
    CHECK(fips_cipher_state_open(&c, UINT64_MAX - 1, NULL, 0, wire, len,
        opened, sizeof(opened), &got) == FIPS_NOISE_OK && got == 1);
    memset(wire, 0xa5, sizeof(wire)); len = SIZE_MAX;
    CHECK(fips_cipher_state_seal(&c, NULL, 0, plain, 1, wire, sizeof(wire), &len) == FIPS_NOISE_NonceOverflow);
    CHECK(fips_cipher_state_seal_with_counter(&c, UINT64_MAX, NULL, 0, plain, 1,
        wire, sizeof(wire), &len) == FIPS_NOISE_NonceOverflow);
    uint64_t counter = 123;
    CHECK(fips_cipher_state_take_counter(&c, &counter) == FIPS_NOISE_NonceOverflow && counter == 123);
    CHECK(len == SIZE_MAX && fips_cipher_state_counter(&c) == UINT64_MAX && filled(wire, sizeof(wire), 0xa5));
    fips_cipher_state_destroy(&c);
    CHECK(filled(&c, sizeof(c), 0));
    CHECK(fips_cipher_state_seal(&c, NULL, 0, plain, 1, wire, sizeof(wire), &len) == FIPS_NOISE_WrongState);
    CHECK(fips_cipher_state_open(&c, 0, NULL, 0, wire, 17, opened, sizeof(opened), &len) == FIPS_NOISE_WrongState);
    return 0;
}

static int session_authentication(void)
{
    fips_noise_session i, r;
    CHECK(make_pair(&i, &r) == 0);
    uint8_t plain[] = {1, 2, 3, 4, 5}, ad[] = {0, 1, 0xa5};
    uint8_t wire[21], bad[21], out[6];
    size_t len = 0, got;
    CHECK(fips_noise_session_seal(&i, ad, sizeof(ad), plain, sizeof(plain), wire, sizeof(wire), &len) == FIPS_NOISE_OK);
    CHECK(len == sizeof(wire) && fips_noise_session_current_send_counter(&i) == 1);
    fips_replay_window before = r.replay_window;
    /* Ciphertext, tag, counter, and AAD mutations must all fail authentication. */
    for (unsigned mutation = 0; mutation < 4; ++mutation) {
        memcpy(bad, wire, sizeof(bad));
        if (mutation < 2) bad[mutation == 0 ? 0 : sizeof(bad) - 1] ^= 1;
        if (mutation == 3) ad[0] ^= 1;
        uint64_t counter = mutation == 2 ? 1 : 0;
        got = SIZE_MAX; memset(out, 0xa5, sizeof(out));
        CHECK(fips_noise_session_open(&r, counter, ad, sizeof(ad), bad, len,
            out, sizeof(out), &got) == FIPS_NOISE_DecryptionFailed);
        CHECK(got == SIZE_MAX && filled(out, sizeof(plain), 0) && out[5] == 0xa5);
        CHECK(!memcmp(&r.replay_window, &before, sizeof(before)));
        CHECK(fips_noise_session_check_replay(&r, counter) == FIPS_NOISE_OK);
        if (mutation == 3) ad[0] ^= 1;
    }
    got = SIZE_MAX;
    CHECK(fips_noise_session_open(&r, 0, ad, sizeof(ad), wire, len,
        out, sizeof(plain) - 1, &got) == FIPS_NOISE_MessageTooLarge);
    CHECK(fips_noise_session_open(&r, 0, ad, sizeof(ad), wire, 15,
        out, sizeof(out), &got) == FIPS_NOISE_MessageTooShort);
    CHECK(got == SIZE_MAX && !memcmp(&r.replay_window, &before, sizeof(before)));
    CHECK(fips_noise_session_open_unmarked(&r, 0, ad, sizeof(ad), wire, len,
        out, sizeof(out), &got) == FIPS_NOISE_OK && got == sizeof(plain) && !memcmp(out, plain, got));
    CHECK(!memcmp(&r.replay_window, &before, sizeof(before)));
    CHECK(fips_noise_session_check_replay(&r, 0) == FIPS_NOISE_OK);
    CHECK(fips_noise_session_open(&r, 0, ad, sizeof(ad), wire, len,
        out, sizeof(out), &got) == FIPS_NOISE_OK);
    CHECK(fips_noise_session_check_replay(&r, 0) == FIPS_NOISE_ReplayDetected);
    before = r.replay_window;
    got = SIZE_MAX; memset(out, 0xa5, sizeof(out));
    CHECK(fips_noise_session_open(&r, 0, ad, sizeof(ad), wire, len,
        out, sizeof(out), &got) == FIPS_NOISE_ReplayDetected);
    CHECK(got == SIZE_MAX && filled(out, sizeof(out), 0xa5));
    CHECK(fips_noise_session_open_unmarked(&r, 0, ad, sizeof(ad), wire, len,
        out, sizeof(out), &got) == FIPS_NOISE_OK); /* No replay check, even if seen. */
    CHECK(!memcmp(&r.replay_window, &before, sizeof(before)));
    CHECK(fips_noise_session_recv_nonce(&r) == 0 && fips_noise_session_highest_received_counter(&r) == 0);
    CHECK(fips_noise_session_open(&r, UINT64_MAX, ad, sizeof(ad), wire, len,
        out, sizeof(out), &got) == FIPS_NOISE_ReplayDetected);
    CHECK(fips_noise_session_accept_replay(&r, UINT64_MAX) == FIPS_NOISE_ReplayDetected);
    fips_noise_session_reset_replay_window(&r);
    CHECK(fips_noise_session_check_replay(&r, 0) == FIPS_NOISE_OK);
    CHECK(fips_noise_session_accept_replay(&r, 0) == FIPS_NOISE_OK);
    CHECK(fips_noise_session_accept_replay(&r, 0) == FIPS_NOISE_ReplayDetected);
    fips_noise_session_destroy(&i); fips_noise_session_destroy(&r);
    CHECK(filled(&i, sizeof(i), 0) && filled(&r, sizeof(r), 0));
    return 0;
}

static int worker_clones_moves(void)
{
    fips_noise_session i, r, moved;
    CHECK(make_pair(&i, &r) == 0);
    fips_cipher_state a, b, recv;
    CHECK(fips_noise_session_send_cipher_clone(&i, &a) == FIPS_NOISE_OK);
    CHECK(fips_noise_session_send_cipher_clone(&i, &b) == FIPS_NOISE_OK);
    CHECK(fips_noise_session_recv_cipher_clone(&r, &recv) == FIPS_NOISE_OK);
    uint64_t ca, cb;
    CHECK(fips_noise_session_take_send_counter(&i, &ca) == FIPS_NOISE_OK && ca == 0);
    CHECK(fips_noise_session_take_send_counter(&i, &cb) == FIPS_NOISE_OK && cb == 1);
    uint8_t plain[] = {9, 8}, wa[18], wb[18], out[2];
    size_t len, got;
    CHECK(fips_cipher_state_seal_with_counter(&a, ca, NULL, 0, plain, 2, wa, sizeof(wa), &len) == FIPS_NOISE_OK);
    CHECK(fips_cipher_state_seal_with_counter(&b, cb, NULL, 0, plain, 2, wb, sizeof(wb), &len) == FIPS_NOISE_OK);
    CHECK(memcmp(wa, wb, sizeof(wa)) != 0);
    CHECK(fips_cipher_state_open(&recv, cb, NULL, 0, wb, sizeof(wb), out, sizeof(out), &got) == FIPS_NOISE_OK);
    CHECK(got == 2 && !memcmp(out, plain, 2));
    CHECK(fips_noise_session_check_replay(&r, cb) == FIPS_NOISE_OK); /* Worker did not mark. */
    CHECK(fips_noise_session_accept_replay(&r, cb) == FIPS_NOISE_OK);
    CHECK(fips_noise_session_current_send_counter(&i) == 2);
    CHECK(fips_cipher_state_counter(&a) == 0 && fips_cipher_state_counter(&b) == 0);
    /* Counter copies diverge; reserving on one does not mutate the other/session.
     * Do NOT seal these locally reserved counters: they overlap the global ones. */
    CHECK(fips_cipher_state_take_counter(&a, &ca) == FIPS_NOISE_OK && ca == 0);
    CHECK(fips_cipher_state_counter(&a) == 1 && fips_cipher_state_counter(&b) == 0);
    CHECK(fips_noise_session_current_send_counter(&i) == 2);
    fips_cipher_state c;
    CHECK(fips_cipher_state_take_cleared(&a, &a) == FIPS_NOISE_WrongState);
    CHECK(fips_cipher_state_take_cleared(&a, &c) == FIPS_NOISE_OK);
    CHECK(filled(&a, sizeof(a), 0) && fips_cipher_state_counter(&c) == 1);
    CHECK(!memcmp(c.key.bytes, b.key.bytes, sizeof(c.key.bytes)));
    fips_cipher_state_clear_slot(&c); CHECK(filled(&c, sizeof(c), 0));
    fips_cipher_state_destroy(&b); CHECK(filled(&b, sizeof(b), 0));
    /* Session destruction cannot erase a worker's separately owned key. */
    CHECK(fips_noise_session_take_cleared(&i, &i) == FIPS_NOISE_WrongState);
    CHECK(fips_noise_session_take_cleared(&i, &moved) == FIPS_NOISE_OK);
    CHECK(filled(&i, sizeof(i), 0) && fips_noise_session_current_send_counter(&moved) == 2);
    CHECK(!memcmp(moved.send_cipher.key.bytes, recv.key.bytes, sizeof(recv.key.bytes)));
    fips_noise_session_destroy(&r); CHECK(filled(&r, sizeof(r), 0));
    CHECK(fips_cipher_state_open(&recv, ca, NULL, 0, wa, sizeof(wa), out, sizeof(out), &got) == FIPS_NOISE_OK);
    fips_cipher_state_destroy(&recv); CHECK(filled(&recv, sizeof(recv), 0));
    memset(&a, 0xa5, sizeof(a));
    CHECK(fips_noise_session_send_cipher_clone(&i, &a) == FIPS_NOISE_WrongState);
    CHECK(fips_noise_session_recv_cipher_clone(&r, &a) == FIPS_NOISE_WrongState);
    CHECK(filled(&a, sizeof(a), 0xa5));
    fips_cipher_state_clear_slot(&a);
    fips_cipher_state_test_set_counter(&moved.send_cipher, UINT64_MAX - 1);
    CHECK(fips_noise_session_take_send_counter(&moved, &ca) == FIPS_NOISE_OK && ca == UINT64_MAX - 1);
    ca = 123;
    CHECK(fips_noise_session_take_send_counter(&moved, &ca) == FIPS_NOISE_NonceOverflow && ca == 123);
    CHECK(fips_noise_session_seal(&moved, NULL, 0, plain, 2, wa, sizeof(wa), &len) == FIPS_NOISE_NonceOverflow);
    fips_noise_session_clear_slot(&moved); CHECK(filled(&moved, sizeof(moved), 0));
    return 0;
}

static int constructor_failures(void)
{
    fips_noise_session out;
    uint8_t bytes[32] = {7}, h[32] = {0}, pub[33] = {0}, epoch[8] = {0};
    fips_aead_key a, b;
    CHECK(fips_aead_key_init(&a, bytes) == FIPS_AEAD_OK);
    CHECK(fips_aead_key_init(&b, bytes) == FIPS_AEAD_OK);
    memset(&out, 0xa5, sizeof(out));
    CHECK(fips_noise_session_from_handshake(&out, (fips_handshake_role)99,
        &a, &b, h, pub, epoch) == FIPS_NOISE_WrongState);
    CHECK(filled(&out, sizeof(out), 0xa5) && !memcmp(a.bytes, bytes, 32) && !memcmp(b.bytes, bytes, 32));
    CHECK(fips_noise_session_from_handshake(&out, FIPS_HANDSHAKE_INITIATOR,
        &a, &a, h, pub, epoch) == FIPS_NOISE_WrongState);
    CHECK(filled(&out, sizeof(out), 0xa5) && !memcmp(a.bytes, bytes, 32));
    fips_aead_key_destroy(&a); fips_aead_key_destroy(&b);
    fips_noise_session_clear_slot(&out);
    return 0;
}

int main(void)
{
    CHECK(window_edges() == 0);
    CHECK(window_shifts() == 0);
    CHECK(cipher_bounds_overflow() == 0);
    CHECK(session_authentication() == 0);
    CHECK(worker_clones_moves() == 0);
    CHECK(constructor_failures() == 0);
    puts("session: 6 test groups passed");
    return 0;
}
