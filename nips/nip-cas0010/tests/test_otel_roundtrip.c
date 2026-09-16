/* NIP-CAS-0010: producer -> consumer loopback.
 *
 * A full in-process relay is impractical here, so the producer publishes into a
 * sink and every captured event is fed to the consumer's verify/decode path —
 * the same code a relay subscription drives. The OTLP bytes must come back
 * byte-identical and attributed to the signing pubkey. */

#include "otel_test_util.h"

typedef struct {
    uint8_t *bytes;
    size_t len;
    size_t cap;
    size_t events;
    char pubkey[65];
    bool all_admitted;
} Reassembly;

static int reassemble(const NostrOtelReceived *received, void *user_data) {
    Reassembly *r = (Reassembly *)user_data;
    if (r->len + received->payload_len > r->cap) {
        size_t cap = (r->len + received->payload_len) * 2;
        uint8_t *grown = realloc(r->bytes, cap);
        OTEL_CHECK(grown != NULL, "reassembly realloc");
        r->bytes = grown;
        r->cap = cap;
    }
    memcpy(r->bytes + r->len, received->payload, received->payload_len);
    r->len += received->payload_len;
    r->events++;
    r->all_admitted = r->all_admitted && received->admitted;
    snprintf(r->pubkey, sizeof(r->pubkey), "%s", received->pubkey);
    return 0;
}

static void run_roundtrip(NostrOtelCompression compression, size_t max_event_bytes,
                          size_t payload_count, size_t payload_len) {
    OtelTestSigner ts;
    otel_test_signer_init(&ts);
    OtelEventSink sink = {0};

    NostrOtelProducerConfig pcfg = {
        .signer = ts.signer,
        .publish = otel_sink_publish,
        .publish_user_data = &sink,
        .compression = compression,
        .compression_threshold = 32,
        .max_event_bytes = max_event_bytes,
        .service = "grasp-gitea",
    };
    NostrOtelProducer *producer = NULL;
    OTEL_CHECK_RC(nostr_otel_producer_new(&pcfg, &producer), NOSTR_OTEL_OK);

    /* Deterministic pseudo-random opaque "OTLP" bytes. */
    size_t total = payload_count * payload_len;
    uint8_t *expected = malloc(total);
    OTEL_CHECK(expected != NULL, "alloc");
    uint32_t state = 0x13572468u;
    for (size_t i = 0; i < total; i++) {
        state = state * 1664525u + 1013904223u;
        expected[i] = (uint8_t)(state >> 24);
    }
    NostrOtelPayload *payloads = calloc(payload_count, sizeof(*payloads));
    OTEL_CHECK(payloads != NULL, "alloc");
    for (size_t i = 0; i < payload_count; i++) {
        payloads[i].data = expected + i * payload_len;
        payloads[i].len = payload_len;
    }

    size_t published = 0;
    OTEL_CHECK_RC(nostr_otel_producer_publish(producer, NOSTR_OTEL_SIGNAL_METRICS, payloads,
                                              payload_count, &published),
                  NOSTR_OTEL_OK);
    OTEL_CHECK(published == sink.count && published > 0, "events published");

    Reassembly r = {0};
    r.all_admitted = true;
    NostrOtelSignal signals[] = {NOSTR_OTEL_SIGNAL_METRICS};
    NostrOtelConsumerConfig ccfg = {
        .signals = signals,
        .signal_count = 1,
        .handler = reassemble,
        .handler_user_data = &r,
    };
    NostrOtelConsumer *consumer = NULL;
    OTEL_CHECK_RC(nostr_otel_consumer_new(&ccfg, &consumer), NOSTR_OTEL_OK);

    for (size_t i = 0; i < sink.count; i++) {
        OTEL_CHECK(strlen(sink.events[i]->content) <= max_event_bytes, "event exceeds cap");
        OTEL_CHECK_RC(nostr_otel_consumer_process(consumer, sink.events[i]), NOSTR_OTEL_OK);
    }
    OTEL_CHECK(r.events == sink.count, "every event decoded");
    OTEL_CHECK(r.len == total, "reassembled length matches input");
    OTEL_CHECK(memcmp(r.bytes, expected, total) == 0, "OTLP bytes survive the round trip");
    OTEL_CHECK(strcmp(r.pubkey, otel_test_signer_pubkey(&ts)) == 0, "attributed to signer");
    OTEL_CHECK(r.all_admitted, "admitted by default");

    free(r.bytes);
    free(payloads);
    free(expected);
    nostr_otel_consumer_free(consumer);
    nostr_otel_producer_free(producer);
    otel_sink_clear(&sink);
    otel_test_signer_clear(&ts);
}

int main(void) {
    /* Single event, identity. */
    run_roundtrip(NOSTR_OTEL_COMPRESSION_IDENTITY, NOSTR_OTEL_DEFAULT_MAX_EVENT_BYTES, 4, 256);
    /* Batched across several events, identity. */
    run_roundtrip(NOSTR_OTEL_COMPRESSION_IDENTITY, 512, 24, 100);
    if (nostr_otel_zstd_available()) {
        run_roundtrip(NOSTR_OTEL_COMPRESSION_ZSTD, NOSTR_OTEL_DEFAULT_MAX_EVENT_BYTES, 8, 4096);
        run_roundtrip(NOSTR_OTEL_COMPRESSION_ZSTD, 2048, 16, 1024);
    } else {
        printf("  (zstd unavailable in this build — identity-only round trip)\n");
    }
    printf("test_otel_roundtrip: OK\n");
    return 0;
}
