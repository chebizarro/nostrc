/* NIP-CAS-0010: base64, `enc` tag parsing and body codec tests. */

#include "otel_test_util.h"

static void test_base64_vectors(void) {
    /* RFC 4648 §10 test vectors. */
    const char *inputs[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar"};
    const char *expected[] = {"", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy"};
    for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
        char *encoded = NULL;
        OTEL_CHECK_RC(nostr_otel_base64_encode((const uint8_t *)inputs[i], strlen(inputs[i]),
                                               &encoded),
                      NOSTR_OTEL_OK);
        OTEL_CHECK(strcmp(encoded, expected[i]) == 0, "base64 vector mismatch");

        uint8_t *decoded = NULL;
        size_t decoded_len = 0;
        OTEL_CHECK_RC(nostr_otel_base64_decode(encoded, &decoded, &decoded_len), NOSTR_OTEL_OK);
        OTEL_CHECK(decoded_len == strlen(inputs[i]), "base64 decode length mismatch");
        OTEL_CHECK(memcmp(decoded, inputs[i], decoded_len) == 0, "base64 decode mismatch");
        free(decoded);
        free(encoded);
    }
}

static void test_base64_binary_roundtrip(void) {
    uint8_t body[1024];
    for (size_t i = 0; i < sizeof(body); i++) {
        body[i] = (uint8_t)((i * 31u + 7u) & 0xFF);
    }
    for (size_t len = 0; len <= sizeof(body); len += 97) {
        char *encoded = NULL;
        OTEL_CHECK_RC(nostr_otel_base64_encode(body, len, &encoded), NOSTR_OTEL_OK);
        uint8_t *decoded = NULL;
        size_t decoded_len = 0;
        OTEL_CHECK_RC(nostr_otel_base64_decode(encoded, &decoded, &decoded_len), NOSTR_OTEL_OK);
        OTEL_CHECK(decoded_len == len, "binary roundtrip length mismatch");
        OTEL_CHECK(len == 0 || memcmp(decoded, body, len) == 0, "binary roundtrip mismatch");
        free(decoded);
        free(encoded);
    }
}

static void test_base64_rejects_malformed(void) {
    const char *bad[] = {"Zg=", "Zm9vY", "Zg=a", "Zm9v!!!!", "=Zm9", "Z===", "Zm9vYmFy "};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        uint8_t *out = NULL;
        size_t out_len = 0;
        int rc = nostr_otel_base64_decode(bad[i], &out, &out_len);
        OTEL_CHECK(rc == NOSTR_OTEL_ERR_MALFORMED_EVENT, "malformed base64 accepted");
        OTEL_CHECK(out == NULL, "malformed base64 leaked an output buffer");
    }
}

static void test_signal_mapping(void) {
    OTEL_CHECK(nostr_otel_signal_kind(NOSTR_OTEL_SIGNAL_TRACES) == 24900, "traces kind");
    OTEL_CHECK(nostr_otel_signal_kind(NOSTR_OTEL_SIGNAL_METRICS) == 24901, "metrics kind");
    OTEL_CHECK(nostr_otel_signal_kind(NOSTR_OTEL_SIGNAL_LOGS) == 24902, "logs kind");
    OTEL_CHECK(nostr_otel_signal_for_kind(24900) == NOSTR_OTEL_SIGNAL_TRACES, "kind 24900");
    OTEL_CHECK(nostr_otel_signal_for_kind(1) == NOSTR_OTEL_SIGNAL_NONE, "kind 1 is not telemetry");
    OTEL_CHECK(strcmp(nostr_otel_signal_domain(NOSTR_OTEL_SIGNAL_LOGS), "logs") == 0, "domain");
    OTEL_CHECK(strcmp(nostr_otel_signal_schema(NOSTR_OTEL_SIGNAL_METRICS),
                      "cascadia.otel.metrics.v1") == 0,
               "schema");
}

