#include <fips/noise/session.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(x) do { if (!(x)) abort(); } while (0)

static uint64_t le64(const uint8_t *p)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= (uint64_t)p[i] << (8 * i);
    return value;
}

/* A bounded set of accepted counter values, not a second sliding bitmap. */
static bool reference_check(uint64_t high, const uint64_t *seen, size_t count, uint64_t counter)
{
    if (counter == UINT64_MAX || (counter <= high && high - counter >= FIPS_REPLAY_WINDOW_SIZE))
        return false;
    for (size_t i = 0; i < count; ++i) if (seen[i] == counter) return false;
    return true;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    uint8_t key_bytes[32] = {0x37}, h[32] = {0}, pub[33] = {0}, epoch[8] = {0};
    fips_aead_key send, recv;
    REQUIRE(fips_aead_key_init(&send, key_bytes) == FIPS_AEAD_OK);
    REQUIRE(fips_aead_key_init(&recv, key_bytes) == FIPS_AEAD_OK);
    fips_noise_session session;
    REQUIRE(fips_noise_session_from_handshake(&session, FIPS_HANDSHAKE_INITIATOR,
        &send, &recv, h, pub, epoch) == FIPS_NOISE_OK);
    uint64_t seen[256], high = 0;
    size_t count = 0;
    /* Check/mark arbitrary sequences, including direct attempts to mark invalid
     * counters. Each operation is compared with the independent set oracle. */
    size_t steps = size / 9;
    if (steps > 256) steps = 256;
    for (size_t i = 0; i < steps; ++i) {
        uint64_t counter = le64(data + 9 * i);
        fips_replay_window before = session.replay_window;
        bool expected = reference_check(high, seen, count, counter);
        REQUIRE(fips_replay_window_check(&session.replay_window, counter) == expected);
        REQUIRE(!memcmp(&before, &session.replay_window, sizeof(before)));
        if (data[9 * i + 8] & 1) {
            REQUIRE(fips_replay_window_mark(&session.replay_window, counter) == expected);
            if (expected) {
                seen[count++] = counter;
                if (counter > high) high = counter;
                REQUIRE(!fips_replay_window_check(&session.replay_window, counter));
            } else REQUIRE(!memcmp(&before, &session.replay_window, sizeof(before)));
        }
        REQUIRE(fips_noise_session_highest_received_counter(&session) == high);
        if (high >= FIPS_REPLAY_WINDOW_SIZE) {
            REQUIRE(!fips_replay_window_check(&session.replay_window, high - FIPS_REPLAY_WINDOW_SIZE));
            uint64_t oldest = high - (FIPS_REPLAY_WINDOW_SIZE - 1);
            REQUIRE(fips_replay_window_check(&session.replay_window, oldest) == reference_check(high, seen, count, oldest));
        }
        for (size_t j = 0; j < count; ++j)
            REQUIRE(!fips_replay_window_check(&session.replay_window, seen[j]));
    }
    uint8_t header[9] = {0};
    size_t used = size < sizeof(header) ? size : sizeof(header);
    if (used) memcpy(header, data, used);
    data += used; size -= used;
    uint64_t counter = le64(header);
    size_t ad_len = header[8] < size ? header[8] : size;
    const uint8_t *ad = data;
    data += ad_len; size -= ad_len;
    uint8_t out[FIPS_NOISE_MAX_MESSAGE_SIZE + 1], wire[FIPS_NOISE_MAX_MESSAGE_SIZE];
    fips_replay_window before = session.replay_window;
    size_t len = SIZE_MAX;
    memset(out, 0xa5, sizeof(out));
    fips_noise_status rc = fips_noise_session_open_unmarked(&session, counter, ad, ad_len,
        data, size, out, sizeof(out) - 1, &len);
    REQUIRE(!memcmp(&before, &session.replay_window, sizeof(before)));
    if (rc == FIPS_NOISE_OK) REQUIRE(size >= FIPS_NOISE_TAG_SIZE && len == size - FIPS_NOISE_TAG_SIZE);
    else REQUIRE(len == SIZE_MAX);
    REQUIRE(out[sizeof(out) - 1] == 0xa5);
    len = SIZE_MAX;
    rc = fips_noise_session_open(&session, counter, ad, ad_len,
        data, size, out, sizeof(out) - 1, &len);
    if (rc == FIPS_NOISE_OK) {
        REQUIRE(!fips_replay_window_check(&session.replay_window, counter));
        REQUIRE(len == size - FIPS_NOISE_TAG_SIZE);
    } else {
        REQUIRE(len == SIZE_MAX);
        REQUIRE(!memcmp(&before, &session.replay_window, sizeof(before)));
    }
    REQUIRE(out[sizeof(out) - 1] == 0xa5);

    /* Valid and tampered packets reach authentication/mark paths even when the
     * arbitrary bytes above were too short or failed authentication. */
    fips_noise_session_reset_replay_window(&session);
    if (counter == UINT64_MAX) counter = 0;
    size_t plain_len = size;
    if (plain_len > FIPS_NOISE_MAX_MESSAGE_SIZE - FIPS_NOISE_TAG_SIZE)
        plain_len = FIPS_NOISE_MAX_MESSAGE_SIZE - FIPS_NOISE_TAG_SIZE;
    fips_cipher_state worker;
    REQUIRE(fips_noise_session_send_cipher_clone(&session, &worker) == FIPS_NOISE_OK);
    REQUIRE(fips_cipher_state_seal_with_counter(&worker, counter, ad, ad_len, data,
        plain_len, wire, sizeof(wire), &len) == FIPS_NOISE_OK);
    size_t wire_len = len;
    before = session.replay_window;
    wire[wire_len - 1] ^= 1;
    len = SIZE_MAX;
    REQUIRE(fips_noise_session_open(&session, counter, ad, ad_len, wire, wire_len,
        out, sizeof(out) - 1, &len) == FIPS_NOISE_DecryptionFailed);
    REQUIRE(len == SIZE_MAX && !memcmp(&before, &session.replay_window, sizeof(before)));
    wire[wire_len - 1] ^= 1;
    REQUIRE(fips_noise_session_open_unmarked(&session, counter, ad, ad_len, wire, wire_len,
        out, sizeof(out) - 1, &len) == FIPS_NOISE_OK && len == plain_len);
    REQUIRE(!memcmp(&before, &session.replay_window, sizeof(before)));
    REQUIRE(fips_noise_session_open(&session, counter, ad, ad_len, wire, wire_len,
        out, sizeof(out) - 1, &len) == FIPS_NOISE_OK && len == plain_len);
    REQUIRE(!memcmp(out, data, plain_len));
    REQUIRE(fips_noise_session_open(&session, counter, ad, ad_len, wire, wire_len,
        out, sizeof(out) - 1, &len) == FIPS_NOISE_ReplayDetected);
    fips_cipher_state_destroy(&worker);
    fips_noise_session_destroy(&session);
    return 0;
}
