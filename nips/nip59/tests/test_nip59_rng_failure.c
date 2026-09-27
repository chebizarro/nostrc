/**
 * NIP-59 randomness failure paths (nostrc-rd8j).
 *
 * This executable compiles src/nip59.c itself with
 * -DNIP59_RAND_BYTES=nip59_test_rand_bytes, so the generator behind gift-wrap
 * timestamps is the stub below. When it fails, nip59 must return an error
 * and wrap nothing; it must never fall back to the real send time.
 */

#undef NDEBUG

#include "nostr/nip59/nip59.h"
#include "nostr-event.h"
#include "nostr-keys.h"
#include "nostr-kinds.h"
#include "nostr-utils.h"

#include <openssl/rand.h>

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum stub_mode { STUB_REAL, STUB_FAIL, STUB_FIXED };

static enum stub_mode mode = STUB_REAL;
static uint32_t fixed_value;
static int calls;

int nip59_test_rand_bytes(unsigned char *buf, int num);

int nip59_test_rand_bytes(unsigned char *buf, int num) {
    calls++;
    switch (mode) {
    case STUB_FAIL:
        return 0;                     /* OpenSSL's failure return */
    case STUB_FIXED:
        assert(num == 4);
        buf[0] = (unsigned char)(fixed_value >> 24);
        buf[1] = (unsigned char)(fixed_value >> 16);
        buf[2] = (unsigned char)(fixed_value >> 8);
        buf[3] = (unsigned char)fixed_value;
        return 1;
    case STUB_REAL:
    default:
        return RAND_bytes(buf, num);
    }
}

static NostrEvent *make_inner(const char *pk) {
    NostrEvent *ev = nostr_event_new();
    assert(ev != NULL);
    nostr_event_set_kind(ev, NOSTR_KIND_TEXT_NOTE);
    nostr_event_set_pubkey(ev, pk);
    nostr_event_set_content(ev, "hidden");
    nostr_event_set_created_at(ev, (int64_t)time(NULL));
    return ev;
}

static void test_generator_failure_is_an_error(const char *sender_pk,
                                               const char *recipient_pk) {
    printf("Testing CSPRNG failure is an error...\n");
    mode = STUB_FAIL;

    int64_t ts = 999;
    calls = 0;
    assert(nostr_nip59_randomize_timestamp(1700000000, 0, &ts) == NIP59_ERR_RANDOMNESS);
    assert(ts == 0);
    assert(calls == 1);
    assert(nostr_nip59_randomize_timestamp(0, 0, &ts) == NIP59_ERR_RANDOMNESS);

    /* No wrap at all rather than one stamped with the send time. */
    NostrEvent *inner = make_inner(sender_pk);
    assert(nostr_nip59_wrap(inner, recipient_pk, NULL) == NULL);

    char *eph_sk = NULL, *eph_pk = NULL;
    assert(nostr_nip59_create_ephemeral_key(&eph_sk, &eph_pk) == NIP59_OK);
    assert(nostr_nip59_wrap(inner, recipient_pk, eph_sk) == NULL);

    uint8_t eph_bin[32];
    assert(nostr_hex2bin(eph_bin, eph_sk, sizeof(eph_bin)));
    assert(nostr_nip59_wrap_with_key(inner, recipient_pk, eph_bin) == NULL);

    free(eph_sk);
    free(eph_pk);
    nostr_event_free(inner);
    printf("  OK: randomize/wrap/wrap_with_key all fail\n");
}

static void test_rejection_exhaustion_is_an_error(void) {
    printf("Testing a generator stuck in the rejected range...\n");
    /* 2^32 % 172800 != 0, so 0xffffffff is always rejected for the default
     * window: a stuck generator must end in an error, not a loop or a biased
     * fallback. */
    mode = STUB_FIXED;
    fixed_value = UINT32_MAX;
    calls = 0;
    int64_t ts = 999;
    assert(nostr_nip59_randomize_timestamp(1700000000, 0, &ts) == NIP59_ERR_RANDOMNESS);
    assert(ts == 0);
    assert(calls == 64);
    printf("  OK: gave up after %d rejected draws\n", calls);
}

static void test_mapping_edges(void) {
    printf("Testing draw -> offset mapping...\n");
    const int64_t base = 1700000000;
    const uint32_t window = 2 * 24 * 60 * 60;
    int64_t ts = 0;

    mode = STUB_FIXED;
    fixed_value = 0;                      /* smallest draw: one second back */
    assert(nostr_nip59_randomize_timestamp(base, window, &ts) == NIP59_OK);
    assert(ts == base - 1);

    fixed_value = window - 1;             /* largest offset: the whole window */
    assert(nostr_nip59_randomize_timestamp(base, window, &ts) == NIP59_OK);
    assert(ts == base - (int64_t)window);

    fixed_value = window;                 /* wraps back to the start */
    assert(nostr_nip59_randomize_timestamp(base, window, &ts) == NIP59_OK);
    assert(ts == base - 1);

    /* A window dividing 2^32 never rejects, even at the top. */
    fixed_value = UINT32_MAX;
    calls = 0;
    assert(nostr_nip59_randomize_timestamp(base, 1024, &ts) == NIP59_OK);
    assert(ts == base - 1024);
    assert(calls == 1);
    printf("  OK: [1, window] offsets, no modulo bias\n");
}

static void test_recovers_with_real_generator(const char *sender_pk,
                                              const char *recipient_pk) {
    printf("Testing wraps work again with the real generator...\n");
    mode = STUB_REAL;
    NostrEvent *inner = make_inner(sender_pk);
    int64_t before = (int64_t)time(NULL);
    NostrEvent *wrap = nostr_nip59_wrap(inner, recipient_pk, NULL);
    int64_t after = (int64_t)time(NULL);
    assert(wrap != NULL);
    assert(nostr_nip59_validate_gift_wrap(wrap));
    int64_t t = nostr_event_get_created_at(wrap);
    assert(t < after && t >= before - 2 * 24 * 60 * 60);
    nostr_event_free(wrap);
    nostr_event_free(inner);
    printf("  OK\n");
}

int main(void) {
    char *alice_sk = nostr_key_generate_private();
    char *alice_pk = alice_sk ? nostr_key_get_public(alice_sk) : NULL;
    char *bob_sk = nostr_key_generate_private();
    char *bob_pk = bob_sk ? nostr_key_get_public(bob_sk) : NULL;
    assert(alice_pk && bob_pk);

    test_generator_failure_is_an_error(alice_pk, bob_pk);
    test_rejection_exhaustion_is_an_error();
    test_mapping_edges();
    test_recovers_with_real_generator(alice_pk, bob_pk);

    free(alice_sk);
    free(alice_pk);
    free(bob_sk);
    free(bob_pk);
    printf("All NIP-59 RNG failure tests passed!\n");
    return 0;
}
