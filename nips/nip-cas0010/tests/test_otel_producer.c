/* NIP-CAS-0010: producer tests — tags, signing, batching and size limits. */

#include "otel_test_util.h"

static const char *tag_value(const NostrTags *tags, const char *name, size_t index) {
    const NostrTag *tag = otel_find_tag(tags, name);
    OTEL_CHECK(tag != NULL, "expected tag missing");
    return nostr_tag_get(tag, index);
}

static void test_build_event_shape(void) {
    OtelTestSigner ts;
    otel_test_signer_init(&ts);
    OtelEventSink sink = {0};

    const char *recipients[] = {
        "0000000000000000000000000000000000000000000000000000000000000001"};
    NostrOtelProducerConfig cfg = {
        .signer = ts.signer,
        .publish = otel_sink_publish,
        .publish_user_data = &sink,
        .service = "loom-worker",
        .recipients = recipients,
        .recipient_count = 1,
    };
    NostrOtelProducer *producer = NULL;
    OTEL_CHECK_RC(nostr_otel_producer_new(&cfg, &producer), NOSTR_OTEL_OK);

    const char *body = "otlp-trace-bytes";
    NostrEvent *event = NULL;
    OTEL_CHECK_RC(nostr_otel_producer_build_event(producer, NOSTR_OTEL_SIGNAL_TRACES,
                                                  (const uint8_t *)body, strlen(body), &event),
                  NOSTR_OTEL_OK);

    OTEL_CHECK(event->kind == NOSTR_OTEL_KIND_TRACES, "kind must be 24900");
    OTEL_CHECK(strcmp(event->pubkey, otel_test_signer_pubkey(&ts)) == 0, "signer pubkey");
    OTEL_CHECK(nostr_event_check_signature(event), "produced event must verify");

    OTEL_CHECK(otel_count_tags(event->tags, "domain") == 1, "exactly one domain tag");
    OTEL_CHECK(otel_count_tags(event->tags, "schema") == 1, "exactly one schema tag");
    OTEL_CHECK(otel_count_tags(event->tags, "enc") == 1, "exactly one enc tag");
    OTEL_CHECK(strcmp(tag_value(event->tags, "domain", 1), "traces") == 0, "domain value");
    OTEL_CHECK(strcmp(tag_value(event->tags, "schema", 1), "cascadia.otel.traces.v1") == 0,
               "schema value");
    OTEL_CHECK(strcmp(tag_value(event->tags, "service", 1), "loom-worker") == 0, "service value");
    OTEL_CHECK(strcmp(tag_value(event->tags, "p", 1), recipients[0]) == 0, "p value");
    OTEL_CHECK(strcmp(tag_value(event->tags, "enc", 1), "otlp-proto") == 0, "enc format");
    OTEL_CHECK(strcmp(tag_value(event->tags, "enc", 2), "identity") == 0, "enc compression");
    OTEL_CHECK(nostr_tag_size(otel_find_tag(event->tags, "enc")) == 3, "enc arity");

    /* Content is base64 of the opaque bytes — the module never reshapes them. */
    uint8_t *decoded = NULL;
    size_t decoded_len = 0;
    OTEL_CHECK_RC(nostr_otel_base64_decode(event->content, &decoded, &decoded_len),
                  NOSTR_OTEL_OK);
    OTEL_CHECK(decoded_len == strlen(body), "content length");
    OTEL_CHECK(memcmp(decoded, body, decoded_len) == 0, "content bytes");
    free(decoded);

    nostr_event_free(event);
    nostr_otel_producer_free(producer);
    otel_sink_clear(&sink);
    otel_test_signer_clear(&ts);
}

static void test_publish_single(void) {
    OtelTestSigner ts;
    otel_test_signer_init(&ts);
    OtelEventSink sink = {0};
    NostrOtelProducerConfig cfg = {
        .signer = ts.signer,
        .publish = otel_sink_publish,
        .publish_user_data = &sink,
    };
    NostrOtelProducer *producer = NULL;
    OTEL_CHECK_RC(nostr_otel_producer_new(&cfg, &producer), NOSTR_OTEL_OK);

    const char *body = "metrics";
    NostrOtelPayload payload = {(const uint8_t *)body, strlen(body)};
    size_t events = 0;
    OTEL_CHECK_RC(nostr_otel_producer_publish(producer, NOSTR_OTEL_SIGNAL_METRICS, &payload, 1,
                                              &events),
                  NOSTR_OTEL_OK);
    OTEL_CHECK(events == 1, "one payload publishes one event");
    OTEL_CHECK(sink.count == 1, "sink received one event");
    OTEL_CHECK(sink.events[0]->kind == NOSTR_OTEL_KIND_METRICS, "metrics kind");

    /* A failing transport surfaces as NOSTR_OTEL_ERR_PUBLISH. */
    sink.fail_next = 1;
    OTEL_CHECK_RC(nostr_otel_producer_publish(producer, NOSTR_OTEL_SIGNAL_METRICS, &payload, 1,
                                              &events),
                  NOSTR_OTEL_ERR_PUBLISH);

    nostr_otel_producer_free(producer);
    otel_sink_clear(&sink);
    otel_test_signer_clear(&ts);
}

