/* nip-concord unit tests.
 *
 * The derivation cases are known-answer vectors computed independently from
 * CORD-02 Appendix A (HKDF-SHA256, empty salt, info = utf8(label) || 0x00 ||
 * id[32] || epoch_be[8]?). They exist to freeze the byte layout: any change to
 * the info construction re-addresses every plane on the wire, so a silent drift
 * here is a migration, not a bug fix.
 */

#include <nip_concord.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

static void fill(uint8_t *buf, size_t len, uint8_t value) {
    memset(buf, value, len);
}

static bool hex_eq(const uint8_t value[32], const char *expected_hex) {
    char got[65];
    nostr_concord_hex_encode_32(value, got);
    return strcmp(got, expected_hex) == 0;
}

/* ------------------------------------------------------------------ */

static void test_hex(void) {
    uint8_t out[32];
    CHECK(nostr_concord_is_lower_hex_32(
        "0000000000000000000000000000000000000000000000000000000000000000"));
    /* Uppercase is not canonical (CORD-01 "Encoding"). */
    CHECK(!nostr_concord_is_lower_hex_32(
        "AAAA000000000000000000000000000000000000000000000000000000000000"));
    /* 63 chars, and 65 chars, both fail closed. */
    CHECK(!nostr_concord_is_lower_hex_32(
        "000000000000000000000000000000000000000000000000000000000000000"));
    CHECK(!nostr_concord_is_lower_hex_32(
        "00000000000000000000000000000000000000000000000000000000000000000"));
    CHECK(!nostr_concord_is_lower_hex_32(""));

    CHECK(nostr_concord_hex_decode_32(
        "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20",
        out));
    for (size_t i = 0; i < 32; i++) CHECK(out[i] == (uint8_t)(i + 1));

    char round[65];
    nostr_concord_hex_encode_32(out, round);
    CHECK(strcmp(round,
                 "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20") ==
          0);
}

static void test_b64url(void) {
    static const uint8_t data[] = { 0x00, 0x01, 0x02, 0x03, 0xfb, 0xff };
    for (size_t len = 0; len <= sizeof(data); len++) {
        char *encoded = nostr_concord_b64url_encode(data, len);
        CHECK(encoded != NULL);
        if (!encoded) continue;
        /* No padding, ever: the fragment rides in a URL. */
        CHECK(strchr(encoded, '=') == NULL);
        size_t decoded_len = 0;
        uint8_t *decoded = nostr_concord_b64url_decode(encoded, &decoded_len);
        CHECK(decoded != NULL);
        if (decoded) {
            CHECK(decoded_len == len);
            CHECK(memcmp(decoded, data, len) == 0);
            free(decoded);
        }
        free(encoded);
    }

    size_t len = 0;
    /* A length of 1 mod 4 is unreachable from unpadded base64url. */
    CHECK(nostr_concord_b64url_decode("A", &len) == NULL);
    CHECK(nostr_concord_b64url_decode("AAAAA", &len) == NULL);
    /* Padding is not accepted. */
    CHECK(nostr_concord_b64url_decode("AA==", &len) == NULL);
    /* Standard-base64 alphabet is not accepted. */
    CHECK(nostr_concord_b64url_decode("+/+/", &len) == NULL);
    /* Non-canonical tails: the unused low bits must be zero, or two distinct
     * strings would decode to the same bytes. */
    CHECK(nostr_concord_b64url_decode("AB", &len) == NULL);
    CHECK(nostr_concord_b64url_decode("AAB", &len) == NULL);
    uint8_t *ok = nostr_concord_b64url_decode("AA", &len);
    CHECK(ok != NULL && len == 1);
    free(ok);
}

