/* Regression tests for libnostr production-readiness audit fixes.
 *
 * Covers:
 *   nostrc-cvr  nostr_key_is_valid_public accepts valid x-only pubkeys
 *   nostrc-e7e  token bucket rejects negative/invalid cost without state change
 *   nostrc-3pn  BIP-39 seed derivation does not truncate the passphrase
 *
 * These are real-behavior tests: they exercise the production code paths and
 * would FAIL against the pre-fix implementations.
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "nostr-keys.h"
#include "nostr-utils.h"
#include "rate_limiter.h"
#include "nostr/crypto/bip39.h"

static void test_pubkey_validation(void) {
    /* A freshly generated keypair must validate. Pre-fix, nostr_key_is_valid_public
     * decoded the 64-char x-only key into a 33-byte buffer and ALWAYS returned
     * false for every valid Nostr pubkey. */
    char *sk = nostr_key_generate_private();
    assert(sk != NULL);
    char *pk = nostr_key_get_public(sk);
    assert(pk != NULL);
    assert(strlen(pk) == 64);

    assert(nostr_key_is_valid_public_hex(pk) == true);
    assert(nostr_key_is_valid_public(pk) == true);   /* regression: was always false */

    /* Malformed / non-hex input is rejected. */
    assert(nostr_key_is_valid_public("not_a_key") == false);
    assert(nostr_key_is_valid_public(NULL) == false);
    /* 64 hex chars but not a point on the curve should be rejected. */
    assert(nostr_key_is_valid_public(
        "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff") == false);

    free(pk);
    free(sk);
    printf("  [ok] pubkey validation\n");
}

static void test_rate_limiter_cost(void) {
    nostr_token_bucket tb;
    tb_init(&tb, /*rate=*/1.0, /*burst=*/5.0);
    tb_set_now(&tb, 1000.0);

    /* Negative cost must be denied and must NOT inflate the bucket. Pre-fix,
     * a negative cost added tokens (tokens -= cost) and always returned true. */
    double before = tb.tokens;
    assert(tb_allow(&tb, -1.0) == false);
    assert(tb.tokens <= before + 1e-9); /* not inflated */

    /* Non-finite cost denied, no state corruption. */
    assert(tb_allow(&tb, NAN) == false);
    assert(tb_allow(&tb, INFINITY) == false);
    assert(isfinite(tb.tokens));

    /* Normal operation still throttles: burst=5, so 5 allowed then denied
     * (no time advance means no refill). */
    tb_init(&tb, 1.0, 5.0);
    tb_set_now(&tb, 2000.0);
    int allowed = 0;
    for (int i = 0; i < 100; i++) {
        if (tb_allow(&tb, 1.0)) allowed++;
    }
    assert(allowed == 5);
    printf("  [ok] rate limiter cost validation\n");
}

static int hex_to_bytes(const char *hex, uint8_t *out, size_t out_len) {
    return nostr_hex2bin(out, hex, out_len) ? 0 : -1;
}

static void test_bip39_seed(void) {
    /* Standard BIP-39 (Trezor) test vector. */
    const char *mnemonic =
        "abandon abandon abandon abandon abandon abandon abandon abandon "
        "abandon abandon abandon about";
    const char *expect_hex =
        "c55257c360c07c72029aebc1b53c05ed0362ada38ead3e3e9efa3708e5349553"
        "1f09a6987599d18264c1e1c92f2cf141630c7a3c4ab7c81b2f001698e7463b04";
    uint8_t expect[64];
    assert(hex_to_bytes(expect_hex, expect, sizeof(expect)) == 0);

    uint8_t seed[64];
    assert(nostr_bip39_seed(mnemonic, "TREZOR", seed) == true);
    assert(memcmp(seed, expect, 64) == 0);

    /* Truncation regression: two passphrases sharing a >256-byte prefix but
     * differing afterward MUST derive different seeds. Pre-fix, the salt was a
     * fixed char[9+256] buffer, so both truncated to the same 255-byte prefix
     * and produced identical seeds. */
    char pass_a[300];
    char pass_b[300];
    memset(pass_a, 'a', sizeof(pass_a));
    memset(pass_b, 'a', sizeof(pass_b));
    pass_a[299] = '\0'; pass_a[298] = 'X';
    pass_b[299] = '\0'; pass_b[298] = 'Y';

    uint8_t seed_a[64], seed_b[64];
    assert(nostr_bip39_seed(mnemonic, pass_a, seed_a) == true);
    assert(nostr_bip39_seed(mnemonic, pass_b, seed_b) == true);
    assert(memcmp(seed_a, seed_b, 64) != 0); /* regression: were equal */
    printf("  [ok] bip39 seed (vector + no passphrase truncation)\n");
}

int main(void) {
    printf("libnostr core audit regression tests:\n");
    test_pubkey_validation();
    test_rate_limiter_cost();
    test_bip39_seed();
    printf("All libnostr core audit tests passed.\n");
    return 0;
}