static void test_enc_missing_defaults(void) {
    NostrTags *tags = nostr_tags_new(0);
    nostr_tags_append(tags, nostr_tag_new("domain", "traces", NULL));

    NostrOtelEncoding enc;
    OTEL_CHECK_RC(nostr_otel_parse_encoding(tags, &enc), NOSTR_OTEL_OK);
    OTEL_CHECK(enc.format == NOSTR_OTEL_FORMAT_OTLP_PROTO, "missing enc must default to proto");
    OTEL_CHECK(enc.compression == NOSTR_OTEL_COMPRESSION_IDENTITY,
               "missing enc must default to identity");

    /* No tags at all behaves the same way. */
    OTEL_CHECK_RC(nostr_otel_parse_encoding(NULL, &enc), NOSTR_OTEL_OK);
    OTEL_CHECK(enc.format == NOSTR_OTEL_FORMAT_OTLP_PROTO, "NULL tags default format");
    OTEL_CHECK(enc.compression == NOSTR_OTEL_COMPRESSION_IDENTITY, "NULL tags default compression");
    nostr_tags_free(tags);
}

static void test_enc_wellformed(void) {
    NostrTags *tags = nostr_tags_new(0);
    nostr_tags_append(tags, nostr_tag_new("enc", "otlp-json", "identity", NULL));
    NostrOtelEncoding enc;
    OTEL_CHECK_RC(nostr_otel_parse_encoding(tags, &enc), NOSTR_OTEL_OK);
    OTEL_CHECK(enc.format == NOSTR_OTEL_FORMAT_OTLP_JSON, "json format");
    OTEL_CHECK(enc.compression == NOSTR_OTEL_COMPRESSION_IDENTITY, "identity compression");
    nostr_tags_free(tags);
}

static void test_enc_rejections(void) {
    NostrOtelEncoding enc;

    /* Duplicate enc tags. */
    NostrTags *dup = nostr_tags_new(0);
    nostr_tags_append(dup, nostr_tag_new("enc", "otlp-proto", "identity", NULL));
    nostr_tags_append(dup, nostr_tag_new("enc", "otlp-proto", "zstd", NULL));
    OTEL_CHECK_RC(nostr_otel_parse_encoding(dup, &enc), NOSTR_OTEL_ERR_MALFORMED_EVENT);
    nostr_tags_free(dup);

    /* Too short. */
    NostrTags *short_tag = nostr_tags_new(0);
    nostr_tags_append(short_tag, nostr_tag_new("enc", "otlp-proto", NULL));
    OTEL_CHECK_RC(nostr_otel_parse_encoding(short_tag, &enc), NOSTR_OTEL_ERR_MALFORMED_EVENT);
    nostr_tags_free(short_tag);

    /* Overlong. */
    NostrTags *long_tag = nostr_tags_new(0);
    nostr_tags_append(long_tag, nostr_tag_new("enc", "otlp-proto", "identity", "extra", NULL));
    OTEL_CHECK_RC(nostr_otel_parse_encoding(long_tag, &enc), NOSTR_OTEL_ERR_MALFORMED_EVENT);
    nostr_tags_free(long_tag);

    /* Unknown tokens. */
    NostrTags *bad_format = nostr_tags_new(0);
    nostr_tags_append(bad_format, nostr_tag_new("enc", "otlp-cbor", "identity", NULL));
    OTEL_CHECK_RC(nostr_otel_parse_encoding(bad_format, &enc),
                  NOSTR_OTEL_ERR_UNSUPPORTED_ENCODING);
    nostr_tags_free(bad_format);

    NostrTags *bad_comp = nostr_tags_new(0);
    nostr_tags_append(bad_comp, nostr_tag_new("enc", "otlp-proto", "gzip", NULL));
    OTEL_CHECK_RC(nostr_otel_parse_encoding(bad_comp, &enc), NOSTR_OTEL_ERR_UNSUPPORTED_ENCODING);
    nostr_tags_free(bad_comp);
}

static void test_body_identity(void) {
    const char *body = "opaque-otlp-bytes\x00\x01\x02";
    size_t len = 20;
    char *content = NULL;
    NostrOtelCompression used = NOSTR_OTEL_COMPRESSION_UNKNOWN;
    OTEL_CHECK_RC(nostr_otel_encode_body((const uint8_t *)body, len,
                                         NOSTR_OTEL_COMPRESSION_IDENTITY, SIZE_MAX, &content,
                                         &used),
                  NOSTR_OTEL_OK);
    OTEL_CHECK(used == NOSTR_OTEL_COMPRESSION_IDENTITY, "identity requested");
    /* Content is base64 even for identity (§3.2). */
    OTEL_CHECK(strcmp(content, "b3BhcXVlLW90bHAtYnl0ZXMAAQI=") == 0, "identity content base64");

    NostrOtelEncoding enc = {NOSTR_OTEL_FORMAT_OTLP_PROTO, NOSTR_OTEL_COMPRESSION_IDENTITY};
    uint8_t *decoded = NULL;
    size_t decoded_len = 0;
    OTEL_CHECK_RC(nostr_otel_decode_body(content, enc, 0, &decoded, &decoded_len), NOSTR_OTEL_OK);
    OTEL_CHECK(decoded_len == len, "identity decode length");
    OTEL_CHECK(memcmp(decoded, body, len) == 0, "identity decode bytes");
    free(decoded);
    free(content);
}