static void test_community_id(void) {
    uint8_t owner[32], salt[32], id[32];
    for (size_t i = 0; i < 32; i++) owner[i] = (uint8_t)i;
    fill(salt, sizeof(salt), 0xaa);

    CHECK(nostr_concord_derive_community_id(owner, salt, id) ==
          NOSTR_CONCORD_OK);
    /* A.4: sha256("concord/community" || owner_xonly || owner_salt). */
    CHECK(hex_eq(
        id, "15eba7e60ca91477de569d0abb924e816bd6f5ce7c17cc1b2b4cfad5c6984aab"));

    CHECK(nostr_concord_verify_community_id(id, owner, salt));
    /* CORD-05 §1: a bundle whose owner proof misses is refused. */
    salt[0] ^= 0x01;
    CHECK(!nostr_concord_verify_community_id(id, owner, salt));
    salt[0] ^= 0x01;
    owner[31] ^= 0x01;
    CHECK(!nostr_concord_verify_community_id(id, owner, salt));
}

static void test_derivations(void) {
    uint8_t owner[32], salt[32], community_id[32];
    for (size_t i = 0; i < 32; i++) owner[i] = (uint8_t)i;
    fill(salt, sizeof(salt), 0xaa);
    CHECK(nostr_concord_derive_community_id(owner, salt, community_id) ==
          NOSTR_CONCORD_OK);

    uint8_t community_root[32], control_root[32];
    fill(community_root, sizeof(community_root), 0x11);
    fill(control_root, sizeof(control_root), 0x44);

    nostr_concord_group_key_t key;

    /* The seed is the secret key whenever the seed is a valid scalar, which is
     * every case here — the A.3 retry branch is ~2^-128 rare. */
    CHECK(nostr_concord_control_read_key(community_root, community_id, 7,
                                         &key) == NOSTR_CONCORD_OK);
    CHECK(hex_eq(key.sk,
                 "e07dd5b20294180190bbdaae3208ff9379ee8ebea26ed8a4e0a2d2c50f89aa74"));
    uint8_t control_read_pk[32];
    memcpy(control_read_pk, key.pk, 32);
    nostr_concord_group_key_clear(&key);

    CHECK(nostr_concord_guestbook_key(community_root, community_id, 7, &key) ==
          NOSTR_CONCORD_OK);
    CHECK(hex_eq(key.sk,
                 "2696be7600cb09a929fbb4211c33bdcbdc445cff54ded2dd8f4e2dacd06275b8"));
    /* Different label, same secret and id: different plane. */
    CHECK(memcmp(key.pk, control_read_pk, 32) != 0);
    nostr_concord_group_key_clear(&key);

    uint8_t channel_id[32], channel_key[32];
    fill(channel_id, sizeof(channel_id), 0x22);
    fill(channel_key, sizeof(channel_key), 0x33);
    CHECK(nostr_concord_channel_key(channel_key, channel_id, 0, &key) ==
          NOSTR_CONCORD_OK);
    CHECK(hex_eq(key.sk,
                 "27de11b0ba9c42d16f64fe94045052bd444e80dd8ee003bd5fbd9f9543c75f59"));
    nostr_concord_group_key_clear(&key);

    /* §9: no secret and no epoch — every member past or present finds the
     * same grave. */
    CHECK(nostr_concord_dissolved_key(community_id, &key) == NOSTR_CONCORD_OK);
    CHECK(hex_eq(key.sk,
                 "acf625d58a6e2738106d957b3a246e1f1eb202b3abcc22e4ce6e01ac923292e3"));
    nostr_concord_group_key_clear(&key);

    /* The signer keypair is a different label off a different secret, so a
     * member holding only the community_root cannot mint Control wraps. */
    nostr_concord_group_key_t signer;
    CHECK(nostr_concord_control_signer_key(control_root, community_id, 7,
                                           &signer) == NOSTR_CONCORD_OK);
    CHECK(memcmp(signer.pk, control_read_pk, 32) != 0);
    nostr_concord_group_key_clear(&signer);

    /* Rotating the epoch rotates the address (§4). */
    nostr_concord_group_key_t epoch8;
    CHECK(nostr_concord_guestbook_key(community_root, community_id, 8,
                                      &epoch8) == NOSTR_CONCORD_OK);
    CHECK(nostr_concord_guestbook_key(community_root, community_id, 7, &key) ==
          NOSTR_CONCORD_OK);
    CHECK(memcmp(epoch8.pk, key.pk, 32) != 0);
    /* And the conversation key is the self-ECDH of the plane key, so it is
     * present and non-degenerate. */
    uint8_t zero[32];
    fill(zero, sizeof(zero), 0);
    CHECK(memcmp(key.conv_key, zero, 32) != 0);
    nostr_concord_group_key_clear(&epoch8);

    /* group_key_clear wipes the secret, not just the handle. */
    nostr_concord_group_key_clear(&key);
    CHECK(memcmp(key.sk, zero, 32) == 0);
    CHECK(memcmp(key.conv_key, zero, 32) == 0);
}