/* A small cap forces the batcher to split a payload list across events, and the
 * concatenation of the published bodies must reproduce the input stream. */
static void test_batching_splits_under_cap(void) {
    OtelTestSigner ts;
    otel_test_signer_init(&ts);
    OtelEventSink sink = {0};
    NostrOtelProducerConfig cfg = {
        .signer = ts.signer,
        .publish = otel_sink_publish,
        .publish_user_data = &sink,
        .max_event_bytes = 256, /* base64 chars */
    };
    NostrOtelProducer *producer = NULL;
    OTEL_CHECK_RC(nostr_otel_producer_new(&cfg, &producer), NOSTR_OTEL_OK);

    enum { PAYLOADS = 12, PAYLOAD_LEN = 64 };
    uint8_t blobs[PAYLOADS][PAYLOAD_LEN];
    NostrOtelPayload payloads[PAYLOADS];
    for (size_t i = 0; i < PAYLOADS; i++) {
        memset(blobs[i], (int)('A' + i), PAYLOAD_LEN);
        payloads[i].data = blobs[i];
        payloads[i].len = PAYLOAD_LEN;
    }
    size_t events = 0;
    OTEL_CHECK_RC(nostr_otel_producer_publish(producer, NOSTR_OTEL_SIGNAL_LOGS, payloads,
                                              PAYLOADS, &events),
                  NOSTR_OTEL_OK);
    /* 64 raw bytes -> 88 base64 chars; at most 2 payloads fit in 256 chars. */
    OTEL_CHECK(events > 1, "oversized batch must be split across events");
    OTEL_CHECK(events == sink.count, "event count matches sink");

    size_t offset = 0;
    for (size_t i = 0; i < sink.count; i++) {
        OTEL_CHECK(strlen(sink.events[i]->content) <= 256, "content exceeds cap");
        uint8_t *decoded = NULL;
        size_t decoded_len = 0;
        OTEL_CHECK_RC(nostr_otel_base64_decode(sink.events[i]->content, &decoded, &decoded_len),
                      NOSTR_OTEL_OK);
        OTEL_CHECK(offset + decoded_len <= PAYLOADS * PAYLOAD_LEN, "batches exceed input");
        OTEL_CHECK(memcmp(decoded, (const uint8_t *)blobs + offset, decoded_len) == 0,
                   "batch bytes must be the input stream in order");
        offset += decoded_len;
        free(decoded);
    }
    OTEL_CHECK(offset == PAYLOADS * PAYLOAD_LEN, "all payload bytes must be published");

    nostr_otel_producer_free(producer);
    otel_sink_clear(&sink);
    otel_test_signer_clear(&ts);
}

static void test_single_payload_over_cap(void) {
    OtelTestSigner ts;
    otel_test_signer_init(&ts);
    OtelEventSink sink = {0};
    NostrOtelProducerConfig cfg = {
        .signer = ts.signer,
        .publish = otel_sink_publish,
        .publish_user_data = &sink,
        .max_event_bytes = 64,
    };
    NostrOtelProducer *producer = NULL;
    OTEL_CHECK_RC(nostr_otel_producer_new(&cfg, &producer), NOSTR_OTEL_OK);

    uint8_t big[512];
    memset(big, 'z', sizeof(big));
    NostrOtelPayload payload = {big, sizeof(big)};
    size_t events = 0;
    OTEL_CHECK_RC(nostr_otel_producer_publish(producer, NOSTR_OTEL_SIGNAL_TRACES, &payload, 1,
                                              &events),
                  NOSTR_OTEL_ERR_BATCH_TOO_LARGE);
    OTEL_CHECK(events == 0, "nothing should have been published");

    NostrEvent *event = NULL;
    OTEL_CHECK_RC(nostr_otel_producer_build_event(producer, NOSTR_OTEL_SIGNAL_TRACES, big,
                                                  sizeof(big), &event),
                  NOSTR_OTEL_ERR_BATCH_TOO_LARGE);
    OTEL_CHECK(event == NULL, "failed build must not return an event");

    nostr_otel_producer_free(producer);
    otel_sink_clear(&sink);
    otel_test_signer_clear(&ts);
}

