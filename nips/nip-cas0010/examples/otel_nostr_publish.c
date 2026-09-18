/*
 * NIP-CAS-0010 example: publish opaque OTLP bytes to relays and consume them
 * back, verified and attributed.
 *
 * Usage:
 *   otel_nostr_publish <relay-url> [<relay-url> ...]
 *
 * Build: this example is OFF by default (-DBUILD_NIP_CAS0010_EXAMPLES=ON),
 * because it signs with the TEST-ONLY local raw-key signer. That signer is not
 * part of the shipped library (CMake option NOSTR_OTEL_ENABLE_TEST_SIGNER,
 * default OFF); the example links its own copy of src/otel_local_signer.c.
 *
 * Signing: this example uses the test-only local-private-key signer with an
 * EPHEMERAL key generated at start-up. Production producers must supply their
 * own NostrOtelSignFn backed by Signet / NIP-46 — the signer is a plain
 * function pointer precisely so no private key has to exist in this process.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nostr-init.h"
#include "nostr-keys.h"
#include "nostr/nip_cas0010/otel.h"

static int on_signal(const NostrOtelReceived *received, void *user_data) {
    (void)user_data;
    printf("received %zu OTLP bytes for kind %d from %s (admitted=%s, service=%s)\n",
           received->payload_len, nostr_otel_signal_kind(received->signal), received->pubkey,
           received->admitted ? "yes" : "no", received->service ? received->service : "-");
    return 0;
}

static void on_error(int err, const NostrEvent *event, void *user_data) {
    (void)user_data;
    fprintf(stderr, "rejected event %s: %s\n", (event && event->id) ? event->id : "(no id)",
            nostr_otel_strerror(err));
}

static bool admit_pubkey(const char *pubkey, void *user_data) {
    const char *allowed = (const char *)user_data;
    return pubkey && allowed && strcmp(pubkey, allowed) == 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <relay-url> [<relay-url> ...]\n", argv[0]);
        return 2;
    }
    nostr_global_init();

    char *privkey = nostr_key_generate_private();
    if (!privkey) {
        fprintf(stderr, "key generation failed\n");
        return 1;
    }
    NostrOtelLocalSigner *local = nostr_otel_local_signer_new(privkey);
    memset(privkey, 0, strlen(privkey));
    free(privkey);
    if (!local) {
        fprintf(stderr, "signer creation failed\n");
        return 1;
    }
    NostrOtelSigner signer;
    nostr_otel_local_signer_bind(local, &signer);

    NostrSimplePool *pool = nostr_simple_pool_new();
    if (!pool) {
        nostr_otel_local_signer_free(local);
        return 1;
    }

    const char **urls = (const char **)&argv[1];
    size_t url_count = (size_t)(argc - 1);

    /* Consumer: subscribe to 24900-24902 and admit only our own pubkey. */
    NostrOtelConsumerConfig ccfg = {
        .handler = on_signal,
        .on_error = on_error,
        .admit = admit_pubkey,
        .admit_user_data = (void *)nostr_otel_local_signer_pubkey(local),
        .policy = NOSTR_OTEL_ADMISSION_DROP,
    };
    NostrOtelConsumer *consumer = NULL;
    int rc = nostr_otel_consumer_new(&ccfg, &consumer);
    if (rc != NOSTR_OTEL_OK) {
        fprintf(stderr, "consumer: %s\n", nostr_otel_strerror(rc));
        goto cleanup;
    }
    nostr_simple_pool_start(pool);
    rc = nostr_otel_consumer_subscribe(consumer, pool, urls, url_count, true);
    if (rc != NOSTR_OTEL_OK) {
        fprintf(stderr, "subscribe: %s\n", nostr_otel_strerror(rc));
        goto cleanup;
    }

    /* Producer: publish a single opaque OTLP payload. The bytes below stand in
     * for a serialized TracesData message produced by an OTLP library; this
     * module never inspects them. */
    NostrOtelProducerConfig pcfg = {
        .signer = signer,
        .publish = nostr_otel_publish_via_pool,
        .publish_user_data = pool,
        .service = "example-service",
        .compression = nostr_otel_zstd_available() ? NOSTR_OTEL_COMPRESSION_ZSTD
                                                   : NOSTR_OTEL_COMPRESSION_IDENTITY,
    };
    NostrOtelProducer *producer = NULL;
    rc = nostr_otel_producer_new(&pcfg, &producer);
    if (rc != NOSTR_OTEL_OK) {
        fprintf(stderr, "producer: %s\n", nostr_otel_strerror(rc));
        goto cleanup;
    }

    static const uint8_t otlp_bytes[] = "opaque-otlp-traces-payload";
    NostrOtelPayload payload = {otlp_bytes, sizeof(otlp_bytes) - 1};
    size_t published = 0;
    rc = nostr_otel_producer_publish(producer, NOSTR_OTEL_SIGNAL_TRACES, &payload, 1, &published);
    if (rc != NOSTR_OTEL_OK) {
        fprintf(stderr, "publish: %s\n", nostr_otel_strerror(rc));
    } else {
        printf("published %zu event(s)\n", published);
    }
    sleep(3); /* let the subscription deliver the ephemeral event back */
    nostr_otel_producer_free(producer);

cleanup:
    nostr_otel_consumer_free(consumer);
    nostr_simple_pool_stop(pool);
    nostr_simple_pool_free(pool);
    nostr_otel_local_signer_free(local);
    nostr_global_cleanup();
    return rc == NOSTR_OTEL_OK ? 0 : 1;
}