static void test_invite_key(void) {
    uint8_t token[CONCORD_INVITE_TOKEN_BYTES];
    for (size_t i = 0; i < sizeof(token); i++) token[i] = (uint8_t)i;

    uint8_t bundle_key[32];
    CHECK(nostr_concord_invite_key(token, bundle_key) == NOSTR_CONCORD_OK);
    /* CORD-05 §2: bundle_key = hkdf(token, "concord/invite-key"). */
    CHECK(hex_eq(
        bundle_key,
        "5bf6803990de4e32bd1ee51dfd122381aa3c4d818fccb6e892a7f7fbcc3a8196"));
}

/* Builds a fragment body and base64url-encodes it the way a minting client
 * would, so the parser is exercised against its own wire format. */
static char *encode_fragment(const uint8_t *body, size_t len) {
    return nostr_concord_b64url_encode(body, len);
}

static void test_invite_fragment(void) {
    uint8_t token[CONCORD_INVITE_TOKEN_BYTES];
    for (size_t i = 0; i < sizeof(token); i++) token[i] = (uint8_t)(0xf0 + i);

    nostr_concord_invite_fragment_t frag;

    /* The common link: stock relays, zero relay bytes. */
    uint8_t stock[2 + CONCORD_INVITE_TOKEN_BYTES];
    stock[0] = CONCORD_INVITE_FRAGMENT_VERSION;
    stock[1] = 0x01;
    memcpy(stock + 2, token, sizeof(token));
    char *encoded = encode_fragment(stock, sizeof(stock));
    CHECK(encoded != NULL);
    CHECK(nostr_concord_invite_fragment_parse(encoded, &frag) ==
          NOSTR_CONCORD_OK);
    CHECK(frag.stock_relays);
    CHECK(frag.n_relays == 0);
    CHECK(frag.version == CONCORD_INVITE_FRAGMENT_VERSION);
    CHECK(memcmp(frag.token, token, sizeof(token)) == 0);
    nostr_concord_invite_fragment_clear(&frag);

    /* A leading '#' is tolerated so a caller can hand over the URL fragment
     * separator included. */
    char with_hash[256];
    snprintf(with_hash, sizeof(with_hash), "#%s", encoded);
    CHECK(nostr_concord_invite_fragment_parse(with_hash, &frag) ==
          NOSTR_CONCORD_OK);
    CHECK(frag.stock_relays);
    nostr_concord_invite_fragment_clear(&frag);
    free(encoded);

    /* Dictionary id, wss-implied literal, verbatim literal — one of each. */
    static const char host[] = "relay.example.org";
    static const char verbatim[] = "ws://localhost:7777";
    uint8_t body[128];
    size_t off = 0;
    body[off++] = CONCORD_INVITE_FRAGMENT_VERSION;
    body[off++] = 0x00;
    body[off++] = 3;
    body[off++] = 2; /* dictionary id 2 */
    body[off++] = 0x00;
    body[off++] = (uint8_t)(sizeof(host) - 1);
    memcpy(body + off, host, sizeof(host) - 1);
    off += sizeof(host) - 1;
    body[off++] = 0xff;
    body[off++] = (uint8_t)(sizeof(verbatim) - 1);
    memcpy(body + off, verbatim, sizeof(verbatim) - 1);
    off += sizeof(verbatim) - 1;
    memcpy(body + off, token, sizeof(token));
    off += sizeof(token);

    encoded = encode_fragment(body, off);
    CHECK(encoded != NULL);
    CHECK(nostr_concord_invite_fragment_parse(encoded, &frag) ==
          NOSTR_CONCORD_OK);
    CHECK(!frag.stock_relays);
    CHECK(frag.n_relays == 3);
    if (frag.n_relays == 3) {
        CHECK(strcmp(frag.relays[0],
                     nostr_concord_relay_dictionary_lookup(2)) == 0);
        CHECK(strcmp(frag.relays[1], "wss://relay.example.org") == 0);
        CHECK(strcmp(frag.relays[2], verbatim) == 0);
    }
    CHECK(memcmp(frag.token, token, sizeof(token)) == 0);
    nostr_concord_invite_fragment_clear(&frag);
    free(encoded);

    /* A legacy generation decodes against the wrong dictionary, so it is
     * refused rather than misread (CORD-05 §3). */
    stock[0] = CONCORD_INVITE_FRAGMENT_VERSION - 1;
    encoded = encode_fragment(stock, sizeof(stock));
    CHECK(nostr_concord_invite_fragment_parse(encoded, &frag) ==
          NOSTR_CONCORD_ERR_UNSUPPORTED_VERSION);
    free(encoded);

    /* Trailing bytes mean the relay section swallowed something: refuse. */
    uint8_t trailing[2 + CONCORD_INVITE_TOKEN_BYTES + 1];
    memset(trailing, 0, sizeof(trailing));
    trailing[0] = CONCORD_INVITE_FRAGMENT_VERSION;
    trailing[1] = 0x01;
    memcpy(trailing + 2, token, sizeof(token));
    encoded = encode_fragment(trailing, sizeof(trailing));
    CHECK(nostr_concord_invite_fragment_parse(encoded, &frag) ==
          NOSTR_CONCORD_ERR_BAD_FRAGMENT);
    free(encoded);

    /* Truncated: no room for a token at all. */
    encoded = encode_fragment(stock, 4);
    CHECK(nostr_concord_invite_fragment_parse(encoded, &frag) !=
          NOSTR_CONCORD_OK);
    free(encoded);

    /* The fragment only needs to find the bundle: 3 bootstrap relays is the
     * ceiling, and an over-count is a hostile connect-storm (CORD-05 §1). */
    uint8_t flood[3 + CONCORD_INVITE_TOKEN_BYTES + 8];
    size_t f = 0;
    flood[f++] = CONCORD_INVITE_FRAGMENT_VERSION;
    flood[f++] = 0x00;
    flood[f++] = CONCORD_MAX_RELAYS_IN_FRAGMENT + 1;
    for (int i = 0; i < CONCORD_MAX_RELAYS_IN_FRAGMENT + 1; i++)
        flood[f++] = 1; /* dictionary id 1 */
    memcpy(flood + f, token, sizeof(token));
    f += sizeof(token);
    encoded = encode_fragment(flood, f);
    CHECK(nostr_concord_invite_fragment_parse(encoded, &frag) ==
          NOSTR_CONCORD_ERR_BAD_FRAGMENT);
    free(encoded);

    /* An unknown dictionary id belongs to a generation we do not have. */
    uint8_t unknown[3 + 1 + CONCORD_INVITE_TOKEN_BYTES];
    size_t u = 0;
    unknown[u++] = CONCORD_INVITE_FRAGMENT_VERSION;
    unknown[u++] = 0x00;
    unknown[u++] = 1;
    unknown[u++] = 200;
    memcpy(unknown + u, token, sizeof(token));
    u += sizeof(token);
    encoded = encode_fragment(unknown, u);
    CHECK(nostr_concord_invite_fragment_parse(encoded, &frag) ==
          NOSTR_CONCORD_ERR_BAD_FRAGMENT);
    free(encoded);

    /* Not base64url at all. */
    CHECK(nostr_concord_invite_fragment_parse("!!!!", &frag) ==
          NOSTR_CONCORD_ERR_BAD_FRAGMENT);
}