static void test_default_cap_is_64k(void) {
    OtelTestSigner ts;
    otel_test_signer_init(&ts);
    OtelEventSink sink = {0};
    NostrOtelProducerConfig cfg = {
        .signer = ts.signer,
        .publish = otel_sink_publish,
        .publish_user_data = &sink,
    };
    NostrOtelProducer *producer = NULL;
    OTEL_CHECK_RC(nostr_otel_producer_new(&cfg, &producer), NOSTR_OTEL_OK);

    /* 49152 raw bytes encode to exactly 65536 base64 chars: the first size that
     * must not fit under the recommended 64 KiB cap. */
    size_t raw_len = 49152;
    uint8_t *raw = malloc(raw_len + 3);
    OTEL_CHECK(raw != NULL, "alloc");
    memset(raw, 'q', raw_len + 3);
    NostrEvent *event = NULL;
    OTEL_CHECK_RC(nostr_otel_producer_build_event(producer, NOSTR_OTEL_SIGNAL_LOGS, raw, raw_len,
                                                  &event),
                  NOSTR_OTEL_OK);
    OTEL_CHECK(strlen(event->content) == NOSTR_OTEL_DEFAULT_MAX_EVENT_BYTES, "cap boundary");
    nostr_event_free(event);

    OTEL_CHECK_RC(nostr_otel_producer_build_event(producer, NOSTR_OTEL_SIGNAL_LOGS, raw,
                                                  raw_len + 3, &event),
                  NOSTR_OTEL_ERR_BATCH_TOO_LARGE);
    free(raw);
    nostr_otel_producer_free(producer);
    otel_sink_clear(&sink);
    otel_test_signer_clear(&ts);
}

static void test_config_validation(void) {
    OtelTestSigner ts;
    otel_test_signer_init(&ts);
    OtelEventSink sink = {0};
    NostrOtelProducer *producer = NULL;

    NostrOtelProducerConfig no_signer = {.publish = otel_sink_publish, .publish_user_data = &sink};
    OTEL_CHECK_RC(nostr_otel_producer_new(&no_signer, &producer), NOSTR_OTEL_ERR_INVALID_ARG);

    NostrOtelProducerConfig no_publish = {.signer = ts.signer};
    OTEL_CHECK_RC(nostr_otel_producer_new(&no_publish, &producer), NOSTR_OTEL_ERR_INVALID_ARG);

    if (!nostr_otel_zstd_available()) {
        NostrOtelProducerConfig zstd_cfg = {
            .signer = ts.signer,
            .publish = otel_sink_publish,
            .publish_user_data = &sink,
            .compression = NOSTR_OTEL_COMPRESSION_ZSTD,
        };
        OTEL_CHECK_RC(nostr_otel_producer_new(&zstd_cfg, &producer),
                      NOSTR_OTEL_ERR_UNSUPPORTED_ENCODING);
    }
    OTEL_CHECK(producer == NULL, "failed construction must not return a producer");

    /* The local signer never accepts a malformed key. */
    OTEL_CHECK(nostr_otel_local_signer_new(NULL) == NULL, "NULL key rejected");
    OTEL_CHECK(nostr_otel_local_signer_new("deadbeef") == NULL, "short key rejected");

    otel_sink_clear(&sink);
    otel_test_signer_clear(&ts);
}

static int failing_sign(NostrEvent *event, void *user_data) {
    (void)event;
    (void)user_data;
    return -1;
}

/* A signer that refuses must surface NOSTR_OTEL_ERR_SIGN and release the
 * encoded content exactly once (ASan catches a double free here). */
static void test_signer_failure(void) {
    OtelEventSink sink = {0};
    NostrOtelProducerConfig cfg = {
        .signer = {.sign = failing_sign, .user_data = NULL},
        .publish = otel_sink_publish,
        .publish_user_data = &sink,
    };
    NostrOtelProducer *producer = NULL;
    OTEL_CHECK_RC(nostr_otel_producer_new(&cfg, &producer), NOSTR_OTEL_OK);

    const char *body = "otlp";
    NostrEvent *event = NULL;
    OTEL_CHECK_RC(nostr_otel_producer_build_event(producer, NOSTR_OTEL_SIGNAL_TRACES,
                                                  (const uint8_t *)body, strlen(body), &event),
                  NOSTR_OTEL_ERR_SIGN);
    OTEL_CHECK(event == NULL, "failed signing must not return an event");

    NostrOtelPayload payload = {(const uint8_t *)body, strlen(body)};
    size_t events = 0;
    OTEL_CHECK_RC(nostr_otel_producer_publish(producer, NOSTR_OTEL_SIGNAL_TRACES, &payload, 1,
                                              &events),
                  NOSTR_OTEL_ERR_SIGN);
    OTEL_CHECK(events == 0 && sink.count == 0, "nothing published when signing fails");

    nostr_otel_producer_free(producer);
    otel_sink_clear(&sink);
}

int main(void) {
    test_build_event_shape();
    test_publish_single();
    test_batching_splits_under_cap();
    test_single_payload_over_cap();
    test_default_cap_is_64k();
    test_config_validation();
    test_signer_failure();
    printf("test_otel_producer: OK\n");
    return 0;
}