static void test_body_zstd(void) {
    if (!nostr_otel_zstd_available()) {
        printf("  (zstd unavailable in this build — skipping zstd body test)\n");
        return;
    }
    uint8_t body[8192];
    for (size_t i = 0; i < sizeof(body); i++) {
        body[i] = (uint8_t)('a' + (i % 7));
    }
    char *content = NULL;
    NostrOtelCompression used = NOSTR_OTEL_COMPRESSION_UNKNOWN;
    OTEL_CHECK_RC(nostr_otel_encode_body(body, sizeof(body), NOSTR_OTEL_COMPRESSION_ZSTD, 1024,
                                         &content, &used),
                  NOSTR_OTEL_OK);
    OTEL_CHECK(used == NOSTR_OTEL_COMPRESSION_ZSTD, "zstd applied above threshold");
    OTEL_CHECK(strlen(content) < sizeof(body), "zstd content should be smaller");

    NostrOtelEncoding enc = {NOSTR_OTEL_FORMAT_OTLP_PROTO, NOSTR_OTEL_COMPRESSION_ZSTD};
    uint8_t *decoded = NULL;
    size_t decoded_len = 0;
    OTEL_CHECK_RC(nostr_otel_decode_body(content, enc, 0, &decoded, &decoded_len), NOSTR_OTEL_OK);
    OTEL_CHECK(decoded_len == sizeof(body), "zstd decode length");
    OTEL_CHECK(memcmp(decoded, body, sizeof(body)) == 0, "zstd decode bytes");

    /* Decompression-bomb guard: a body larger than the caller's bound is rejected. */
    uint8_t *bounded = NULL;
    size_t bounded_len = 0;
    OTEL_CHECK_RC(nostr_otel_decode_body(content, enc, 64, &bounded, &bounded_len),
                  NOSTR_OTEL_ERR_PAYLOAD_TOO_LARGE);
    OTEL_CHECK(bounded == NULL, "bomb guard leaked a buffer");

    free(decoded);
    free(content);

    /* Below the threshold the producer stays on identity. */
    char *small = NULL;
    used = NOSTR_OTEL_COMPRESSION_UNKNOWN;
    OTEL_CHECK_RC(nostr_otel_encode_body(body, 16, NOSTR_OTEL_COMPRESSION_ZSTD, 1024, &small,
                                         &used),
                  NOSTR_OTEL_OK);
    OTEL_CHECK(used == NOSTR_OTEL_COMPRESSION_IDENTITY, "below threshold stays identity");
    free(small);
}

static void test_identity_bound(void) {
    uint8_t body[256];
    memset(body, 'x', sizeof(body));
    char *content = NULL;
    OTEL_CHECK_RC(nostr_otel_encode_body(body, sizeof(body), NOSTR_OTEL_COMPRESSION_IDENTITY,
                                         SIZE_MAX, &content, NULL),
                  NOSTR_OTEL_OK);
    NostrOtelEncoding enc = {NOSTR_OTEL_FORMAT_OTLP_PROTO, NOSTR_OTEL_COMPRESSION_IDENTITY};
    uint8_t *decoded = NULL;
    size_t decoded_len = 0;
    OTEL_CHECK_RC(nostr_otel_decode_body(content, enc, 64, &decoded, &decoded_len),
                  NOSTR_OTEL_ERR_PAYLOAD_TOO_LARGE);
    OTEL_CHECK(decoded == NULL, "identity bound leaked a buffer");
    free(content);
}

int main(void) {
    test_base64_vectors();
    test_base64_binary_roundtrip();
    test_base64_rejects_malformed();
    test_signal_mapping();
    test_enc_missing_defaults();
    test_enc_wellformed();
    test_enc_rejections();
    test_body_identity();
    test_body_zstd();
    test_identity_bound();
    printf("test_otel_encoding: OK\n");
    return 0;
}