static void test_relay_dictionary(void) {
    size_t n = 0;
    const char *const *relays = nostr_concord_stock_relays(&n);
    CHECK(relays != NULL);
    CHECK(n == 4);
    /* The dictionary is 1-based and Vector and Soapbox ship it identically. */
    CHECK(nostr_concord_relay_dictionary_lookup(0) == NULL);
    CHECK(strcmp(nostr_concord_relay_dictionary_lookup(1),
                 "wss://jskitty.com/nostr") == 0);
    CHECK(strcmp(nostr_concord_relay_dictionary_lookup(4),
                 "wss://relay.dreamith.to") == 0);
    CHECK(nostr_concord_relay_dictionary_lookup(5) == NULL);
    CHECK(nostr_concord_relay_dictionary_lookup(255) == NULL);
}

static void test_stream_layers(void) {
    uint8_t community_id[32], community_root[32];
    fill(community_id, sizeof(community_id), 0x5a);
    fill(community_root, sizeof(community_root), 0x11);

    nostr_concord_group_key_t key;
    CHECK(nostr_concord_guestbook_key(community_root, community_id, 3, &key) ==
          NOSTR_CONCORD_OK);

    static const char rumor[] =
        "{\"kind\":3306,\"content\":\"join\",\"tags\":[[\"ms\",\"42\"]]}";
    char *sealed = NULL;
    CHECK(nostr_concord_stream_seal(key.conv_key, rumor, &sealed) ==
          NOSTR_CONCORD_OK);
    CHECK(sealed != NULL);

    char *opened = NULL;
    CHECK(nostr_concord_stream_open(key.conv_key, sealed, &opened) ==
          NOSTR_CONCORD_OK);
    CHECK(opened != NULL && strcmp(opened, rumor) == 0);
    free(opened);

    /* A different plane's key does not open it: the MAC fails closed. */
    nostr_concord_group_key_t other;
    CHECK(nostr_concord_guestbook_key(community_root, community_id, 4,
                                      &other) == NOSTR_CONCORD_OK);
    opened = NULL;
    CHECK(nostr_concord_stream_open(other.conv_key, sealed, &opened) ==
          NOSTR_CONCORD_ERR_BAD_NIP44);
    CHECK(opened == NULL);
    nostr_concord_group_key_clear(&other);
    free(sealed);

    /* Empty content is "" on the wire but is never a NIP-44 plaintext. */
    sealed = NULL;
    CHECK(nostr_concord_stream_seal(key.conv_key, "", &sealed) ==
          NOSTR_CONCORD_ERR_BAD_CONTENT);
    CHECK(sealed == NULL);

    /* Garbage ciphertext is refused, not crashed on. */
    opened = NULL;
    CHECK(nostr_concord_stream_open(key.conv_key, "not-base64!", &opened) ==
          NOSTR_CONCORD_ERR_BAD_NIP44);

    nostr_concord_group_key_clear(&key);
}

static void test_invite_bundle(void) {
    uint8_t token[CONCORD_INVITE_TOKEN_BYTES];
    for (size_t i = 0; i < sizeof(token); i++) token[i] = (uint8_t)(i * 7 + 1);

    static const char bundle[] =
        "{\"community_id\":\"ab\",\"channels\":[],\"relays\":[]}";
    char *content = NULL;
    CHECK(nostr_concord_invite_bundle_encrypt(bundle, token, &content) ==
          NOSTR_CONCORD_OK);
    CHECK(content != NULL);

    char *plain = NULL;
    CHECK(nostr_concord_invite_bundle_decrypt(content, token, &plain) ==
          NOSTR_CONCORD_OK);
    CHECK(plain != NULL && strcmp(plain, bundle) == 0);
    free(plain);

    /* The token is the only thing that opens a bundle: the relay holding it
     * and the base domain serving the link both fail here. */
    token[0] ^= 0xff;
    plain = NULL;
    CHECK(nostr_concord_invite_bundle_decrypt(content, token, &plain) ==
          NOSTR_CONCORD_ERR_BAD_NIP44);
    CHECK(plain == NULL);
    free(content);
}

static void test_ordering(void) {
    int ms = -1;
    CHECK(nostr_concord_parse_ms("0", &ms) && ms == 0);
    CHECK(nostr_concord_parse_ms("42", &ms) && ms == 42);
    CHECK(nostr_concord_parse_ms("999", &ms) && ms == 999);
    /* Decimal, no leading zeros (CORD-01 "Encoding"). */
    CHECK(!nostr_concord_parse_ms("042", &ms));
    CHECK(!nostr_concord_parse_ms("00", &ms));
    CHECK(!nostr_concord_parse_ms("1000", &ms));
    CHECK(!nostr_concord_parse_ms("", &ms));
    CHECK(!nostr_concord_parse_ms("-1", &ms));
    CHECK(!nostr_concord_parse_ms("1e2", &ms));
    CHECK(!nostr_concord_parse_ms(" 1", &ms));

    int64_t t = 0;
    CHECK(nostr_concord_order_key(1686840217, 250, &t));
    CHECK(t == 1686840217LL * 1000 + 250);
    /* Out of range is malformed: the entry is dropped, never interpreted, or
     * the excess would smuggle a forged future past the clock check. */
    CHECK(!nostr_concord_order_key(1686840217, 1000, &t));
    CHECK(!nostr_concord_order_key(1686840217, -1, &t));
}

static void test_status_strings(void) {
    /* Every status has a distinct, non-empty string: a caller logging one
     * should never see "unknown status" for a value the library returns. */
    static const nostr_concord_status_t all[] = {
        NOSTR_CONCORD_OK,
        NOSTR_CONCORD_ERR_NULL,
        NOSTR_CONCORD_ERR_OOM,
        NOSTR_CONCORD_ERR_WRONG_KIND,
        NOSTR_CONCORD_ERR_BAD_PUBKEY,
        NOSTR_CONCORD_ERR_BAD_CONTENT,
        NOSTR_CONCORD_ERR_BAD_TAG,
        NOSTR_CONCORD_ERR_CARDINALITY,
        NOSTR_CONCORD_ERR_BAD_NIP44,
        NOSTR_CONCORD_ERR_BAD_SIGNATURE,
        NOSTR_CONCORD_ERR_COMMUNITY_ID,
        NOSTR_CONCORD_ERR_EPOCH,
        NOSTR_CONCORD_ERR_CHANNEL,
        NOSTR_CONCORD_ERR_INVITE_EXPIRED,
        NOSTR_CONCORD_ERR_LIST_FULL,
        NOSTR_CONCORD_ERR_SNAPSHOT_MALFORMED,
        NOSTR_CONCORD_ERR_CRYPTO,
        NOSTR_CONCORD_ERR_BAD_FRAGMENT,
        NOSTR_CONCORD_ERR_UNSUPPORTED_VERSION,
    };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        const char *s = nostr_concord_status_string(all[i]);
        CHECK(s != NULL && *s != '\0');
        CHECK(strcmp(s, "unknown status") != 0);
    }
}

static void test_null_arguments(void) {
    uint8_t buf[32];
    nostr_concord_group_key_t key;
    CHECK(nostr_concord_derive_community_id(NULL, buf, buf) ==
          NOSTR_CONCORD_ERR_NULL);
    CHECK(!nostr_concord_verify_community_id(NULL, buf, buf));
    CHECK(nostr_concord_hkdf(NULL, buf, 32, buf, false, 0, buf) ==
          NOSTR_CONCORD_ERR_NULL);
    CHECK(nostr_concord_hkdf("concord/control", buf, 0, buf, false, 0, buf) ==
          NOSTR_CONCORD_ERR_NULL);
    CHECK(nostr_concord_group_key("concord/control", NULL, 32, buf, true, 0,
                                  &key) == NOSTR_CONCORD_ERR_NULL);
    CHECK(nostr_concord_invite_key(NULL, buf) == NOSTR_CONCORD_ERR_NULL);
    CHECK(nostr_concord_stream_seal(buf, NULL, NULL) ==
          NOSTR_CONCORD_ERR_NULL);
    CHECK(!nostr_concord_hex_decode_32(NULL, buf));
    CHECK(!nostr_concord_is_lower_hex_32(NULL));
    /* Clearing a NULL handle is a no-op, not a crash. */
    nostr_concord_group_key_clear(NULL);
    nostr_concord_invite_fragment_clear(NULL);
}

int main(void) {
    test_hex();
    test_b64url();
    test_community_id();
    test_derivations();
    test_invite_key();
    test_invite_fragment();
    test_relay_dictionary();
    test_stream_layers();
    test_invite_bundle();
    test_ordering();
    test_status_strings();
    test_null_arguments();

    if (g_failures) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("test_nip_concord: all checks passed\n");
    return 0;
}
